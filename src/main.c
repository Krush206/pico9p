/* pico_9p_server.c — RP2040 9P file server over USB CDC.
 *
 * Builds with the Makefile in this directory. Requires the Raspberry Pi Pico
 * SDK (PICO_SDK_PATH) and arm-none-eabi-gcc.
 *
 * The c9 library runs in server mode (C9_NO_CLIENT): s9proc() reads one 9P
 * T-message from the CDC receive stream, dispatches it to srv_t() which
 * replies via the s9* helpers writing to the CDC transmit stream.
 *
 * USB CDC is used because no UART is available on this target.
 */

#include <string.h>
#include "hardware/gpio.h"
#include "hardware/irq.h"
#include "hardware/timer.h"
#include "tusb.h"

#include "c9/proto.h"
#include "9psrv.h"

/* Onboard LED (Pico: GPIO 25). Toggled once per served 9P message so you can
 * see activity without a terminal. */
#define LED_PIN 25u

/* CDC transfer unit. Keep <= c.msize. */
#define CDC_MSIZE 8192

typedef struct
{
    uint8_t rx[CDC_MSIZE];
    uint8_t tx[CDC_MSIZE];
    uint32_t txoff;
} CdcState;

static CdcState cdc;
static Srv srv;
static volatile bool led_state = false;

/* ------------------------------------------------------------------ */
/* tinyUSB / time                                                     */
/* ------------------------------------------------------------------ */

/* osal_none requires the application to provide a millis clock.        */
uint32_t
tusb_time_millis_api(void)
{
    return time_us_32() / 1000u;
}

/*--------------------------------------------------------------------+
 * TinyUSB callbacks
 *--------------------------------------------------------------------*/
void
tud_mount_cb(void)
{
    gpio_put(LED_PIN, 1);
}

void
tud_umount_cb(void)
{
    gpio_put(LED_PIN, 0);
}

void
tud_suspend_cb(bool remote_wakeup_en)
{
    (void) remote_wakeup_en;
    gpio_put(LED_PIN, 0);
}

void
tud_resume_cb(void)
{
    gpio_put(LED_PIN, tud_mounted());
}

void
tud_cdc_line_state_cb(uint8_t itf, bool dtr, bool rts)
{
    (void) itf; (void) dtr; (void) rts;
}

void
tud_cdc_rx_cb(uint8_t itf)
{
    (void) itf;
}

/*--------------------------------------------------------------------+
 * c9 ctx transport over USB CDC
 *--------------------------------------------------------------------*/

/* Blocking read: fill exactly `size` bytes from the CDC receive FIFO,
 * pumping tud_task() so the USB stack keeps moving while we wait.        */
static uint8_t *
cdc_read(C9ctx *ctx, uint32_t size, int *err)
{
    CdcState *s = (CdcState *) ctx->aux;
    uint32_t n = 0;
    *err = 0;
    while (n < size) {
        tud_task();
        uint32_t avail = tud_cdc_available();
        if (avail == 0)
            continue;
        uint32_t want = size - n;
        if (avail < want)
            want = avail;
        uint32_t got = tud_cdc_read(s->rx + n, want);
        n += got;
    }
    return s->rx;
}

/* Flush the TX buffer to the CDC endpoint, blocking until accepted.        */
static int
cdc_end(C9ctx *ctx);

/* Return a pointer into the TX buffer for `size` bytes; flush first if it
 * does not fit. */
static uint8_t *
cdc_begin(C9ctx *ctx, uint32_t size)
{
    CdcState *s = (CdcState *) ctx->aux;
    if (s->txoff + size > sizeof(s->tx))
        cdc_end(ctx);
    if (size > sizeof(s->tx))
        return NULL;
    uint8_t *b = s->tx + s->txoff;
    s->txoff += size;
    return b;
}

/* Flush the TX buffer to the CDC endpoint, blocking until accepted.        */
static int
cdc_end(C9ctx *ctx)
{
    CdcState *s = (CdcState *) ctx->aux;
    if (s->txoff == 0)
        return 0;
    uint32_t n = 0;
    while (n < s->txoff) {
        tud_task();
        uint32_t wr = tud_cdc_write(s->tx + n, s->txoff - n);
        if (wr == 0)
            continue;       /* TX FIFO full: keep pumping USB */
        n += wr;
    }
    tud_cdc_write_flush();
    s->txoff = 0;
    return 0;
}

static void
cdc_error(C9ctx *ctx, const char *fmt, ...)
{
    (void) ctx;
    (void) fmt;
    /* Activity/fault indication: blink the LED quickly.                  */
    for (int i = 0; i < 4; i++) {
        gpio_put(LED_PIN, 1);
        busy_wait_us(100000);
        gpio_put(LED_PIN, 0);
        busy_wait_us(100000);
    }
}

/*--------------------------------------------------------------------+
 * 9P server dispatch bridge
 *--------------------------------------------------------------------*/
static void
srv_dispatch(C9ctx *c, C9t *t)
{
    srv_t(c, t, &srv);
    /* blink once per served message */
    led_state = !led_state;
    gpio_put(LED_PIN, led_state);
}

/*--------------------------------------------------------------------+
 * main
 *--------------------------------------------------------------------*/
int
main(void)
{
    /* onboard LED */
    gpio_init(LED_PIN);
    gpio_set_dir(LED_PIN, GPIO_OUT);
    gpio_put(LED_PIN, 0);

    /* 9P file tree (GPIO pins + flash store) */
    srv_init(&srv);

    /* TinyUSB device stack (CDC only) */
    tusb_rhport_init_t dev_init = { .role = TUSB_ROLE_DEVICE, .speed = TUSB_SPEED_AUTO };
    tusb_init(0, &dev_init);
    irq_set_enabled(USBCTRL_IRQ, true);

    /* c9 context, wired to CDC */
    C9ctx c;
    memset(&c, 0, sizeof c);
    c.read  = cdc_read;
    c.begin = cdc_begin;
    c.end   = cdc_end;
    c.error = cdc_error;
    c.t     = srv_dispatch;
    c.aux   = (struct C9aux *)&cdc;
    c.msize = CDC_MSIZE;

    /* Serve 9P forever. s9proc blocks inside cdc_read() until a complete
     * T-message arrives, then dispatches and writes the response.          */
    for (;;) {
        s9proc(&c);
    }

    return 0;
}
