CC      ?= cc
CFLAGS  += -Wall -Wextra -O2 -Wno-deprecated-declarations
LDLIBS  += -framework IOKit -framework CoreFoundation
OBJS     = main.o usb.o rndis.o net.o dhcp.o

all: erg-tetherd ErgTether.app

erg-tetherd: $(OBJS)
	$(CC) $(CFLAGS) -o $@ $(OBJS) $(LDLIBS)

ergtetherbar: menubar/ErgTetherBar.swift
	swiftc -swift-version 5 -O $< -o $@

# A real bundle, not a bare executable: UNUserNotificationCenter needs a bundle
# identifier and traps without one, and LSUIElement is what keeps a menu bar app
# out of the Dock. Ad-hoc signing is enough for notifications to be delivered
# locally; shipping to anyone else would need a Developer ID and notarization.
ErgTether.app: ergtetherbar menubar/Info.plist
	rm -rf $@
	mkdir -p $@/Contents/MacOS
	cp menubar/Info.plist $@/Contents/Info.plist
	cp ergtetherbar $@/Contents/MacOS/ErgTether
	codesign --force --sign - $@

$(OBJS): tether.h rndis.h

clean:
	rm -rf erg-tetherd ergtetherbar ErgTether.app $(OBJS)

.PHONY: all clean

# Install as a system service: the daemon watches for phones and connects on its
# own; the menu bar app starts at login. The binaries go to a root-owned
# directory on purpose -- a LaunchDaemon runs as root, so anyone who can write
# its program can become root.
PREFIX ?= /usr/local/libexec

install: erg-tetherd ErgTether.app
	sudo install -d -o root -g wheel -m 755 $(PREFIX)
	sudo install -o root -g wheel -m 755 erg-tetherd $(PREFIX)/erg-tetherd
	sudo rm -rf $(PREFIX)/ErgTether.app
	sudo cp -R ErgTether.app $(PREFIX)/ErgTether.app
	sudo install -o root -g wheel -m 644 launchd/com.ergtether.daemon.plist \
		/Library/LaunchDaemons/com.ergtether.daemon.plist
	install -m 644 launchd/com.ergtether.bar.plist \
		$(HOME)/Library/LaunchAgents/com.ergtether.bar.plist
	sudo launchctl bootstrap system /Library/LaunchDaemons/com.ergtether.daemon.plist
	launchctl bootstrap gui/$$(id -u) $(HOME)/Library/LaunchAgents/com.ergtether.bar.plist
	@echo "installed — plug in a tethering phone and it should just connect"

uninstall:
	-sudo launchctl bootout system/com.ergtether.daemon
	-launchctl bootout gui/$$(id -u)/com.ergtether.bar
	sudo rm -f /Library/LaunchDaemons/com.ergtether.daemon.plist
	rm -f $(HOME)/Library/LaunchAgents/com.ergtether.bar.plist
	sudo rm -rf $(PREFIX)/erg-tetherd $(PREFIX)/ErgTether.app

pkg: erg-tetherd ErgTether.app
	@packaging/build-pkg.sh

.PHONY: install uninstall pkg
