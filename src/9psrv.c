/* 9psrv.c — minimal 9P file server for the RP2040, served over USB CDC.
 *
 * Tree:
 *   /                 directory
 *   /dev/             directory
 *   /dev/gpio0..gpio29  read => "0"/"1", write "0"/"1" to drive the pin
 *   /flash/           directory
 *   /flash/<name>     file stored in the reserved flash region (XIP-readable)
 *
 * The flash store is deliberately simple: one 4 KiB block per file, a small
 * directory kept (master copy in SRAM, persisted to sector 0 of the reserved
 * region). Reads are served straight from XIP; writes erase + program the
 * block sector.
 */

#include <string.h>
#include <stdio.h>
#include "hardware/gpio.h"
#include "hardware/flash.h"

#include "c9/proto.h"
#include "9psrv.h"

#ifndef PICO_9P_FLASH_OFFSET
#define PICO_9P_FLASH_OFFSET 0x00180000u   /* last 256 KiB of a 2 MiB device */
#endif
#ifndef PICO_9P_FLASH_SIZE
#define PICO_9P_FLASH_SIZE   0x00040000u
#endif

#define FLASH_BLOCK 4096u            /* one flash sector per file */
#define FLASH_DIR_BLOCK 0u           /* directory lives in block 0 */
#define FLASH_FIRST_FILE_BLOCK 1u

/* XIP_BASE comes from the SDK (hardware/addressmap.h via hardware/flash.h). */
#define FLASH_DIR_ADDR   (XIP_BASE + PICO_9P_FLASH_OFFSET + FLASH_DIR_BLOCK*FLASH_BLOCK)
#define FLASH_FILE_ADDR(idx) (XIP_BASE + PICO_9P_FLASH_OFFSET + (FLASH_FIRST_FILE_BLOCK + (idx))*FLASH_BLOCK)

static Srv srv_storage;
static Srv *the_srv = &srv_storage;

static SrvNode *
node_alloc(Srv *srv, const char *name, uint64_t qid, uint32_t mode,
           int isdir, int isgpio, int gpio_num, int flash_idx, uint64_t parent)
{
    if (srv->nnodes >= SRV_MAX_NODES)
        return NULL;
    SrvNode *n = &srv->nodes[srv->nnodes++];
    memset(n, 0, sizeof(*n));
    snprintf(n->name, sizeof(n->name), "%s", name);
    n->qid = qid;
    n->mode = mode;
    n->isdir = isdir;
    n->isgpio = isgpio;
    n->gpio_num = gpio_num;
    n->flash_idx = flash_idx;
    n->parent = parent;
    return n;
}

static SrvNode *
node_find_child(Srv *srv, SrvNode *cur, const char *name)
{
    for (int i = 0; i < srv->nnodes; i++) {
        SrvNode *c = &srv->nodes[i];
        if (c->parent != cur->qid)
            continue;
        if (strcmp(c->name, name) == 0)
            return c;
    }
    return NULL;
}

static SrvNode *
node_by_qid(Srv *srv, uint64_t qid)
{
    for (int i = 0; i < srv->nnodes; i++)
        if (srv->nodes[i].qid == qid)
            return &srv->nodes[i];
    return NULL;
}

/* ------------------------------------------------------------------ */
/* fid table                                                          */
/* ------------------------------------------------------------------ */

static SrvFid *
fid_lookup(Srv *srv, uint32_t fid)
{
    if (fid >= SRV_MAX_FIDS || !srv->fids[fid].used)
        return NULL;
    return &srv->fids[fid];
}

static void
fid_bind(Srv *srv, uint32_t fid, SrvNode *node)
{
    if (fid >= SRV_MAX_FIDS)
        return;
    srv->fids[fid].used = 1;
    srv->fids[fid].qid = node ? node->qid : SRV_QID_ROOT;
    srv->fids[fid].node = node;
}

static void
fid_release(Srv *srv, uint32_t fid)
{
    if (fid < SRV_MAX_FIDS)
        srv->fids[fid].used = 0;
}

/* ------------------------------------------------------------------ */
/* GPIO                                                               */
/* ------------------------------------------------------------------ */

int
srv_gpio_read(uint8_t num)
{
    return gpio_get(num);
}

void
srv_gpio_write(uint8_t num, int val)
{
    gpio_put(num, val ? 1 : 0);
}

/* ------------------------------------------------------------------ */
/* flash file store                                                   */
/* ------------------------------------------------------------------ */

typedef struct {
    char   name[32];
    uint32_t offset;   /* offset within reserved region */
    uint32_t length;
    uint8_t  valid;
} FlashDirEnt;

typedef struct {
    uint32_t count;
    FlashDirEnt ent[16];   /* max 16 files */
} FlashDir;

#define FLASH_DIR_ADDR   (XIP_BASE + PICO_9P_FLASH_OFFSET + FLASH_DIR_BLOCK*FLASH_BLOCK)
#define FLASH_FILE_ADDR(idx) (XIP_BASE + PICO_9P_FLASH_OFFSET + (FLASH_FIRST_FILE_BLOCK + (idx))*FLASH_BLOCK)

static FlashDir dir_ram;     /* master copy, persisted to flash block 0 */

static void
flash_dir_load(void)
{
    /* directory is XIP-readable; on first boot it's all 0xFF (erased) */
    memcpy(&dir_ram, (const void *)FLASH_DIR_ADDR, sizeof(dir_ram));
    /* sanity: if garbage, treat as empty */
    if (dir_ram.count > 16)
        memset(&dir_ram, 0, sizeof(dir_ram));
    for (int i = 0; i < dir_ram.count; i++)
        if (!dir_ram.ent[i].valid)
            dir_ram.ent[i].name[0] = 0;
}

static void
flash_dir_persist(void)
{
    uint32_t offs = PICO_9P_FLASH_OFFSET + FLASH_DIR_BLOCK*FLASH_BLOCK;
    flash_range_erase(offs, FLASH_BLOCK);
    flash_range_program(offs, (const uint8_t *)&dir_ram, sizeof(dir_ram));
}

static int
flash_dir_index(const char *name)
{
    for (int i = 0; i < dir_ram.count; i++)
        if (dir_ram.ent[i].valid && strcmp(dir_ram.ent[i].name, name) == 0)
            return i;
    return -1;
}

static int
flash_block_free_idx(void)
{
    for (int i = 0; i < dir_ram.count; i++)
        if (!dir_ram.ent[i].valid)
            return i;
    if (dir_ram.count >= 16)
        return -1;
    return dir_ram.count;
}

int
srv_flash_create(const char *name)
{
    if (strlen(name) >= 32 || flash_dir_index(name) >= 0)
        return -1;
    int idx = flash_block_free_idx();
    if (idx < 0)
        return -1;
    uint32_t offs = PICO_9P_FLASH_OFFSET + (FLASH_FIRST_FILE_BLOCK + idx)*FLASH_BLOCK;
    flash_range_erase(offs, FLASH_BLOCK);
    memset(dir_ram.ent[idx].name, 0, sizeof(dir_ram.ent[idx].name));
    memcpy(dir_ram.ent[idx].name, name, strlen(name));
    dir_ram.ent[idx].offset = offs;
    dir_ram.ent[idx].length = 0;
    dir_ram.ent[idx].valid = 1;
    if (idx == dir_ram.count)
        dir_ram.count = idx + 1;
    flash_dir_persist();
    return idx;
}

int
srv_flash_remove(const char *name)
{
    int idx = flash_dir_index(name);
    if (idx < 0)
        return -1;
    dir_ram.ent[idx].valid = 0;
    dir_ram.ent[idx].length = 0;
    dir_ram.ent[idx].offset = 0;
    memset(dir_ram.ent[idx].name, 0, sizeof(dir_ram.ent[idx].name));
    flash_dir_persist();
    return 0;
}

int
srv_flash_list(char names[SRV_MAX_NODES][SRV_FLASH_NAME_MAX], int *n)
{
    int c = 0;
    for (int i = 0; i < dir_ram.count && c < SRV_MAX_NODES; i++) {
        if (dir_ram.ent[i].valid) {
            snprintf(names[c], SRV_FLASH_NAME_MAX, "%s", dir_ram.ent[i].name);
            c++;
        }
    }
    *n = c;
    return 0;
}

int
srv_flash_read(int idx, uint64_t off, uint8_t *buf, uint32_t count)
{
    if (idx < 0 || idx >= dir_ram.count || !dir_ram.ent[idx].valid)
        return -1;
    if (off >= dir_ram.ent[idx].length)
        return 0;
    uint32_t avail = dir_ram.ent[idx].length - (uint32_t)off;
    if (count > avail)
        count = avail;
    const uint8_t *src = (const uint8_t *)(uintptr_t)(FLASH_FILE_ADDR(idx) + off);
    memcpy(buf, src, count);
    return (int)count;
}

int
srv_flash_write(int idx, uint64_t off, const uint8_t *buf, uint32_t count)
{
    if (idx < 0 || idx >= dir_ram.count || !dir_ram.ent[idx].valid)
        return -1;
    if (off + count > FLASH_BLOCK - 16)
        return -1;
    uint32_t offs = PICO_9P_FLASH_OFFSET + (FLASH_FIRST_FILE_BLOCK + idx)*FLASH_BLOCK;
    flash_range_erase(offs, FLASH_BLOCK);
    uint32_t pg = (count + FLASH_PAGE_SIZE - 1) & ~(FLASH_PAGE_SIZE - 1);
    static uint8_t pgbuf[FLASH_PAGE_SIZE];
    memset(pgbuf, 0xff, pg);
    memcpy(pgbuf, buf, count);
    flash_range_program(offs, pgbuf, pg);
    dir_ram.ent[idx].length = off + count;
    flash_dir_persist();
    return (int)count;
}

/* ------------------------------------------------------------------ */
/* 9P request dispatch                                                */
/* ------------------------------------------------------------------ */

/* Walk path[0..n-1] starting from `cur`. On success returns the final node
 * and fills qids[0..nqid) with the qid of each element walked. */
static SrvNode *
resolve_walk(Srv *srv, SrvNode *cur, const char *path[], int n,
             C9qid qids[], int *nqid)
{
    *nqid = 0;
    for (int i = 0; i < n; i++) {
        SrvNode *child = node_find_child(srv, cur, path[i]);
        if (!child)
            return NULL;
        qids[*nqid] = (C9qid){ .path = child->qid,
                               .type = child->isdir ? C9qtdir : C9qtfile,
                               .version = 0 };
        (*nqid)++;
        cur = child;
    }
    return cur;
}

static int
srv_respond_version(C9ctx *c)
{
    return s9version(c);
}

static int
srv_respond_attach(C9ctx *c)
{
    static const C9qid rootqid = { .type = C9qtdir, .version = 0, .path = SRV_QID_ROOT };
    return s9attach(c, 0xffff, &rootqid);
}

static int
srv_respond_walk(C9ctx *c, C9t *t, Srv *srv)
{
    /* count path elements (wname is NULL-terminated per proto.h) */
    int n = 0;
    while (n < C9maxpathel && t->walk.wname[n] != NULL)
        n++;
    SrvNode *cur = node_by_qid(srv, SRV_QID_ROOT);
    C9qid qids[C9maxpathel];
    int nqid = 0;
    SrvNode *end = resolve_walk(srv, cur, (const char **)t->walk.wname, n, qids, &nqid);
    if (!end && n > 0)
        return C9Epath;
    if (!end)
        end = node_by_qid(srv, SRV_QID_ROOT);
    /* bind newfid -> walked node */
    fid_bind(srv, t->walk.newfid, end);
    C9qid *qidptrs[C9maxpathel];
    for (int i = 0; i < nqid; i++)
        qidptrs[i] = &qids[i];
    return s9walk(c, t->tag, qidptrs);
}

static int
srv_respond_open(C9ctx *c, C9t *t, Srv *srv)
{
    SrvFid *f = fid_lookup(srv, t->fid);
    if (!f || !f->node)
        return C9Epath;
    C9qid q = { .type = f->node->isdir ? C9qtdir : C9qtfile,
                .version = 0, .path = f->node->qid };
    return s9open(c, t->tag, &q, 4096);
}

static int
srv_respond_read(C9ctx *c, C9t *t, Srv *srv)
{
    (void)srv;
    SrvFid *f = fid_lookup(srv, t->fid);
    if (!f || !f->node)
        return C9Epath;
    SrvNode *n = f->node;
    uint8_t buf[4096];
    uint32_t count = t->read.size;
    if (count > sizeof buf)
        count = sizeof buf;
    int nr = 0;

    if (n->isdir) {
        char list[4096];
        int l = 0;
        if (n->qid == SRV_QID_ROOT) {
            const char *names[] = { "dev", "flash", NULL };
            for (int i = 0; names[i]; i++)
                l += snprintf(list + l, sizeof(list) - l, "%s\n", names[i]);
        } else if (n->qid == SRV_QID_DEV) {
            for (int i = 0; i < 30 && l < (int)sizeof(list) - 4; i++)
                l += snprintf(list + l, sizeof(list) - l, "gpio%d\n", i);
        } else if (n->qid == SRV_QID_FLASH) {
            char names[SRV_MAX_NODES][SRV_FLASH_NAME_MAX];
            int nn = 0;
            srv_flash_list(names, &nn);
            for (int i = 0; i < nn && l < (int)sizeof(list) - 4; i++)
                l += snprintf(list + l, sizeof(list) - l, "%s\n", names[i]);
        }
        if (t->read.offset >= (uint64_t)l)
            return s9read(c, t->tag, buf, 0);
        uint32_t avail = l - t->read.offset;
        if (count > avail)
            count = avail;
        memcpy(buf, list + t->read.offset, count);
        return s9read(c, t->tag, buf, count);
    }
    if (n->isgpio) {
        char v[2] = { srv_gpio_read(n->gpio_num) ? '1' : '0', '\n' };
        if (t->read.offset >= 2)
            return s9read(c, t->tag, buf, 0);
        uint32_t avail = 2 - t->read.offset;
        if (count > avail)
            count = avail;
        memcpy(buf, v + t->read.offset, count);
        return s9read(c, t->tag, buf, count);
    }
    if (n->flash_idx >= 0) {
        nr = srv_flash_read(n->flash_idx, t->read.offset, buf, count);
        if (nr < 0)
            return C9Epath;
        return s9read(c, t->tag, buf, (uint32_t)nr);
    }
    return s9read(c, t->tag, buf, 0);
}

static int
srv_respond_write(C9ctx *c, C9t *t, Srv *srv)
{
    (void)srv;
    SrvFid *f = fid_lookup(srv, t->fid);
    if (!f || !f->node)
        return C9Epath;
    SrvNode *n = f->node;
    if (n->isgpio) {
        if (t->write.size < 1)
            return 0;
        int val = (t->write.data[0] == '1');
        srv_gpio_write(n->gpio_num, val);
        return s9write(c, t->tag, t->write.size);
    }
    if (n->flash_idx >= 0) {
        int nw = srv_flash_write(n->flash_idx, t->write.offset,
                                 t->write.data, t->write.size);
        if (nw < 0)
            return C9Epath;
        return s9write(c, t->tag, (uint32_t)nw);
    }
    return s9write(c, t->tag, 0);
}

static int
srv_respond_stat(C9ctx *c, C9t *t, Srv *srv)
{
    (void)srv;
    SrvFid *f = fid_lookup(srv, t->fid);
    if (!f || !f->node)
        return C9Epath;
    SrvNode *n = f->node;
    static C9stat st;
    memset(&st, 0, sizeof st);
    st.qid.type = n->isdir ? C9qtdir : C9qtfile;
    st.qid.path = n->qid;
    st.qid.version = 0;
    st.mode = n->mode;
    st.size = n->isdir ? 0 : n->length;
    st.name = n->name;
    st.uid = (char *)"glenda";
    st.gid = (char *)"glenda";
    st.muid = (char *)"glenda";
    st.atime = 0;
    st.mtime = 0;
    return s9stat(c, t->tag, &st);
}

static int
srv_respond_clunk(C9ctx *c, C9t *t, Srv *srv)
{
    (void)srv;
    fid_release(srv, t->fid);
    return s9clunk(c, t->tag);
}

static int
srv_respond_remove(C9ctx *c, C9t *t, Srv *srv)
{
    (void)c; (void)t; (void)srv;
    return s9remove(c, t->tag);
}

static int
srv_respond_flush(C9ctx *c, C9t *t, Srv *srv)
{
    (void)c; (void)t; (void)srv;
    return s9flush(c, t->tag);
}

static int
srv_respond_auth(C9ctx *c, C9t *t, Srv *srv)
{
    (void)c; (void)t; (void)srv;
    static const C9qid aqid = { .type = C9qtauth, .version = 0, .path = 0 };
    return s9auth(c, t->tag, &aqid);
}

void
srv_init(Srv *srv)
{
    memset(srv, 0, sizeof(*srv));
    flash_dir_load();

    /* root */
    node_alloc(srv, "/", SRV_QID_ROOT, C9permdir | 0755, 1, 0, -1, -1, 0);
    /* dev */
    node_alloc(srv, "dev", SRV_QID_DEV, C9permdir | 0755, 1, 0, -1, -1, SRV_QID_ROOT);
    /* flash */
    node_alloc(srv, "flash", SRV_QID_FLASH, C9permdir | 0755, 1, 0, -1, -1, SRV_QID_ROOT);
    /* gpio0..gpio29 */
    for (int i = 0; i < 30; i++) {
        char name[16];
        snprintf(name, sizeof name, "gpio%d", i);
        gpio_init(i);
        gpio_set_dir(i, GPIO_IN);
        gpio_pull_down(i);
        node_alloc(srv, name, SRV_QID_GPIO_BASE + i, 0664, 0, 1, i, -1, SRV_QID_DEV);
    }

    /* flash files discovered from the persisted directory */
    char names[SRV_MAX_NODES][SRV_FLASH_NAME_MAX];
    int nn = 0;
    srv_flash_list(names, &nn);
    for (int i = 0; i < nn; i++) {
        int idx = flash_dir_index(names[i]);
        node_alloc(srv, names[i], SRV_QID_FLASH_BASE + i,
                   0664, 0, 0, -1, idx, SRV_QID_FLASH);
    }
}

void
srv_t(C9ctx *c, C9t *t, Srv *srv)
{
    switch (t->type) {
    case Tversion:   srv_respond_version(c); break;
    case Tattach:    srv_respond_attach(c); break;
    case Twalk:      srv_respond_walk(c, t, srv); break;
    case Topen:      srv_respond_open(c, t, srv); break;
    case Tread:      srv_respond_read(c, t, srv); break;
    case Twrite:     srv_respond_write(c, t, srv); break;
    case Tstat:      srv_respond_stat(c, t, srv); break;
    case Tclunk:     srv_respond_clunk(c, t, srv); break;
    case Tremove:    srv_respond_remove(c, t, srv); break;
    case Tflush:     srv_respond_flush(c, t, srv); break;
    case Tauth:      srv_respond_auth(c, t, srv); break;
    case Tcreate:
    default:         s9error(c, t->tag, "unsupported"); break;
    }
}
