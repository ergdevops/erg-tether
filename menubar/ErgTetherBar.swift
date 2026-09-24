// SPDX-License-Identifier: Apache-2.0
// TetherBar — a menu bar extra for erg-tetherd.
//
// macOS has no notification-area affordance for USB tethering the way Windows
// and Linux do, because it has never supported the protocol. This supplies one:
// a status item that reflects link state at a glance and can start and stop the
// daemon.
//
// The daemon runs as root and publishes /var/run/erg-tether.json once a
// second; this app is unprivileged and only reads it. Start and stop are done by
// re-invoking the daemon binary under sudo, which the sudoers rule permits.

import AppKit
import Foundation
import UserNotifications

/// The daemon lives next to this binary. The path must be absolute and must
/// match the sudoers rule exactly, so resolve it rather than guessing.
let toolPath: String = {
    if let override = ProcessInfo.processInfo.environment["RNDIS_TETHER"] { return override }
    let exe = Bundle.main.executablePath
        ?? URL(fileURLWithPath: CommandLine.arguments[0]).path
    return URL(fileURLWithPath: exe)
        .resolvingSymlinksInPath()
        .deletingLastPathComponent()
        .appendingPathComponent("erg-tetherd")
        .path
}()
let statusPath = "/var/run/erg-tether.json"
let controlPath = "/var/run/erg-tether.ctl"

struct Status: Decodable {
    let state: String
    let pid: Int
    let iface: String
    let device_mac: String
    let address: String
    let gateway: String
    let rx_frames: UInt64
    let rx_bytes: UInt64
    let tx_frames: UInt64
    let tx_bytes: UInt64
    let since: Int64
}

func humanBytes(_ n: UInt64) -> String {
    let units = ["B", "KiB", "MiB", "GiB"]
    var v = Double(n), i = 0
    while v >= 1024 && i < units.count - 1 { v /= 1024; i += 1 }
    return i == 0 ? "\(n) B" : String(format: "%.1f %@", v, units[i])
}

func humanRate(_ bytesPerSec: Double) -> String {
    let bits = bytesPerSec * 8
    if bits >= 1_000_000 { return String(format: "%.1f Mbps", bits / 1_000_000) }
    if bits >= 1_000     { return String(format: "%.0f kbps", bits / 1_000) }
    return String(format: "%.0f bps", bits)
}

func humanDuration(_ seconds: Int) -> String {
    let h = seconds / 3600, m = (seconds % 3600) / 60, s = seconds % 60
    if h > 0 { return String(format: "%dh %02dm", h, m) }
    if m > 0 { return String(format: "%dm %02ds", m, s) }
    return "\(s)s"
}

/// Connect/disconnect notifications.
///
/// UNUserNotificationCenter requires a bundle identifier and will trap without
/// one, so a bare executable cannot use it — hence the .app bundle. When we are
/// not bundled (running the raw binary during development) we fall back to
/// osascript, which works from anywhere.
final class Notifier {
    static let shared = Notifier()
    private var bundled = Bundle.main.bundleIdentifier != nil
    private var authorized = false

    func requestAuthorization() {
        guard bundled else { return }
        UNUserNotificationCenter.current()
            .requestAuthorization(options: [.alert, .sound]) { ok, _ in
                DispatchQueue.main.async { self.authorized = ok }
            }
    }

    func post(_ title: String, _ body: String) {
        guard UserDefaults.standard.object(forKey: "notifications") == nil
                || UserDefaults.standard.bool(forKey: "notifications") else { return }

        if bundled && authorized {
            let content = UNMutableNotificationContent()
            content.title = title
            content.body = body
            content.sound = .default
            UNUserNotificationCenter.current().add(
                UNNotificationRequest(identifier: UUID().uuidString,
                                      content: content, trigger: nil))
        } else {
            // Quoting matters: the text contains IP addresses and device names.
            let esc = { (s: String) in s.replacingOccurrences(of: "\"", with: "\\\"") }
            let script = "display notification \"\(esc(body))\" with title \"\(esc(title))\""
            let task = Process()
            task.executableURL = URL(fileURLWithPath: "/usr/bin/osascript")
            task.arguments = ["-e", script]
            try? task.run()
        }
    }
}

final class ErgTetherBar: NSObject, NSApplicationDelegate, UNUserNotificationCenterDelegate {
    private let item = NSStatusBar.system.statusItem(withLength: NSStatusItem.variableLength)
    private let menu = NSMenu()
    private var timer: Timer?

    private var last: Status?
    private var lastSampleTime = Date()
    private var rxRate = 0.0, txRate = 0.0
    private var busy = false
    private var lastState: String?          // nil until the first poll, so launching is silent

    // Info rows are rebuilt each tick; actions are created once so the menu
    // does not flicker while it is open.
    private let statusRow  = NSMenuItem(title: "—", action: nil, keyEquivalent: "")
    private let addressRow = NSMenuItem(title: "", action: nil, keyEquivalent: "")
    private let gatewayRow = NSMenuItem(title: "", action: nil, keyEquivalent: "")
    private let trafficRow = NSMenuItem(title: "", action: nil, keyEquivalent: "")
    private let rateRow    = NSMenuItem(title: "", action: nil, keyEquivalent: "")
    private let uptimeRow  = NSMenuItem(title: "", action: nil, keyEquivalent: "")
    private let toggleRow  = NSMenuItem(title: "Connect", action: #selector(toggle),
                                        keyEquivalent: "")
    private let notifyRow  = NSMenuItem(title: "Notify on connect / disconnect",
                                        action: #selector(toggleNotifications), keyEquivalent: "")

    private var notificationsEnabled: Bool {
        UserDefaults.standard.object(forKey: "notifications") == nil
            ? true : UserDefaults.standard.bool(forKey: "notifications")
    }

    func applicationDidFinishLaunching(_ note: Notification) {
        if Bundle.main.bundleIdentifier != nil {
            UNUserNotificationCenter.current().delegate = self
        }
        Notifier.shared.requestAuthorization()

        for row in [statusRow, addressRow, gatewayRow, trafficRow, rateRow, uptimeRow] {
            row.isEnabled = false
            menu.addItem(row)
        }
        menu.addItem(.separator())
        toggleRow.target = self
        menu.addItem(toggleRow)
        menu.addItem(.separator())
        notifyRow.target = self
        notifyRow.state = notificationsEnabled ? .on : .off
        menu.addItem(notifyRow)
        menu.addItem(.separator())
        let quit = NSMenuItem(title: "Quit ErgTether", action: #selector(quit), keyEquivalent: "q")
        quit.target = self
        menu.addItem(quit)
        item.menu = menu

        refresh()
        timer = Timer.scheduledTimer(withTimeInterval: 1.0, repeats: true) { [weak self] _ in
            self?.refresh()
        }
    }

    private func readStatus() -> Status? {
        guard let data = FileManager.default.contents(atPath: statusPath) else { return nil }
        return try? JSONDecoder().decode(Status.self, from: data)
    }

    private func refresh() {
        let now = readStatus()

        // Derive throughput from the byte deltas between samples.
        if let n = now, let p = last, n.pid == p.pid {
            let dt = Date().timeIntervalSince(lastSampleTime)
            if dt > 0.2 {
                rxRate = Double(n.rx_bytes &- p.rx_bytes) / dt
                txRate = Double(n.tx_bytes &- p.tx_bytes) / dt
                lastSampleTime = Date()
                last = n
            }
        } else {
            rxRate = 0; txRate = 0
            lastSampleTime = Date()
            last = now
        }

        let state = now?.state ?? "offline"
        announce(state, now)
        let up = state == "up"
        let symbol: String
        switch state {
        case "up":                    symbol = "iphone.radiowaves.left.and.right"
        case "connecting":            symbol = "iphone.badge.play"
        case "waiting", "paused":     symbol = "iphone"
        default:                      symbol = "iphone.slash"
        }
        if let img = NSImage(systemSymbolName: symbol, accessibilityDescription: "USB tethering") {
            img.isTemplate = true
            item.button?.image = img
            item.button?.title = ""
        } else {
            item.button?.title = up ? "USB↕" : "USB"
        }
        item.button?.appearsDisabled = !up
        item.button?.toolTip = up
            ? "USB tethering active on \(now!.iface) — \(now!.address)"
            : "USB tethering: \(describe(state))"

        if let s = now, s.state == "up" {
            statusRow.title  = "Status: Connected"
            addressRow.title = "Address: \(s.address) (\(s.iface))"
            gatewayRow.title = "Phone: \(s.gateway)"
            trafficRow.title = "Traffic: ↓ \(humanBytes(s.rx_bytes))  ↑ \(humanBytes(s.tx_bytes))"
            rateRow.title    = "Rate: ↓ \(humanRate(rxRate))  ↑ \(humanRate(txRate))"
            let secs = max(0, Int(Date().timeIntervalSince1970) - Int(s.since))
            uptimeRow.title  = "Up: \(humanDuration(secs))"
            for row in [addressRow, gatewayRow, trafficRow, rateRow, uptimeRow] {
                row.isHidden = false
            }
            toggleRow.title = "Pause tethering"
        } else {
            statusRow.title = "Status: \(describe(state))"
            for row in [addressRow, gatewayRow, trafficRow, rateRow, uptimeRow] {
                row.isHidden = true
            }
            toggleRow.title = state == "paused" ? "Resume tethering" : "Pause tethering"
            toggleRow.isHidden = (now == nil)   // no daemon: nothing to control
        }
        toggleRow.isEnabled = !busy
    }

    /// Pause and resume go through the daemon's control file, not sudo. The app
    /// stays unprivileged and the install needs no sudoers rule.
    @objc private func toggle() {
        guard !busy, let s = readStatus() else { return }
        let paused = s.state == "paused"
        busy = true

        do {
            try (paused ? "start\n" : "stop\n")
                .write(toFile: controlPath, atomically: false, encoding: .utf8)
        } catch {
            busy = false
            warn("Could not write \(controlPath): \(error.localizedDescription)")
            return
        }

        // The daemon polls once a second; give it a beat before re-reading.
        DispatchQueue.main.asyncAfter(deadline: .now() + 1.5) { [weak self] in
            self?.busy = false
            self?.refresh()
        }
    }

    private func describe(_ state: String) -> String {
        switch state {
        case "waiting":    return "Waiting for a phone"
        case "connecting": return "Connecting…"
        case "paused":     return "Paused"
        case "stopping":   return "Stopping…"
        case "error":      return "Error"
        default:           return "Service not running"
        }
    }

    private func warn(_ text: String) {
        let a = NSAlert()
        a.messageText = "Could not change tethering state"
        a.informativeText = text
        a.alertStyle = .warning
        a.runModal()
    }

    /// Fire a notification when the link comes up or goes away. Only real
    /// transitions count, and a pause the user asked for is not news.
    private func announce(_ state: String, _ status: Status?) {
        defer { lastState = state }
        guard let previous = lastState, previous != state else { return }

        if state == "up", let s = status {
            Notifier.shared.post("USB tethering connected",
                                 "\(s.address) on \(s.iface) — phone at \(s.gateway)")
        } else if previous == "up" {
            switch state {
            case "waiting":  Notifier.shared.post("USB tethering disconnected", "Phone unplugged.")
            case "error":    Notifier.shared.post("USB tethering disconnected", "The link failed.")
            case "offline":  Notifier.shared.post("USB tethering disconnected", "The service stopped.")
            case "paused":   break          // the user just did this; they know
            case "stopping": break          // transient, the next state is the real one
            default:         Notifier.shared.post("USB tethering disconnected", state)
            }
        }
    }

    @objc private func toggleNotifications() {
        let on = !notificationsEnabled
        UserDefaults.standard.set(on, forKey: "notifications")
        notifyRow.state = on ? .on : .off
    }

    /// A menu bar app counts as foreground, so without this the banner is
    /// suppressed exactly when it is most useful.
    func userNotificationCenter(_ center: UNUserNotificationCenter,
                                willPresent notification: UNNotification,
                                withCompletionHandler handler:
                                    @escaping (UNNotificationPresentationOptions) -> Void) {
        handler([.banner, .sound])
    }

    @objc private func quit() { NSApplication.shared.terminate(nil) }
}

let app = NSApplication.shared
let delegate = ErgTetherBar()
app.delegate = delegate
app.setActivationPolicy(.accessory)      // menu bar only, no Dock icon
app.run()
