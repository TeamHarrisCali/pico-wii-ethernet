#ifndef DBGLOG_H
#define DBGLOG_H

#include <stdbool.h>

struct netif;

/* Log to a RAM ring buffer; dbg_pump() drains it to UART0 (GP0/GP1), the optional USB CDC port
 * and, once Wi-Fi is up, UDP datagrams. dbg_printf() must not be called from an interrupt. */
void dbg_init(void);
void dbg_printf(const char *fmt, ...) __attribute__((format(printf, 1, 2)));
void dbg_pump(void);
void dbg_udp_enable(struct netif *wifi); /* NULL disables */

#endif
