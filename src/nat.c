/*
 * Small NAPT for IPv4, installed as lwIP's LWIP_HOOK_IP4_INPUT.
 *
 * Packets from the USB (Wii) side that are not for the Pico get their source rewritten to the
 * Wi-Fi address and a mapped port; packets from the Wi-Fi side that match a mapping get their
 * destination rewritten back. The hook then returns 0 and lwIP forwards the packet as usual
 * (IP_FORWARD). Checksums are fixed incrementally (RFC 1624) on raw network-order words.
 * TCP, UDP and ICMP echo only; fragments are dropped.
 */
#include "nat.h"

#include <stdbool.h>
#include <stdint.h>
#include <stdio.h>
#include <string.h>

#include "lwip/opt.h"
#include "lwip/arch.h"
#include "lwip/def.h"
#include "lwip/ip4_addr.h"
#include "lwip/netif.h"
#include "lwip/pbuf.h"
#include "lwip/sys.h"

#include "dbglog.h"

#define PROTO_ICMP 1
#define PROTO_TCP  6
#define PROTO_UDP  17

#define NAT_ENTRIES   128
#define NAT_PORT_MIN  10000
#define NAT_PORT_MAX  16383 /* below lwIP's own ephemeral range (0x4000..) */

#define TMO_UDP_MS      60000
#define TMO_ICMP_MS     20000
#define TMO_TCP_NEW_MS  60000
#define TMO_TCP_EST_MS  (30 * 60000)
#define TMO_TCP_END_MS  10000

enum { TCP_NEW = 0, TCP_EST, TCP_END };

typedef struct {
    uint8_t used, proto, state, fins, from_outside; /* from_outside: created by an unsolicited inbound packet */
    uint16_t int_port, ext_port, rem_port; /* host byte order; ICMP uses int_port/ext_port as echo id */
    uint32_t int_ip, rem_ip;               /* raw network order */
    uint32_t last_ms;
} entry_t;

static entry_t tbl[NAT_ENTRIES];
static struct netif *g_usb, *g_wifi;
static ip4_addr_t g_wii_ip;

/* 1: forward every unsolicited inbound TCP/UDP packet to the Wii (all ports open, like a DMZ). */
#ifndef WII_FORWARD_ALL_PORTS
#define WII_FORWARD_ALL_PORTS 1
#endif
static uint16_t next_port = NAT_PORT_MIN;

static struct {
    uint32_t out, in, dropped, new_maps, evicted, wifi_rx, wifi_rx_ours, badsum, fwd_in;
} st;

/* ---- raw (network-order) helpers ---- */
static inline uint16_t rd16(const uint8_t *p) { uint16_t v; memcpy(&v, p, 2); return v; }
static inline uint32_t rd32(const uint8_t *p) { uint32_t v; memcpy(&v, p, 4); return v; }
static inline void wr16(uint8_t *p, uint16_t v) { memcpy(p, &v, 2); }
static inline void wr32(uint8_t *p, uint32_t v) { memcpy(p, &v, 4); }

/* RFC 1624: HC' = ~(~HC + ~m + m'), all in the same (raw) 16-bit word order. */
static uint16_t cs16(uint16_t hc, uint16_t m, uint16_t mp)
{
    uint32_t s = (uint16_t)~hc;
    s += (uint16_t)~m;
    s += mp;
    s = (s & 0xffff) + (s >> 16);
    s = (s & 0xffff) + (s >> 16);
    return (uint16_t)~s;
}

static uint16_t cs32(uint16_t hc, uint32_t m, uint32_t mp)
{
    hc = cs16(hc, (uint16_t)m, (uint16_t)mp);
    return cs16(hc, (uint16_t)(m >> 16), (uint16_t)(mp >> 16));
}

/* ---- table ---- */
static uint32_t timeout_ms(const entry_t *e)
{
    switch (e->proto) {
    case PROTO_UDP: return TMO_UDP_MS;
    case PROTO_ICMP: return TMO_ICMP_MS;
    default:
        return e->state == TCP_NEW ? TMO_TCP_NEW_MS : e->state == TCP_EST ? TMO_TCP_EST_MS : TMO_TCP_END_MS;
    }
}

static bool live(const entry_t *e, uint32_t now)
{
    return e->used && (now - e->last_ms) < timeout_ms(e);
}

static entry_t *find_out(uint8_t proto, uint32_t ip, uint16_t port, uint32_t rip, uint16_t rport, uint32_t now)
{
    for (int i = 0; i < NAT_ENTRIES; i++) {
        entry_t *e = &tbl[i];
        if (!live(e, now) || e->proto != proto || e->int_ip != ip || e->int_port != port)
            continue;
        if (proto == PROTO_UDP || (e->rem_ip == rip && (proto == PROTO_ICMP || e->rem_port == rport)))
            return e;
    }
    return NULL;
}

static entry_t *find_in(uint8_t proto, uint16_t ext_port, uint32_t rip, uint16_t rport, uint32_t now)
{
    for (int i = 0; i < NAT_ENTRIES; i++) {
        entry_t *e = &tbl[i];
        if (!live(e, now) || e->proto != proto || e->ext_port != ext_port)
            continue;
        if (proto == PROTO_UDP || (e->rem_ip == rip && (proto == PROTO_ICMP || e->rem_port == rport)))
            return e;
    }
    return NULL;
}

static bool ext_port_busy(uint8_t proto, uint16_t port, uint32_t now)
{
    for (int i = 0; i < NAT_ENTRIES; i++)
        if (live(&tbl[i], now) && tbl[i].proto == proto && tbl[i].ext_port == port)
            return true;
    return false;
}

static entry_t *alloc_entry(uint8_t proto, uint32_t ip, uint16_t port, uint32_t rip, uint16_t rport, uint32_t now,
    uint16_t force_ext)
{
    entry_t *slot = NULL;
    for (int i = 0; i < NAT_ENTRIES && !slot; i++)
        if (!live(&tbl[i], now))
            slot = &tbl[i];
    if (!slot) { /* evict the least recently used */
        slot = &tbl[0];
        for (int i = 1; i < NAT_ENTRIES; i++)
            if ((now - tbl[i].last_ms) > (now - slot->last_ms))
                slot = &tbl[i];
        st.evicted++;
    }

    uint16_t ext = force_ext;
    if (!ext && proto != PROTO_ICMP && port >= 1024 && !ext_port_busy(proto, port, now))
        ext = port; /* keep the Wii's port when possible (helps peer-to-peer games) */
    for (int tries = 0; !ext && tries < NAT_PORT_MAX - NAT_PORT_MIN + 1; tries++) {
        uint16_t c = next_port;
        next_port = next_port >= NAT_PORT_MAX ? NAT_PORT_MIN : next_port + 1;
        if (!ext_port_busy(proto, c, now))
            ext = c;
    }
    if (!ext)
        return NULL;

    memset(slot, 0, sizeof *slot);
    slot->used = 1; slot->proto = proto; slot->int_ip = ip; slot->int_port = port;
    slot->ext_port = ext; slot->rem_ip = rip; slot->rem_port = rport; slot->last_ms = now;
    st.new_maps++;
    return slot;
}

static void tcp_track(entry_t *e, uint8_t flags, bool outbound)
{
    if (flags & 0x04) { /* RST */
        e->state = TCP_END;
    } else if (flags & 0x01) { /* FIN */
        if (++e->fins >= 2)
            e->state = TCP_END;
    } else if (e->from_outside) {
        if (outbound && e->state == TCP_NEW)
            e->state = TCP_EST; /* the Wii answered an unsolicited connection */
    } else if (!outbound && e->state == TCP_NEW) {
        e->state = TCP_EST;
    }
}


#ifndef NAT_VERIFY
#define NAT_VERIFY 0 /* 1: verify every translated packet with a full checksum pass and log the first ones */
#endif

#if NAT_VERIFY
/* Debug: full-recompute checksum verification of a translated packet (first few + mismatch count). */
static uint32_t sum16(const uint8_t *b, uint32_t n, uint32_t acc)
{
    for (; n > 1; b += 2, n -= 2)
        acc += rd16(b);
    if (n) {
        uint16_t v = 0;
        memcpy(&v, b, 1);
        acc += v;
    }
    return acc;
}

static uint16_t fold(uint32_t s)
{
    while (s >> 16)
        s = (s & 0xffff) + (s >> 16);
    return (uint16_t)s;
}

static void verify_packet(const char *dir, const uint8_t *ip, unsigned ihl, uint8_t proto)
{
    static uint32_t shown;
    uint16_t tot = lwip_ntohs(rd16(ip + 2));
    bool ip_ok = fold(sum16(ip, ihl, 0)) == 0xffff;
    bool l4_ok = true;
    if (proto != PROTO_ICMP && !(proto == PROTO_UDP && rd16(ip + ihl + 6) == 0)) {
        uint32_t acc = sum16(ip + 12, 8, 0);
        acc += lwip_htons(proto) ;
        acc += lwip_htons(tot - ihl);
        l4_ok = fold(sum16(ip + ihl, tot - ihl, acc)) == 0xffff;
    } else if (proto == PROTO_ICMP) {
        l4_ok = fold(sum16(ip + ihl, tot - ihl, 0)) == 0xffff;
    }
    if (!ip_ok || !l4_ok)
        st.badsum++;
    if (shown < 12 || !ip_ok || !l4_ok) {
        if (shown < 40) {
            shown++;
            dbg_printf("nat %s proto %u len %u ipcsum %s l4csum %s", dir, proto, tot, ip_ok ? "ok" : "BAD", l4_ok ? "ok" : "BAD");
        }
    }
}


#else
#define verify_packet(dir, ip, ihl, proto) ((void)0)
#endif


#ifndef NAT_TRACE
#define NAT_TRACE 1 /* log the first translated packets (proto, endpoints, TCP flags, size) */
#endif

#if NAT_TRACE
static void trace(const char *dir, const entry_t *e, const uint8_t *ip, unsigned ihl, uint8_t proto)
{
    static uint32_t shown;
    const uint8_t *l4 = ip + ihl;
    /* Keep the log readable during long transfers: only DNS queries, new TCP connections, resets and non-DNS UDP. */
    bool interesting = proto == PROTO_UDP || (proto == PROTO_TCP && (l4[13] & 0x06 /* SYN or RST */) && !(l4[13] & 0x10));
    if (!interesting || shown >= 400)
        return;
    shown++;
    uint32_t rem = (dir[0] == 'o') ? rd32(ip + 16) : rd32(ip + 12);
    uint16_t rport = proto == PROTO_ICMP ? 0 : lwip_ntohs(rd16(l4 + (dir[0] == 'o' ? 2 : 0)));
    const uint8_t *r = (const uint8_t *)&rem;
    char qname[64] = ""; /* DNS name or UDP payload hex */
    if (proto == PROTO_UDP && dir[0] == 'o' && rport == 53) { /* show the DNS name the Wii looks up */
        unsigned tot = lwip_ntohs(rd16(ip + 2)), pos = ihl + 8 + 12, n = 0;
        while (pos < tot && ip[pos] && n < sizeof qname - 2) {
            unsigned l = ip[pos++];
            while (l-- && pos < tot && n < sizeof qname - 2)
                qname[n++] = ip[pos++];
            qname[n++] = '.';
        }
        qname[n] = 0;
    }
    if (proto == PROTO_UDP && rport != 53) { /* other UDP (e.g. the Wii's NAT test): show the payload bytes */
        unsigned tot = lwip_ntohs(rd16(ip + 2)), pos = ihl + 8, n = 0;
        for (; pos < tot && n + 3 < sizeof qname && n < 3 * 24; pos++, n += 2)
            snprintf(qname + n, 3, "%02x", ip[pos]);
        qname[n] = 0;
    }
    dbg_printf("nat %s %s wii:%u <-> %u.%u.%u.%u:%u ext:%u len %u%s%s%s%s %s", dir,
        proto == PROTO_TCP ? "tcp" : proto == PROTO_UDP ? "udp" : "icmp", e->int_port, r[0], r[1], r[2], r[3], rport,
        e->ext_port, lwip_ntohs(rd16(ip + 2)), proto == PROTO_TCP && (l4[13] & 2) ? " SYN" : "",
        proto == PROTO_TCP && (l4[13] & 16) ? " ACK" : "", proto == PROTO_TCP && (l4[13] & 1) ? " FIN" : "",
        proto == PROTO_TCP && (l4[13] & 4) ? " RST" : "", qname);
}
#else
#define trace(dir, e, ip, ihl, proto) ((void)0)
#endif

/* Forward a translated packet ourselves. lwIP's ip4_forward() zeroes the IP/L4 checksums when software
 * checksum generation is on (it assumes the output path regenerates them, which a plain forward does not),
 * so the packet would leave with checksum 0. Returns 1: the hook has consumed the pbuf. */
static int forward(struct pbuf *p, uint8_t *ip, struct netif *out)
{
    if (ip[8] <= 1 || p->tot_len > out->mtu) {
        st.dropped++;
        pbuf_free(p);
        return 1;
    }
    uint16_t oldw = rd16(ip + 8);
    ip[8]--;
    wr16(ip + 10, cs16(rd16(ip + 10), oldw, rd16(ip + 8)));
    ip4_addr_t d;
    d.addr = rd32(ip + 16);
    out->output(out, p, &d);
    pbuf_free(p);
    return 1;
}

/* ---- translation ---- */
static int nat_outbound(struct pbuf *p, uint8_t *ip, unsigned ihl, uint8_t proto)
{
    uint32_t now = sys_now();
    uint32_t src = rd32(ip + 12), dst = rd32(ip + 16);
    ip4_addr_t d, s;
    d.addr = dst; s.addr = src;

    /* Anything addressed to the Pico or to the private link itself is for lwIP, not for NAT. */
    if (dst == IPADDR_BROADCAST || ip4_addr_ismulticast(&d))
        return 0;
    if (ip4_addr_netcmp(&d, netif_ip4_addr(g_usb), netif_ip4_netmask(g_usb)))
        return 0;
    if (ip4_addr_cmp(&d, netif_ip4_addr(g_wifi)))
        return 0;

    const ip4_addr_t *wip = netif_ip4_addr(g_wifi);
    if (ip4_addr_isany(wip) || !ip4_addr_netcmp(&s, netif_ip4_addr(g_usb), netif_ip4_netmask(g_usb)) ||
        (rd16(ip + 6) & PP_HTONS(0x3fff))) {
        st.dropped++; /* Wi-Fi not up, spoofed source, or a fragment */
        goto drop;
    }

    uint8_t *l4 = ip + ihl;
    uint16_t l4len = p->len - ihl;
    uint32_t new_src = wip->addr;
    entry_t *e;

    if (proto == PROTO_ICMP) {
        if (l4len < 8 || l4[0] != 8) /* echo request only */
            { st.dropped++; goto drop; }
        uint16_t id = lwip_ntohs(rd16(l4 + 4));
        e = find_out(proto, src, id, dst, 0, now);
        if (!e && !(e = alloc_entry(proto, src, id, dst, 0, now, 0)))
            { st.dropped++; goto drop; }
        e->last_ms = now;
        uint16_t nid = lwip_htons(e->ext_port);
        wr16(l4 + 2, cs16(rd16(l4 + 2), rd16(l4 + 4), nid));
        wr16(l4 + 4, nid);
    } else {
        unsigned min = proto == PROTO_TCP ? 20 : 8;
        if (l4len < min)
            { st.dropped++; goto drop; }
        uint16_t sport = lwip_ntohs(rd16(l4)), dport = lwip_ntohs(rd16(l4 + 2));
        e = find_out(proto, src, sport, dst, dport, now);
        if (!e && !(e = alloc_entry(proto, src, sport, dst, dport, now, 0)))
            { st.dropped++; goto drop; }
        e->last_ms = now;
        if (proto == PROTO_TCP)
            tcp_track(e, l4[13], true);
        uint16_t nport = lwip_htons(e->ext_port);
        uint8_t *csum = l4 + (proto == PROTO_TCP ? 16 : 6);
        if (!(proto == PROTO_UDP && rd16(csum) == 0)) { /* UDP checksum 0 = none */
            uint16_t c = cs32(rd16(csum), src, new_src);
            c = cs16(c, rd16(l4), nport);
            if (proto == PROTO_UDP && c == 0)
                c = 0xffff;
            wr16(csum, c);
        }
        wr16(l4, nport);
    }
    wr16(ip + 10, cs32(rd16(ip + 10), src, new_src));
    wr32(ip + 12, new_src);
    verify_packet("out", ip, ihl, proto);
    trace("out", e, ip, ihl, proto);
    st.out++;
    return forward(p, ip, g_wifi);

drop:
    pbuf_free(p);
    return 1;
}

static int nat_inbound(struct pbuf *p, uint8_t *ip, unsigned ihl, uint8_t proto)
{
    uint32_t now = sys_now();
    uint32_t src = rd32(ip + 12), dst = rd32(ip + 16);

    if (dst != netif_ip4_addr(g_wifi)->addr || (rd16(ip + 6) & PP_HTONS(0x3fff)))
        return 0;

    uint8_t *l4 = ip + ihl;
    uint16_t l4len = p->len - ihl;
    entry_t *e;

    if (proto == PROTO_ICMP) {
        if (l4len < 8 || l4[0] != 0) /* echo reply only */
            return 0;
        e = find_in(proto, lwip_ntohs(rd16(l4 + 4)), src, 0, now);
        if (!e)
            return 0;
        uint16_t nid = lwip_htons(e->int_port);
        wr16(l4 + 2, cs16(rd16(l4 + 2), rd16(l4 + 4), nid));
        wr16(l4 + 4, nid);
    } else if (proto == PROTO_TCP || proto == PROTO_UDP) {
        unsigned min = proto == PROTO_TCP ? 20 : 8;
        if (l4len < min)
            return 0;
        e = find_in(proto, lwip_ntohs(rd16(l4 + 2)), src, lwip_ntohs(rd16(l4)), now);
        if (!e) {
            /* Not a reply to anything the Wii sent. The Pico's own sockets are only the DHCP client (port 68)
             * and the DNS self-test (replies from port 53); everything else goes to the Wii. */
            uint16_t sport = lwip_ntohs(rd16(l4)), dport = lwip_ntohs(rd16(l4 + 2));
            if (!WII_FORWARD_ALL_PORTS || ip4_addr_isany(&g_wii_ip) || (proto == PROTO_UDP && (dport == 68 || sport == 53)))
                return 0;
            e = alloc_entry(proto, g_wii_ip.addr, dport, src, sport, now, dport);
            if (!e) {
                st.dropped++;
                pbuf_free(p);
                return 1;
            }
            e->from_outside = 1;
            st.fwd_in++;
        }
        e->last_ms = now;
        if (proto == PROTO_TCP)
            tcp_track(e, l4[13], false);
        uint16_t nport = lwip_htons(e->int_port);
        uint8_t *csum = l4 + (proto == PROTO_TCP ? 16 : 6);
        if (!(proto == PROTO_UDP && rd16(csum) == 0)) {
            uint16_t c = cs32(rd16(csum), dst, e->int_ip);
            c = cs16(c, rd16(l4 + 2), nport);
            if (proto == PROTO_UDP && c == 0)
                c = 0xffff;
            wr16(csum, c);
        }
        wr16(l4 + 2, nport);
    } else {
        return 0;
    }
    e->last_ms = now;
    wr16(ip + 10, cs32(rd16(ip + 10), dst, e->int_ip));
    wr32(ip + 16, e->int_ip);
    verify_packet("in", ip, ihl, proto);
    trace("in", e, ip, ihl, proto);
    st.in++;
    return forward(p, ip, g_usb);
}

int nat_ip4_input(struct pbuf *p, struct netif *inp)
{
    if (!g_usb || !g_wifi || (inp != g_usb && inp != g_wifi))
        return 0;
    if (p->len < 20)
        return 0; /* let lwIP reject it */
    uint8_t *ip = p->payload;
    if ((ip[0] >> 4) != 4)
        return 0;
    unsigned ihl = (ip[0] & 0x0f) * 4;
    if (ihl < 20 || p->len < ihl + 8 || lwip_ntohs(rd16(ip + 2)) > p->tot_len)
        return 0;

    uint8_t proto = ip[9];
    if (inp == g_usb) {
        if (proto != PROTO_TCP && proto != PROTO_UDP && proto != PROTO_ICMP) {
            /* Unknown protocol: only drop it if it would be routed out. */
            uint32_t dst = rd32(ip + 16);
            ip4_addr_t d;
            d.addr = dst;
            if (dst == IPADDR_BROADCAST || ip4_addr_ismulticast(&d) ||
                ip4_addr_netcmp(&d, netif_ip4_addr(g_usb), netif_ip4_netmask(g_usb)))
                return 0;
            st.dropped++;
            pbuf_free(p);
            return 1;
        }
        return nat_outbound(p, ip, ihl, proto);
    }
    st.wifi_rx++;
    if (rd32(ip + 16) == netif_ip4_addr(g_wifi)->addr)
        st.wifi_rx_ours++;
    return nat_inbound(p, ip, ihl, proto);
}

void nat_init(struct netif *usb, struct netif *wifi, ip4_addr_t wii_ip)
{
    g_usb = usb;
    g_wifi = wifi;
    g_wii_ip = wii_ip;
}

void nat_log_stats(void)
{
    uint32_t now = sys_now();
    int n = 0;
    for (int i = 0; i < NAT_ENTRIES; i++)
        n += live(&tbl[i], now);
    dbg_printf("nat: %d active, out %lu in %lu dropped %lu new %lu evicted %lu; wifi ip rx %lu (%lu to us) badsum %lu fwd-in %lu", n,
        (unsigned long)st.out, (unsigned long)st.in, (unsigned long)st.dropped, (unsigned long)st.new_maps,
        (unsigned long)st.evicted, (unsigned long)st.wifi_rx, (unsigned long)st.wifi_rx_ours, (unsigned long)st.badsum, (unsigned long)st.fwd_in);
}
