# ErgTether

Android USB tethering for macOS.

macOS ships host drivers for CDC-ECM and CDC-NCM but has never supported
**RNDIS**, which is what most Android phones still use for USB tethering. Plug a
tethering phone into a Mac and its interfaces enumerate with nothing bound above
them — the connection is simply unavailable. Linux and Windows have supported
this for years.

ErgTether implements RNDIS in userspace and bridges it into the normal network
stack, with a menu bar indicator and automatic hot-plug detection. No kernel
extension, no DriverKit entitlement, no SIP changes.

```
host stack ── feth9 ══peer══ feth10 ── BPF ── erg-tetherd ── USB ── phone
```

## Install

### Homebrew

```sh
brew tap ergdevops/ergtether
brew trust ergdevops/ergtether
brew install --cask ergtether
```

Homebrew refuses to load casks from third-party taps until you trust them, so
the `brew trust` step is required, not optional.

### From source

```sh
git clone https://github.com/ergdevops/erg-tether
cd erg-tether
./install.sh
```

Command Line Tools are enough; Xcode is not required. `./uninstall.sh` removes
everything it touched.

## Usage

Once installed there is nothing to run. Plug in a phone with USB tethering
enabled and it connects on its own, with a notification and a menu bar icon.

The menu bar shows link state, the addresses in use, throughput and uptime, and
offers Pause / Resume.

For manual control:

```sh
erg-tetherd            # connect once, run until the phone is unplugged
erg-tetherd -w         # watch for phones and connect automatically
erg-tetherd -k         # stop the running instance
erg-tetherd -P         # probe: negotiate and report, change nothing
erg-tetherd -h         # all options
```

By default the tether does not become your default route, so it will not disturb
a VPN. To send everything over the phone, use `-r`, or add a route for specific
destinations.

## Requirements

- macOS 13 or later (developed on macOS 26, Apple Silicon)
- An Android phone with USB tethering enabled
- **Mobile data switched on.** Android brings the RNDIS gadget up from the
  tethering toggle alone, but will not finish tether setup — address, DHCP, NAT
  — without a working upstream. Without it the link comes up and stays silent,
  which looks exactly like a bug in this software and is not one.

## How it works

`feth(4)` interfaces come in linked pairs: what one transmits, the other
receives. The host stack gets an address on `feth9` and treats it as an ordinary
Ethernet adapter; the daemon sits on `feth10` with a BPF handle, wrapping and
unwrapping RNDIS frames between it and the phone's USB bulk endpoints.

The tethering subnet is discovered rather than assumed — the daemon sends a DHCP
DISCOVER as a raw Ethernet frame down the USB link and configures itself from
the lease. It is not always `192.168.42.0/24`.

| File | Role |
|---|---|
| `usb.c` | device discovery, interface claiming, bulk pipes |
| `rndis.c` | RNDIS control channel — initialize, query, set, keepalive |
| `dhcp.c` | DHCP client spoken over the USB link |
| `net.c` | the `feth` pair, BPF, host addressing |
| `main.c` | packet pumps, hot-plug supervisor, lifecycle |
| `menubar/` | the menu bar app (Swift) |

## Limitations

- Synchronous bulk I/O on dedicated threads. Fine for cellular speeds; async
  transfers with a submission queue would be the next step for throughput.
- `feth9` is not a macOS *network service*, so it does not appear in System
  Settings, DNS from the lease is not applied, and `-r` does not stick —
  IPMonitor reasserts the primary service's default route. Moving to
  `IOEthernetControllerCreate()` would fix all three.
- The DHCP client is one-shot: it accepts the OFFER but never sends a REQUEST or
  renews. Adequate for a single-client link; not correct DHCP.
- Hot-plug detection polls the IO registry once a second rather than using IOKit
  matching notifications.
- `/var/run/erg-tether.ctl` is world-writable so the unprivileged menu bar app
  can pause tethering. Any local user can therefore pause it — a deliberate
  trade against shipping a passwordless-sudo rule.
- Ad-hoc signed only. Distributing to other machines needs a Developer ID and
  notarization.
- Handles one phone at a time.

## Documentation

- [docs/protocol.md](docs/protocol.md) — RNDIS implementation notes, and the
  non-obvious failures worth knowing before you touch the protocol code
- [docs/troubleshooting.md](docs/troubleshooting.md) — symptoms and what they
  actually mean

## License

MIT — see [LICENSE](LICENSE) and [NOTICE](NOTICE).
