#ifndef DHCPD_H
#define DHCPD_H

#include <stdbool.h>

#include "lwip/ip4_addr.h"

struct netif;

/* One-client DHCP server on the Wii-facing netif. Hands out client_ip with the Pico as router
 * and, as DNS server, the resolver the Pico learned over Wi-Fi (8.8.8.8 until then). */
/* ready() is asked before every reply; while it returns false the Wii's DHCP requests are ignored (it retries),
 * so the Wii does not start its connection test before the Pico has an Internet uplink. */
void dhcpd_init(struct netif *usb, ip4_addr_t client_ip, bool (*ready)(void));

#endif
