#ifndef NAT_H
#define NAT_H

#include "lwip/ip4_addr.h"

struct netif;

/* usb = the Wii-facing netif (inside), wifi = the upstream netif (outside). */
void nat_init(struct netif *usb, struct netif *wifi, ip4_addr_t wii_ip);
void nat_log_stats(void);

#endif
