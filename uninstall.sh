#!/bin/bash
# Remove erg-tetherd and everything it installed.
set -uo pipefail

LIBEXEC=/usr/local/libexec
DAEMON_PLIST=/Library/LaunchDaemons/com.ergtether.daemon.plist
AGENT_PLIST="$HOME/Library/LaunchAgents/com.ergtether.bar.plist"

say()  { printf '  %s\n' "$*"; }
step() { printf '\n==> %s\n' "$*"; }

[ "$(id -u)" -ne 0 ] || { echo "run as yourself, not root" >&2; exit 1; }

step "Stopping services"
sudo launchctl bootout system/com.ergtether.daemon 2>/dev/null && say "daemon stopped"  || say "daemon was not running"
launchctl bootout "gui/$(id -u)/com.ergtether.bar" 2>/dev/null && say "menu bar stopped" || say "menu bar was not running"
sudo "$LIBEXEC/erg-tetherd" -k 2>/dev/null || true
pkill -f "ErgTether.app/Contents/MacOS/ErgTether" 2>/dev/null || true
pkill -f 'erg-tetherd -w'    2>/dev/null || true

step "Removing launchd jobs"
sudo rm -f "$DAEMON_PLIST" && say "removed $DAEMON_PLIST"
rm -f "$AGENT_PLIST"       && say "removed $AGENT_PLIST"

step "Removing binaries"
sudo rm -rf "$LIBEXEC/erg-tetherd" "$LIBEXEC/ErgTether.app"
say "removed from $LIBEXEC"

step "Removing runtime files"
sudo rm -f /var/run/erg-tether.json /var/run/erg-tether.ctl /var/log/erg-tether.log
[ -f /etc/sudoers.d/erg-tetherd ] && sudo rm -f /etc/sudoers.d/erg-tetherd && say "removed the sudoers rule"
say "cleaned"

step "Tearing down leftover interfaces"
# The daemon destroys its feth pair on a clean exit; a kill -9 would not have.
for i in feth9 feth10; do
    if ifconfig "$i" >/dev/null 2>&1; then
        sudo ifconfig "$i" destroy 2>/dev/null && say "destroyed $i"
    fi
done
ifconfig feth9 >/dev/null 2>&1 || say "no leftovers"

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

printf '\nUninstalled. The source tree is untouched.\n'
