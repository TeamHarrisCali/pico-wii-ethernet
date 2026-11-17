#!/usr/bin/env python3
"""Print the Pico's UDP debug log (port 5140). Run on a PC on the same Wi-Fi network as the Pico."""
import socket, sys

port = int(sys.argv[1]) if len(sys.argv) > 1 else 5140
s = socket.socket(socket.AF_INET, socket.SOCK_DGRAM)
s.setsockopt(socket.SOL_SOCKET, socket.SO_REUSEADDR, 1)
s.bind(("", port))
print(f"listening on UDP {port}, Ctrl-C to stop", flush=True)
while True:
    data, addr = s.recvfrom(2048)
    sys.stdout.write(data.decode(errors="replace"))
    sys.stdout.flush()
