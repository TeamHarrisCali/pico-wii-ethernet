#ifndef AX88772_H
#define AX88772_H

#include <stdbool.h>
#include <stdint.h>

#include "device/usbd_pvt.h"

#define AX_VID          0x0B95
#define AX_PID          0x7720

#define AX_EP_INT_IN    0x81
#define AX_EP_BULK_IN   0x82
#define AX_EP_BULK_OUT  0x03
#define AX_BULK_MAXPKT  64 /* full speed */

/* Called with each Ethernet frame (no AX header, no FCS) the host sends. */
typedef void (*ax_rx_cb_t)(const uint8_t *frame, uint16_t len);

void ax_init(ax_rx_cb_t rx_cb);
const uint8_t *ax_nic_mac(void);     /* the adapter MAC the host reads (Wii's MAC) */
const uint8_t *ax_gateway_mac(void); /* MAC for the Pico's own side of the link */
bool ax_send_frame(const uint8_t *frame, uint16_t len); /* queue a frame to the host */
void ax_set_link(bool up);
bool ax_configured(void);
void ax_task(void); /* call from the main loop */

extern const usbd_class_driver_t ax_class_driver;

#endif
