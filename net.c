/* SPDX-License-Identifier: Apache-2.0 */
/* Host-side plumbing: a feth(4) pair plus a BPF handle.
 *
 *   host stack --- feth9  <==peer==>  feth10 --- BPF --- this process
 *
 * The host stack gets an address on feth9. Anything it transmits there appears
 * as input on feth10, where we read it off BPF; anything we write to BPF is
 * transmitted on feth10 and arrives as input on feth9. */
#include "tether.h"
#include <ifaddrs.h>
#include <netinet/in.h>
#include <arpa/inet.h>

static int  created = 0;

/* Overridable with -a / -g. Android usually serves 192.168.42.0/24, but not
 * always -- a Samsung on One UI was observed handing out 10.253.124.0/24. Never
 * assume the subnet; learn it or let the user state it. */
const char *g_host_addr = STATIC_ADDR;
const char *g_phone_gw  = PHONE_GW;
static char saved_gw[64] = "";

static int run(const char *fmt, ...)
{
    char cmd[512];
    va_list ap;
    va_start(ap, fmt);
    vsnprintf(cmd, sizeof cmd, fmt, ap);
    va_end(ap);

    int rc = system(cmd);
    if (rc != 0) log_warn("command failed (%d): %s", rc, cmd);
    return rc;
}

static int quiet(const char *fmt, ...)
{
    char cmd[512], full[560];
    va_list ap;
    va_start(ap, fmt);
    vsnprintf(cmd, sizeof cmd, fmt, ap);
    va_end(ap);
    snprintf(full, sizeof full, "%s >/dev/null 2>&1", cmd);
    return system(full);
}

static int iface_flags(const char *name)
{
    int s = socket(AF_INET, SOCK_DGRAM, 0);
    if (s < 0) return -1;
    struct ifreq ifr;
    memset(&ifr, 0, sizeof ifr);
    strlcpy(ifr.ifr_name, name, sizeof ifr.ifr_name);
    int rc = ioctl(s, SIOCGIFFLAGS, &ifr);
    close(s);
    return rc < 0 ? -1 : (ifr.ifr_flags & 0xffff);
}

/* Return the interface's IPv4 address, or NULL if it has none. */
static const char *iface_addr(const char *name)
{
    static char out[INET_ADDRSTRLEN];
    struct ifaddrs *ifa, *p;
    const char *result = NULL;

    if (getifaddrs(&ifa) != 0) return NULL;
    for (p = ifa; p; p = p->ifa_next) {
        if (!p->ifa_addr || p->ifa_addr->sa_family != AF_INET) continue;
        if (strcmp(p->ifa_name, name) != 0) continue;
        struct sockaddr_in *sin = (struct sockaddr_in *)p->ifa_addr;
        if (inet_ntop(AF_INET, &sin->sin_addr, out, sizeof out)) result = out;
        break;
    }
    freeifaddrs(ifa);
    return result;
}

/* Bring the interface up and confirm the kernel agrees. `ifconfig up` can
 * silently lose to whatever touched the interface last, so never assume. */
static int force_up(const char *name)
{
    for (int attempt = 0; attempt < 3; attempt++) {
        run("/sbin/ifconfig %s up", name);
        int f = iface_flags(name);
        if (f >= 0 && (f & IFF_UP)) return 0;
        usleep(200 * 1000);
    }
    log_err("%s refuses to come up (flags 0x%04x)", name, iface_flags(name));
    return -1;
}

int net_create(const uint8_t mac[6])
{
    quiet("/sbin/ifconfig " IF_HOST " destroy");
    quiet("/sbin/ifconfig " IF_PUMP " destroy");

    if (run("/sbin/ifconfig " IF_HOST " create") != 0 ||
        run("/sbin/ifconfig " IF_PUMP " create") != 0) {
        log_err("could not create the feth pair (need sudo?)");
        return -1;
    }
    created = 1;

    if (run("/sbin/ifconfig " IF_HOST " peer " IF_PUMP) != 0) {
        log_err("could not peer " IF_HOST " with " IF_PUMP);
        return -1;
    }

    /* Adopt the MAC the phone handed us. Android's gadget filters on destination
     * MAC, so using its permanent address is what Linux's rndis_host does too. */
    run("/sbin/ifconfig " IF_HOST " lladdr %02x:%02x:%02x:%02x:%02x:%02x",
        mac[0], mac[1], mac[2], mac[3], mac[4], mac[5]);

    run("/sbin/ifconfig " IF_HOST " mtu 1500");
    run("/sbin/ifconfig " IF_PUMP " mtu 1500");

    if (force_up(IF_PUMP) < 0 || force_up(IF_HOST) < 0) return -1;
    log_info("%s flags 0x%04x, %s flags 0x%04x",
             IF_HOST, iface_flags(IF_HOST), IF_PUMP, iface_flags(IF_PUMP));
    return 0;
}

/* Open the first free /dev/bpfN and bind it to the pump interface. */
int bpf_open(void)
{
    int fd = -1;
    char path[32];
    for (int i = 0; i < 256; i++) {
        snprintf(path, sizeof path, "/dev/bpf%d", i);
        fd = open(path, O_RDWR);
        if (fd >= 0) break;
        if (errno != EBUSY && errno != EACCES && errno != ENOENT) break;
    }
    if (fd < 0) { log_err("no free /dev/bpf* (need sudo?): %s", strerror(errno)); return -1; }

    u_int blen = BPF_BUFLEN;
    if (ioctl(fd, BIOCSBLEN, &blen) < 0) { log_err("BIOCSBLEN: %s", strerror(errno)); goto fail; }

    struct ifreq ifr;
    memset(&ifr, 0, sizeof ifr);
    strlcpy(ifr.ifr_name, IF_PUMP, sizeof ifr.ifr_name);
    if (ioctl(fd, BIOCSETIF, &ifr) < 0) {
        log_err("BIOCSETIF %s: %s", IF_PUMP, strerror(errno)); goto fail;
    }

    u_int on = 1, off = 0;
    if (ioctl(fd, BIOCIMMEDIATE, &on) < 0)  { log_err("BIOCIMMEDIATE: %s", strerror(errno)); goto fail; }
    if (ioctl(fd, BIOCSHDRCMPLT, &on) < 0)  { log_err("BIOCSHDRCMPLT: %s", strerror(errno)); goto fail; }
    if (ioctl(fd, BIOCSSEESENT, &off) < 0)  log_warn("BIOCSSEESENT: %s", strerror(errno));

    struct timeval tv = { .tv_sec = 0, .tv_usec = 200 * 1000 };
    if (ioctl(fd, BIOCSRTIMEOUT, &tv) < 0)  log_warn("BIOCSRTIMEOUT: %s", strerror(errno));

    log_info("BPF %s bound to %s (buffer %u bytes)", path, IF_PUMP, blen);
    return fd;

fail:
    close(fd);
    return -1;
}

/* Address the host side.
 *
 * We do NOT hand the interface to IPConfiguration: `ipconfig set ... DHCP` only
 * manages interfaces that are configured network services, and on anything else
 * it claims the interface, fails, and deconfigures it — leaving it down. So we
 * address it statically. Android's USB tethering is deterministic about this:
 * dnsmasq serves 192.168.42.0/24 with the phone itself on .129. */
int net_configure(int want_default_route)
{
    run("/sbin/ifconfig %s inet %s netmask 255.255.255.0", IF_HOST, g_host_addr);

    /* Try to enable IPv6 so a link-local address exists. feth(4) does not
     * implement the IPv6 interface ioctls (SIOCGIFINFO_IN6 returns EINVAL), so
     * this is best-effort and quiet -- IPv4 is the path that matters. */
    quiet("/sbin/ifconfig " IF_HOST " inet6 -ifdisabled");

    if (force_up(IF_HOST) < 0) return -1;

    const char *addr = iface_addr(IF_HOST);
    if (!addr) { log_err("could not address " IF_HOST); return -1; }
    log_info("%s is %s, phone is %s", IF_HOST, addr, g_phone_gw);

    if (want_default_route) {
        FILE *f = popen("/sbin/route -n get default 2>/dev/null | "
                        "awk '/gateway:/{print $2}'", "r");
        if (f) {
            if (fgets(saved_gw, sizeof saved_gw, f)) saved_gw[strcspn(saved_gw, "\n")] = 0;
            pclose(f);
        }
        if (saved_gw[0]) {
            log_warn("moving the default route from %s to %s — "
                     "this will disturb VPN tunnels until shutdown", saved_gw, g_phone_gw);
            run("/sbin/route -n change default %s", g_phone_gw);
        } else {
            run("/sbin/route -n add default %s", g_phone_gw);
        }
    } else {
        log_info("default route untouched; test with:  sudo ping -b %s %s",
                 IF_HOST, g_phone_gw);
    }
    return 0;
}

void net_destroy(void)
{
    if (!created) return;
    if (saved_gw[0]) {
        log_info("restoring the default route to %s", saved_gw);
        quiet("/sbin/route -n change default %s", saved_gw);
        saved_gw[0] = 0;
    }
    quiet("/sbin/ifconfig " IF_HOST " destroy");
    quiet("/sbin/ifconfig " IF_PUMP " destroy");
    created = 0;
}
