#!/bin/bash
# Install erg-tetherd as a system service.
#
# The daemon watches for an Android RNDIS gadget and connects on its own; the
# menu bar app starts at login. After this, plugging in a tethering phone should
# just work, the way it does on Linux and Windows.
set -euo pipefail

LIBEXEC=/usr/local/libexec
DAEMON_PLIST=/Library/LaunchDaemons/com.ergtether.daemon.plist
AGENT_PLIST="$HOME/Library/LaunchAgents/com.ergtether.bar.plist"
SRC="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"

say()  { printf '  %s\n' "$*"; }
step() { printf '\n==> %s\n' "$*"; }
die()  { printf '\nerror: %s\n' "$*" >&2; exit 1; }

[ "$(uname -s)" = "Darwin" ] || die "macOS only"
[ "$(id -u)" -ne 0 ] || die "run as yourself, not root — the script will sudo where it needs to"

step "Building"
if [ -f "$SRC/Makefile" ]; then
    command -v cc      >/dev/null || die "cc not found — install the Command Line Tools: xcode-select --install"
    command -v swiftc  >/dev/null || die "swiftc not found — install the Command Line Tools: xcode-select --install"
    make -C "$SRC" >/dev/null
    say "built erg-tetherd and ErgTether.app"
else
    say "no Makefile; using the prebuilt binaries beside this script"
fi
[ -x "$SRC/erg-tetherd" ] || die "erg-tetherd is missing"
[ -d "$SRC/ErgTether.app" ] || die "ErgTether.app is missing"

step "Stopping anything already running"
sudo launchctl bootout system/com.ergtether.daemon        2>/dev/null || true
launchctl bootout "gui/$(id -u)/com.ergtether.bar"        2>/dev/null || true
sudo "$SRC/erg-tetherd" -k                                 2>/dev/null || true
pkill -f "ErgTether.app/Contents/MacOS/ErgTether"            2>/dev/null || true

step "Installing binaries to $LIBEXEC"
# Root-owned on purpose: a LaunchDaemon runs as root, so anyone who can write
# its program can become root.
sudo install -d -o root -g wheel -m 755 "$LIBEXEC"
sudo install -o root -g wheel -m 755 "$SRC/erg-tetherd" "$LIBEXEC/erg-tetherd"
sudo rm -rf "$LIBEXEC/ErgTether.app"
sudo cp -R "$SRC/ErgTether.app" "$LIBEXEC/ErgTether.app"
sudo chown -R root:wheel "$LIBEXEC/ErgTether.app"
say "installed"

step "Installing launchd jobs"
sudo install -o root -g wheel -m 644 "$SRC/launchd/com.ergtether.daemon.plist" "$DAEMON_PLIST"
mkdir -p "$(dirname "$AGENT_PLIST")"
install -m 644 "$SRC/launchd/com.ergtether.bar.plist" "$AGENT_PLIST"
sudo launchctl bootstrap system "$DAEMON_PLIST"
launchctl bootstrap "gui/$(id -u)" "$AGENT_PLIST"
say "daemon:  com.ergtether.daemon (root, starts at boot)"
say "menubar: com.ergtether.bar (starts at login)"

step "Removing artifacts from the pre-rename name"
for f in /etc/sudoers.d/rndis-tether /etc/sudoers.d/erg-tether; do
    [ -f "$f" ] && sudo rm -f "$f" && say "removed $f"
done
sudo rm -f /var/run/rndis-tether.json /var/run/rndis-tether.ctl /var/log/rndis-tether.log
sudo launchctl bootout system/com.local.rndis-tether 2>/dev/null && say "unloaded the old daemon job" || true
launchctl bootout "gui/$(id -u)/com.local.tetherbar" 2>/dev/null && say "unloaded the old menu bar job" || true
sudo rm -f /Library/LaunchDaemons/com.local.rndis-tether.plist
rm -f "$HOME/Library/LaunchAgents/com.local.tetherbar.plist"
sudo rm -rf /usr/local/libexec/rndis-tether /usr/local/libexec/TetherBar.app
say "done"

step "Removing the development sudoers rule, if present"
# Earlier versions needed passwordless sudo so the menu bar could stop the
# daemon. It now uses an unprivileged control file instead, so this is dead
# weight and worth taking back.
if [ -f /etc/sudoers.d/erg-tetherd ]; then
    sudo rm -f /etc/sudoers.d/erg-tetherd
    say "removed /etc/sudoers.d/erg-tetherd — no longer needed"
else
    say "none present"
fi

step "Checking"
sleep 3
if sudo launchctl print system/com.ergtether.daemon >/dev/null 2>&1; then
    say "daemon is loaded"
else
    die "the daemon did not load — see /var/log/erg-tether.log"
fi
if [ -f /var/run/erg-tether.json ]; then
    say "status: $(/usr/bin/python3 -c 'import json;print(json.load(open("/var/run/erg-tether.json"))["state"])' 2>/dev/null || echo unknown)"
fi

cat <<'EOF'

Done. macOS will ask once for permission to show notifications — allow it if you
want connect/disconnect banners. Plug in an Android phone with USB tethering enabled and it will connect
by itself. Watch the menu bar icon, or:

  tail -f /var/log/erg-tether.log

Note: Android will not complete tether setup without a working upstream, so the
phone needs mobile data on. Uninstall with ./uninstall.sh
EOF
