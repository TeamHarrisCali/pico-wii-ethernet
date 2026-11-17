#ifndef LWIPOPTS_H
#define LWIPOPTS_H

/* lwIP configuration: bare metal (poll mode), two Ethernet netifs, IPv4 forwarding + NAT hook. */

#define NO_SYS                      1
#define LWIP_SOCKET                 0
#define LWIP_NETCONN                0
#define LWIP_TIMERS                 1

#define MEM_LIBC_MALLOC             0
#define MEM_ALIGNMENT               4
#define MEM_SIZE                    20000
#define MEMP_NUM_PBUF               16
#define MEMP_NUM_UDP_PCB            8
#define MEMP_NUM_ARP_QUEUE          16
#define PBUF_POOL_SIZE              24
/* One pbuf holds a whole frame, so the NAT hook always sees contiguous headers. */
#define PBUF_POOL_BUFSIZE           1600

#define LWIP_IPV4                   1
#define LWIP_IPV6                   0
#define LWIP_ARP                    1
#define LWIP_ETHERNET               1
#define ARP_QUEUEING                1
#define LWIP_ICMP                   1
#define LWIP_RAW                    0
#define LWIP_UDP                    1
#define LWIP_TCP                    0
#define LWIP_DHCP                   1
#define LWIP_DNS                    1
#define LWIP_AUTOIP                 0
#define LWIP_IGMP                   0
#define LWIP_BROADCAST_PING         0
#define LWIP_NETIF_STATUS_CALLBACK  1
#define LWIP_NETIF_LINK_CALLBACK    1
#define LWIP_NETIF_HOSTNAME         1
#define LWIP_STATS                  0
#define LWIP_CHKSUM_ALGORITHM       3

#define IP_FORWARD                  1
#define IP_REASSEMBLY               0
#define IP_FRAG                     0
#define IP_DEFAULT_TTL              64
#define ETH_PAD_SIZE                0

/* NAT lives in src/nat.c and sees every IPv4 packet before lwIP does. */
struct pbuf;
struct netif;
int nat_ip4_input(struct pbuf *p, struct netif *inp);
#define LWIP_HOOK_IP4_INPUT(p, inp) nat_ip4_input((p), (inp))

#endif
