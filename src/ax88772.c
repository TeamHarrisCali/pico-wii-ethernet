/*
 * Emulation of an ASIX AX88772 USB 10/100 Ethernet adapter (device side).
 * Register map and framing follow Linux drivers/net/usb/asix*.{c,h}; see docs/DESIGN.md.
 */
#include "ax88772.h"

#include <stdio.h>
#include <string.h>

#include "pico/stdlib.h"
#include "pico/bootrom.h"
#include "pico/unique_id.h"
#include "tusb.h"
#include "device/dcd.h"

#include "dbglog.h"
#include "wifi_config.h"

/* ---- vendor request codes (asix.h) ---- */
#define AX_CMD_SET_SW_MII        0x06
#define AX_CMD_READ_MII_REG      0x07
#define AX_CMD_WRITE_MII_REG     0x08
#define AX_CMD_STATMNGSTS_REG    0x09
#define AX_CMD_SET_HW_MII        0x0a
#define AX_CMD_READ_EEPROM       0x0b
#define AX_CMD_WRITE_EEPROM      0x0c
#define AX_CMD_WRITE_ENABLE      0x0d
#define AX_CMD_WRITE_DISABLE     0x0e
#define AX_CMD_READ_RX_CTL       0x0f
#define AX_CMD_WRITE_RX_CTL      0x10
#define AX_CMD_READ_IPG012       0x11
#define AX_CMD_WRITE_IPG0        0x12
#define AX_CMD_READ_NODE_ID      0x13 /* same code as WRITE_IPG1 */
#define AX_CMD_WRITE_NODE_ID     0x14 /* same code as WRITE_IPG2 */
#define AX_CMD_WRITE_MULTI_FILTER 0x16
#define AX_CMD_READ_PHY_ID       0x19
#define AX_CMD_READ_MEDIUM_STATUS 0x1a
#define AX_CMD_WRITE_MEDIUM_MODE 0x1b
#define AX_CMD_READ_MONITOR_MODE 0x1c
#define AX_CMD_WRITE_MONITOR_MODE 0x1d
#define AX_CMD_READ_GPIOS        0x1e
#define AX_CMD_WRITE_GPIOS       0x1f
#define AX_CMD_SW_RESET          0x20
#define AX_CMD_SW_PHY_STATUS     0x21
#define AX_CMD_SW_PHY_SELECT     0x22
#define AX_QCTCTRL               0x2a
#define AX_CMD_BOOTSEL           0x7e /* not an ASIX request: wValue 0xB007 reboots into the USB bootloader */

#define AX_EMBD_PHY_ADDR         0x10
#define AX_RX_CTL_SO             0x0080

#define FRAME_MAX   1536 /* largest Ethernet frame accepted either way */
#define TXQ_N       24
#define OUT_BUF     2048

/* ---- device state ---- */
static ax_rx_cb_t rx_cb;
static uint8_t nic_mac[6], gw_mac[6];
static bool link_up = true;
static bool configured;

static uint16_t reg_rx_ctl, reg_medium = 0x0316, reg_phy_sel;
static uint8_t reg_ipg[3], reg_gpio, reg_monitor, reg_mcast[8];
static bool sw_mii; /* host owns the MII bus: SET_SW_MII sets it, SET_HW_MII clears it, STATMNGSTS bit 0 shows it */
static uint16_t phy_bmcr = 0x3100;
static uint16_t phy_advertise = 0x01e1;

static uint8_t ctrl_buf[64] __attribute__((aligned(4)));

/* ---- bulk / interrupt state ---- */
typedef struct {
    uint16_t len;
    uint8_t data[4 + FRAME_MAX + 2] __attribute__((aligned(4)));
} txslot_t;

static txslot_t txq[TXQ_N];
static uint8_t txq_head, txq_tail, txq_count;
static bool tx_busy, tx_zlp;
static uint8_t out_buf[OUT_BUF] __attribute__((aligned(4)));
static uint8_t int_buf[8] __attribute__((aligned(4)));
static bool int_busy, int_pending;
static uint8_t drv_rhport;

static struct {
    uint32_t tx_frames, tx_drops, rx_frames, rx_bad;
} stats;

/* ---- SETUP packet capture (filled from the USB IRQ by the linker wrap, printed in ax_task) ---- */
#define SETUPQ_N 32
static struct { uint32_t t_us; uint8_t b[8]; } setupq[SETUPQ_N];
static volatile uint32_t setupq_head;
static uint32_t setupq_tail;

void __real_dcd_event_handler(dcd_event_t const *event, bool in_isr);

/* dcd_event_setup_received() is an inline helper, so wrap the function it calls instead. */
void __wrap_dcd_event_handler(dcd_event_t const *event, bool in_isr)
{
    if (event->event_id == DCD_EVENT_SETUP_RECEIVED) {
        uint32_t i = setupq_head % SETUPQ_N;
        setupq[i].t_us = time_us_32();
        memcpy(setupq[i].b, &event->setup_received, 8);
        setupq_head++;
    }
    __real_dcd_event_handler(event, in_isr);
}

static const char *vendor_name(uint8_t r)
{
    switch (r) {
    case AX_CMD_SET_SW_MII: return "SET_SW_MII";
    case AX_CMD_READ_MII_REG: return "READ_MII_REG";
    case AX_CMD_WRITE_MII_REG: return "WRITE_MII_REG";
    case AX_CMD_STATMNGSTS_REG: return "STATMNGSTS_REG";
    case AX_CMD_SET_HW_MII: return "SET_HW_MII";
    case AX_CMD_READ_EEPROM: return "READ_EEPROM";
    case AX_CMD_WRITE_EEPROM: return "WRITE_EEPROM";
    case AX_CMD_WRITE_ENABLE: return "WRITE_ENABLE";
    case AX_CMD_WRITE_DISABLE: return "WRITE_DISABLE";
    case AX_CMD_READ_RX_CTL: return "READ_RX_CTL";
    case AX_CMD_WRITE_RX_CTL: return "WRITE_RX_CTL";
    case AX_CMD_READ_IPG012: return "READ_IPG012";
    case AX_CMD_WRITE_IPG0: return "WRITE_IPG0";
    case AX_CMD_READ_NODE_ID: return "READ_NODE_ID/WRITE_IPG1";
    case AX_CMD_WRITE_NODE_ID: return "WRITE_NODE_ID/WRITE_IPG2";
    case AX_CMD_WRITE_MULTI_FILTER: return "WRITE_MULTI_FILTER";
    case AX_CMD_READ_PHY_ID: return "READ_PHY_ID";
    case AX_CMD_READ_MEDIUM_STATUS: return "READ_MEDIUM_STATUS";
    case AX_CMD_WRITE_MEDIUM_MODE: return "WRITE_MEDIUM_MODE";
    case AX_CMD_READ_MONITOR_MODE: return "READ_MONITOR_MODE";
    case AX_CMD_WRITE_MONITOR_MODE: return "WRITE_MONITOR_MODE";
    case AX_CMD_READ_GPIOS: return "READ_GPIOS";
    case AX_CMD_WRITE_GPIOS: return "WRITE_GPIOS";
    case AX_CMD_SW_RESET: return "SW_RESET";
    case AX_CMD_SW_PHY_STATUS: return "SW_PHY_STATUS";
    case AX_CMD_SW_PHY_SELECT: return "SW_PHY_SELECT";
    case AX_QCTCTRL: return "QCTCTRL";
    case AX_CMD_BOOTSEL: return "BOOTSEL";
    default: return "UNKNOWN";
    }
}

static const char *std_name(uint8_t r)
{
    static const char *const n[] = { "GET_STATUS", "CLEAR_FEATURE", "?", "SET_FEATURE", "?", "SET_ADDRESS",
        "GET_DESCRIPTOR", "SET_DESCRIPTOR", "GET_CONFIGURATION", "SET_CONFIGURATION", "GET_INTERFACE",
        "SET_INTERFACE", "SYNCH_FRAME" };
    return r < sizeof n / sizeof n[0] ? n[r] : "?";
}

static void log_setups(void)
{
    static uint8_t last[8];
    static uint32_t repeats;
    while (setupq_tail != setupq_head) {
        if (setupq_head - setupq_tail > SETUPQ_N)
            setupq_tail = setupq_head - SETUPQ_N;
        const uint8_t *b = setupq[setupq_tail % SETUPQ_N].b;
        uint32_t t = setupq[setupq_tail % SETUPQ_N].t_us;
        if (!memcmp(last, b, 8)) { /* the Wii polls some requests hundreds of times: count, don't print */
            repeats++;
            setupq_tail++;
            continue;
        }
        if (repeats) {
            dbg_printf("  (previous request repeated %lu more times)", (unsigned long)repeats);
            repeats = 0;
        }
        memcpy(last, b, 8);
        uint8_t type = (b[0] >> 5) & 3;
        uint16_t value = b[2] | b[3] << 8, index = b[4] | b[5] << 8, length = b[6] | b[7] << 8;
        const char *name = type == 0 ? std_name(b[1]) : type == 2 ? vendor_name(b[1]) : "class";
        dbg_printf("SETUP t=%lu bm=%02x req=%02x val=%04x idx=%04x len=%u  %s %s %s", (unsigned long)(t / 1000),
            b[0], b[1], value, index, length, type == 0 ? "std" : type == 1 ? "class" : "vendor", name,
            (b[0] & 0x80) ? "IN" : "OUT");
        setupq_tail++;
    }
}

/* ---- emulated embedded PHY (address 0x10) ---- */
static uint16_t phy_read(uint8_t phy, uint8_t reg)
{
    if (phy != AX_EMBD_PHY_ADDR)
        return 0;
    switch (reg) {
    case 0: return phy_bmcr;
    case 1: return link_up ? 0x786d : 0x7849;
    case 2: return 0x003b; /* PHY ID 0x003b1861: AX88772A internal PHY, as in Linux drivers/net/phy/ax88796b.c */
    case 3: return 0x1861;
    case 4: return phy_advertise;
    case 5: return link_up ? 0xcde1 : 0x0001;
    case 6: return 0x0001;
    default: return 0;
    }
}

static void phy_write(uint8_t phy, uint8_t reg, uint16_t v)
{
    if (phy != AX_EMBD_PHY_ADDR)
        return;
    if (reg == 0)
        phy_bmcr = v & ~(0x8000 | 0x0200); /* reset and restart-autoneg bits self-clear */
    else if (reg == 4)
        phy_advertise = v;
}

/* EEPROM image laid out like the AX88772C datasheet map (word offsets). The Wii reads words 0x00..0x15, takes the
 * MAC from 4..6 and the PHY address from the low byte of 0x11 (0xff there made it read PHY 0xff). */
static uint16_t eeprom_word(uint16_t off)
{
    switch (off) {
    case 0x00: return 0x5a15; /* signature, preload word count */
    case 0x01: return 0x1dec; /* flags: bus powered, remote wakeup, copper, CRC handling */
    case 0x04: return nic_mac[0] | nic_mac[1] << 8;
    case 0x05: return nic_mac[2] | nic_mac[3] << 8;
    case 0x06: return nic_mac[4] | nic_mac[5] << 8;
    case 0x07: return 0x0409; /* language ID */
    case 0x0f: return 0x0500; /* PHY register offsets reported in the interrupt packet: 5 and 0 */
    case 0x10: return 0x0600; /* max packet size */
    case 0x11: return 0xe010; /* primary PHY id 0x10 (embedded), no secondary */
    case 0x12: return 0x2010; /* pause frame water marks */
    case 0x18: return 0x00f2; /* checksum */
    default: return off < 0x20 ? 0x0000 : 0xffff;
    }
}

/* ---- vendor requests ---- */
static bool vendor_in(const tusb_control_request_t *r, uint8_t *buf, uint16_t *len)
{
    switch (r->bRequest) {
    case AX_CMD_READ_MII_REG: {
        uint16_t v = phy_read(r->wValue & 0xff, r->wIndex & 0xff);
        buf[0] = v; buf[1] = v >> 8; *len = 2;
        /* One line per change: the Wii's homebrew-mode driver polls a register every 3 ms. */
        static uint32_t last_key = 0xffffffff;
        uint32_t key = (r->wValue & 0xff) << 24 | (r->wIndex & 0xff) << 16 | v;
        if (key != last_key) {
            last_key = key;
            dbg_printf("  -> phy %02x reg %02x = %04x", r->wValue & 0xff, r->wIndex & 0xff, v);
        }
        return true;
    }
    /* Bit 0 (AX_HOST_EN) = host owns the MII: Linux waits for it to be 1 after SET_SW_MII, and the Wii (from its
     * log) polls it ~100 times after SET_HW_MII, so it must fall back to 0. Bits 6:4 = chip code 0 = AX88772. */
    case AX_CMD_STATMNGSTS_REG: buf[0] = sw_mii ? 0x01 : 0x00; *len = 1; return true;
    case AX_CMD_READ_EEPROM: {
        uint16_t w = eeprom_word(r->wValue);
        buf[0] = w; buf[1] = w >> 8; *len = 2;
        return true;
    }
    case AX_CMD_READ_RX_CTL: buf[0] = reg_rx_ctl; buf[1] = reg_rx_ctl >> 8; *len = 2; return true;
    case AX_CMD_READ_IPG012: memcpy(buf, reg_ipg, 3); *len = 3; return true;
    case AX_CMD_READ_NODE_ID: memcpy(buf, nic_mac, 6); *len = 6; return true;
    case AX_CMD_READ_PHY_ID: buf[0] = 0x00; buf[1] = AX_EMBD_PHY_ADDR; *len = 2; return true;
    case AX_CMD_READ_MEDIUM_STATUS: buf[0] = reg_medium; buf[1] = reg_medium >> 8; *len = 2; return true;
    case AX_CMD_READ_MONITOR_MODE: buf[0] = reg_monitor; *len = 1; return true;
    case AX_CMD_READ_GPIOS: buf[0] = reg_gpio; *len = 1; return true;
    case AX_CMD_SW_PHY_STATUS: buf[0] = 0; *len = 1; return true;
    case AX_QCTCTRL: buf[0] = 0; *len = 1; return true;
    default: return false;
    }
}

static bool vendor_out_known(uint8_t req)
{
    switch (req) {
    case AX_CMD_SET_SW_MII: case AX_CMD_WRITE_MII_REG: case AX_CMD_SET_HW_MII: case AX_CMD_WRITE_EEPROM:
    case AX_CMD_WRITE_ENABLE: case AX_CMD_WRITE_DISABLE: case AX_CMD_WRITE_RX_CTL: case AX_CMD_WRITE_IPG0:
    case AX_CMD_READ_NODE_ID: case AX_CMD_WRITE_NODE_ID: case AX_CMD_WRITE_MULTI_FILTER:
    case AX_CMD_WRITE_MEDIUM_MODE: case AX_CMD_WRITE_MONITOR_MODE: case AX_CMD_WRITE_GPIOS:
    case AX_CMD_SW_RESET: case AX_CMD_SW_PHY_SELECT: case AX_QCTCTRL: case AX_CMD_BOOTSEL:
        return true;
    default:
        return false;
    }
}

static void vendor_out(const tusb_control_request_t *r, const uint8_t *d, uint16_t len)
{
    switch (r->bRequest) {
    case AX_CMD_SET_SW_MII: sw_mii = true; break;
    case AX_CMD_SET_HW_MII: sw_mii = false; break;
    case AX_CMD_WRITE_MII_REG:
        if (len >= 2)
            phy_write(r->wValue & 0xff, r->wIndex & 0xff, d[0] | d[1] << 8);
        break;
    case AX_CMD_WRITE_RX_CTL:
        reg_rx_ctl = r->wValue;
        dbg_printf("  RX_CTL = %04x%s", reg_rx_ctl, (reg_rx_ctl & AX_RX_CTL_SO) ? " (start operation)" : "");
        break;
    case AX_CMD_WRITE_IPG0:
        reg_ipg[0] = r->wValue; reg_ipg[1] = r->wValue >> 8; reg_ipg[2] = r->wIndex;
        break;
    case AX_CMD_WRITE_NODE_ID:
        /* Writes are accepted but ignored: the MAC the host reads stays ours. */
        break;
    case AX_CMD_WRITE_MULTI_FILTER:
        if (len >= 8) memcpy(reg_mcast, d, 8);
        break;
    case AX_CMD_WRITE_MEDIUM_MODE: reg_medium = r->wValue; break;
    case AX_CMD_WRITE_MONITOR_MODE: reg_monitor = r->wValue; break;
    case AX_CMD_WRITE_GPIOS: reg_gpio = r->wValue; break;
    case AX_CMD_SW_PHY_SELECT: reg_phy_sel = r->wValue; break;
    case AX_CMD_BOOTSEL:
        if (r->wValue == 0xb007)
            reset_usb_boot(0, 0);
        break;
    default: break;
    }
}

static bool vendor_control(uint8_t rhport, uint8_t stage, const tusb_control_request_t *r)
{
    if (stage == CONTROL_STAGE_ACK)
        return true;

    if (stage == CONTROL_STAGE_DATA) {
        /* OUT data stage finished; the payload is in ctrl_buf. */
        if (r->bmRequestType_bit.direction == TUSB_DIR_OUT) {
            char hex[3 * 16 + 1] = "";
            for (uint16_t i = 0; i < r->wLength && i < 16; i++)
                snprintf(hex + i * 3, 4, "%02x ", ctrl_buf[i]);
            dbg_printf("  OUT data: %s", hex);
            vendor_out(r, ctrl_buf, r->wLength);
        }
        return true;
    }

    /* SETUP stage */
    if (r->bmRequestType_bit.recipient != TUSB_REQ_RCPT_DEVICE) {
        dbg_printf("  vendor request with non-device recipient: STALL");
        return false;
    }

    if (r->bmRequestType_bit.direction == TUSB_DIR_IN) {
        uint16_t len = 0;
        memset(ctrl_buf, 0, sizeof ctrl_buf);
        if (!vendor_in(r, ctrl_buf, &len)) {
            dbg_printf("  UNKNOWN vendor IN request %02x: STALL", r->bRequest);
            return false;
        }
        if (len > r->wLength)
            len = r->wLength;
        return tud_control_xfer(rhport, r, ctrl_buf, len);
    }

    if (!vendor_out_known(r->bRequest)) {
        dbg_printf("  UNKNOWN vendor OUT request %02x: STALL", r->bRequest);
        return false;
    }
    if (r->wLength) {
        if (r->wLength > sizeof ctrl_buf)
            return false;
        return tud_control_xfer(rhport, r, ctrl_buf, r->wLength); /* applied at DATA stage */
    }
    vendor_out(r, NULL, 0);
    return tud_control_status(rhport, r);
}

/* TinyUSB routes vendor-type requests addressed to the device to this function. */
bool tud_vendor_control_xfer_cb(uint8_t rhport, uint8_t stage, tusb_control_request_t const *request)
{
    return vendor_control(rhport, stage, request);
}

/* ---- bulk data path ---- */
static void tx_pump(void)
{
    if (!configured || tx_busy || !txq_count)
        return;
    if (!usbd_edpt_claim(drv_rhport, AX_EP_BULK_IN))
        return;
    txslot_t *s = &txq[txq_tail];
    if (usbd_edpt_xfer(drv_rhport, AX_EP_BULK_IN, s->data, s->len))
        tx_busy = true;
    else
        usbd_edpt_release(drv_rhport, AX_EP_BULK_IN);
}

static void int_pump(void)
{
    if (!configured || int_busy || !int_pending)
        return;
    if (!usbd_edpt_claim(drv_rhport, AX_EP_INT_IN))
        return;
    /* Linux asix_status(): byte 2 bit 0 is the link flag. Other bytes are a guess. */
    memset(int_buf, 0, sizeof int_buf);
    int_buf[0] = 0xa1;
    int_buf[2] = link_up ? 1 : 0;
    /* Datasheet: after the first 4 bytes come PHY register 5 (partner ability) and register 0, 16 bit each. */
    uint16_t lpa = phy_read(AX_EMBD_PHY_ADDR, 5), bmcr = phy_read(AX_EMBD_PHY_ADDR, 0);
    int_buf[4] = lpa; int_buf[5] = lpa >> 8; int_buf[6] = bmcr; int_buf[7] = bmcr >> 8;
    int_pending = false;
    if (usbd_edpt_xfer(drv_rhport, AX_EP_INT_IN, int_buf, sizeof int_buf))
        int_busy = true;
    else
        usbd_edpt_release(drv_rhport, AX_EP_INT_IN);
}

bool ax_send_frame(const uint8_t *frame, uint16_t len)
{
    if (!configured || len < 14 || len > FRAME_MAX)
        return false;
    if (txq_count == TXQ_N) {
        stats.tx_drops++;
        return false;
    }
    txslot_t *s = &txq[txq_head];
    uint16_t nlen = ~len;
    s->data[0] = len; s->data[1] = len >> 8; s->data[2] = nlen; s->data[3] = nlen >> 8;
    memcpy(s->data + 4, frame, len);
    s->len = 4 + len;
    if (len & 1)
        s->data[s->len++] = 0; /* pad to even */
    txq_head = (txq_head + 1) % TXQ_N;
    txq_count++;
    stats.tx_frames++;
    tx_pump();
    return true;
}

static bool hdr_ok(const uint8_t *b)
{
    return (uint16_t)((b[0] | b[1] << 8) ^ (b[2] | b[3] << 8)) == 0xffff;
}

static void parse_out(const uint8_t *b, uint32_t n)
{
    while (n >= 4) {
        if (!hdr_ok(b)) {
            stats.rx_bad++;
            dbg_printf("bad bulk OUT header %02x %02x %02x %02x (%lu bytes left)", b[0], b[1], b[2], b[3], (unsigned long)n);
            return;
        }
        uint16_t len = b[0] | b[1] << 8;
        b += 4; n -= 4;
        if (len == 0)
            continue; /* the 00 00 ff ff trailer parses as an empty packet */
        if (len > n || len > FRAME_MAX) {
            stats.rx_bad++;
            dbg_printf("bad bulk OUT length %u (have %lu)", len, (unsigned long)n);
            return;
        }
        stats.rx_frames++;
        if (rx_cb)
            rx_cb(b, len);
        b += len; n -= len;
        /* Tolerate one pad byte after an odd-length packet when more packets follow. */
        if ((len & 1) && n >= 5 && !hdr_ok(b) && hdr_ok(b + 1)) {
            b++; n--;
        }
    }
}

static void out_arm(uint8_t rhport)
{
    if (usbd_edpt_claim(rhport, AX_EP_BULK_OUT)) {
        if (!usbd_edpt_xfer(rhport, AX_EP_BULK_OUT, out_buf, sizeof out_buf))
            usbd_edpt_release(rhport, AX_EP_BULK_OUT);
    }
}

/* ---- TinyUSB class driver ---- */
static void drv_init(void) {}
static bool drv_deinit(void) { return true; }

static void drv_reset(uint8_t rhport)
{
    (void)rhport;
    configured = false;
    tx_busy = tx_zlp = int_busy = int_pending = false;
    txq_head = txq_tail = txq_count = 0;
}

static uint16_t drv_open(uint8_t rhport, tusb_desc_interface_t const *itf, uint16_t max_len)
{
    if (itf->bInterfaceClass != TUSB_CLASS_VENDOR_SPECIFIC || itf->bNumEndpoints != 3)
        return 0;
    uint16_t need = sizeof(tusb_desc_interface_t) + 3 * sizeof(tusb_desc_endpoint_t);
    TU_VERIFY(max_len >= need, 0);

    const uint8_t *p = tu_desc_next(itf);
    for (int i = 0; i < 3; i++) {
        TU_ASSERT(usbd_edpt_open(rhport, (tusb_desc_endpoint_t const *)p), 0);
        p = tu_desc_next(p);
    }
    drv_rhport = rhport;
    drv_reset(rhport);
    configured = true;
    int_pending = true;
    dbg_printf("AX88772 interface opened (configured)");
    out_arm(rhport);
    return need;
}

static bool drv_control(uint8_t rhport, uint8_t stage, tusb_control_request_t const *r)
{
    if (stage != CONTROL_STAGE_SETUP)
        return true;
    if (r->bmRequestType_bit.type == TUSB_REQ_TYPE_STANDARD && r->bRequest == TUSB_REQ_SET_INTERFACE)
        return tud_control_status(rhport, r);
    if (r->bmRequestType_bit.type == TUSB_REQ_TYPE_STANDARD && r->bRequest == TUSB_REQ_GET_INTERFACE) {
        ctrl_buf[0] = 0;
        return tud_control_xfer(rhport, r, ctrl_buf, 1);
    }
    return false;
}

static bool drv_xfer(uint8_t rhport, uint8_t ep, xfer_result_t result, uint32_t xferred)
{
    (void)result;
    switch (ep) {
    case AX_EP_BULK_OUT:
        parse_out(out_buf, xferred);
        out_arm(rhport);
        break;
    case AX_EP_BULK_IN:
        if (!tx_zlp && xferred && (xferred % AX_BULK_MAXPKT) == 0 &&
            usbd_edpt_claim(rhport, AX_EP_BULK_IN)) {
            /* Transfer ended on a packet boundary: terminate it with a zero-length packet. */
            tx_zlp = true;
            if (usbd_edpt_xfer(rhport, AX_EP_BULK_IN, NULL, 0))
                break;
            usbd_edpt_release(rhport, AX_EP_BULK_IN);
        }
        tx_zlp = false;
        tx_busy = false;
        if (txq_count) {
            txq_tail = (txq_tail + 1) % TXQ_N;
            txq_count--;
        }
        tx_pump();
        break;
    case AX_EP_INT_IN:
        /* The real chip answers every interrupt poll with the current status (link bit + PHY registers),
         * so keep a fresh packet queued: the Wii's log shows it going silent after restarting autoneg. */
        int_busy = false;
        int_pending = true;
        int_pump();
        break;
    default:
        break;
    }
    return true;
}

const usbd_class_driver_t ax_class_driver = {
    .name = "AX88772",
    .init = drv_init,
    .deinit = drv_deinit,
    .reset = drv_reset,
    .open = drv_open,
    .control_xfer_cb = drv_control,
    .xfer_cb = drv_xfer,
    .sof = NULL,
};

usbd_class_driver_t const *usbd_app_driver_get_cb(uint8_t *driver_count)
{
    *driver_count = 1;
    return &ax_class_driver;
}

/* ---- public ---- */
void ax_init(ax_rx_cb_t cb)
{
    rx_cb = cb;
    pico_unique_board_id_t id;
    pico_get_unique_board_id(&id);
    nic_mac[0] = 0x02; /* locally administered, unicast */
    for (int i = 0; i < 5; i++)
        nic_mac[i + 1] = id.id[i + 3] ^ id.id[i];
    memcpy(gw_mac, nic_mac, 6);
    gw_mac[5] ^= 0x01;
    dbg_printf("NIC MAC %02x:%02x:%02x:%02x:%02x:%02x, gateway MAC ...:%02x", nic_mac[0], nic_mac[1],
        nic_mac[2], nic_mac[3], nic_mac[4], nic_mac[5], gw_mac[5]);
}

const uint8_t *ax_nic_mac(void) { return nic_mac; }
const uint8_t *ax_gateway_mac(void) { return gw_mac; }
bool ax_configured(void) { return configured; }

void ax_set_link(bool up)
{
    if (up == link_up)
        return;
    link_up = up;
    int_pending = true;
    dbg_printf("link %s", up ? "up" : "down");
}

void ax_task(void)
{
    log_setups();
    int_pump();
    tx_pump();

    static uint32_t last_ms;
    uint32_t now = to_ms_since_boot(get_absolute_time());
    if (configured && now - last_ms >= 10000) {
        last_ms = now;
        dbg_printf("usb: host->pico %lu frames (%lu bad), pico->host %lu frames (%lu dropped), rx_ctl=%04x",
            (unsigned long)stats.rx_frames, (unsigned long)stats.rx_bad, (unsigned long)stats.tx_frames,
            (unsigned long)stats.tx_drops, reg_rx_ctl);
    }
}
