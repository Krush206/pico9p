#ifndef _9PSRV_H
#define _9PSRV_H

#include <stdint.h>

struct C9ctx;
struct C9t;

/* ---------------------------------------------------------------
 * A tiny 9P file tree served by the RP2040.
 *
 * Tree:
 *   /                 directory
 *   /dev/             directory
 *   /dev/gpio0 .. gpio29   read=>"0"/"1", write "0"/"1" sets the pin
 *   /flash/           directory
 *   /flash/<name>     file stored in the reserved flash region
 * --------------------------------------------------------------- */

#define SRV_QID_ROOT     0
#define SRV_QID_DEV      1
#define SRV_QID_FLASH    2
#define SRV_QID_GPIO_BASE  10   /* gpioN => 10+N */
#define SRV_QID_FLASH_BASE 100  /* flash files => 100+index */

#define SRV_MAX_NODES  64
#define SRV_MAX_FIDS   16
#define SRV_FLASH_NAME_MAX 32

/* A node in the 9P tree. */
typedef struct SrvNode SrvNode;
struct SrvNode
{
    char     name[SRV_FLASH_NAME_MAX];
    uint64_t qid;        /* unique qid.path */
    uint64_t parent;     /* parent node qid (for path resolution) */
    uint32_t mode;       /* C9perm* / C9permdir */
    uint64_t length;     /* file size in bytes */
    int      isdir;
    int      isgpio;     /* dev/gpioN device */
    int      gpio_num;   /* if isgpio */
    int      flash_idx;  /* if flash file */
    /* flash file content backing (cached in SRAM; written back to flash) */
    uint8_t  *content;
    uint32_t content_size;
    uint32_t content_alloc;
};

/* Fid state: which node a fid is attached to. */
typedef struct
{
    int    used;
    uint64_t qid;     /* node qid the fid refers to */
    SrvNode *node;
} SrvFid;

/* Server-wide state. */
typedef struct
{
    SrvNode nodes[SRV_MAX_NODES];
    int     nnodes;
    SrvFid  fids[SRV_MAX_FIDS];
    uint32_t msize;
} Srv;

extern void srv_init(Srv *srv);
extern void srv_t(C9ctx *c, C9t *t, Srv *srv);

/* GPIO access */
extern int srv_gpio_read(uint8_t num);
extern void srv_gpio_write(uint8_t num, int val);

/* Flash file storage */
extern int srv_flash_read(int idx, uint64_t off, uint8_t *buf, uint32_t count);
extern int srv_flash_write(int idx, uint64_t off, const uint8_t *buf, uint32_t count);
extern int srv_flash_list(char names[SRV_MAX_NODES][SRV_FLASH_NAME_MAX], int *n);
extern int srv_flash_create(const char *name);
extern int srv_flash_remove(const char *name);

#endif
