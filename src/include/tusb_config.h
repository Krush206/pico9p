#ifndef TUSB_CONFIG_H_
#define TUSB_CONFIG_H_

#ifdef __cplusplus
 extern "C" {
#endif

/*--------------------------------------------------------------------
 * KEY: https://tinyusb.org/api.html#configuration
 *--------------------------------------------------------------------*/

/* USB device mode configuration */
#define CFG_TUSB_MCU              OPT_MCU_RP2040
#define CFG_TUSB_OS               OPT_OS_PICO
#define CFG_TUSB_DEBUG            0

/* RP2040: enable device mode on roothub port 0 */
#define CFG_TUSB_RHPORT0_MODE     (OPT_MODE_DEVICE)

/* USB Device */
#define CFG_TUD_ENDPOINT0_SIZE    64

/*------------- CDC -------------*/
#define CFG_TUD_CDC               1
#define CFG_TUD_CDC_RX_BUFSIZE    4096
#define CFG_TUD_CDC_TX_BUFSIZE    4096

/*--------------------------------------------------------------------
 * RP2040 specific
 *--------------------------------------------------------------------*/
#define CFG_TUD_RP2040_USB_DEVICE_ENUMERATION_FIX 0

#ifdef __cplusplus
 }
#endif

#endif
