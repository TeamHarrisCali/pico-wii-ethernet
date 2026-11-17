#include "dhcpd.h"

#include <string.h>

#include "lwip/dns.h"
#include "lwip/netif.h"
#include "lwip/pbuf.h"
#include "lwip/udp.h"

#include "dbglog.h"
#include "wifi_config.h"

#define DHCP_SERVER_PORT 67
#define DHCP_CLIENT_PORT 68
#define LEASE_SECS       3600

#define BOOTP_LEN        236
#define OPT_OFF          240 /* BOOTP header + magic cookie */

enum { DISCOVER = 1, OFFER = 2, REQUEST = 3, DECLINE = 4, ACK = 5, NAK = 6, RELEASE = 7, INFORM = 8 };

static struct udp_pcb *pcb;
static struct netif *usb_if;
static ip4_addr_t lease_ip;
static bool (*uplink_ready)(void);

static uint8_t *opt_put(uint8_t *o, uint8_t code, uint8_t len, const void *data)
{
    *o++ = code;
    *o++ = len;
    memcpy(o, data, len);
    return o + len;
}

static const uint8_t *opt_find(const uint8_t *opts, uint16_t len, uint8_t code)
{
    for (uint16_t i = 0; i < len;) {
        uint8_t c = opts[i];
        if (c == 255)
            break;
        if (c == 0) {
            i++;
            continue;
        }
        if (i + 1 >= len || i + 2 + opts[i + 1] > len)
            break;
        if (c == code)
            return &opts[i + 1]; /* [len, data...] */
        i += 2 + opts[i + 1];
    }
    return NULL;
}

static void send_reply(const uint8_t *req, uint8_t type, bool with_ip)
{
    uint8_t buf[320];
    memset(buf, 0, sizeof buf);
    buf[0] = 2; buf[1] = 1; buf[2] = 6; /* BOOTREPLY, Ethernet, hlen 6 */
    memcpy(buf + 4, req + 4, 4);        /* xid */
    memcpy(buf + 10, req + 10, 2);      /* flags */
    if (with_ip)
        memcpy(buf + 16, &lease_ip.addr, 4); /* yiaddr */
    memcpy(buf + 20, netif_ip4_addr(usb_if), 4); /* siaddr */
    memcpy(buf + 28, req + 28, 16);     /* chaddr */
    buf[236] = 0x63; buf[237] = 0x82; buf[238] = 0x53; buf[239] = 0x63;

    uint8_t *o = buf + OPT_OFF;
    o = opt_put(o, 53, 1, &type);
    o = opt_put(o, 54, 4, netif_ip4_addr(usb_if));
    if (type != NAK) {
        uint32_t lease = PP_HTONL(LEASE_SECS), t1 = PP_HTONL(LEASE_SECS / 2), t2 = PP_HTONL(LEASE_SECS * 7 / 8);
        o = opt_put(o, 51, 4, &lease);
        o = opt_put(o, 58, 4, &t1);
        o = opt_put(o, 59, 4, &t2);
        o = opt_put(o, 1, 4, netif_ip4_netmask(usb_if));
        o = opt_put(o, 3, 4, netif_ip4_addr(usb_if));
        const ip_addr_t *dns = dns_getserver(0);
        ip4_addr_t d;
        d.addr = ip_addr_isany(dns) ? PP_HTONL(0x08080808) : ip_2_ip4(dns)->addr;
#ifdef WII_DNS
        if (strlen(WII_DNS))
            ip4addr_aton(WII_DNS, &d);
#endif
        o = opt_put(o, 6, 4, &d);
        ip4_addr_t bc;
        bc.addr = (lease_ip.addr & netif_ip4_netmask(usb_if)->addr) | ~netif_ip4_netmask(usb_if)->addr;
        o = opt_put(o, 28, 4, &bc);
    }
    *o++ = 255;

    struct pbuf *p = pbuf_alloc(PBUF_TRANSPORT, sizeof buf, PBUF_RAM);
    if (!p)
        return;
    memcpy(p->payload, buf, sizeof buf);
    udp_sendto_if(pcb, p, IP_ADDR_BROADCAST, DHCP_CLIENT_PORT, usb_if);
    pbuf_free(p);
}

static void recv_cb(void *arg, struct udp_pcb *upcb, struct pbuf *p, const ip_addr_t *addr, u16_t port)
{
    (void)arg; (void)upcb; (void)addr; (void)port;
    uint8_t req[400];
    uint16_t n = pbuf_copy_partial(p, req, sizeof req, 0);
    pbuf_free(p);
    if (n < OPT_OFF + 4 || req[0] != 1 || req[1] != 1 || req[2] != 6 || req[236] != 0x63 || req[237] != 0x82)
        return;

    const uint8_t *t = opt_find(req + OPT_OFF, n - OPT_OFF, 53);
    if (!t || t[0] != 1)
        return;
    uint8_t type = t[1];
    if (uplink_ready && !uplink_ready()) {
        dbg_printf("dhcp: request ignored, Wi-Fi not up yet");
        return;
    }

    const uint8_t *sid = opt_find(req + OPT_OFF, n - OPT_OFF, 54);
    if (sid && sid[0] == 4 && memcmp(sid + 1, netif_ip4_addr(usb_if), 4) != 0)
        return; /* addressed to another server */

    dbg_printf("dhcp: %s from %02x:%02x:%02x:%02x:%02x:%02x", type == DISCOVER ? "DISCOVER" : type == REQUEST ? "REQUEST" :
        type == INFORM ? "INFORM" : type == RELEASE ? "RELEASE" : "other", req[28], req[29], req[30], req[31], req[32], req[33]);

    if (type == DISCOVER) {
        send_reply(req, OFFER, true);
    } else if (type == REQUEST) {
        const uint8_t *want = opt_find(req + OPT_OFF, n - OPT_OFF, 50);
        uint32_t ask = 0;
        if (want && want[0] == 4)
            memcpy(&ask, want + 1, 4);
        else
            memcpy(&ask, req + 12, 4); /* ciaddr when renewing */
        send_reply(req, (ask == 0 || ask == lease_ip.addr) ? ACK : NAK, (ask == 0 || ask == lease_ip.addr));
    } else if (type == INFORM) {
        send_reply(req, ACK, false);
    }
}

void dhcpd_init(struct netif *usb, ip4_addr_t client_ip, bool (*ready)(void))
{
    usb_if = usb;
    uplink_ready = ready;
    lease_ip = client_ip;
    pcb = udp_new();
    if (!pcb)
        return;
    udp_bind_netif(pcb, usb);
    udp_bind(pcb, IP_ADDR_ANY, DHCP_SERVER_PORT);
    udp_recv(pcb, recv_cb, NULL);
}
