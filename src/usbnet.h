#ifndef USBNET_H
#define USBNET_H

#include <stdint.h>

#include "lwip/netif.h"

/* The lwIP interface that faces the Wii through the emulated AX88772. */
extern struct netif usb_netif;

void usbnet_init(const char *ip, const char *mask);
void usbnet_rx(const uint8_t *frame, uint16_t len); /* frame from the Wii */

#endif
