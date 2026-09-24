/* SPDX-License-Identifier: Apache-2.0 */
/* RNDIS control channel: encapsulated commands over the comm interface's
 * default control pipe, responses fetched after the device notifies us. */
#include "tether.h"

static uint32_t next_request_id = 1;
static pthread_mutex_t ctrl_lock = PTHREAD_MUTEX_INITIALIZER;

/* SEND_ENCAPSULATED_COMMAND — host to device, class request, interface recipient. */
static IOReturn send_encapsulated(const void *msg, uint16_t len)
{
    IOUSBDevRequest req = {
        .bmRequestType = USBmakebmRequestType(kUSBOut, kUSBClass, kUSBInterface),
        .bRequest      = USB_CDC_SEND_ENCAPSULATED_COMMAND,
        .wValue        = 0,
        .wIndex        = 0,          /* IOUSBLib fills in the interface number */
        .wLength       = len,
        .pData         = (void *)msg,
    };
    return (*g_ctrl)->ControlRequest(g_ctrl, 0, &req);
}

/* GET_ENCAPSULATED_RESPONSE — device to host. */
static IOReturn get_encapsulated(void *buf, uint16_t len, uint32_t *got)
{
    IOUSBDevRequest req = {
        .bmRequestType = USBmakebmRequestType(kUSBIn, kUSBClass, kUSBInterface),
        .bRequest      = USB_CDC_GET_ENCAPSULATED_RESPONSE,
        .wValue        = 0,
        .wIndex        = 0,
        .wLength       = len,
        .pData         = buf,
    };
    IOReturn r = (*g_ctrl)->ControlRequest(g_ctrl, 0, &req);
    if (got) *got = req.wLenDone;
    return r;
}

/* One full command/response round trip.
 *
 * Strictly the device raises RESPONSE_AVAILABLE on the comm interface's interrupt
 * endpoint before a response is readable. Polling GET_ENCAPSULATED_RESPONSE is
 * simpler and works on every Android gadget I know of, because u_ether queues the
 * reply synchronously; we retry to absorb the occasional not-ready. */
int rndis_command(const void *out, uint16_t outlen,
                  void *in, uint16_t inlen, uint32_t expect_type)
{
    pthread_mutex_lock(&ctrl_lock);
    IOReturn r = send_encapsulated(out, outlen);
    if (r != kIOReturnSuccess) {
        log_err("SEND_ENCAPSULATED_COMMAND failed: 0x%08x (%s)", r, ioerr(r));
        pthread_mutex_unlock(&ctrl_lock);
        return -1;
    }

    for (int attempt = 0; attempt < 20; attempt++) {
        uint32_t got = 0;
        memset(in, 0, inlen);
        r = get_encapsulated(in, inlen, &got);
        if (r == kIOReturnSuccess && got >= 12) {
            uint32_t type = le32(((uint32_t *)in)[0]);
            if (type == expect_type) { pthread_mutex_unlock(&ctrl_lock); return 0; }
            /* Unsolicited status (link up/down) can arrive here — skip and retry. */
            if (type == RNDIS_MSG_INDICATE) continue;
        }
        usleep(20 * 1000);
    }
    log_err("no %08x response from device", expect_type);
    pthread_mutex_unlock(&ctrl_lock);
    return -1;
}

int rndis_init(uint32_t *dev_max_transfer)
{
    struct rndis_init m = {
        .msg_type          = le32(RNDIS_MSG_INIT),
        .msg_len           = le32(sizeof m),
        .request_id        = le32(next_request_id++),
        .major_version     = le32(1),
        .minor_version     = le32(0),
        .max_transfer_size = le32(RNDIS_MAX_TRANSFER),
    };
    struct rndis_init_c c;
    int ok = -1;
    for (int attempt = 1; attempt <= 3; attempt++) {
        m.request_id = le32(next_request_id++);
        if ((ok = rndis_command(&m, sizeof m, &c, sizeof c, RNDIS_MSG_INIT_C)) == 0) break;
        log_warn("INITIALIZE attempt %d failed; retrying", attempt);
        sleep(1);
    }
    if (ok < 0) {
        log_err("device is not answering encapsulated commands — "
                "retry with -R to force a re-enumeration");
        return -1;
    }
    if (le32(c.status) != RNDIS_STATUS_SUCCESS) {
        log_err("INITIALIZE failed, status 0x%08x", le32(c.status));
        return -1;
    }

    /* The two max_transfer_size values are NOT the same quantity:
     *   ours (in INITIALIZE)   = the most the device may send us per transfer
     *   theirs (in the CMPLT)  = the most the device will accept from us
     * So our RX buffer stays at what we advertised, and their value bounds TX.
     * Clamping the RX buffer to their number would under-size it whenever a
     * device reports less than we advertised, and truncate reads. */
    *dev_max_transfer = le32(c.max_transfer_size);

    log_info("RNDIS v%u.%u  medium %u  rx up to %u (advertised)  tx up to %u (device)  "
             "max_packets/msg %u  align %u",
             le32(c.major_version), le32(c.minor_version), le32(c.medium),
             RNDIS_MAX_TRANSFER, *dev_max_transfer,
             le32(c.max_packets_per_message), 1u << le32(c.packet_alignment));
    return 0;
}

int rndis_query(uint32_t oid, void *out, uint32_t outlen)
{
    struct rndis_query m = {
        .msg_type   = le32(RNDIS_MSG_QUERY),
        .msg_len    = le32(sizeof m),
        .request_id = le32(next_request_id++),
        .oid        = le32(oid),
        .len        = le32(0),
        .offset     = le32(sizeof m - RNDIS_OFFSET_BASE),
        .handle     = le32(0),
    };
    uint8_t buf[1024];
    if (rndis_command(&m, sizeof m, buf, sizeof buf, RNDIS_MSG_QUERY_C) < 0) return -1;

    struct rndis_query_c *c = (struct rndis_query_c *)buf;
    if (le32(c->status) != RNDIS_STATUS_SUCCESS) {
        log_err("QUERY oid 0x%08x failed, status 0x%08x", oid, le32(c->status));
        return -1;
    }
    uint32_t off = le32(c->offset) + RNDIS_OFFSET_BASE;
    uint32_t len = le32(c->len);
    if (off + len > sizeof buf || len < outlen) {
        log_err("QUERY oid 0x%08x: bad reply (offset %u len %u)", oid, off, len);
        return -1;
    }
    memcpy(out, buf + off, outlen);
    return 0;
}

int rndis_set(uint32_t oid, const void *val, uint32_t vallen)
{
    uint8_t msg[sizeof(struct rndis_query) + 64];
    if (vallen > 64) return -1;

    struct rndis_query *m = (struct rndis_query *)msg;
    m->msg_type   = le32(RNDIS_MSG_SET);
    m->msg_len    = le32(sizeof(struct rndis_query) + vallen);
    m->request_id = le32(next_request_id++);
    m->oid        = le32(oid);
    m->len        = le32(vallen);
    m->offset     = le32(sizeof(struct rndis_query) - RNDIS_OFFSET_BASE);
    m->handle     = le32(0);
    memcpy(msg + sizeof(struct rndis_query), val, vallen);

    struct rndis_set_c c;
    if (rndis_command(msg, (uint16_t)(sizeof(struct rndis_query) + vallen),
                      &c, sizeof c, RNDIS_MSG_SET_C) < 0) return -1;
    if (le32(c.status) != RNDIS_STATUS_SUCCESS) {
        log_err("SET oid 0x%08x failed, status 0x%08x", oid, le32(c.status));
        return -1;
    }
    return 0;
}

/* Ask the phone whether its own network link is up. NdisMediaStateConnected
 * is 0; anything else means the phone is not actually tethering, which is the
 * difference between "our driver is wrong" and "turn tethering back on". */
int rndis_media_connected(void)
{
    uint32_t st = 0;
    if (rndis_query(OID_GEN_MEDIA_CONNECT_STATUS, &st, 4) < 0) return -1;
    return le32(st) == 0;
}

/* Re-assert the packet filter. Cheap, and it is what moves the gadget's RNDIS
 * state machine back to DATA_INITIALIZED with the carrier on. */
int rndis_open_filter(void)
{
    uint32_t filter = le32(RNDIS_PACKET_TYPE_DIRECTED | RNDIS_PACKET_TYPE_MULTICAST |
                           RNDIS_PACKET_TYPE_ALL_MULTICAST | RNDIS_PACKET_TYPE_BROADCAST);
    return rndis_set(OID_GEN_CURRENT_PACKET_FILTER, &filter, 4);
}

int rndis_keepalive(void)
{
    struct rndis_keepalive m = {
        .msg_type   = le32(RNDIS_MSG_KEEPALIVE),
        .msg_len    = le32(sizeof m),
        .request_id = le32(next_request_id++),
    };
    struct rndis_keepalive_c c;
    return rndis_command(&m, sizeof m, &c, sizeof c, RNDIS_MSG_KEEPALIVE_C);
}

void rndis_halt(void)
{
    struct rndis_halt m = {
        .msg_type   = le32(RNDIS_MSG_HALT),
        .msg_len    = le32(sizeof m),
        .request_id = le32(next_request_id++),
    };
    send_encapsulated(&m, sizeof m);   /* HALT has no completion message */
}

/* Bring the link up: negotiate, learn the MAC the phone expects us to use,
 * then open the packet filter. Nothing flows until the filter is set — this is
 * the single most common place a from-scratch RNDIS implementation stalls. */
int rndis_bringup(uint8_t mac[6], uint32_t *dev_max_transfer)
{
    if (rndis_init(dev_max_transfer) < 0) return -1;

    if (rndis_query(OID_802_3_PERMANENT_ADDRESS, mac, 6) < 0) {
        log_err("could not read the device MAC");
        return -1;
    }
    log_info("device MAC %02x:%02x:%02x:%02x:%02x:%02x",
             mac[0], mac[1], mac[2], mac[3], mac[4], mac[5]);

    uint32_t mtu = 0;
    if (rndis_query(OID_GEN_MAXIMUM_FRAME_SIZE, &mtu, 4) == 0)
        log_info("max frame size %u", le32(mtu));

    if (rndis_open_filter() < 0) {
        log_err("could not set the packet filter — no traffic will flow");
        return -1;
    }

    int media = rndis_media_connected();
    log_info("packet filter open; phone reports media %s",
             media == 1 ? "CONNECTED" : media == 0 ? "DISCONNECTED (tethering off?)"
                                                   : "status unavailable");
    return 0;
}
