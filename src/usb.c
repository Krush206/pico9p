/* usb_descriptors.c — TinyUSB descriptors for a CDC-only device (RP2040).
 *
 * The device exposes a single USB CDC interface. The 9P server speaks 9P
 * over the CDC bulk endpoints; no MSC/HID.
 */

#include <string.h>
#include <stdio.h>
#include "tusb.h"

/* VID/PID: pick a private VID (0xCafe is TinyUSB's example VID; change for
 * real deployment) and a product id unique to this device class.            */
#define USB_VID   0xCafe
#define USB_PID   0xC001
#define USB_BCD   0x0200

/*--------------------------------------------------------------------+
 * Device descriptor
 *--------------------------------------------------------------------*/
static tusb_desc_device_t const desc_device = {
    .bLength            = sizeof(tusb_desc_device_t),
    .bDescriptorType    = TUSB_DESC_DEVICE,
    .bcdUSB             = USB_BCD,

    /* CDC uses the Interface Association Descriptor (IAD) mechanism        */
    .bDeviceClass       = TUSB_CLASS_MISC,
    .bDeviceSubClass    = MISC_SUBCLASS_COMMON,
    .bDeviceProtocol    = MISC_PROTOCOL_IAD,
    .bMaxPacketSize0    = CFG_TUD_ENDPOINT0_SIZE,

    .idVendor           = USB_VID,
    .idProduct          = USB_PID,
    .bcdDevice          = 0x0100,

    .iManufacturer      = 0x01,
    .iProduct           = 0x02,
    .iSerialNumber      = 0x03,

    .bNumConfigurations = 0x01
};

uint8_t const *tud_descriptor_device_cb(void)
{
    return (uint8_t const *) &desc_device;
}

/*--------------------------------------------------------------------+
 * Configuration descriptor (CDC only)
 *--------------------------------------------------------------------*/
enum {
    ITF_NUM_CDC = 0,
    ITF_NUM_CDC_DATA,
    ITF_NUM_TOTAL
};

#define CONFIG_TOTAL_LEN (TUD_CONFIG_DESC_LEN + TUD_CDC_DESC_LEN)

#if CFG_TUSB_MCU == OPT_MCU_LPC175X_6X || CFG_TUSB_MCU == OPT_MCU_LPC177X_8X || \
    CFG_TUSB_MCU == OPT_MCU_LPC40XX
    #define EPNUM_CDC_NOTIF   0x81
    #define EPNUM_CDC_OUT     0x02
    #define EPNUM_CDC_IN      0x82
#elif CFG_TUSB_MCU == OPT_MCU_CXD56
    #define EPNUM_CDC_NOTIF   0x83
    #define EPNUM_CDC_OUT     0x02
    #define EPNUM_CDC_IN      0x81
#elif CFG_TUD_ENDPOINT_ONE_DIRECTION_ONLY
    #define EPNUM_CDC_NOTIF   0x81
    #define EPNUM_CDC_OUT     0x08
    #define EPNUM_CDC_IN      0x89
#else
    #define EPNUM_CDC_NOTIF   0x81
    #define EPNUM_CDC_OUT     0x02
    #define EPNUM_CDC_IN      0x83
#endif

static uint8_t const desc_fs_configuration[] = {
    TUD_CONFIG_DESCRIPTOR(1, ITF_NUM_TOTAL, 0, CONFIG_TOTAL_LEN, 0x00, 100),
    TUD_CDC_DESCRIPTOR(ITF_NUM_CDC, 4, EPNUM_CDC_NOTIF, 16, EPNUM_CDC_OUT, EPNUM_CDC_IN, 64),
};

uint8_t const *tud_descriptor_configuration_cb(uint8_t index)
{
    (void) index;
    return desc_fs_configuration;
}

/*--------------------------------------------------------------------+
 * String descriptors
 *--------------------------------------------------------------------*/
enum {
    STRID_LANGID = 0,
    STRID_MANUFACTURER,
    STRID_PRODUCT,
    STRID_SERIAL,
};

static char const *string_desc_arr[] = {
    (const char[]) { 0x09, 0x04 }, /* 0: English (0x0409) */
    "c9 9P server",                /* 1: Manufacturer     */
    "Raspberry Pi Pico 9P",        /* 2: Product          */
    NULL,                          /* 3: Serial (use unique ID) */
    "c9 9P CDC",                   /* 4: CDC interface    */
};

static uint16_t _desc_str[32 + 1];

uint16_t const *tud_descriptor_string_cb(uint8_t index, uint16_t langid)
{
    (void) langid;
    size_t chr_count = 0;

    switch (index) {
    case STRID_LANGID:
        memcpy(&_desc_str[1], string_desc_arr[0], 2);
        chr_count = 1;
        break;

    case STRID_SERIAL:
        /* Use a fixed serial so the host sees a stable device.            */
        {
            static char serial[17];
            snprintf(serial, sizeof(serial), "PICO9P%06X", USB_VID ^ USB_PID);
            chr_count = strlen(serial);
            for (size_t i = 0; i < chr_count; i++)
                _desc_str[1 + i] = serial[i];
        }
        break;

    default:
        if (index >= (uint8_t)(sizeof(string_desc_arr) / sizeof(string_desc_arr[0])))
            return NULL;
        {
            const char *str = string_desc_arr[index];
            chr_count = strlen(str);
            size_t const max_count = sizeof(_desc_str) / sizeof(_desc_str[0]) - 1;
            if (chr_count > max_count)
                chr_count = max_count;
            for (size_t i = 0; i < chr_count; i++)
                _desc_str[1 + i] = str[i];
        }
        break;
    }

    _desc_str[0] = (uint16_t)((TUSB_DESC_STRING << 8) | (2 * chr_count + 2));
    return _desc_str;
}

/*--------------------------------------------------------------------+
 * Device qualifier / other-speed (not used; full-speed only device)
 *--------------------------------------------------------------------*/
uint8_t const *tud_descriptor_device_qualifier_cb(void)
{
    return NULL;
}

uint8_t const *tud_descriptor_other_speed_configuration_cb(uint8_t index)
{
    (void) index;
    return NULL;
}
