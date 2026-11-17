#ifndef TUSB_CONFIG_H
#define TUSB_CONFIG_H

#ifndef CFG_TUSB_MCU
#define CFG_TUSB_MCU OPT_MCU_RP2040
#endif

#ifndef BOARD_TUD_RHPORT
#define BOARD_TUD_RHPORT 0
#endif

#define CFG_TUSB_RHPORT0_MODE   (OPT_MODE_DEVICE)

#define CFG_TUSB_MEM_SECTION
#define CFG_TUSB_MEM_ALIGN      __attribute__((aligned(4)))

#define CFG_TUD_ENDPOINT0_SIZE  64

/* The AX88772 interface is a custom class driver (src/ax88772.c), not a TinyUSB class. */
#define CFG_TUD_CDC             (WII_ETH_DEBUG_CDC)
#define CFG_TUD_MSC             0
#define CFG_TUD_HID             0
#define CFG_TUD_MIDI            0
#define CFG_TUD_VENDOR          0

#define CFG_TUD_CDC_RX_BUFSIZE  256
#define CFG_TUD_CDC_TX_BUFSIZE  2048
#define CFG_TUD_CDC_EP_BUFSIZE  64

#endif
