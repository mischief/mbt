// SPDX-License-Identifier: ISC
/* BEP 55 holepunch codec: round trips, exact wire bytes, malformed input. */

#include <errno.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <arpa/inet.h>
#include <netinet/in.h>

#include "mbt/holepunch.h"

#define CHECK(c, ...) do { if (!(c)) { \
	fprintf(stderr, "FAIL %s:%d: ", __FILE__, __LINE__); \
	fprintf(stderr, __VA_ARGS__); fprintf(stderr, "\n"); exit(1); } } while (0)

static void
v4(struct mbt_hp_msg *m, int type, const char *ip, uint16_t port, uint32_t err)
{
	struct sockaddr_in *sin = (struct sockaddr_in *)&m->addr;
	memset(m, 0, sizeof(*m));
	m->type = type;
	m->err = err;
	sin->sin_family = AF_INET;
	sin->sin_port = htons(port);
	CHECK(inet_pton(AF_INET, ip, &sin->sin_addr) == 1, "inet_pton %s", ip);
}

static void
v6(struct mbt_hp_msg *m, int type, const char *ip, uint16_t port, uint32_t err)
{
	struct sockaddr_in6 *sin6 = (struct sockaddr_in6 *)&m->addr;
	memset(m, 0, sizeof(*m));
	m->type = type;
	m->err = err;
	sin6->sin6_family = AF_INET6;
	sin6->sin6_port = htons(port);
	CHECK(inet_pton(AF_INET6, ip, &sin6->sin6_addr) == 1, "inet_pton %s", ip);
}

static void
same(const struct mbt_hp_msg *a, const struct mbt_hp_msg *b)
{
	CHECK(a->type == b->type, "type %d != %d", a->type, b->type);
	CHECK(a->err == b->err, "err %u != %u", a->err, b->err);
	CHECK(a->addr.ss_family == b->addr.ss_family, "family");
	if (a->addr.ss_family == AF_INET) {
		const struct sockaddr_in *x = (const void *)&a->addr, *y = (const void *)&b->addr;
		CHECK(x->sin_port == y->sin_port, "port");
		CHECK(memcmp(&x->sin_addr, &y->sin_addr, 4) == 0, "addr");
	} else {
		const struct sockaddr_in6 *x = (const void *)&a->addr, *y = (const void *)&b->addr;
		CHECK(x->sin6_port == y->sin6_port, "port");
		CHECK(memcmp(&x->sin6_addr, &y->sin6_addr, 16) == 0, "addr");
	}
}

static void
test_wire_v4(void)
{
	static const uint8_t want[] = {
		0x01, 0x00, 192, 0, 2, 7, 0x1a, 0xe1, 0, 0, 0, 0,
	};
	struct mbt_hp_msg m, d;
	uint8_t buf[MBT_HP_MSG_MAX_LEN];

	printf("test_wire_v4\n");
	v4(&m, MBT_HP_CONNECT, "192.0.2.7", 6881, 0);
	CHECK(mbt_hp_encode(&m, buf, sizeof(buf)) == MBT_HP_MSG_V4_LEN, "len");
	CHECK(memcmp(buf, want, sizeof(want)) == 0, "bytes");
	CHECK(mbt_hp_decode(want, sizeof(want), &d) == 0, "decode");
	same(&m, &d);
}

static void
test_wire_v6_error(void)
{
	static const uint8_t want[] = {
		0x02, 0x01,
		0x20, 0x01, 0x0d, 0xb8, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0x42,
		0xc3, 0x50, 0, 0, 0, 0x03,
	};
	struct mbt_hp_msg m, d;
	uint8_t buf[MBT_HP_MSG_MAX_LEN];

	printf("test_wire_v6_error\n");
	v6(&m, MBT_HP_ERROR, "2001:db8::42", 50000, MBT_HP_ERR_NO_SUPPORT);
	CHECK(mbt_hp_encode(&m, buf, sizeof(buf)) == MBT_HP_MSG_V6_LEN, "len");
	CHECK(memcmp(buf, want, sizeof(want)) == 0, "bytes");
	CHECK(mbt_hp_decode(want, sizeof(want), &d) == 0, "decode");
	same(&m, &d);
}

static void
test_roundtrip(void)
{
	static const int types[] = { MBT_HP_RENDEZVOUS, MBT_HP_CONNECT, MBT_HP_ERROR };
	struct mbt_hp_msg m, d;
	uint8_t buf[MBT_HP_MSG_MAX_LEN];

	printf("test_roundtrip\n");
	for (size_t i = 0; i < 3; i++) {
		uint32_t err = types[i] == MBT_HP_ERROR ? MBT_HP_ERR_NO_SELF : 0;
		v4(&m, types[i], "10.1.2.3", 1, err);
		int n = mbt_hp_encode(&m, buf, sizeof(buf));
		CHECK(n == MBT_HP_MSG_V4_LEN, "v4 len %d", n);
		CHECK(mbt_hp_decode(buf, (size_t)n, &d) == 0, "v4 decode");
		same(&m, &d);
		v6(&m, types[i], "fe80::1", 65535, err);
		n = mbt_hp_encode(&m, buf, sizeof(buf));
		CHECK(n == MBT_HP_MSG_V6_LEN, "v6 len %d", n);
		CHECK(mbt_hp_decode(buf, (size_t)n, &d) == 0, "v6 decode");
		same(&m, &d);
	}
}

/* err is only written for error messages, whatever the struct holds. */
static void
test_encode_err_only_for_error(void)
{
	struct mbt_hp_msg m;
	uint8_t buf[MBT_HP_MSG_MAX_LEN];

	printf("test_encode_err_only_for_error\n");
	v4(&m, MBT_HP_RENDEZVOUS, "10.0.0.1", 80, 7);
	CHECK(mbt_hp_encode(&m, buf, sizeof(buf)) == MBT_HP_MSG_V4_LEN, "len");
	CHECK(buf[8] == 0 && buf[9] == 0 && buf[10] == 0 && buf[11] == 0,
	    "err written for rendezvous");
}

static void
test_encode_bad(void)
{
	struct mbt_hp_msg m;
	uint8_t buf[MBT_HP_MSG_MAX_LEN];

	printf("test_encode_bad\n");
	v4(&m, MBT_HP_CONNECT, "10.0.0.1", 80, 0);
	CHECK(mbt_hp_encode(&m, buf, MBT_HP_MSG_V4_LEN - 1) == -ENOBUFS, "short buf");
	m.type = 3;
	CHECK(mbt_hp_encode(&m, buf, sizeof(buf)) == -EINVAL, "bad type");
	v4(&m, MBT_HP_CONNECT, "10.0.0.1", 80, 0);
	m.addr.ss_family = AF_UNIX;
	CHECK(mbt_hp_encode(&m, buf, sizeof(buf)) == -EINVAL, "bad family");
}

static void
test_decode_bad(void)
{
	struct mbt_hp_msg m, d;
	uint8_t buf[MBT_HP_MSG_MAX_LEN + 4];

	printf("test_decode_bad\n");
	v4(&m, MBT_HP_CONNECT, "10.0.0.1", 80, 0);
	CHECK(mbt_hp_encode(&m, buf, sizeof(buf)) == MBT_HP_MSG_V4_LEN, "len");

	/* every truncation and one extra byte */
	for (size_t n = 0; n < MBT_HP_MSG_V4_LEN; n++)
		CHECK(mbt_hp_decode(buf, n, &d) == -EPROTO, "truncated to %zu", n);
	buf[MBT_HP_MSG_V4_LEN] = 0;
	CHECK(mbt_hp_decode(buf, MBT_HP_MSG_V4_LEN + 1, &d) == -EPROTO, "trailing");

	/* v6 type byte on a v4-sized message */
	buf[1] = 0x01;
	CHECK(mbt_hp_decode(buf, MBT_HP_MSG_V4_LEN, &d) == -EPROTO, "v6 len mismatch");
	buf[1] = 0x02;
	CHECK(mbt_hp_decode(buf, MBT_HP_MSG_V4_LEN, &d) == -EPROTO, "addr type 2");
	buf[1] = 0x00;

	buf[0] = 0x03;
	CHECK(mbt_hp_decode(buf, MBT_HP_MSG_V4_LEN, &d) == -EPROTO, "msg type 3");
	buf[0] = MBT_HP_CONNECT;

	/* nonzero err_code on a non-error message */
	buf[11] = 1;
	CHECK(mbt_hp_decode(buf, MBT_HP_MSG_V4_LEN, &d) == -EPROTO, "err on connect");

	/* unknown error code on an error message passes through */
	buf[0] = MBT_HP_ERROR;
	buf[11] = 0x99;
	CHECK(mbt_hp_decode(buf, MBT_HP_MSG_V4_LEN, &d) == 0, "unknown err code");
	CHECK(d.err == 0x99, "err value");
	CHECK(strcmp(mbt_hp_error_str(d.err), "unknown") == 0, "err str");
}

int
main(void)
{
	test_wire_v4();
	test_wire_v6_error();
	test_roundtrip();
	test_encode_err_only_for_error();
	test_encode_bad();
	test_decode_bad();
	printf("all holepunch codec tests passed\n");
	return 0;
}
