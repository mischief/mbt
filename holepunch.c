// SPDX-License-Identifier: ISC
/* BEP 55 holepunch message codec. See mbt/holepunch.h. */
#include "mbt/compat.h"
#include <errno.h>
#include <stdint.h>
#include <string.h>
#include <sys/socket.h>
#include <netinet/in.h>

#include "mbt/holepunch.h"

enum {
	HP_ADDR_V4 = 0x00,
	HP_ADDR_V6 = 0x01,
};

static void
put16(uint8_t *p, uint16_t v)
{
	p[0] = (uint8_t)(v >> 8);
	p[1] = (uint8_t)v;
}

static void
put32(uint8_t *p, uint32_t v)
{
	p[0] = (uint8_t)(v >> 24);
	p[1] = (uint8_t)(v >> 16);
	p[2] = (uint8_t)(v >> 8);
	p[3] = (uint8_t)v;
}

static uint16_t
get16(const uint8_t *p)
{
	return (uint16_t)((p[0] << 8) | p[1]);
}

static uint32_t
get32(const uint8_t *p)
{
	return ((uint32_t)p[0] << 24) | ((uint32_t)p[1] << 16) |
	       ((uint32_t)p[2] << 8) | (uint32_t)p[3];
}

static int
type_ok(int type)
{
	return type == MBT_HP_RENDEZVOUS || type == MBT_HP_CONNECT ||
	       type == MBT_HP_ERROR;
}

int
mbt_hp_encode(const struct mbt_hp_msg *msg, void *buf, size_t cap)
{
	uint8_t *b = buf;
	size_t alen, len;
	const uint8_t *addr;
	uint16_t port;

	if (!type_ok(msg->type))
		return -EINVAL;
	if (msg->addr.ss_family == AF_INET) {
		const struct sockaddr_in *sin = (const struct sockaddr_in *)&msg->addr;
		addr = (const uint8_t *)&sin->sin_addr;
		port = ntohs(sin->sin_port);
		alen = 4;
	} else if (msg->addr.ss_family == AF_INET6) {
		const struct sockaddr_in6 *sin6 = (const struct sockaddr_in6 *)&msg->addr;
		addr = (const uint8_t *)&sin6->sin6_addr;
		port = ntohs(sin6->sin6_port);
		alen = 16;
	} else {
		return -EINVAL;
	}

	len = 2 + alen + 2 + 4;
	if (cap < len)
		return -ENOBUFS;
	b[0] = (uint8_t)msg->type;
	b[1] = alen == 4 ? HP_ADDR_V4 : HP_ADDR_V6;
	memcpy(b + 2, addr, alen);
	put16(b + 2 + alen, port);
	put32(b + 4 + alen, msg->type == MBT_HP_ERROR ? msg->err : 0);
	return (int)len;
}

int
mbt_hp_decode(const void *buf, size_t len, struct mbt_hp_msg *msg)
{
	const uint8_t *b = buf;
	size_t alen;

	if (len < 2)
		return -EPROTO;
	if (!type_ok(b[0]))
		return -EPROTO;
	if (b[1] == HP_ADDR_V4)
		alen = 4;
	else if (b[1] == HP_ADDR_V6)
		alen = 16;
	else
		return -EPROTO;
	if (len != 2 + alen + 2 + 4)
		return -EPROTO;

	memset(msg, 0, sizeof(*msg));
	msg->type = (enum mbt_hp_type)b[0];
	msg->err = get32(b + 4 + alen);
	if (msg->type != MBT_HP_ERROR && msg->err != 0)
		return -EPROTO;

	if (alen == 4) {
		struct sockaddr_in *sin = (struct sockaddr_in *)&msg->addr;
		sin->sin_family = AF_INET;
		memcpy(&sin->sin_addr, b + 2, 4);
		sin->sin_port = htons(get16(b + 6));
	} else {
		struct sockaddr_in6 *sin6 = (struct sockaddr_in6 *)&msg->addr;
		sin6->sin6_family = AF_INET6;
		memcpy(&sin6->sin6_addr, b + 2, 16);
		sin6->sin6_port = htons(get16(b + 18));
	}
	return 0;
}

const char *
mbt_hp_type_str(enum mbt_hp_type type)
{
	switch (type) {
	case MBT_HP_RENDEZVOUS: return "rendezvous";
	case MBT_HP_CONNECT:    return "connect";
	case MBT_HP_ERROR:      return "error";
	}
	return "unknown";
}

const char *
mbt_hp_error_str(uint32_t err)
{
	switch (err) {
	case MBT_HP_ERR_NONE:          return "none";
	case MBT_HP_ERR_NO_SUCH_PEER:  return "NoSuchPeer";
	case MBT_HP_ERR_NOT_CONNECTED: return "NotConnected";
	case MBT_HP_ERR_NO_SUPPORT:    return "NoSupport";
	case MBT_HP_ERR_NO_SELF:       return "NoSelf";
	}
	return "unknown";
}
