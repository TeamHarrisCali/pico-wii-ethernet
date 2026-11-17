# Design note: Pico W as a Wi-Fi to USB-Ethernet adapter for the Wii

Goal: a Raspberry Pi Pico W plugged into the Wii's USB port that the Wii's IOS
treats as an ASIX AX88772 USB Ethernet adapter, with the Pico routing the Wii's
traffic out over Wi-Fi.

Sources used: Linux `drivers/net/usb/asix.h`, `asix_devices.c` (the `asix` module
on this machine, `modinfo asix`), `asix_common.c` (from memory, cross-checked
against asix.h), public ASIX datasheet knowledge. **Nothing here comes from the
Wii's IOS itself**; that is the main unknown (see the end).

## 1. Architecture

```
 Wii ==USB== [ TinyUSB device: fake AX88772 ] --- lwIP netif "usb" 192.168.7.1/24
                                                      |  DHCP server (gives Wii 192.168.7.2)
                                                      |  IPv4 forward + NAT hook (src/nat.c)
                                              lwIP netif "wifi" (cyw43, DHCP client) --- AP / hotspot
```

Wi-Fi station mode cannot carry the Wii's MAC address, so the Pico is a router,
not a bridge. lwIP does the forwarding (`IP_FORWARD=1`); upstream lwIP has no
NAPT, so `src/nat.c` implements it in `LWIP_HOOK_IP4_INPUT`: rewrite source
IP/port on the way out, destination IP/port on the way back, fix checksums
incrementally (RFC 1624), then let lwIP forward normally. Supported: TCP, UDP,
ICMP echo. The hook also forwards the translated packet itself (`forward()` in nat.c): lwIP's
`ip4_forward()` zeroes the IP/L4 checksums when software checksum generation is on, which breaks
plain forwarding. Not supported: IP fragments, ICMP error translation, IPv6.

Everything runs in one thread (`pico_cyw43_arch_lwip_poll`): the main loop calls
`tud_task()`, `cyw43_arch_poll()`, and the USB/DHCP/log pumps. No locking
needed.

## 2. USB identity

| Field | Value | Notes |
|---|---|---|
| VID:PID | `0B95:7720` | ASIX AX88772. Linux `asix` matches it. Also used by Nintendo's RVL-015 (reported, not verified). Configurable in `src/usb_descriptors.c`. |
| Other AX88772 IDs | `0B95:772A` (772A), `0B95:772B` (772B) | If the Wii ignores 7720, try 772A. 772A/B have extra registers and a different RX header (see below), so 7720 is the safest. |
| Device class | `FF/FF/00` | vendor specific |
| Speed | full speed (12 Mbit/s) | RP2040 has no high-speed PHY. Real adapter is HS. **Unknown** whether the Wii's driver cares. |
| Interface 0 | `FF/FF/00`, 3 endpoints | |
| EP `0x81` | interrupt IN, 8 bytes, interval 11 | link-status packet |
| EP `0x82` | bulk IN, 64 bytes (FS) | device to host, Ethernet frames |
| EP `0x03` | bulk OUT, 64 bytes (FS) | host to device |
| Strings | "ASIX Elec. Corp." / "AX88772" / "000001" | |

Endpoint layout and strings are from memory of real AX88772 `lsusb -v` dumps.
Linux does not care about endpoint numbers (usbnet finds the bulk pair).

## 3. Vendor control requests

All are `bmRequestType` 0xC0 (device to host) or 0x40 (host to device), recipient
device, `bRequest` as below. Values are what `asix_devices.c` uses on the AX88772.

| bRequest | Name | Dir | wValue / wIndex / data | Emulation |
|---|---|---|---|---|
| 0x06 | SET_SW_MII | OUT | none | MII bus handed to software (no-op) |
| 0x07 | READ_MII_REG | IN | wValue=phy addr, wIndex=reg, 2 bytes | read emulated PHY |
| 0x08 | WRITE_MII_REG | OUT | wValue=phy addr, wIndex=reg, 2 bytes data | write emulated PHY (BMCR reset self-clears) |
| 0x09 | STATMNGSTS_REG | IN | 1 byte | bit 0 = AX_HOST_EN = host owns the MII: 1 after SET_SW_MII (Linux waits for it), 0 after SET_HW_MII (the Wii polls it after releasing the MII); bits 6:4 = chip code 0 |
| 0x0A | SET_HW_MII | OUT | none | no-op |
| 0x0B | READ_EEPROM | IN | wValue=word offset, 2 bytes | words 4..6 = MAC, otherwise 0xFFFF |
| 0x0C / 0x0D / 0x0E | WRITE_EEPROM / WRITE_ENABLE / WRITE_DISABLE | OUT | | accepted, ignored |
| 0x0F | READ_RX_CTL | IN | 2 bytes | return stored value |
| 0x10 | WRITE_RX_CTL | OUT | wValue=ctl | store; bit 0x80 (SO) = start operation, bit 0x01 promisc, 0x08 broadcast, 0x10 multicast |
| 0x11 | READ_IPG012 | IN | 3 bytes | stored |
| 0x12 | WRITE_IPG0 | OUT | wValue=ipg0 \| ipg1<<8, wIndex=ipg2 | stored |
| 0x13 | READ_NODE_ID | IN | 6 bytes | MAC address |
| 0x14 | WRITE_NODE_ID | OUT | 6 bytes | stored as MAC (we still use our own when the host reads it back) |
| 0x16 | WRITE_MULTI_FILTER | OUT | 8 bytes | stored, ignored (we accept all multicast) |
| 0x19 | READ_PHY_ID | IN | 2 bytes | `{0x00, 0x10}`: embedded PHY at address 0x10 (`buf[1]`) |
| 0x1A | READ_MEDIUM_STATUS | IN | 2 bytes | stored medium mode |
| 0x1B | WRITE_MEDIUM_MODE | OUT | wValue=mode | store; Linux writes `FD\|PS\|AC\|RE` = 0x0316 (PS = 100 Mbit) |
| 0x1C / 0x1D | READ/WRITE_MONITOR_MODE | IN/OUT | 1 byte | stored |
| 0x1E | READ_GPIOS | IN | 1 byte | stored |
| 0x1F | WRITE_GPIOS | OUT | wValue=gpio bits | stored |
| 0x20 | SW_RESET | OUT | wValue=flags (RR 1, RT 2, PRTE 4, PRL 8, BZ 0x10, IPRL 0x20, IPPD 0x40) | accepted; reset clears RX ctl |
| 0x21 | SW_PHY_STATUS | IN | 1 byte | 0 |
| 0x22 | SW_PHY_SELECT | OUT | wValue=sel (0 internal, 4 external) | stored |
| 0x2A | QCTCTRL | OUT/IN | | accepted, ignored |

Anything else is **logged and STALLed**; that is the iteration loop with the Wii.

### Linux init sequence (what a minimal host does)

`ax88772_bind` then `ax88772_hw_reset` (original chip): GPIOS write (5 ms delay),
PHY_SELECT, a series of SW_RESET writes with waits of 10 to 150 ms
(`IPPD|PRL` then `CLEAR` then `IPRL|PRL` etc.), WRITE_RX_CTL (0), WRITE_NODE_ID,
WRITE_MEDIUM_MODE, WRITE_IPG0, WRITE_RX_CTL (`SO|AB` = 0x88). PHY autoneg is
started through READ/WRITE_MII_REG, link comes from BMSR bit 2 / the interrupt
EP. The emulation answers all of these.

### Emulated PHY (address 0x10)

| Reg | Value |
|---|---|
| 0 BMCR | 0x3100 (autoneg, 100 Mbit, FD); reset and restart-autoneg bits self-clear (Linux treats link as down while ANRESTART reads 1) |
| 1 BMSR | 0x786D link up, autoneg complete (0x7849 when link down) |
| 2/3 PHYSID | 0x003B / 0x1861 (PHY ID 0x003b1861, AX88772A internal PHY as in Linux `ax88796b.c`) |
| 4 ADVERTISE | 0x01E1 |
| 5 LPA | 0xCDE1 (partner: 100FD) |
| 6 EXPANSION | 0x0001 |

## 4. Bulk packet framing (AX88772 / 772A in non-hardware-checksum mode)

**Device to host (EP 0x82), one or more packets per transfer:**

```
| u16 len | u16 ~len | Ethernet frame (len bytes, no FCS) | pad to even |
```
Header is little endian; `len` low 11 bits are checked by Linux against the low 11
bits of `~len`. We send one frame per transfer, and add a zero-length packet when
the transfer is an exact multiple of the 64-byte packet size.

**Host to device (EP 0x03), as Linux `asix_tx_fixup` does it:**

```
| u16 len | u16 ~len | frame | optional 00 00 FF FF |
```
The trailer (`0xFFFF0000` LE) is added when `(len + 4) % maxpacket == 0` so the
transfer ends with a short packet. The parser also tolerates several packets
back to back and zero-length packets (the trailer itself parses as len 0).

The AX88772B uses a different RX header (hardware checksum info) and Linux needs
a different bind path; not emulated.

## 5. Interrupt endpoint

Linux's `asix_status` reads 8 bytes, `link = byte[2] & 1`. The AX88772C datasheet says the packet also
carries two PHY registers (EEPROM word 0x0F picks them, default 5 and 0) in bytes 4..5 and 6..7. The real chip
answers **every** interrupt poll with the current values, so the firmware keeps a fresh packet queued at all
times. (With a single packet at configuration the Wii went silent after restarting autonegotiation: it appears
to wait for link status on this endpoint.) Bytes 0..1 (`A1 00`) are a guess.

## 6. How the Wii's IOS probably uses it (assumptions)

- IOS has a built-in USB Ethernet driver (a "USBETH" style module reached from the
  network stack), matched on VID/PID. The user's tests show Realtek parts are
  ignored and AX88772-family parts are used (Nintendo's RVL-015 and Plugable
  USB2-E100 are AX88772). A Linksys EtherFast reportedly fails too.
- The driver is probably derived from ASIX's reference driver, which does the same
  register sequence Linux does (see section 3). It likely reads the MAC with
  READ_NODE_ID and uses that MAC as the Wii's own MAC.
- It sets RX control with SO set, and probably accepts broadcast (AB) for DHCP/ARP.
- It does DHCP itself in IOS's lwip-like stack over the bulk pipe.

## 7. Unknowns (honest list)

1. Which VID/PIDs the Wii IOS matches. Assumed `0B95:7720`; fallback `772A`.
2. Whether IOS needs a high-speed device (Pico is full speed only).
3. Exact register sequence/timing in IOS; it may issue requests Linux never does
   (hence the logging and STALL-on-unknown).
4. Whether IOS wants the interrupt EP data in a specific format or polls it.
5. Whether IOS sends multiple packets per bulk OUT, and its padding convention.
6. Whether IOS needs link state to follow real Wi-Fi state or just "always up".
   Default: always up (`WII_LINK_FOLLOWS_WIFI` in config).
7. Whether the Wii accepts a 192.168.7.x gateway network. DHCP gives it a lease
   with router/DNS; games should not care.
8. EP numbers/maxpacket for the interrupt EP and string descriptors are from
   memory of real dumps.
9. DNS: the Wii is told the upstream resolver learned by the Pico's Wi-Fi DHCP
   (8.8.8.8 until known).
10. Power: Pico W needs about 100 to 150 mA in bursts, within the Wii port's 500 mA.
