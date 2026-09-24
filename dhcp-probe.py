#!/usr/bin/env python3
# SPDX-License-Identifier: Apache-2.0
"""Send a DHCP DISCOVER out an interface and report any OFFER.

An ARP probe can only test an address you already guessed. DISCOVER is a
broadcast, so this answers "is anything serving DHCP on this link, and what
subnet does it hand out" without assuming Android's usual 192.168.42.0/24.

    sudo ./dhcp-probe.py feth9
"""
import random
import socket
import struct
import subprocess
import sys
import time

IP_BOUND_IF = 25          # macOS, <netinet/in.h>
MAGIC = 0x63825363


def iface_mac(name):
    out = subprocess.check_output(["/sbin/ifconfig", name], text=True)
    for line in out.splitlines():
        line = line.strip()
        if line.startswith("ether "):
            return bytes.fromhex(line.split()[1].replace(":", ""))
    raise SystemExit(f"{name} has no MAC address")


def iface_broadcast(name):
    """Directed broadcast for the interface's subnet.

    Sending to 255.255.255.255 does NOT reliably leave the interface you want:
    IP_BOUND_IF does not override destination routing for the all-ones address,
    and the datagram silently departs via the default route instead. The subnet
    broadcast has an on-link route through this interface, so it cannot stray.
    """
    out = subprocess.check_output(["/sbin/ifconfig", name], text=True)
    for line in out.splitlines():
        line = line.strip()
        if line.startswith("inet ") and "broadcast" in line:
            parts = line.split()
            return parts[parts.index("broadcast") + 1]
    raise SystemExit(f"{name} has no IPv4 broadcast address -- is the tether running?")


def parse_options(data):
    opts, i = {}, 240
    while i < len(data) and data[i] != 0xFF:
        if data[i] == 0:
            i += 1
            continue
        tag, length = data[i], data[i + 1]
        opts[tag] = data[i + 2:i + 2 + length]
        i += 2 + length
    return opts


def main():
    ifname = sys.argv[1] if len(sys.argv) > 1 else "feth9"
    mac = iface_mac(ifname)

    s = socket.socket(socket.AF_INET, socket.SOCK_DGRAM)
    s.setsockopt(socket.SOL_SOCKET, socket.SO_REUSEADDR, 1)
    s.setsockopt(socket.SOL_SOCKET, socket.SO_BROADCAST, 1)
    s.setsockopt(socket.IPPROTO_IP, IP_BOUND_IF, socket.if_nametoindex(ifname))
    s.bind(("0.0.0.0", 68))

    xid = random.randrange(1 << 32)
    pkt = struct.pack("!BBBBIHHIIII16s64s128sI",
                      1, 1, 6, 0,          # BOOTREQUEST, ethernet, 6-byte MAC
                      xid, 0, 0x8000,      # broadcast flag: reply to everyone
                      0, 0, 0, 0,          # ciaddr yiaddr siaddr giaddr
                      mac + b"\x00" * 10, b"", b"", MAGIC)
    pkt += bytes([53, 1, 1])               # message type: DISCOVER
    pkt += bytes([55, 4, 1, 3, 6, 15])     # request netmask, router, dns, domain
    pkt += b"\xff"

    bcast = iface_broadcast(ifname)
    print(f"DISCOVER out {ifname} to {bcast}:67  "
          f"xid={xid:#010x}  chaddr={mac.hex(':')}")
    s.sendto(pkt, (bcast, 67))

    s.settimeout(1.0)
    deadline = time.time() + 10
    while time.time() < deadline:
        try:
            data, addr = s.recvfrom(2048)
        except socket.timeout:
            continue
        if len(data) < 240 or struct.unpack("!I", data[236:240])[0] != MAGIC:
            continue
        if struct.unpack("!I", data[4:8])[0] != xid:
            continue

        opts = parse_options(data)
        print(f"\nOFFER from {addr[0]}")
        print(f"  your address : {socket.inet_ntoa(data[16:20])}")
        if 1 in opts: print(f"  netmask      : {socket.inet_ntoa(opts[1])}")
        if 3 in opts: print(f"  router       : {socket.inet_ntoa(opts[3][:4])}")
        if 6 in opts:
            dns = [socket.inet_ntoa(opts[6][j:j + 4]) for j in range(0, len(opts[6]), 4)]
            print(f"  dns          : {', '.join(dns)}")
        return 0

    print("\nno OFFER in 10s — nothing is serving DHCP on this link, "
          "so the phone is not running its tethering service")
    return 1


sys.exit(main())
