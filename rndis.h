/* RNDIS wire protocol — Remote NDIS over USB.
 * Reference: Linux drivers/net/usb/rndis_host.c + include/linux/usb/rndis_host.h
 * All fields are little-endian on the wire. */
#ifndef RNDIS_H
#define RNDIS_H
#include <stdint.h>
#include <assert.h>

/* Message types */
#define RNDIS_MSG_PACKET        0x00000001u
#define RNDIS_MSG_INIT          0x00000002u
#define RNDIS_MSG_INIT_C        0x80000002u
#define RNDIS_MSG_HALT          0x00000003u
#define RNDIS_MSG_QUERY         0x00000004u
#define RNDIS_MSG_QUERY_C       0x80000004u
#define RNDIS_MSG_SET           0x00000005u
#define RNDIS_MSG_SET_C         0x80000005u
#define RNDIS_MSG_RESET         0x00000006u
#define RNDIS_MSG_RESET_C       0x80000006u
#define RNDIS_MSG_INDICATE      0x00000007u
#define RNDIS_MSG_KEEPALIVE     0x00000008u
#define RNDIS_MSG_KEEPALIVE_C   0x80000008u

#define RNDIS_STATUS_SUCCESS    0x00000000u
#define RNDIS_STATUS_MEDIA_CONNECT    0x4001000Bu
#define RNDIS_STATUS_MEDIA_DISCONNECT 0x4001000Cu

/* OIDs we need */
#define OID_GEN_MAXIMUM_FRAME_SIZE      0x00010106u
#define OID_GEN_LINK_SPEED              0x00010107u
#define OID_GEN_CURRENT_PACKET_FILTER   0x0001010Eu
#define OID_GEN_MEDIA_CONNECT_STATUS    0x00010114u
#define OID_802_3_PERMANENT_ADDRESS     0x01010101u
#define OID_802_3_CURRENT_ADDRESS       0x01010102u

/* Packet filter bits */
#define RNDIS_PACKET_TYPE_DIRECTED      0x0001u
#define RNDIS_PACKET_TYPE_MULTICAST     0x0002u
#define RNDIS_PACKET_TYPE_ALL_MULTICAST 0x0004u
#define RNDIS_PACKET_TYPE_BROADCAST     0x0008u
#define RNDIS_PACKET_TYPE_PROMISCUOUS   0x0020u

/* USB CDC control requests carried on the comm interface */
#define USB_CDC_SEND_ENCAPSULATED_COMMAND 0x00
#define USB_CDC_GET_ENCAPSULATED_RESPONSE 0x01

struct rndis_init {            /* 24 bytes */
    uint32_t msg_type, msg_len, request_id;
    uint32_t major_version, minor_version, max_transfer_size;
} __attribute__((packed));
struct rndis_init_c {          /* 52 bytes */
    uint32_t msg_type, msg_len, request_id, status;
    uint32_t major_version, minor_version, device_flags, medium;
    uint32_t max_packets_per_message, max_transfer_size, packet_alignment;
    uint32_t af_list_offset, af_list_size;
} __attribute__((packed));
struct rndis_query {           /* 28 bytes; also used for SET */
    uint32_t msg_type, msg_len, request_id, oid;
    uint32_t len, offset, handle;
} __attribute__((packed));
struct rndis_query_c {         /* 24 bytes */
    uint32_t msg_type, msg_len, request_id, status;
    uint32_t len, offset;
} __attribute__((packed));
struct rndis_set_c {           /* 16 bytes */
    uint32_t msg_type, msg_len, request_id, status;
} __attribute__((packed));
struct rndis_keepalive {       /* 12 bytes */
    uint32_t msg_type, msg_len, request_id;
} __attribute__((packed));
struct rndis_keepalive_c {     /* 16 bytes */
    uint32_t msg_type, msg_len, request_id, status;
} __attribute__((packed));
struct rndis_halt {            /* 12 bytes */
    uint32_t msg_type, msg_len, request_id;
} __attribute__((packed));
struct rndis_packet {          /* 44 bytes, prepended to every Ethernet frame */
    uint32_t msg_type, msg_len;
    uint32_t data_offset, data_len;      /* data_offset is relative to byte 8 */
    uint32_t oob_data_offset, oob_data_len, num_oob;
    uint32_t packet_data_offset, packet_data_len;
    uint32_t vc_handle, reserved;
} __attribute__((packed));

/* Offsets inside QUERY/SET/PACKET are measured from byte 8 of the message,
 * i.e. from the start of the request_id field. */
#define RNDIS_OFFSET_BASE 8

/* The wire layout is fixed; never let the compiler pad these. */
_Static_assert(sizeof(struct rndis_packet)  == 44, "rndis_packet must be 44 bytes");
_Static_assert(sizeof(struct rndis_init)    == 24, "rndis_init must be 24 bytes");
_Static_assert(sizeof(struct rndis_query)   == 28, "rndis_query must be 28 bytes");
_Static_assert(sizeof(struct rndis_init_c)  == 52, "rndis_init_c must be 52 bytes");

#endif
