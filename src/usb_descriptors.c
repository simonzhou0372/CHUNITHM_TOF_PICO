/*
 * USB Descriptors for Chuni245Tof
 * CDC Serial + NKRO Keyboard
 * Following Reference implementation exactly
 */

#include "tusb.h"
#include "pico/unique_id.h"
#include <string.h>

#define USB_VID   0x0F0D
#define USB_PID   0x0092

//--------------------------------------------------------------------+
// HID Report Descriptor (NKRO Keyboard only)
//--------------------------------------------------------------------+

// NKRO Report Descriptor - exactly from Reference
static const uint8_t desc_hid_report_nkro[] = {
    HID_USAGE_PAGE(HID_USAGE_PAGE_DESKTOP),
    HID_USAGE(HID_USAGE_DESKTOP_KEYBOARD),
    HID_COLLECTION(HID_COLLECTION_APPLICATION),
        // Modifier byte (8 bits)
        HID_REPORT_SIZE(1),
        HID_REPORT_COUNT(8),
        HID_USAGE_PAGE(HID_USAGE_PAGE_KEYBOARD),
        HID_USAGE_MIN(224),
        HID_USAGE_MAX(231),
        HID_LOGICAL_MIN(0),
        HID_LOGICAL_MAX(1),
        HID_INPUT(HID_DATA | HID_VARIABLE | HID_ABSOLUTE),
        // LED output (5 bits)
        HID_REPORT_COUNT(5),
        HID_REPORT_SIZE(1),
        HID_USAGE_PAGE(HID_USAGE_PAGE_LED),
        HID_USAGE_MIN(1),
        HID_USAGE_MAX(5),
        HID_OUTPUT(HID_DATA | HID_VARIABLE | HID_ABSOLUTE),
        // LED padding (3 bits)
        HID_REPORT_COUNT(1),
        HID_REPORT_SIZE(3),
        HID_OUTPUT(HID_CONSTANT),
        // Key bitmap (120 bits = 15 bytes)
        HID_REPORT_SIZE(1),
        HID_REPORT_COUNT(120),
        HID_LOGICAL_MIN(0),
        HID_LOGICAL_MAX(1),
        HID_USAGE_PAGE(HID_USAGE_PAGE_KEYBOARD),
        HID_USAGE_MIN(0),
        HID_USAGE_MAX(119),
        HID_INPUT(HID_DATA | HID_VARIABLE | HID_ABSOLUTE),
    HID_COLLECTION_END
};

//--------------------------------------------------------------------+
// Device Descriptor
//--------------------------------------------------------------------+

static const tusb_desc_device_t desc_device = {
    .bLength            = sizeof(tusb_desc_device_t),
    .bDescriptorType    = TUSB_DESC_DEVICE,
    .bcdUSB             = 0x0200,
    .bDeviceClass       = 0x00,
    .bDeviceSubClass    = 0x00,
    .bDeviceProtocol    = 0x00,
    .bMaxPacketSize0    = 64,
    .idVendor           = USB_VID,
    .idProduct          = USB_PID,
    .bcdDevice          = 0x0100,
    .iManufacturer      = 0x01,
    .iProduct           = 0x02,
    .iSerialNumber      = 0x03,
    .bNumConfigurations = 0x01
};

//--------------------------------------------------------------------+
// Configuration Descriptor
//--------------------------------------------------------------------+

enum {
    ITF_NUM_NKRO,
    ITF_NUM_CDC,
    ITF_NUM_CDC_DATA,
    ITF_NUM_TOTAL
};

#define CONFIG_TOTAL_LEN (TUD_CONFIG_DESC_LEN + TUD_HID_DESC_LEN + TUD_CDC_DESC_LEN)

#define EPNUM_NKRO      0x81
#define EPNUM_CDC_NOTIF 0x82
#define EPNUM_CDC_OUT   0x03
#define EPNUM_CDC_IN    0x83

static const uint8_t desc_configuration[] = {
    TUD_CONFIG_DESCRIPTOR(1, ITF_NUM_TOTAL, 0, CONFIG_TOTAL_LEN,
                          TUSB_DESC_CONFIG_ATT_REMOTE_WAKEUP, 100),

    // NKRO Keyboard (Interface 0)
    TUD_HID_DESCRIPTOR(ITF_NUM_NKRO, 3, HID_ITF_PROTOCOL_NONE,
                       sizeof(desc_hid_report_nkro), EPNUM_NKRO, 64, 1),

    // CDC Serial (Interface 1)
    TUD_CDC_DESCRIPTOR(ITF_NUM_CDC, 4, EPNUM_CDC_NOTIF, 8,
                       EPNUM_CDC_OUT, EPNUM_CDC_IN, 64),
};

//--------------------------------------------------------------------+
// String Descriptors
//--------------------------------------------------------------------+

static const char *string_desc_arr[] = {
    (const char[]){0x09, 0x04},     // 0: English
    "USST",                         // 1: Manufacturer
    "Chuni245Tof Keyboard",         // 2: Product
    "Chuni245Tof NKRO",             // 3: NKRO Interface
    "Chuni245Tof CDC",              // 4: CDC Interface
};

//--------------------------------------------------------------------+
// Callbacks
//--------------------------------------------------------------------+

uint8_t const* tud_descriptor_device_cb(void) {
    return (uint8_t const*)&desc_device;
}

uint8_t const* tud_descriptor_configuration_cb(uint8_t index) {
    (void)index;
    return desc_configuration;
}

uint8_t const* tud_hid_descriptor_report_cb(uint8_t itf) {
    if (itf == ITF_NUM_NKRO) {
        return desc_hid_report_nkro;
    }
    return NULL;
}

uint16_t const* tud_descriptor_string_cb(uint8_t index, uint16_t langid) {
    (void)langid;
    static uint16_t str[64];

    if (index == 0) {
        str[1] = 0x0409;
        str[0] = (TUSB_DESC_STRING << 8) | 4;
        return str;
    }

    // Serial number from flash
    if (index == 3) {
        pico_unique_board_id_t u;
        pico_get_unique_board_id(&u);
        for (int i = 0; i < 12; i++) {
            uint8_t c = u.id[i/2];
            c = (i & 1) ? (c & 0xF) : (c >> 4);
            str[i+1] = c < 10 ? '0'+c : 'A'+c-10;
        }
        str[0] = (TUSB_DESC_STRING << 8) | 26;
        return str;
    }

    size_t arr_size = sizeof(string_desc_arr) / sizeof(string_desc_arr[0]);
    if (index >= arr_size) return NULL;

    const char *s = string_desc_arr[index];
    int len = strlen(s);
    if (len > 63) len = 63;

    for (int i = 0; i < len; i++) str[i+1] = s[i];
    str[0] = (TUSB_DESC_STRING << 8) | (2*len + 2);
    return str;
}