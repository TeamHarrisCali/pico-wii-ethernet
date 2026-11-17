#include "dbglog.h"

#include <stdarg.h>
#include <stdio.h>
#include <string.h>

#include "pico/stdlib.h"
#include "hardware/sync.h"
#include "hardware/uart.h"
#include "lwip/ip_addr.h"
#include "lwip/netif.h"
#include "lwip/pbuf.h"
#include "lwip/udp.h"

#if WII_ETH_DEBUG_CDC
#include "tusb.h"
#endif

#include "wifi_config.h"

#define RING_SIZE   16384
#define UDP_CHUNK   1000

/* head and the three tails only ever grow; byte i lives at ring[i % RING_SIZE]. */
static char ring[RING_SIZE];
static volatile uint32_t head;
static uint32_t tail_uart, tail_cdc, tail_udp;

static struct udp_pcb *udp_pcb;
static struct netif *udp_netif;
static ip_addr_t udp_dest;
static uint32_t udp_last_us;

void dbg_init(void)
{
    uart_init(uart0, 115200);
    gpio_set_function(0, GPIO_FUNC_UART);
    gpio_set_function(1, GPIO_FUNC_UART);
}

static void ring_put(const char *s, int n)
{
    uint32_t save = save_and_disable_interrupts();
    for (int i = 0; i < n; i++)
        ring[head++ % RING_SIZE] = s[i];
    restore_interrupts(save);
}

void dbg_printf(const char *fmt, ...)
{
    char line[200];
    uint32_t ms = (uint32_t)(time_us_64() / 1000); /* 64-bit source: time_us_32() wraps every 71 minutes */
    int n = snprintf(line, sizeof line, "[%5lu.%03lu] ", (unsigned long)(ms / 1000), (unsigned long)(ms % 1000));
    va_list ap;
    va_start(ap, fmt);
    n += vsnprintf(line + n, sizeof line - n - 2, fmt, ap);
    va_end(ap);
    if (n > (int)sizeof line - 2)
        n = sizeof line - 2;
    line[n++] = '\n';
    ring_put(line, n);
}

void dbg_udp_enable(struct netif *wifi)
{
    udp_netif = NULL;
    if (!wifi || !strlen(LOG_UDP_DEST))
        return;
    if (!udp_pcb) {
        udp_pcb = udp_new();
        if (!udp_pcb)
            return;
    }
    if (!ipaddr_aton(LOG_UDP_DEST, &udp_dest))
        return;
    /* Replay the whole backlog once the network is up: it holds the early USB requests. */
    uint32_t h = head;
    tail_udp = (h - tail_udp > RING_SIZE) ? h - RING_SIZE : tail_udp;
    udp_netif = wifi;
}

static void pump_udp(uint32_t h)
{
    if (!udp_netif || tail_udp == h)
        return;
    uint32_t now = time_us_32();
    if (now - udp_last_us < 3000)
        return;
    if (h - tail_udp > RING_SIZE)
        tail_udp = h - RING_SIZE;

    uint32_t avail = h - tail_udp;
    if (avail > UDP_CHUNK)
        avail = UDP_CHUNK;
    /* Send whole lines only. */
    uint32_t len = 0;
    for (uint32_t i = 0; i < avail; i++)
        if (ring[(tail_udp + i) % RING_SIZE] == '\n')
            len = i + 1;
    if (!len && avail == UDP_CHUNK)
        len = avail;
    if (!len)
        return;

    struct pbuf *p = pbuf_alloc(PBUF_TRANSPORT, len, PBUF_RAM);
    if (!p)
        return;
    for (uint32_t i = 0; i < len; i++)
        ((char *)p->payload)[i] = ring[(tail_udp + i) % RING_SIZE];
    err_t e = udp_sendto_if(udp_pcb, p, &udp_dest, LOG_UDP_PORT, udp_netif);
    pbuf_free(p);
    udp_last_us = now;
    if (e == ERR_OK)
        tail_udp += len;
}

void dbg_pump(void)
{
    uint32_t h = head;

    if (h - tail_uart > RING_SIZE)
        tail_uart = h - RING_SIZE;
    while (tail_uart != h && uart_is_writable(uart0))
        uart_putc_raw(uart0, ring[tail_uart++ % RING_SIZE]);

#if WII_ETH_DEBUG_CDC
    if (h - tail_cdc > RING_SIZE)
        tail_cdc = h - RING_SIZE;
    if (tud_cdc_connected()) {
        uint32_t room = tud_cdc_write_available();
        while (tail_cdc != h && room) {
            tud_cdc_write_char(ring[tail_cdc++ % RING_SIZE]);
            room--;
        }
        tud_cdc_write_flush();
    }
#else
    (void)tail_cdc;
#endif

    pump_udp(h);
}
