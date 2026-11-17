/*
 * Pico W as a Wi-Fi to USB-Ethernet adapter for the Nintendo Wii.
 * USB side: emulated ASIX AX88772 (src/ax88772.c). Router side: lwIP with DHCP server and NAT.
 */
#include <stdio.h>

#include "pico/stdlib.h"
#include "pico/cyw43_arch.h"
#include "tusb.h"
#include "lwip/dns.h"
#include "lwip/etharp.h"
#include "lwip/netif.h"

#include "ax88772.h"
#include "dbglog.h"
#include "dhcpd.h"
#include "nat.h"
#include "usbnet.h"
#include "wifi_config.h"

#define WIFI_RETRY_MS      30000 /* give a join in progress this long */
#define WIFI_FAIL_RETRY_MS 3000  /* retry quickly after a failed join */

static struct netif *wifi_netif(void)
{
    return &cyw43_state.netif[CYW43_ITF_STA];
}

static bool wifi_has_ip(void)
{
    return cyw43_tcpip_link_status(&cyw43_state, CYW43_ITF_STA) == CYW43_LINK_UP &&
           !ip4_addr_isany_val(*netif_ip4_addr(wifi_netif()));
}

static char dns_result[64] = "pending";

static void dns_test_cb(const char *name, const ip_addr_t *addr, void *arg)
{
    (void)arg;
    snprintf(dns_result, sizeof dns_result, "%s", addr ? ipaddr_ntoa(addr) : "FAILED");
    dbg_printf("selftest: dns %s -> %s", name, addr ? ipaddr_ntoa(addr) : "FAILED/timeout");
}

static void wifi_task(void)
{
    static bool was_up, first = true;
    static uint32_t last_try;
    uint32_t now = to_ms_since_boot(get_absolute_time());
    bool up = wifi_has_ip();

    if (up != was_up) {
        was_up = up;
        if (up) {
            netif_set_default(wifi_netif());
            char ip[16], gw[16], mask[16], dns[16];
            ip4addr_ntoa_r(netif_ip4_addr(wifi_netif()), ip, sizeof ip);
            ip4addr_ntoa_r(netif_ip4_gw(wifi_netif()), gw, sizeof gw);
            ip4addr_ntoa_r(netif_ip4_netmask(wifi_netif()), mask, sizeof mask);
            ipaddr_ntoa_r(dns_getserver(0), dns, sizeof dns);
            dbg_printf("wifi up: ip %s mask %s gw %s dns %s", ip, mask, gw, dns);
            dbg_udp_enable(wifi_netif());
            /* Self-test of the Pico's own path to the Internet (no NAT involved). */
            ip_addr_t r;
            err_t e = dns_gethostbyname("example.com", &r, dns_test_cb, NULL);
            if (e == ERR_OK)
                dns_test_cb("example.com", &r, NULL);
            else if (e != ERR_INPROGRESS)
                dbg_printf("selftest: dns query error %d", e);
        } else {
            dbg_printf("wifi down");
            dbg_udp_enable(NULL);
        }
#if WII_LINK_FOLLOWS_WIFI
        ax_set_link(up);
#endif
    }

    if (!up) {
        int st = cyw43_tcpip_link_status(&cyw43_state, CYW43_ITF_STA);
        bool failed = st < 0;
        if (first || (failed && now - last_try >= WIFI_FAIL_RETRY_MS) || now - last_try >= WIFI_RETRY_MS) {
            first = false;
            last_try = now;
            dbg_printf("wifi: joining \"%s\" (link status %d)", WIFI_SSID, st);
            int rc = cyw43_arch_wifi_connect_async(WIFI_SSID, WIFI_PASSWORD, WIFI_AUTH);
            if (rc)
                dbg_printf("wifi: connect request failed (%d)", rc);
        }
    }

    /* LED: solid = Wi-Fi up, slow blink = trying. */
    static uint32_t led_ms;
    static bool led;
    if (up) {
        cyw43_arch_gpio_put(CYW43_WL_GPIO_LED_PIN, 1);
    } else if (now - led_ms >= 500) {
        led_ms = now;
        led = !led;
        cyw43_arch_gpio_put(CYW43_WL_GPIO_LED_PIN, led);
    }
}

int main(void)
{
    dbg_init();
    dbg_printf("pico-wii-ethernet starting");

    ax_init(usbnet_rx);

    if (cyw43_arch_init_with_country(WIFI_COUNTRY)) {
        dbg_printf("cyw43 init failed");
        for (;;)
            dbg_pump();
    }
    cyw43_arch_enable_sta_mode();
    cyw43_wifi_pm(&cyw43_state, CYW43_PERFORMANCE_PM);
    netif_set_default(wifi_netif());

    usbnet_init(USB_NET_PICO_IP, USB_NET_MASK);
    ip4_addr_t wii_ip;
    ip4addr_aton(USB_NET_WII_IP, &wii_ip);
    dhcpd_init(&usb_netif, wii_ip, wifi_has_ip);
    nat_init(&usb_netif, wifi_netif(), wii_ip);
    dbg_printf("router: pico %s, wii %s", USB_NET_PICO_IP, USB_NET_WII_IP);

    /* USB comes up last so the host never sees a device that cannot answer yet. */
    tud_init(BOARD_TUD_RHPORT);

    uint32_t stats_ms = 0;
    for (;;) {
        tud_task();
        cyw43_arch_poll();
        ax_task();
        wifi_task();

        uint32_t now = to_ms_since_boot(get_absolute_time());
        if (now - stats_ms >= 10000) {
            stats_ms = now;
            nat_log_stats();
            struct eth_addr *mac;
            const ip4_addr_t *ipp;
            bool gw_ok = etharp_find_addr(wifi_netif(), netif_ip4_gw(wifi_netif()), &mac, &ipp) >= 0;
            if (!wifi_has_ip())
                dbg_printf("wifi not connected yet");
            if (wifi_has_ip())
                dbg_printf("wifi ip %s, gateway ARP %s, dns self-test %s", ip4addr_ntoa(netif_ip4_addr(wifi_netif())),
                    gw_ok ? "resolved" : "NOT resolved", dns_result);
        }
        dbg_pump();
    }
}

void tud_mount_cb(void)   { dbg_printf("usb: configured by host"); }
void tud_umount_cb(void)  { dbg_printf("usb: unmounted"); }
void tud_suspend_cb(bool remote_wakeup_en) { (void)remote_wakeup_en; dbg_printf("usb: suspended"); }
void tud_resume_cb(void)  { dbg_printf("usb: resumed"); }

#if WII_ETH_DEBUG_CDC
/* Bench builds only: opening the port at 1200 baud reboots into BOOTSEL (no button needed). */
#include "pico/bootrom.h"
void tud_cdc_line_coding_cb(uint8_t itf, cdc_line_coding_t const *coding)
{
    (void)itf;
    if (coding->bit_rate == 1200)
        reset_usb_boot(0, 0);
}
#endif
