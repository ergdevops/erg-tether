#ifndef TETHER_H
#define TETHER_H

#include <stdio.h>
#include <stdlib.h>
#include <stdarg.h>
#include <string.h>
#include <unistd.h>
#include <errno.h>
#include <fcntl.h>
#include <pthread.h>
#include <signal.h>
#include <stdint.h>
#include <sys/ioctl.h>
#include <sys/socket.h>
#include <sys/time.h>
#include <net/bpf.h>
#include <net/if.h>
#include <libkern/OSByteOrder.h>
#include <sys/stat.h>
#include <arpa/inet.h>
#include <netinet/in.h>
#include <time.h>

#include <CoreFoundation/CoreFoundation.h>
#include <IOKit/IOKitLib.h>
#include <IOKit/IOCFPlugIn.h>
#include <IOKit/usb/IOUSBLib.h>

#include "rndis.h"

/* Interface names. IF_HOST carries the IP address; IF_PUMP is where we read and
 * write raw frames. Keep them adjacent so `ifconfig` output reads sensibly. */
#define IF_HOST "feth9"
#define IF_PUMP "feth10"

#define RNDIS_MAX_TRANSFER 16384      /* what we advertise in INITIALIZE */
#define BPF_BUFLEN         (256 * 1024)
#define KEEPALIVE_SECS     5

/* Android USB tethering always serves 192.168.42.0/24 with the phone on .129. */
#define STATUS_PATH  "/var/run/erg-tether.json"
#define CONTROL_PATH "/var/run/erg-tether.ctl"
#define STATIC_ADDR "192.168.42.100"
#define PHONE_GW    "192.168.42.129"

/* Everything on the RNDIS wire is little-endian; arm64 macOS is too, but be
 * explicit so the intent survives a copy-paste to a big-endian host. */
#define le32(x) OSSwapHostToLittleInt32(x)

/* --- usb.c --- */
extern IOUSBInterfaceInterface190 **g_ctrl;
extern IOUSBInterfaceInterface190 **g_data;
extern uint8_t  g_pipe_in, g_pipe_out;
extern uint16_t g_maxpkt_out;
extern int g_opt_clearstall, g_opt_dump;
int  usb_open(uint16_t vid, uint16_t pid);
int  usb_reset_device(uint16_t vid, uint16_t pid);
int  usb_find_rndis(uint16_t *vid, uint16_t *pid);
const char *ioerr(IOReturn r);
void usb_close(void);

/* --- rndis.c --- */
int  rndis_bringup(uint8_t mac[6], uint32_t *dev_max_transfer);
int  rndis_query(uint32_t oid, void *out, uint32_t outlen);
int  rndis_set(uint32_t oid, const void *val, uint32_t vallen);
int  rndis_keepalive(void);
int  rndis_media_connected(void);
int  rndis_open_filter(void);
void rndis_halt(void);

/* --- net.c --- */
extern const char *g_host_addr, *g_phone_gw;
extern volatile uint32_t g_peer_ip;   /* phone's IPv4, learned from its traffic */
int  net_create(const uint8_t mac[6]);
int  bpf_open(void);
int  net_configure(int want_default_route);
void net_destroy(void);

/* --- dhcp.c --- */
uint32_t dhcp_build_discover(uint8_t *frame, const uint8_t mac[6], uint32_t xid);
int      dhcp_parse_reply(const uint8_t *frame, uint32_t len, uint32_t xid,
                          uint32_t *yiaddr, uint32_t *router, uint32_t *mask);

/* --- main.c --- */
extern volatile sig_atomic_t g_run;
void log_info(const char *fmt, ...);
void log_warn(const char *fmt, ...);
void log_err(const char *fmt, ...);

#endif
