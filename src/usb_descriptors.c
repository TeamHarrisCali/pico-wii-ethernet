/*
 * USB descriptors: a plain AX88772 (VID 0B95, PID 7720), one vendor-specific interface with
 * interrupt IN 0x81, bulk IN 0x82 and bulk OUT 0x03, as on the real adapter.
 * With -DWII_ETH_DEBUG_CDC=ON a CDC serial port is added as interface 1 (bench use only).
 */
#include <string.h>

#include "tusb.h"

#include "ax88772.h"
#include "dbglog.h"

#define AX_ITF_DESC_LEN (9 + 3 * 7)

#define AX_ITF_DESC(itfnum)                                                              \
    9, TUSB_DESC_INTERFACE, itfnum, 0, 3, 0xff, 0xff, 0x00, 0,                           \
    7, TUSB_DESC_ENDPOINT, AX_EP_INT_IN, TUSB_XFER_INTERRUPT, U16_TO_U8S_LE(8), 11,      \
    7, TUSB_DESC_ENDPOINT, AX_EP_BULK_IN, TUSB_XFER_BULK, U16_TO_U8S_LE(AX_BULK_MAXPKT), 0, \
    7, TUSB_DESC_ENDPOINT, AX_EP_BULK_OUT, TUSB_XFER_BULK, U16_TO_U8S_LE(AX_BULK_MAXPKT), 0

#if WII_ETH_DEBUG_CDC
#define ITF_COUNT   3
#define CFG_TOTAL   (TUD_CONFIG_DESC_LEN + AX_ITF_DESC_LEN + TUD_CDC_DESC_LEN)
#else
#define ITF_COUNT   1
#define CFG_TOTAL   (TUD_CONFIG_DESC_LEN + AX_ITF_DESC_LEN)
#endif

static const tusb_desc_device_t desc_device = {
    .bLength = sizeof(tusb_desc_device_t),
    .bDescriptorType = TUSB_DESC_DEVICE,
    .bcdUSB = 0x0200,
#if WII_ETH_DEBUG_CDC
    .bDeviceClass = TUSB_CLASS_MISC,
    .bDeviceSubClass = MISC_SUBCLASS_COMMON,
    .bDeviceProtocol = MISC_PROTOCOL_IAD,
#else
    .bDeviceClass = 0xff,
    .bDeviceSubClass = 0xff,
    .bDeviceProtocol = 0x00,
#endif
    .bMaxPacketSize0 = CFG_TUD_ENDPOINT0_SIZE,
    .idVendor = AX_VID,
    .idProduct = AX_PID,
    .bcdDevice = 0x0001,
    .iManufacturer = 1,
    .iProduct = 2,
    .iSerialNumber = 3,
    .bNumConfigurations = 1,
};

static const uint8_t desc_config[] = {
    /* config 1, bus powered + remote wakeup, 200 mA */
    TUD_CONFIG_DESCRIPTOR(1, ITF_COUNT, 0, CFG_TOTAL, 0xa0, 200),
    AX_ITF_DESC(0),
#if WII_ETH_DEBUG_CDC
    TUD_CDC_DESCRIPTOR(1, 4, 0x84, 8, 0x05, 0x85, 64),
#endif
};

static const char *const strings[] = {
    (const char[]){ 0x09, 0x04 }, /* language: English (US) */
    "ASIX Elec. Corp.",
    "AX88772",
    "000001",
    "Pico Wii Ethernet debug",
};

uint8_t const *tud_descriptor_device_cb(void)
{
    dbg_printf("GET_DESCRIPTOR device");
    return (uint8_t const *)&desc_device;
}

uint8_t const *tud_descriptor_configuration_cb(uint8_t index)
{
    (void)index;
    dbg_printf("GET_DESCRIPTOR configuration (%u bytes)", (unsigned)CFG_TOTAL);
    return desc_config;
}

uint16_t const *tud_descriptor_string_cb(uint8_t index, uint16_t langid)
{
    static uint16_t buf[33];
    uint8_t n;
    (void)langid;
    dbg_printf("GET_DESCRIPTOR string %u", index);

    if (index == 0) {
        memcpy(&buf[1], strings[0], 2);
        n = 1;
    } else {
        if (index >= sizeof strings / sizeof strings[0])
            return NULL;
        const char *s = strings[index];
        n = strlen(s);
        if (n > 31)
            n = 31;
        for (uint8_t i = 0; i < n; i++)
            buf[1 + i] = s[i];
    }
    buf[0] = (TUSB_DESC_STRING << 8) | (2 * n + 2);
    return buf;
}
