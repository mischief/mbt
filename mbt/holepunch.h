// SPDX-License-Identifier: ISC
#ifndef MBT_HOLEPUNCH_H
#define MBT_HOLEPUNCH_H

/*
 * BEP 55 holepunch: ut_holepunch extension message codec.
 *
 * Payload (after the BEP 10 extended message header):
 *   msg_type  1 byte   MBT_HP_RENDEZVOUS, MBT_HP_CONNECT or MBT_HP_ERROR
 *   addr_type 1 byte   0 = IPv4, 1 = IPv6
 *   addr      4 or 16 bytes, big-endian
 *   port      2 bytes, big-endian
 *   err_code  4 bytes, big-endian; 0 unless msg_type is MBT_HP_ERROR
 *
 * so a message is exactly 12 (IPv4) or 24 (IPv6) bytes.
 */

#include "export.h"

#include <stddef.h>
#include <stdint.h>
#include <sys/socket.h>

enum mbt_hp_type {
	MBT_HP_RENDEZVOUS = 0x00, /* ask the relay to introduce us to addr */
	MBT_HP_CONNECT    = 0x01, /* from the relay: start uTP to addr */
	MBT_HP_ERROR      = 0x02, /* from the relay: rendezvous failed */
};

enum mbt_hp_error {
	MBT_HP_ERR_NONE           = 0x00,
	MBT_HP_ERR_NO_SUCH_PEER   = 0x01, /* target endpoint is invalid */
	MBT_HP_ERR_NOT_CONNECTED  = 0x02, /* relay not connected to target */
	MBT_HP_ERR_NO_SUPPORT     = 0x03, /* target lacks ut_holepunch */
	MBT_HP_ERR_NO_SELF        = 0x04, /* target endpoint is the relay */
};

#define MBT_HP_MSG_V4_LEN  12
#define MBT_HP_MSG_V6_LEN  24
#define MBT_HP_MSG_MAX_LEN MBT_HP_MSG_V6_LEN

struct mbt_hp_msg {
	enum mbt_hp_type type;
	/* AF_INET or AF_INET6 sockaddr; the port is in the sockaddr, in
	 * network byte order as usual. */
	struct sockaddr_storage addr;
	uint32_t err;             /* enum mbt_hp_error, 0 unless MBT_HP_ERROR */
};

/*
 * Encode msg into buf. Returns the payload length (12 or 24), or -EINVAL for
 * an unknown type or address family, or -ENOBUFS if cap is too small.
 */
MBT_API int mbt_hp_encode(const struct mbt_hp_msg *msg, void *buf, size_t cap);

/*
 * Decode a payload. Returns 0, or -EPROTO if the length does not match the
 * address type, the address type or message type is unknown, or a
 * non-error message carries a nonzero err_code. Unknown error codes in an
 * error message are passed through.
 */
MBT_API int mbt_hp_decode(const void *buf, size_t len, struct mbt_hp_msg *msg);

/* Names for logging. */
MBT_API const char *mbt_hp_type_str(enum mbt_hp_type type);
MBT_API const char *mbt_hp_error_str(uint32_t err);

#endif
