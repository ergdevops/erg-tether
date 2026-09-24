# Implementation notes

Things that are not obvious from the RNDIS specification, and cost real time to
find. Worth reading before changing `rndis.c`, `dhcp.c` or the transmit path.

`drivers/net/usb/rndis_host.c` in the Linux tree is a better reference than
Microsoft's specification — it encodes the device quirks that the spec does not.

## RNDIS basics

- The control channel is **not an endpoint**. Commands go out as
  `SEND_ENCAPSULATED_COMMAND` (`bmRequestType 0x21`, `bRequest 0x00`) on the
  communications interface's default control pipe; replies come back via
  `GET_ENCAPSULATED_RESPONSE` (`0xA1`, `0x01`).
- All `offset` fields in QUERY / SET / PACKET messages are measured **from byte 8**
  of the message, not from its start. A packet header is 44 bytes, so its
  `data_offset` is 36.
- **Nothing flows until `OID_GEN_CURRENT_PACKET_FILTER` is set.** This is the
  most common place a from-scratch implementation stalls: the link negotiates
  cleanly and then stays completely silent.
- The host adopts the MAC from `OID_802_3_PERMANENT_ADDRESS`. Android's gadget
  filters on destination MAC, so using the address it hands you is what Linux
  does too.
- One bulk transfer may carry several chained packet messages. Walk the whole
  buffer rather than assuming one frame per transfer.

## Never send a zero-length packet

USB says a transfer that is an exact multiple of the endpoint's maximum packet
size needs an explicit end-of-transfer marker, and a zero-length packet is the
obvious choice. **It breaks Android's gadget**, with a symptom that points
nowhere near the cause:

- the first 12–14 frames transmit normally
- every `WritePipeTO` afterwards returns `kIOUSBTransactionTimeout`, forever
- the phone never sends a single frame
- the control channel stays perfectly healthy throughout, and the phone
  continues to report `media CONNECTED`

The cause is on the gadget side. `u_ether` re-arms its receive ring from
`process_rx_w()`, which is only queued when `rx_complete()` sees a **successful**
unwrap. A zero-length packet arrives as its own empty transfer, fails
`rndis_rm_hdr()`, and the request returns to the free list without being
re-submitted. The ring drains to empty — roughly a dozen buffers, which is the
frame count — and the endpoint NAKs from then on. Nothing recovers it.

Linux avoids this: `rndis_info` omits `FLAG_SEND_ZLP`, so usbnet appends a single
padding byte instead. `msg_len` already carries the true length, so the extra
byte is ignored by the receiver.

## Never issue SET_INTERFACE

From the same `driver_info`: `FLAG_NO_SETINT`.

Issuing `SET_INTERFACE` on the data interface looks like the natural way to make
the gadget re-run `gether_connect()` and refill that receive ring. It is not. It
wedges Android's gadget badly enough that the control endpoint stops answering
encapsulated commands entirely, and only re-enumeration clears it.
`usb_reset_device()` (`-R`) exists because of this.

## HALT poisons the next session

`RNDIS_MSG_HALT` drops the gadget to `RNDIS_UNINITIALIZED` and calls
`netif_carrier_off()` plus `netif_stop_queue()`. Since the receive ring can only
be refilled from a completion, and completions need queued buffers, an emptied
ring stays dead until the netdev is bounced — so the *next* run wedges after a
frame or two. Linux sends HALT on unbind, but it re-binds cleanly afterwards;
this daemon does not, so HALT is opt-in (`-H`) rather than default.

## macOS specifics

**`ipconfig set <if> DHCP` will not work here.** It only manages interfaces that
are configured *network services*. Point it at a bare `feth` and IPConfiguration
claims the interface, fails, and deconfigures it — leaving it administratively
**down** after you just brought it up, with no error worth reading:

```
feth9:  flags=8802<BROADCAST,SIMPLEX,MULTICAST>          <- down
feth10: flags=8843<UP,BROADCAST,RUNNING,SIMPLEX,MULTICAST>
```

`force_up()` in `net.c` reads the flags back via `SIOCGIFFLAGS` after every
`ifconfig up` rather than trusting the exit code.

**`IP_BOUND_IF` does not override destination routing for 255.255.255.255.** A
DHCP DISCOVER sent through a socket bound to the tether interface departs via the
default route instead and never reaches the phone — silently, reporting a false
negative. This is why `dhcp.c` builds the Ethernet frame by hand and pushes it
out the bulk endpoint: layer 2 has no routing to lose, and it works before the
interface has an address, which is the point.

**`feth` does not implement the IPv6 interface ioctls.** `SIOCGIFINFO_IN6`
returns `EINVAL`, so there is no link-local address to test against.

## The tethering subnet is not fixed

Android usually serves `192.168.42.0/24` with the phone on `.129`. It is not a
guarantee — a Samsung on One UI was observed serving `10.253.124.0/24` with the
phone on `.142`.

Assuming the subnet is the single easiest way to lose an afternoon, because every
symptom mimics a driver fault: ARP goes unanswered, DHCP gets no reply, and the
phone appears mute — when in fact the host is simply on a different network. Hence
DHCP-over-USB at startup, with a fallback that watches for the phone's own IPv4
traffic and takes an address beside it.

## `media CONNECTED` does not mean tethering works

`OID_GEN_MEDIA_CONNECT_STATUS` reflects whether the gadget's netdev is up. It
says nothing about whether Android configured an address on it, started dnsmasq,
or installed NAT rules. A phone with mobile data off reports `CONNECTED` and
serves nothing.
