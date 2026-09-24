/* erg-tetherd — userspace Android USB tethering for macOS.
 *
 * macOS ships host drivers for CDC-ECM and CDC-NCM but has never had one for
 * RNDIS, which is what most Android phones still use for USB tethering. The
 * interfaces enumerate and then sit there with nothing bound above them.
 *
 * This claims them from userspace via IOUSBLib — no kext, no DriverKit
 * entitlement, no SIP changes — speaks the RNDIS control protocol, and pumps
 * Ethernet frames into a feth(4) pair so the normal network stack sees a real
 * interface it can run DHCP on.
 *
 *   host stack <-- feth9 <==peer==> feth10 <-- BPF --> us <-- USB --> phone
 */
#include "tether.h"

volatile sig_atomic_t g_run = 1;             /* process lifetime */
static volatile sig_atomic_t g_session = 0;  /* one phone's link */
int g_opt_clearstall = 0, g_opt_dump = 0;
static int g_opt_halt = 0;

static int      g_bpf = -1;
static uint32_t g_tx_max = RNDIS_MAX_TRANSFER;   /* most the phone will accept per transfer */
static uint64_t rx_frames, tx_frames, rx_bytes, tx_bytes;
static time_t   g_started;
static char     g_state[16] = "starting";
static char     g_devmac[24] = "";
volatile uint32_t g_peer_ip = 0;
static volatile uint32_t g_lease_addr = 0, g_lease_router = 0, g_lease_mask = 0;
static uint32_t g_dhcp_xid = 0;

/* ---- logging ---- */
static void vlog(const char *tag, const char *fmt, va_list ap)
{
    struct timeval tv; gettimeofday(&tv, NULL);
    struct tm tm; localtime_r(&tv.tv_sec, &tm);
    char ts[16]; strftime(ts, sizeof ts, "%H:%M:%S", &tm);
    fprintf(stderr, "%s.%03d %s ", ts, (int)(tv.tv_usec / 1000), tag);
    vfprintf(stderr, fmt, ap);
    fputc('\n', stderr);
}
void log_info(const char *fmt, ...) { va_list a; va_start(a, fmt); vlog("   ", fmt, a); va_end(a); }
void log_warn(const char *fmt, ...) { va_list a; va_start(a, fmt); vlog("warn", fmt, a); va_end(a); }
void log_err (const char *fmt, ...) { va_list a; va_start(a, fmt); vlog("ERR ", fmt, a); va_end(a); }

static void hexdump(const char *tag, const uint8_t *b, size_t n)
{
    fprintf(stderr, "        %s (%zu bytes)\n", tag, n);
    for (size_t i = 0; i < n; i += 16) {
        fprintf(stderr, "          %04zx  ", i);
        for (size_t j = 0; j < 16 && i + j < n; j++) fprintf(stderr, "%02x ", b[i + j]);
        fputc('\n', stderr);
    }
}

/* Learn the phone's IPv4 address by watching its own traffic.
 *
 * The tethering subnet is NOT reliably 192.168.42.0/24 -- a Samsung on One UI
 * served 10.253.124.0/24. Guessing it makes a correctly working driver look
 * broken in every visible way: ARP goes unanswered, DHCP gets no reply, and the
 * phone appears mute, because the host is simply on another network. So watch
 * for the first real IPv4 source from the phone and adopt its subnet. */
static void learn_peer(const uint8_t *f, uint32_t len)
{
    if (g_peer_ip || len < 28) return;

    uint16_t ethertype = (uint16_t)((f[12] << 8) | f[13]);
    uint32_t src = 0;

    if (ethertype == 0x0800 && len >= 34)
        memcpy(&src, f + 26, 4);                  /* IPv4 source address */
    else if (ethertype == 0x0806 && len >= 42)
        memcpy(&src, f + 28, 4);                  /* ARP sender protocol address */
    else
        return;

    uint32_t h = ntohl(src);
    uint8_t  a = (uint8_t)(h >> 24);
    if (h == 0 || a == 0 || a >= 224) return;     /* unspecified, multicast, broadcast */
    if (a == 169 && ((h >> 16) & 0xff) == 254) return;   /* IPv4 link-local */

    g_peer_ip = src;
}

static void on_signal(int sig) { (void)sig; g_run = 0; }

/* One Ethernet frame out the bulk endpoint, wrapped in an RNDIS packet header.
 * Shared by the transmit pump and the DHCP client, hence the lock. */
static pthread_mutex_t tx_lock = PTHREAD_MUTEX_INITIALIZER;

static int usb_send_frame(const uint8_t *frame, uint32_t flen)
{
    uint8_t msg[sizeof(struct rndis_packet) + 2048 + 8];
    if (flen < 14 || flen > 2048) return -1;

    struct rndis_packet *h = (struct rndis_packet *)msg;
    memset(h, 0, sizeof *h);
    h->msg_type    = le32(RNDIS_MSG_PACKET);
    h->msg_len     = le32(sizeof *h + flen);
    h->data_offset = le32(sizeof *h - RNDIS_OFFSET_BASE);
    h->data_len    = le32(flen);
    memcpy(msg + sizeof *h, frame, flen);

    uint32_t total = sizeof *h + flen;
    if (total > g_tx_max) return -1;

    /* Pad rather than send a ZLP -- see the note in the README. */
    if (g_maxpkt_out && total % g_maxpkt_out == 0) { msg[total] = 0; total++; }

    pthread_mutex_lock(&tx_lock);
    IOReturn r = (*g_data)->WritePipeTO(g_data, g_pipe_out, msg, total, 0, 500);
    pthread_mutex_unlock(&tx_lock);

    if (r == kIOUSBPipeStalled) {
        (*g_data)->ClearPipeStallBothEnds(g_data, g_pipe_out);
        return -1;
    }
    return r == kIOReturnSuccess ? 0 : -1;
}

/* ---- phone -> host ----
 * One bulk transfer can carry several chained RNDIS_MSG_PACKET messages, so walk
 * the whole buffer rather than assuming one frame per transfer. */
static void *rx_thread(void *arg)
{
    (void)arg;
    uint8_t *buf = malloc(RNDIS_MAX_TRANSFER);
    if (!buf) { log_err("rx buffer alloc failed"); g_session = 0; return NULL; }

    while (g_session) {
        UInt32 n = RNDIS_MAX_TRANSFER;
        IOReturn r = (*g_data)->ReadPipeTO(g_data, g_pipe_in, buf, &n, 0, 1000);

        if (r == kIOReturnTimeout || r == kIOUSBTransactionTimeout) continue;
        if (r == kIOUSBPipeStalled) {
            log_warn("bulk IN stalled, clearing");
            (*g_data)->ClearPipeStallBothEnds(g_data, g_pipe_in);
            continue;
        }
        if (r == kIOReturnNotResponding || r == kIOReturnNoDevice) {
            log_err("device went away");
            g_session = 0;
            break;
        }
        if (r != kIOReturnSuccess) { log_warn("ReadPipeTO: 0x%08x", r); continue; }

        uint32_t off = 0;
        while (off + sizeof(struct rndis_packet) <= n) {
            struct rndis_packet *p = (struct rndis_packet *)(buf + off);
            uint32_t type = le32(p->msg_type);
            uint32_t mlen = le32(p->msg_len);

            if (type != RNDIS_MSG_PACKET) {
                log_warn("unexpected message 0x%08x on the data pipe", type);
                break;
            }
            if (mlen < sizeof(struct rndis_packet) || off + mlen > n) {
                log_warn("truncated packet message (len %u, %u left)", mlen, n - off);
                break;
            }

            uint32_t doff = le32(p->data_offset) + RNDIS_OFFSET_BASE;
            uint32_t dlen = le32(p->data_len);
            if (g_opt_dump && rx_frames < 6)
                hexdump("rx rndis message head", buf + off, mlen < 108 ? mlen : 108);
            if (doff + dlen <= mlen && dlen >= 14) {
                learn_peer(buf + off + doff, dlen);
                if (!g_lease_addr) {
                    uint32_t y = 0, rt = 0, mk = 0;
                    if (dhcp_parse_reply(buf + off + doff, dlen, g_dhcp_xid, &y, &rt, &mk) && y) {
                        g_lease_addr = y; g_lease_router = rt; g_lease_mask = mk;
                    }
                }
                if (write(g_bpf, buf + off + doff, dlen) < 0) {
                    if (errno != ENOBUFS) log_warn("bpf write: %s", strerror(errno));
                } else {
                    rx_frames++; rx_bytes += dlen;
                }
            }
            off += mlen;
        }
    }
    free(buf);
    return NULL;
}

/* ---- host -> phone ----
 * BPF hands us a batch of records, each with a bpf_hdr prefix and word-aligned
 * stride. Wrap each frame in a 44-byte RNDIS packet header and ship it. */
/* The bulk OUT endpoint NAKs when the gadget has nothing queued to receive
 * into. Re-issuing SET_INTERFACE makes it run gether_connect() again and refill
 * its receive ring; re-asserting the filter puts the RNDIS state machine back
 * in DATA_INITIALIZED. Ask the phone about its link first, so the log says
 * which side is at fault. */
static void tx_recover(void)
{
    int media = rndis_media_connected();
    log_warn("bulk OUT is not draining after %llu frames; phone reports media %s",
             (unsigned long long)tx_frames,
             media == 1 ? "CONNECTED" : media == 0 ? "DISCONNECTED — is tethering still on?"
                                                   : "status unavailable");
    if (tx_frames < 5)
        log_warn("wedged almost immediately — the gadget's receive ring was "
                 "already empty. Restart with -R to rebuild it.");
    if (g_opt_clearstall) (*g_data)->ClearPipeStallBothEnds(g_data, g_pipe_out);
    rndis_open_filter();
}

static void *tx_thread(void *arg)
{
    (void)arg;
    int stuck = 0, dumped = 0;
    uint8_t *bbuf = malloc(BPF_BUFLEN);
    uint8_t *msg  = malloc(sizeof(struct rndis_packet) + 2048 + 8);
    if (!bbuf || !msg) {
        log_err("tx buffer alloc failed");
        free(bbuf); free(msg);       /* either one may have succeeded */
        g_session = 0;
        return NULL;
    }

    while (g_session) {
        ssize_t n = read(g_bpf, bbuf, BPF_BUFLEN);
        if (n <= 0) {
            if (n < 0 && errno != EINTR && errno != EAGAIN)
                log_warn("bpf read: %s", strerror(errno));
            continue;
        }

        uint8_t *p = bbuf, *end = bbuf + n;
        while (p + sizeof(struct bpf_hdr) <= end) {
            struct bpf_hdr *bh = (struct bpf_hdr *)p;
            uint8_t *frame = p + bh->bh_hdrlen;
            uint32_t flen  = bh->bh_caplen;

            if (flen >= 14 && flen <= 2048 && frame + flen <= end) {
                struct rndis_packet *h = (struct rndis_packet *)msg;
                memset(h, 0, sizeof *h);
                h->msg_type    = le32(RNDIS_MSG_PACKET);
                h->msg_len     = le32(sizeof *h + flen);
                h->data_offset = le32(sizeof *h - RNDIS_OFFSET_BASE);
                h->data_len    = le32(flen);
                memcpy(msg + sizeof *h, frame, flen);

                uint32_t total = sizeof *h + flen;
                if (total > g_tx_max) { log_warn("frame %u too big for device", flen); goto next; }

                /* A transfer that lands exactly on a packet boundary needs an
                 * explicit end-of-transfer marker. usbnet does NOT send a ZLP for
                 * RNDIS (rndis_info omits FLAG_SEND_ZLP); it appends one padding
                 * byte instead, because msg_len already carries the true length.
                 * A ZLP arrives at the gadget as its own empty transfer, fails
                 * rndis_rm_hdr(), and stops u_ether re-arming its receive ring. */
                if (g_maxpkt_out && total % g_maxpkt_out == 0) {
                    msg[total] = 0;
                    total++;
                }

                if (g_opt_dump && dumped < 8) {
                    log_info("tx frame %d: bpf caplen %u datalen %u, usb transfer %u",
                             dumped, bh->bh_caplen, bh->bh_datalen, total);
                    hexdump("rndis message head", msg, total < 108 ? total : 108);
                    dumped++;
                }
                pthread_mutex_lock(&tx_lock);
                IOReturn r = (*g_data)->WritePipeTO(g_data, g_pipe_out, msg, total, 0, 500);
                pthread_mutex_unlock(&tx_lock);
                if (r == kIOUSBPipeStalled) {
                    log_warn("bulk OUT stalled, clearing");
                    (*g_data)->ClearPipeStallBothEnds(g_data, g_pipe_out);
                    stuck = 0;
                } else if (r != kIOReturnSuccess) {
                    /* A NAKing OUT endpoint means the gadget has no receive
                     * buffers queued. Re-arm it rather than logging forever. */
                    if (++stuck == 4) tx_recover();
                    else if (stuck == 1) log_warn("WritePipeTO: 0x%08x", r);
                } else {
                    stuck = 0;
                    tx_frames++; tx_bytes += flen;
                }
            }
        next:
            p += BPF_WORDALIGN(bh->bh_hdrlen + bh->bh_caplen);
        }
    }
    free(bbuf); free(msg);
    return NULL;
}

static void *keepalive_thread(void *arg)
{
    (void)arg;
    int last_media = 1;
    while (g_session) {
        for (int i = 0; i < KEEPALIVE_SECS * 10 && g_session; i++) usleep(100 * 1000);
        if (!g_session) break;
        if (rndis_keepalive() < 0) {
            log_err("keepalive failed — link is gone");
            g_session = 0;
            break;
        }
        int media = rndis_media_connected();
        if (media != last_media) {
            log_info("phone media state -> %s",
                     media == 1 ? "CONNECTED" : media == 0 ? "DISCONNECTED" : "unknown");
            last_media = media;
        }
    }
    return NULL;
}

/* Publish state for the menu bar app. Written atomically so a reader never
 * sees a half-written file. */
static void status_write(void)
{
    char tmp[] = STATUS_PATH ".XXXXXX";
    int fd = mkstemp(tmp);
    if (fd < 0) return;

    FILE *f = fdopen(fd, "w");
    if (!f) { close(fd); unlink(tmp); return; }

    fprintf(f,
        "{\n"
        "  \"state\": \"%s\",\n"
        "  \"pid\": %d,\n"
        "  \"iface\": \"%s\",\n"
        "  \"device_mac\": \"%s\",\n"
        "  \"address\": \"%s\",\n"
        "  \"gateway\": \"%s\",\n"
        "  \"rx_frames\": %llu,\n"
        "  \"rx_bytes\": %llu,\n"
        "  \"tx_frames\": %llu,\n"
        "  \"tx_bytes\": %llu,\n"
        "  \"since\": %lld\n"
        "}\n",
        g_state, (int)getpid(), IF_HOST, g_devmac,
        g_host_addr, g_phone_gw,
        (unsigned long long)rx_frames, (unsigned long long)rx_bytes,
        (unsigned long long)tx_frames, (unsigned long long)tx_bytes,
        (long long)g_started);
    fclose(f);
    chmod(tmp, 0644);                 /* the menu bar app runs as the user */
    rename(tmp, STATUS_PATH);
}

/* Unprivileged control channel.
 *
 * The menu bar app runs as the user and must be able to pause and resume
 * tethering. Shelling out to sudo would mean shipping a passwordless-root rule
 * for a cosmetic button, so instead the daemon exposes a world-writable command
 * file and polls it. The worst a local user can do with it is turn their own
 * tethering off, which is a far better trade than handing out root. */
static int g_paused = 0;

static void control_init(void)
{
    int fd = open(CONTROL_PATH, O_CREAT | O_TRUNC | O_WRONLY, 0666);
    if (fd >= 0) { fchmod(fd, 0666); close(fd); }
}

static void control_poll(void)
{
    FILE *f = fopen(CONTROL_PATH, "r");
    if (!f) return;
    char cmd[32] = "";
    char *got = fgets(cmd, sizeof cmd, f);
    fclose(f);
    if (!got || !cmd[0]) return;

    if (!strncmp(cmd, "stop", 4)) {
        if (!g_paused) log_info("paused by request");
        g_paused = 1;
    } else if (!strncmp(cmd, "start", 5)) {
        if (g_paused) log_info("resumed by request");
        g_paused = 0;
    }
    if (truncate(CONTROL_PATH, 0) != 0) unlink(CONTROL_PATH);
}

static void usage(const char *argv0)
{
    fprintf(stderr,
        "usage: sudo %s [-v VID] [-p PID] [-P]\n"
        "  -v VID   USB vendor id  (default: autodetect any RNDIS device)\n"
        "  -p PID   USB product id (default: autodetect)\n"
        "  -a ADDR  host address on the tether link (default 192.168.42.100)\n"
        "  -g ADDR  the phone's address / gateway (default 192.168.42.129)\n"
        "  -P       probe only: negotiate RNDIS, print details, exit\n"
        "  -r       also move the default route to the phone (restored on exit)\n"
        "  -C       clear endpoint stalls / reset data toggles at startup\n"
        "  -R       re-enumerate the device first (recovers a wedged gadget)\n"
        "  -k       stop the running instance\n"
        "  -w       watch for phones and connect automatically (daemon mode)\n"
        "  -x       hex-dump the first few frames in each direction\n"
        "  -H       send RNDIS HALT on exit (degrades the next run; off by default)\n", argv0);
}


/* ---- one link, start to finish ----
 *
 * Split out from main() so the watcher can bring a link up and down repeatedly
 * as phones are plugged and unplugged, without restarting the process. */
static pthread_t th_rx, th_tx, th_ka;
static int session_live = 0;

static void log_counters(void)
{
    static uint64_t last_rx, last_tx;
    if (rx_frames == last_rx && tx_frames == last_tx) return;
    log_info("rx %llu frames / %llu KiB   tx %llu frames / %llu KiB",
             (unsigned long long)rx_frames, (unsigned long long)rx_bytes / 1024,
             (unsigned long long)tx_frames, (unsigned long long)tx_bytes / 1024);
    last_rx = rx_frames; last_tx = tx_frames;
}

static int session_start(uint16_t vid, uint16_t pid, int want_route, int addr_given)
{
    rx_frames = tx_frames = rx_bytes = tx_bytes = 0;
    g_peer_ip = 0; g_lease_addr = g_lease_router = g_lease_mask = 0;
    if (!addr_given) { g_host_addr = STATIC_ADDR; g_phone_gw = PHONE_GW; }
    snprintf(g_state, sizeof g_state, "connecting");
    status_write();

    uint8_t mac[6];
    if (usb_open(vid, pid) < 0) return -1;
    if (rndis_bringup(mac, &g_tx_max) < 0) return -1;

    g_started = time(NULL);
    snprintf(g_devmac, sizeof g_devmac, "%02x:%02x:%02x:%02x:%02x:%02x",
             mac[0], mac[1], mac[2], mac[3], mac[4], mac[5]);

    if (net_create(mac) < 0) return -1;
    if ((g_bpf = bpf_open()) < 0) return -1;

    g_session = 1;
    session_live = 1;
    pthread_create(&th_rx, NULL, rx_thread, NULL);
    pthread_create(&th_tx, NULL, tx_thread, NULL);
    pthread_create(&th_ka, NULL, keepalive_thread, NULL);

    static char host_buf[INET_ADDRSTRLEN], gw_buf[INET_ADDRSTRLEN];
    if (!addr_given) {
        uint8_t frame[512];
        g_dhcp_xid = (uint32_t)(time(NULL) ^ (getpid() << 16));
        log_info("discovering the tethering subnet (DHCP over the USB link) ...");

        for (int i = 0; i < 40 && g_session && !g_lease_addr && !g_peer_ip; i++) {
            if (i % 8 == 0) {
                uint32_t n = dhcp_build_discover(frame, mac, g_dhcp_xid);
                usb_send_frame(frame, n);
            }
            usleep(250 * 1000);
        }

        if (g_lease_addr) {
            struct in_addr a = { .s_addr = g_lease_addr };
            struct in_addr g = { .s_addr = g_lease_router ? g_lease_router : g_peer_ip };
            snprintf(host_buf, sizeof host_buf, "%s", inet_ntoa(a));
            snprintf(gw_buf, sizeof gw_buf, "%s", inet_ntoa(g));
            g_host_addr = host_buf; g_phone_gw = gw_buf;
            log_info("DHCP lease: address %s, gateway %s", g_host_addr, g_phone_gw);
        } else if (g_peer_ip) {
            /* No DHCP answer, but the phone told us its subnet by talking. Take
             * an address next to it; on a point-to-point USB link we are the
             * only other host. */
            uint32_t h = ntohl(g_peer_ip);
            uint8_t last = (uint8_t)(h & 0xff);
            snprintf(gw_buf, sizeof gw_buf, "%u.%u.%u.%u",
                     (h >> 24) & 0xff, (h >> 16) & 0xff, (h >> 8) & 0xff, last);
            snprintf(host_buf, sizeof host_buf, "%u.%u.%u.%u",
                     (h >> 24) & 0xff, (h >> 16) & 0xff, (h >> 8) & 0xff,
                     last == 100 ? 101 : 100);
            g_host_addr = host_buf; g_phone_gw = gw_buf;
            log_info("learned the phone at %s from its own traffic; taking %s",
                     g_phone_gw, g_host_addr);
        } else {
            log_warn("could not discover the subnet; falling back to %s/%s",
                     g_host_addr, g_phone_gw);
        }
    }

    if (!g_session) return -1;                 /* device vanished mid-bring-up */
    if (net_configure(want_route) < 0) { snprintf(g_state, sizeof g_state, "error"); return -1; }

    snprintf(g_state, sizeof g_state, "up");
    log_info("tethering is up on " IF_HOST);
    return 0;
}

static void session_stop(void)
{
    if (session_live) {
        snprintf(g_state, sizeof g_state, "stopping");
        status_write();
        g_session = 0;
        pthread_join(th_rx, NULL);
        pthread_join(th_tx, NULL);
        pthread_join(th_ka, NULL);
        session_live = 0;
    }
    if (g_bpf >= 0) { close(g_bpf); g_bpf = -1; }
    net_destroy();
    /* Deliberately NOT halting by default. RNDIS_MSG_HALT drops the gadget to
     * RNDIS_UNINITIALIZED and calls netif_carrier_off() + netif_stop_queue().
     * u_ether's receive ring can only be refilled from a completion, and
     * completions need queued buffers -- so an emptied ring stays dead until the
     * netdev is bounced, and the NEXT run wedges after a frame or two. */
    if (g_opt_halt) rndis_halt();
    usb_close();
    snprintf(g_state, sizeof g_state, "waiting");
}

int main(int argc, char **argv)
{
    uint16_t vid = 0x04e8, pid = 0x6863;
    int probe_only = 0, want_route = 0, do_reset = 0, addr_given = 0;
    int do_kill = 0, watch = 0, dev_given = 0, c;

    while ((c = getopt(argc, argv, "v:p:a:g:PrCRxHkwh")) != -1) {
        switch (c) {
        case 'v': vid = (uint16_t)strtoul(optarg, NULL, 0); dev_given = 1; break;
        case 'p': pid = (uint16_t)strtoul(optarg, NULL, 0); dev_given = 1; break;
        case 'P': probe_only = 1; break;
        case 'r': want_route = 1; break;
        case 'C': g_opt_clearstall = 1; break;
        case 'R': do_reset = 1; break;
        case 'x': g_opt_dump = 1; break;
        case 'H': g_opt_halt = 1; break;
        case 'k': do_kill = 1; break;
        case 'w': watch = 1; break;
        case 'a': g_host_addr = optarg; addr_given = 1; break;
        case 'g': g_phone_gw  = optarg; break;
        default:  usage(argv[0]); return 2;
        }
    }

    /* -k: stop a running instance. The menu bar app is unprivileged, so it
     * cannot signal the daemon directly; it re-invokes this binary, which the
     * sudoers rule already permits. */
    if (do_kill) {
        FILE *f = fopen(STATUS_PATH, "r");
        if (!f) { fprintf(stderr, "not running\n"); return 1; }
        char line[256]; int pid = 0;
        while (fgets(line, sizeof line, f))
            if (sscanf(line, " \"pid\": %d", &pid) == 1) break;
        fclose(f);
        if (pid <= 0) { fprintf(stderr, "no pid in " STATUS_PATH "\n"); return 1; }
        if (kill(pid, SIGINT) != 0) { perror("kill"); return 1; }
        fprintf(stderr, "stopped pid %d\n", pid);
        return 0;
    }

    if (geteuid() != 0) {
        log_err("must run as root (claiming USB interfaces, BPF, and ifconfig)");
        return 1;
    }

    signal(SIGINT,  on_signal);
    signal(SIGTERM, on_signal);
    signal(SIGPIPE, SIG_IGN);

    if (do_reset && usb_reset_device(vid, pid) < 0) return 1;

    if (probe_only) {
        if (!dev_given && usb_find_rndis(&vid, &pid) == 0)
            log_info("found an RNDIS device at %04x:%04x", vid, pid);
        if (usb_open(vid, pid) < 0) return 1;
        uint8_t mac[6];
        if (rndis_bringup(mac, &g_tx_max) < 0) { usb_close(); return 1; }
        /* Deliberately NOT sending HALT. On the gadget, RNDIS_MSG_HALT drops the
         * session to RNDIS_UNINITIALIZED and calls netif_carrier_off() plus
         * netif_stop_queue(). A probe is supposed to be free of side effects, and
         * leaving the phone halted poisons whatever runs next. */
        log_info("probe complete — RNDIS negotiated, session left up, "
                 "no network changes made");
        usb_close();
        return 0;
    }

    if (watch) {
        log_info("watching for an Android RNDIS device ...");
        snprintf(g_state, sizeof g_state, "waiting");
        control_init();
        status_write();

        int tick = 0, backoff = 0;
        while (g_run) {
            /* Presence is always decided by a live registry query; -v/-p only
             * override which ids we then open. */
            uint16_t dv = 0, dp = 0;
            int present = (usb_find_rndis(&dv, &dp) == 0);
            if (dev_given) { dv = vid; dp = pid; }

            control_poll();
            if (g_paused) {
                if (session_live) { log_info("stopping — paused"); session_stop(); }
                snprintf(g_state, sizeof g_state, "paused");
                status_write();
                sleep(1);
                continue;
            }

            if (!session_live && present && backoff == 0) {
                log_info("phone attached (%04x:%04x) — connecting", dv, dp);
                if (session_start(dv, dp, want_route, addr_given) < 0) {
                    session_stop();
                    backoff = 3;   /* a gadget that is not ready yet: do not hammer it */
                }
            } else if (session_live && (!present || !g_session)) {
                log_info(present ? "link dropped — reconnecting" : "phone detached");
                session_stop();
                backoff = 2;
            }
            if (backoff > 0) backoff--;

            status_write();
            sleep(1);
            if (session_live && ++tick % 5 == 0) log_counters();
        }
        session_stop();
        unlink(STATUS_PATH);
        unlink(CONTROL_PATH);
        log_info("done");
        return 0;
    }

    /* One-shot: require the device now, run until it goes away or we are told to stop. */
    if (!dev_given && usb_find_rndis(&vid, &pid) == 0)
        log_info("found an RNDIS device at %04x:%04x", vid, pid);
    if (session_start(vid, pid, want_route, addr_given) < 0) { session_stop(); return 1; }

    int tick = 0;
    while (g_run && g_session) {
        sleep(1);
        if (!g_run) break;
        status_write();
        if (++tick % 5 == 0) log_counters();
    }

    log_info("shutting down");
    session_stop();
    unlink(STATUS_PATH);
    log_info("done — rx %llu frames, tx %llu frames",
             (unsigned long long)rx_frames, (unsigned long long)tx_frames);
    return 0;
}
