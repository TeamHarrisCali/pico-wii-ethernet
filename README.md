# pico-wii-ethernet

> **AI-generated project, tested by me.** The firmware, documentation and this README were written by an AI
> assistant (Claude, by Anthropic). I ran it on real hardware (a Raspberry Pi Pico W and an original Nintendo Wii)
> and tested it myself, and the results below are what I saw. It has not been reviewed or tested beyond that, so
> expect rough edges, and use it at your own risk. Not affiliated with or endorsed by Nintendo or ASIX.

Raspberry Pi Pico W as a Wi-Fi to USB-Ethernet adapter for an original Nintendo Wii.
To the Wii it looks like an ASIX AX88772 USB adapter (`0B95:7720`, what Nintendo's LAN
adapter uses). Behind that the Pico is a tiny router: DHCP server for the Wii, NAT, and
a Wi-Fi client toward your network.

Status: **works on a real Wii (system menu 4.3U).** The Wii's connection test passes through the Pico, with the Pico
on a phone hotspot (WPA2). On a Linux PC the `asix` driver binds, DHCP gives 192.168.7.2 and NAT carries ping, DNS
and HTTP. Games, the Wii Shop and the system update check have not been tried. Design, protocol details and
the list of unknowns: [docs/DESIGN.md](docs/DESIGN.md). What the Wii's driver actually does is in the same file.

## Build

Needs `arm-none-eabi-gcc`, `cmake`, `ninja`, and a pico-sdk checkout with submodules
(`git clone --depth 1 --recurse-submodules --shallow-submodules https://github.com/raspberrypi/pico-sdk ~/pico-sdk`).

```bash
cp config/wifi_config.h.example config/wifi_config.h   # then edit it (see below)
BUILD_DIR=build-wii ./build.sh
```

Result: `build-wii/pico_wii_ethernet.uf2`. `PICO_SDK_PATH` defaults to `~/pico-sdk`.

Bench build for a PC (adds a USB serial port with the log; do not use on the Wii). On this build,
opening the serial port at 1200 baud (set 1200 baud with Python `termios`; `stty` can hang on this port) reboots the Pico into BOOTSEL, so you can
reflash without touching the button:

```bash
BUILD_DIR=build ./build.sh -DWII_ETH_DEBUG_CDC=ON
```

## Config

Edit `config/wifi_config.h` (local only, never commit or share it; it is git-ignored):

- `WIFI_SSID`, `WIFI_PASSWORD`, `WIFI_AUTH`: 2.4 GHz network the Pico joins. A phone hotspot works.
- `WIFI_COUNTRY`: default USA.
- `USB_NET_*`: the private Pico/Wii network (default 192.168.7.0/24). Must differ from your Wi-Fi's subnet.
- `WII_LINK_FOLLOWS_WIFI`: 0 = the Wii always sees "cable plugged in" (default).
- `LOG_UDP_DEST` / `LOG_UDP_PORT`: where debug lines are sent (default: broadcast, port 5140).

The file currently in the folder is a placeholder with no network. Rebuild after every change.

## Flash

1. Unplug the Pico, hold BOOTSEL, plug it in, release. It shows up as a USB drive `RPI-RP2`.
2. Copy `build-wii/pico_wii_ethernet.uf2` onto it (or `picotool load -x build-wii/pico_wii_ethernet.uf2`).
3. It reboots by itself. The LED blinks while it tries Wi-Fi and stays on once it has an address.

## Test on a Wii

1. On a PC on the same Wi-Fi network (a phone hotspot works; many guest networks isolate clients and block the log), start the log listener:
   `python3 tools/udplog.py` (or `nc -u -l 5140`, `socat -u UDP-RECV:5140 -`). The Pico sends its log there once Wi-Fi is up,
   including everything that happened before.
2. Plug the Pico into a Wii USB port (the rear ports are the safe choice; the Wii may need to be
   powered on first or after, try both).
3. Wait for the LED to stay on (Wi-Fi up; the Pico ignores the Wii's DHCP requests until then). Wii Menu: Wii Options, Wii Settings, page 2, Internet, Connection Settings, pick a connection,
   choose **Wired Connection** (it only appears if the Wii recognised an adapter), Save, then run the connection test.
4. Report back: what the Wii said (error code or success), and the log. Lines to look for:
   - `SETUP ... vendor <NAME>`: every USB request the Wii made. `UNKNOWN ... STALL` lines are
     requests the firmware could not answer: those are what to implement next.
   - `dhcp: DISCOVER` / `REQUEST`: the Wii reached the DHCP stage.
   - `usb: host->pico N frames`, `nat: ... out/in`: traffic counters, printed every 10 s.

No Wii-side software or SD card changes are needed.

Debug without a Wi-Fi network: UART0 on GP0 (TX) / GP1 (RX), 115200 8N1, or the bench build
and `cat /dev/ttyACM0`.

## Check on a PC

With the bench build: `lsusb` shows `0b95:7720 ASIX Electronics Corp. AX88772`, and the `asix`
module should bind and create an `enx...` interface (`ip -br link`). `dmesg | grep -i asix` shows it.

## Layout

| File | Purpose |
|---|---|
| `src/ax88772.c` | USB class driver: vendor requests, emulated PHY, bulk framing, SETUP log |
| `src/usb_descriptors.c` | AX88772 descriptors |
| `src/usbnet.c` | lwIP interface that faces the Wii |
| `src/dhcpd.c` | one-client DHCP server |
| `src/nat.c` | NAPT (TCP, UDP, ICMP echo) as an lwIP input hook |
| `src/dbglog.c` | log ring buffer to UART, CDC and UDP |
| `src/main.c` | Wi-Fi join/retry, main loop |

## Limits

Full-speed USB (about 1 MB/s at best), IPv4 only, no IP fragments, no ICMP error translation,
128 NAT entries, one Wii. Wi-Fi is 2.4 GHz only.

## Reaching the Wii from the network

By default (`WII_FORWARD_ALL_PORTS 1`) every inbound TCP or UDP connection that is not a reply to something the Wii sent
is forwarded to the Wii, on the same port number. So an FTP server on the Wii (for example `ftpii`) or `wiiload` is
reachable at the **Pico's Wi-Fi IP** (printed in the log as `wifi ip ...`), e.g. `ftp 192.168.1.50`. Limits: the Wii
keeps a private address, so it is not a true bridge; broadcast/multicast discovery does not cross; ICMP ping to the
Pico's IP is answered by the Pico, not the Wii; the Pico's own DHCP client port (UDP 68) is not forwarded. Anyone on the
Wi-Fi network can reach the Wii, so turn this off (`0`) on networks you do not trust. Note some Wi-Fi networks isolate
clients from each other, which blocks this regardless of the Pico.

## Notes from bring-up

- The Wii's USB driver reads the adapter's EEPROM (words 0 to 0x15) for the MAC (words 4..6) and the PHY address
  (word 0x11, must be 0x10), resets the chip, restarts autonegotiation, then waits for link status on the
  interrupt endpoint, which must answer every poll. All of that is emulated; see DESIGN.md.
- Homebrew (cIOS) apps use a different Wii driver path than the System Menu: it resets the chip Linux-style and
  then polls PHY register 2 until it reads the real AX88772 PHY ID (0x003b1861). With a made-up ID it looped forever;
  with the real one the Homebrew Browser downloads over the Pico.
- Wii error 52220 on a public guest network: the Wii's NAT-type test sends UDP to GameSpy servers (69.10.30.x ports 27900/27901)
  and gets no reply on that network (same from a PC there), so some guest networks fail the test. The phone hotspot passes.
- Wii error 52120 appeared when the Wii's test ran before the Pico's Wi-Fi was up. The Pico now holds back DHCP
  until Wi-Fi has an address and retries a failed join after 3 seconds.
- `tools/udplog.py` prints the UDP log. In a bench build (`-DWII_ETH_DEBUG_CDC=ON`) you can read the same log on `/dev/ttyACM0`.
- A vendor request `0x7e` with wValue `0xB007` reboots the Pico into BOOTSEL (handy for flashing without the button).
