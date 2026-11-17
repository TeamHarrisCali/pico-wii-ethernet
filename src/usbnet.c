#include "usbnet.h"

#include <string.h>

#include "lwip/etharp.h"
#include "lwip/ip4_addr.h"
#include "lwip/netif.h"
#include "lwip/pbuf.h"
#include "netif/ethernet.h"

#include "ax88772.h"

struct netif usb_netif;

static err_t linkoutput(struct netif *n, struct pbuf *p)
{
    static uint8_t frame[1600];
    (void)n;
    if (p->tot_len > sizeof frame)
        return ERR_BUF;
    pbuf_copy_partial(p, frame, p->tot_len, 0);
    uint16_t len = p->tot_len;
    if (len < 60) { /* pad runt frames like a NIC would */
        memset(frame + len, 0, 60 - len);
        len = 60;
    }
    ax_send_frame(frame, len);
    return ERR_OK;
}

static err_t netif_setup(struct netif *n)
{
    n->name[0] = 'u';
    n->name[1] = 's';
    n->output = etharp_output;
    n->linkoutput = linkoutput;
    n->mtu = 1500;
    n->hwaddr_len = 6;
    memcpy(n->hwaddr, ax_gateway_mac(), 6);
    n->flags = NETIF_FLAG_BROADCAST | NETIF_FLAG_ETHARP | NETIF_FLAG_ETHERNET | NETIF_FLAG_LINK_UP;
#if LWIP_NETIF_HOSTNAME
    n->hostname = "pico-wii";
#endif
    return ERR_OK;
}

void usbnet_init(const char *ip, const char *mask)
{
    ip4_addr_t a, m, gw;
    ip4addr_aton(ip, &a);
    ip4addr_aton(mask, &m);
    ip4_addr_set_zero(&gw);
    netif_add(&usb_netif, &a, &m, &gw, NULL, netif_setup, ethernet_input);
    netif_set_up(&usb_netif);
}

void usbnet_rx(const uint8_t *frame, uint16_t len)
{
    struct pbuf *p = pbuf_alloc(PBUF_RAW, len, PBUF_POOL);
    if (!p)
        return;
    pbuf_take(p, frame, len);
    if (usb_netif.input(p, &usb_netif) != ERR_OK)
        pbuf_free(p);
}
