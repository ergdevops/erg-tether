/* SPDX-License-Identifier: Apache-2.0 */
/* Minimal DHCP client, spoken directly down the USB path.
 *
 * We do not hand this to the host stack. Sending a DISCOVER through a socket
 * means fighting the routing table: IP_BOUND_IF does not override destination
 * routing for 255.255.255.255, so the datagram quietly leaves via the default
 * interface and never reaches the phone. Building the Ethernet frame ourselves
 * and pushing it out the bulk endpoint sidesteps routing entirely, and works
 * before the interface has any address at all -- which is the whole point,
 * because the tethering subnet is what we are trying to discover. */
#include "tether.h"

#define BOOTP_MAGIC 0x63825363u

static uint16_t ip_checksum(const void *buf, size_t len)
{
    const uint8_t *p = buf;
    uint32_t sum = 0;
    for (size_t i = 0; i + 1 < len; i += 2) sum += (uint32_t)((p[i] << 8) | p[i + 1]);
    if (len & 1) sum += (uint32_t)(p[len - 1] << 8);
    while (sum >> 16) sum = (sum & 0xffff) + (sum >> 16);
    return (uint16_t)~sum;
}

/* Ethernet + IPv4 + UDP + BOOTP DISCOVER, broadcast. Returns the frame length. */
uint32_t dhcp_build_discover(uint8_t *f, const uint8_t mac[6], uint32_t xid)
{
    memset(f, 0, 342);

    memset(f, 0xff, 6);                       /* dst: broadcast          */
    memcpy(f + 6, mac, 6);                    /* src: our MAC            */
    f[12] = 0x08; f[13] = 0x00;               /* ethertype: IPv4         */

    uint8_t *ip = f + 14;
    ip[0] = 0x45;                             /* v4, 20-byte header      */
    uint16_t iplen = 20 + 8 + 300;
    ip[2] = (uint8_t)(iplen >> 8); ip[3] = (uint8_t)(iplen & 0xff);
    ip[8] = 64;                               /* TTL                     */
    ip[9] = 17;                               /* UDP                     */
    memset(ip + 12, 0, 4);                    /* src 0.0.0.0             */
    memset(ip + 16, 0xff, 4);                 /* dst 255.255.255.255     */
    uint16_t csum = ip_checksum(ip, 20);
    ip[10] = (uint8_t)(csum >> 8); ip[11] = (uint8_t)(csum & 0xff);

    uint8_t *udp = ip + 20;
    udp[0] = 0; udp[1] = 68;                  /* sport 68                */
    udp[2] = 0; udp[3] = 67;                  /* dport 67                */
    uint16_t ulen = 8 + 300;
    udp[4] = (uint8_t)(ulen >> 8); udp[5] = (uint8_t)(ulen & 0xff);
    /* UDP checksum is optional over IPv4; zero means "not computed". */

    uint8_t *bp = udp + 8;
    bp[0] = 1;                                /* BOOTREQUEST             */
    bp[1] = 1;                                /* ethernet                */
    bp[2] = 6;                                /* MAC length              */
    bp[4] = (uint8_t)(xid >> 24); bp[5] = (uint8_t)(xid >> 16);
    bp[6] = (uint8_t)(xid >> 8);  bp[7] = (uint8_t)xid;
    bp[10] = 0x80;                            /* broadcast flag          */
    memcpy(bp + 28, mac, 6);                  /* chaddr                  */

    uint8_t *o = bp + 236;
    o[0] = (uint8_t)(BOOTP_MAGIC >> 24); o[1] = (uint8_t)(BOOTP_MAGIC >> 16);
    o[2] = (uint8_t)(BOOTP_MAGIC >> 8);  o[3] = (uint8_t)BOOTP_MAGIC;
    o += 4;
    *o++ = 53; *o++ = 1; *o++ = 1;            /* message type: DISCOVER  */
    *o++ = 55; *o++ = 4; *o++ = 1; *o++ = 3; *o++ = 6; *o++ = 15;
    *o++ = 255;                               /* end                     */

    return 14 + iplen;
}

/* Parse a BOOTP reply. Returns 1 and fills the outputs on a match. */
int dhcp_parse_reply(const uint8_t *f, uint32_t len, uint32_t xid,
                     uint32_t *yiaddr, uint32_t *router, uint32_t *mask)
{
    if (len < 14 + 20 + 8 + 240) return 0;
    if (((f[12] << 8) | f[13]) != 0x0800) return 0;

    const uint8_t *ip = f + 14;
    uint32_t ihl = (uint32_t)(ip[0] & 0x0f) * 4;
    if ((ip[0] >> 4) != 4 || ip[9] != 17 || ihl < 20) return 0;

    const uint8_t *udp = ip + ihl;
    if (((udp[0] << 8) | udp[1]) != 67) return 0;     /* from a DHCP server */

    const uint8_t *bp = udp + 8;
    if (bp[0] != 2) return 0;                          /* BOOTREPLY */
    uint32_t got = ((uint32_t)bp[4] << 24) | ((uint32_t)bp[5] << 16) |
                   ((uint32_t)bp[6] << 8)  | bp[7];
    if (got != xid) return 0;

    memcpy(yiaddr, bp + 16, 4);
    *router = 0; *mask = 0;

    const uint8_t *o = bp + 236;
    if (((uint32_t)o[0] << 24 | (uint32_t)o[1] << 16 |
         (uint32_t)o[2] << 8  | o[3]) != BOOTP_MAGIC) return 0;
    o += 4;

    const uint8_t *end = f + len;
    while (o + 1 < end && *o != 255) {
        if (*o == 0) { o++; continue; }
        uint8_t tag = o[0], l = o[1];
        if (o + 2 + l > end) break;
        if (tag == 1 && l >= 4) memcpy(mask, o + 2, 4);
        if (tag == 3 && l >= 4) memcpy(router, o + 2, 4);
        o += 2 + l;
    }
    return 1;
}
