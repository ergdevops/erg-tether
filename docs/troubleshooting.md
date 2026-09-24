# Troubleshooting

## The phone connects but nothing works

Check **mobile data is on**. Android brings the RNDIS gadget and its netdev up
from the tethering toggle alone, but will not complete tether setup — address,
dnsmasq, NAT — without a usable upstream. The result is a live interface with an
IPv6 link-local address and nothing else, which looks exactly like a driver bug.
Stock Android can sometimes use Wi-Fi as the upstream; Samsung One UI generally
requires cellular.

Also check the **USB mode notification**, not just the Settings toggle. On
Samsung these are different code paths, and the switch can read as on while the
connection stays charge-only.

## Nothing answers on the expected subnet

The tethering subnet is not always `192.168.42.0/24`. Find the phone's real
address by capturing its traffic while the daemon runs:

```sh
sudo tcpdump -i feth9 -n -e 'not port 5353'
```

Look for an IPv4 source from the phone's MAC. The daemon normally discovers this
by itself via DHCP; if that failed, pass the values explicitly with `-a` and `-g`.

## `dhcp-probe.py` reports no OFFER

Confirm the DISCOVER actually reached the wire before believing it:

```sh
sudo tcpdump -i feth9 -n -e -c 1 'udp port 67 or udp port 68'
```

Sending to `255.255.255.255` does **not** reliably leave the interface you chose
— `IP_BOUND_IF` does not override destination routing for the all-ones address,
so the datagram departs via the default route and the probe reports a false
negative.

## Transmit stops after about a dozen frames

The gadget's receive ring drained and was never re-armed, because it is
rejecting the frames being sent. See [protocol.md](protocol.md) — almost always
a zero-length packet or a stray `SET_INTERFACE`.

## `SEND_ENCAPSULATED_COMMAND failed: kIOReturnNotResponding`

The gadget is wedged. Re-enumerate with `erg-tetherd -R`; cycling USB tethering
on the phone alone often will not clear it.

## The interface has no `UP` flag and no address

Something handed it to IPConfiguration, which deconfigures interfaces that are
not network services. See [protocol.md](protocol.md).

## `ping: bad interface name`

`feth9` only exists while the daemon is running.

## `ping: sendto: No route to host` for an outside address

`ping -b` binds the socket to an interface but does not create a route. Add a
host route, or run the daemon with `-r`:

```sh
sudo route add -host 8.8.8.8 <phone address>
```

If the phone's own address is not ARP-resolvable, a host route will not help
either — fix that first.

## Reading the logs

```sh
tail -f /var/log/erg-tether.log       # when installed as a service
sudo erg-tetherd -w -x                # run in the foreground with frame dumps
```
