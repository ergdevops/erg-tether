/* SPDX-License-Identifier: Apache-2.0 */
/* USB transport: find the phone's RNDIS interfaces and claim them with IOUSBLib. */
#include "tether.h"

static IOUSBDeviceInterface  **dev_iface  = NULL;
IOUSBInterfaceInterface190   **g_ctrl     = NULL;   /* comm interface, class 0xE0 */
IOUSBInterfaceInterface190   **g_data     = NULL;   /* data interface, class 0x0A */
uint8_t  g_pipe_in = 0, g_pipe_out = 0;
uint16_t g_maxpkt_out = 512;

/* The handful of IOReturn codes this driver actually produces. Hex alone makes
 * the log unreadable when you are bisecting a wedged gadget. */
const char *ioerr(IOReturn r)
{
    switch (r) {
    case kIOReturnSuccess:         return "success";
    case kIOReturnNotResponding:   return "device not responding";
    case kIOReturnNoDevice:        return "no device";
    case kIOReturnExclusiveAccess: return "exclusive access held by another client";
    case kIOReturnNotOpen:         return "interface not open";
    case kIOReturnBadArgument:     return "bad argument";
    case kIOReturnAborted:         return "aborted";
    case kIOReturnTimeout:         return "timeout";
    case kIOUSBTransactionTimeout: return "USB transaction timeout (device NAKing)";
    case kIOUSBPipeStalled:        return "pipe stalled";
    case kIOUSBTransactionReturned:return "transaction returned";
    case kIOUSBNoAsyncPortErr:     return "no async port";
    default:                       return "unknown";
    }
}

/* Wrap a raw io_service_t in the matching IOUSBLib COM-style interface. */
static void *plugin_for(io_service_t svc, CFUUIDRef type, CFUUIDRef uuid)
{
    IOCFPlugInInterface **plug = NULL;
    SInt32 score = 0;
    void *result = NULL;

    if (IOCreatePlugInInterfaceForService(svc, type, kIOCFPlugInInterfaceID,
                                          &plug, &score) != kIOReturnSuccess || !plug)
        return NULL;

    (*plug)->QueryInterface(plug, CFUUIDGetUUIDBytes(uuid), &result);
    (*plug)->Release(plug);
    return result;
}

/* Find any attached device exposing an RNDIS control interface.
 *
 * Matching on the interface class triple rather than a vendor/product id is what
 * makes hot-plug detection work for arbitrary phones: every Android RNDIS gadget
 * presents 0xE0/0x01/0x03 regardless of who made it. */
int usb_find_rndis(uint16_t *vid, uint16_t *pid)
{
    CFMutableDictionaryRef match = IOServiceMatching(kIOUSBInterfaceClassName);
    if (!match) return -1;

    int cls = 0xE0, sub = 0x01, proto = 0x03;
    CFNumberRef n;
    n = CFNumberCreate(NULL, kCFNumberIntType, &cls);
    CFDictionarySetValue(match, CFSTR(kUSBInterfaceClass), n);    CFRelease(n);
    n = CFNumberCreate(NULL, kCFNumberIntType, &sub);
    CFDictionarySetValue(match, CFSTR(kUSBInterfaceSubClass), n); CFRelease(n);
    n = CFNumberCreate(NULL, kCFNumberIntType, &proto);
    CFDictionarySetValue(match, CFSTR(kUSBInterfaceProtocol), n); CFRelease(n);

    io_iterator_t it = 0;
    if (IOServiceGetMatchingServices(kIOMainPortDefault, match, &it) != kIOReturnSuccess)
        return -1;
    io_service_t intf = IOIteratorNext(it);
    IOObjectRelease(it);
    if (!intf) return -1;

    /* The interface's ids live on the device above it; the composite-device nub
     * sits in between, so search up the parent chain rather than one level. */
    const uint32_t up = kIORegistryIterateRecursively | kIORegistryIterateParents;
    CFTypeRef cv = IORegistryEntrySearchCFProperty(intf, kIOServicePlane,
                                                   CFSTR(kUSBVendorID),  NULL, up);
    CFTypeRef cp = IORegistryEntrySearchCFProperty(intf, kIOServicePlane,
                                                   CFSTR(kUSBProductID), NULL, up);
    IOObjectRelease(intf);

    int v = 0, p = 0;
    if (cv) { CFNumberGetValue(cv, kCFNumberIntType, &v); CFRelease(cv); }
    if (cp) { CFNumberGetValue(cp, kCFNumberIntType, &p); CFRelease(cp); }
    if (!v || !p) return -1;

    *vid = (uint16_t)v;
    *pid = (uint16_t)p;
    return 0;
}

/* Find the device by id and hand back an opened IOUSBDeviceInterface. */
static IOUSBDeviceInterface **find_device(uint16_t vid, uint16_t pid)
{
    CFMutableDictionaryRef match = IOServiceMatching(kIOUSBDeviceClassName);
    if (!match) return NULL;

    CFNumberRef n;
    int v = vid, p = pid;
    n = CFNumberCreate(NULL, kCFNumberIntType, &v);
    CFDictionarySetValue(match, CFSTR(kUSBVendorID), n);  CFRelease(n);
    n = CFNumberCreate(NULL, kCFNumberIntType, &p);
    CFDictionarySetValue(match, CFSTR(kUSBProductID), n); CFRelease(n);

    io_iterator_t it = 0;
    if (IOServiceGetMatchingServices(kIOMainPortDefault, match, &it) != kIOReturnSuccess)
        return NULL;
    io_service_t svc = IOIteratorNext(it);
    IOObjectRelease(it);
    if (!svc) return NULL;

    IOUSBDeviceInterface **d =
        plugin_for(svc, kIOUSBDeviceUserClientTypeID, kIOUSBDeviceInterfaceID);
    IOObjectRelease(svc);
    return d;
}

/* Force a re-enumeration — the programmatic equivalent of unplugging the cable.
 * Use it when the gadget has stopped answering encapsulated commands; an RNDIS
 * state machine that has been knocked over does not recover on its own, and on
 * Android a tethering toggle alone often does not rebuild it. */
int usb_reset_device(uint16_t vid, uint16_t pid)
{
    IOUSBDeviceInterface **d = find_device(vid, pid);
    if (!d) { log_err("no USB device %04x:%04x to reset", vid, pid); return -1; }

    IOReturn r = (*d)->USBDeviceOpen(d);
    if (r == kIOReturnExclusiveAccess) r = (*d)->USBDeviceOpenSeize(d);
    if (r != kIOReturnSuccess) {
        log_err("USBDeviceOpen: 0x%08x (%s)", r, ioerr(r));
        (*d)->Release(d);
        return -1;
    }

    log_info("re-enumerating the device ...");
    r = (*d)->USBDeviceReEnumerate(d, 0);
    if (r != kIOReturnSuccess) log_warn("USBDeviceReEnumerate: 0x%08x (%s)", r, ioerr(r));

    /* Our handles die with the old IOService; the device comes back as a new one. */
    (*d)->Release(d);

    for (int i = 0; i < 20; i++) {
        usleep(500 * 1000);
        IOUSBDeviceInterface **again = find_device(vid, pid);
        if (again) {
            (*again)->Release(again);
            log_info("device is back after %.1fs", (i + 1) * 0.5);
            sleep(1);            /* let the interfaces finish matching */
            return 0;
        }
    }
    log_err("device did not come back after re-enumeration");
    return -1;
}

int usb_open(uint16_t vid, uint16_t pid)
{
    CFMutableDictionaryRef match = IOServiceMatching(kIOUSBDeviceClassName);
    if (!match) { log_err("IOServiceMatching failed"); return -1; }

    CFNumberRef n;
    int v = vid, p = pid;
    n = CFNumberCreate(NULL, kCFNumberIntType, &v);
    CFDictionarySetValue(match, CFSTR(kUSBVendorID), n);  CFRelease(n);
    n = CFNumberCreate(NULL, kCFNumberIntType, &p);
    CFDictionarySetValue(match, CFSTR(kUSBProductID), n); CFRelease(n);

    io_iterator_t it = 0;
    if (IOServiceGetMatchingServices(kIOMainPortDefault, match, &it) != kIOReturnSuccess) {
        log_err("IOServiceGetMatchingServices failed"); return -1;
    }

    io_service_t svc = IOIteratorNext(it);
    IOObjectRelease(it);
    if (!svc) {
        log_err("no USB device %04x:%04x — is the phone plugged in?", vid, pid);
        return -1;
    }

    dev_iface = plugin_for(svc, kIOUSBDeviceUserClientTypeID, kIOUSBDeviceInterfaceID);
    IOObjectRelease(svc);
    if (!dev_iface) { log_err("could not build IOUSBDeviceInterface"); return -1; }

    /* Walk every interface in the active configuration and pick out the RNDIS pair.
     * We never call SetConfiguration: the phone already has config 1 active, and
     * re-setting it would bounce any other client attached to the device. */
    IOUSBFindInterfaceRequest req = {
        .bInterfaceClass    = kIOUSBFindInterfaceDontCare,
        .bInterfaceSubClass = kIOUSBFindInterfaceDontCare,
        .bInterfaceProtocol = kIOUSBFindInterfaceDontCare,
        .bAlternateSetting  = kIOUSBFindInterfaceDontCare,
    };
    if ((*dev_iface)->CreateInterfaceIterator(dev_iface, &req, &it) != kIOReturnSuccess) {
        log_err("CreateInterfaceIterator failed"); return -1;
    }

    io_service_t usbif;
    while ((usbif = IOIteratorNext(it))) {
        IOUSBInterfaceInterface190 **ii =
            plugin_for(usbif, kIOUSBInterfaceUserClientTypeID, kIOUSBInterfaceInterfaceID190);
        IOObjectRelease(usbif);
        if (!ii) continue;

        uint8_t cls = 0, sub = 0, proto = 0, num = 0;
        (*ii)->GetInterfaceClass(ii, &cls);
        (*ii)->GetInterfaceSubClass(ii, &sub);
        (*ii)->GetInterfaceProtocol(ii, &proto);
        (*ii)->GetInterfaceNumber(ii, &num);

        /* Android's gadget presents 0xE0/0x01/0x03 (Wireless, RF, RNDIS) for control
         * and 0x0A (CDC Data) for data. Some ROMs ship RNDIS-over-CDC as 0x02/0x02/0xFF,
         * so accept that shape for the control interface too. */
        int is_ctrl = (cls == 0xE0 && sub == 0x01 && proto == 0x03) ||
                      (cls == 0x02 && sub == 0x02 && proto == 0xFF);
        int is_data = (cls == 0x0A);

        if (is_ctrl && !g_ctrl) {
            g_ctrl = ii;
            log_info("control interface %u  class %02x/%02x/%02x", num, cls, sub, proto);
            continue;
        }
        if (is_data && !g_data) {
            g_data = ii;
            log_info("data    interface %u  class %02x/%02x/%02x", num, cls, sub, proto);
            continue;
        }
        (*ii)->Release(ii);
    }
    IOObjectRelease(it);

    if (!g_ctrl || !g_data) {
        log_err("device is not exposing an RNDIS interface pair — "
                "enable USB tethering on the phone and retry");
        return -1;
    }

    /* Claim both. Nothing on macOS binds class 0xE0, so a plain open should win;
     * Seize is the fallback in case something else (EDR agent, browser WebUSB)
     * got there first. */
    for (int i = 0; i < 2; i++) {
        IOUSBInterfaceInterface190 **ii = i ? g_data : g_ctrl;
        IOReturn r = (*ii)->USBInterfaceOpen(ii);
        if (r == kIOReturnExclusiveAccess)
            r = (*ii)->USBInterfaceOpenSeize(ii);
        if (r != kIOReturnSuccess) {
            log_err("USBInterfaceOpen(%s) failed: 0x%08x (%s)",
                    i ? "data" : "control", r, ioerr(r));
            return -1;
        }
    }

    /* Issue SET_INTERFACE on the data interface.
     *
     * On the gadget side this runs rndis_set_alt() -> gether_connect(), which
     * allocates and queues the receive requests on the bulk OUT endpoint. The
     * composite framework calls set_alt(0) once at SET_CONFIGURATION time, so a
     * fresh device arrives with a dozen or so buffers already queued; drain
     * those without re-arming and every subsequent write NAKs forever, which
     * shows up here as kIOUSBTransactionTimeout. It also resets the endpoint
     * data toggles, which we want after claiming an interface we did not
     * enumerate ourselves. */
    /* Locate the bulk IN/OUT pair on the data interface. Pipe 0 is always the
     * default control pipe, so endpoints start at 1.
     *
     * Note we deliberately do NOT issue SET_INTERFACE here. It looks like the
     * right way to make the gadget re-run gether_connect() and re-arm its
     * receive ring, but Linux sets FLAG_NO_SETINT for RNDIS devices, and in
     * practice it wedges Android's gadget so hard that the control endpoint
     * stops answering encapsulated commands until the device is re-enumerated. */
    uint8_t nep = 0;
    (*g_data)->GetNumEndpoints(g_data, &nep);
    for (uint8_t pipe = 1; pipe <= nep; pipe++) {
        uint8_t dir, epnum, tt, interval;
        uint16_t maxpkt;
        if ((*g_data)->GetPipeProperties(g_data, pipe, &dir, &epnum, &tt,
                                         &maxpkt, &interval) != kIOReturnSuccess)
            continue;
        if (tt != kUSBBulk) continue;
        if (dir == kUSBIn  && !g_pipe_in)  {
            g_pipe_in = pipe;
            log_info("bulk IN  pipe %u  ep %u  maxpacket %u", pipe, epnum, maxpkt);
        }
        if (dir == kUSBOut && !g_pipe_out) {
            g_pipe_out = pipe; g_maxpkt_out = maxpkt;
            log_info("bulk OUT pipe %u  ep %u  maxpacket %u", pipe, epnum, maxpkt);
        }
    }
    if (!g_pipe_in || !g_pipe_out) { log_err("no bulk endpoint pair found"); return -1; }

    if (g_opt_clearstall) {
        log_info("clearing endpoint stalls / resetting data toggles");
        (*g_data)->ClearPipeStallBothEnds(g_data, g_pipe_in);
        (*g_data)->ClearPipeStallBothEnds(g_data, g_pipe_out);
    }
    return 0;
}

void usb_close(void)
{
    if (g_data) { (*g_data)->USBInterfaceClose(g_data); (*g_data)->Release(g_data); g_data = NULL; }
    if (g_ctrl) { (*g_ctrl)->USBInterfaceClose(g_ctrl); (*g_ctrl)->Release(g_ctrl); g_ctrl = NULL; }
    if (dev_iface) { (*dev_iface)->Release(dev_iface); dev_iface = NULL; }
}
