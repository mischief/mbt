// SPDX-License-Identifier: ISC
/*
 * Peer wire framing tests: a real mbt_peer on one end of a socketpair, raw
 * bytes written to the other end. Each case sends a message whose length
 * prefix disagrees with what its handler reads (too long, too short, an
 * extension handshake larger than any fixed buffer) followed by a marker
 * HAVE message, and checks that the marker still parses: the stream stayed
 * framed. A desync turns the marker into garbage or a bogus length.
 */

#include <errno.h>
#include <signal.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>
#include <sys/socket.h>

#include "mbt/event.h"
#include "mbt/conn.h"
#include "mbt/infohash.h"
#include "mbt/peer.h"

#define CHECK(c, ...) do { if (!(c)) { \
	fprintf(stderr, "FAIL %s:%d: ", __FILE__, __LINE__); \
	fprintf(stderr, __VA_ARGS__); fprintf(stderr, "\n"); exit(1); } } while (0)

#define BT_HAVE       4
#define BT_REQUEST    6
#define BT_PORT       9
#define BT_EXTENSION  20
#define MARKER_PIECE  77
#define PIECES        128

static const char IH_HEX[] = "0123456789abcdef0123456789abcdef01234567";

struct rig {
	struct mbt_ev *ev;
	struct mbt_peer *peer;
	struct mbt_infohash *ih;
	int fd;                 /* remote end */
	int active;
	int have_marker;        /* saw HAVE(MARKER_PIECE) */
	int other_haves;        /* any HAVE that is not the marker */
	int requests;
	int error;
	int err;
	uint32_t req[3];
	char version[64];
};

static void
on_active(struct mbt_peer *p, void *arg)
{
	struct rig *r = arg;
	(void)p;
	r->active = 1;
}

static void
on_have(struct mbt_peer *p, uint32_t piece, void *arg)
{
	struct rig *r = arg;
	(void)p;
	if (piece == MARKER_PIECE) {
		r->have_marker = 1;
	} else {
		r->other_haves++;
	}
}

static void
on_request(struct mbt_peer *p, uint32_t piece, uint32_t off, uint32_t len,
           void *arg)
{
	struct rig *r = arg;
	(void)p;
	r->requests++;
	r->req[0] = piece;
	r->req[1] = off;
	r->req[2] = len;
}

static void
on_error(struct mbt_peer *p, int err, void *arg)
{
	struct rig *r = arg;
	(void)p;
	r->error = 1;
	r->err = err;
}


static void
put32(uint8_t *p, uint32_t v)
{
	p[0] = (uint8_t)(v >> 24);
	p[1] = (uint8_t)(v >> 16);
	p[2] = (uint8_t)(v >> 8);
	p[3] = (uint8_t)v;
}

static void
xwrite(int fd, const void *buf, size_t len)
{
	const uint8_t *b = buf;
	while (len > 0) {
		ssize_t n = write(fd, b, len);
		CHECK(n > 0, "write: %s", strerror(errno));
		b += n;
		len -= (size_t)n;
	}
}

/* Write one message: 4-byte length, id, body. `len` overrides the length
 * prefix when nonzero (to lie about the size). */
static void
send_msg(int fd, uint8_t id, const void *body, size_t blen)
{
	uint8_t hdr[5];
	put32(hdr, (uint32_t)(1 + blen));
	hdr[4] = id;
	xwrite(fd, hdr, 5);
	if (blen)
		xwrite(fd, body, blen);
}

static void
send_have(int fd, uint32_t piece)
{
	uint8_t b[4];
	put32(b, piece);
	send_msg(fd, BT_HAVE, b, 4);
}

/* Step the loop until `done` is set, an error, or about 2 s pass. The loop
 * is stepped rather than run/stopped: with sd-event a stop is permanent. */
static void
run_until(struct rig *r, const int *done)
{
	for (int i = 0; i < 200 && !*done && !r->error; i++)
		mbt_ev_step(r->ev, 10);
}

static void
run(struct rig *r)
{
	run_until(r, &r->have_marker);
}

/* Bring up a peer with extensions enabled on our side of the socketpair,
 * do the remote handshake, and run until the peer is ACTIVE. `ext_body`
 * is the bencoded extension handshake the remote sends (NULL: a minimal
 * one). */
static void
rig_open(struct rig *r, const char *ext_body, size_t ext_len)
{
	struct mbt_peer_callbacks cb = {
		.on_active = on_active,
		.on_have = on_have,
		.on_request = on_request,
		.on_error = on_error,
	};
	uint8_t id[20], hs[68], rx[68];
	struct mbt_conn *conn = NULL;
	int sv[2];

	memset(r, 0, sizeof(*r));
	CHECK(mbt_ev_new_default(&r->ev) == 0, "ev");
	CHECK(socketpair(AF_UNIX, SOCK_STREAM, 0, sv) == 0, "socketpair");
	r->fd = sv[1];
	CHECK(mbt_infohash_new(&r->ih, MBT_INFOHASH_SHA1, IH_HEX) == 0, "ih");
	CHECK(mbt_conn_new(r->ev, &conn, sv[0], NULL, NULL, NULL) == 0, "conn");

	memset(id, 'L', sizeof(id));
	CHECK(mbt_peer_new(&r->peer, r->ev, conn, r->ih, id, PIECES,
	    MBT_SOURCE_TRACKER, &cb, r) == 0, "peer_new");

	/* Read our handshake, answer with one that enables BEP 10. */
	size_t got = 0;
	while (got < sizeof(rx)) {
		ssize_t n = read(r->fd, rx + got, sizeof(rx) - got);
		CHECK(n > 0, "read handshake");
		got += (size_t)n;
	}
	memcpy(hs, rx, 68);
	memset(hs + 20, 0, 8);
	hs[20 + 5] = 0x10;                  /* extension protocol */
	memset(hs + 48, 'R', 20);           /* remote peer id */
	xwrite(r->fd, hs, 68);

	if (!ext_body) {
		ext_body = "d1:md6:ut_pexi1eee";
		ext_len = strlen(ext_body);
	}
	uint8_t *m = malloc(1 + ext_len);
	CHECK(m != NULL, "malloc");
	m[0] = 0;                           /* extended handshake */
	memcpy(m + 1, ext_body, ext_len);
	send_msg(r->fd, BT_EXTENSION, m, 1 + ext_len);
	free(m);

	run_until(r, &r->active);
	CHECK(r->active, "peer did not go active");
	CHECK(!r->error, "peer errored during handshake (%d)", r->err);
}

static void
rig_close(struct rig *r)
{
	mbt_peer_free(r->peer);
	mbt_infohash_unref(r->ih);
	close(r->fd);
	mbt_ev_destroy(r->ev);
}

static void
expect_marker(struct rig *r, const char *what)
{
	send_have(r->fd, MARKER_PIECE);
	run(r);
	CHECK(!r->error, "%s: peer errored (%d)", what, r->err);
	CHECK(r->have_marker, "%s: marker HAVE not seen, stream desynced", what);
}

/* HAVE with trailing bytes: the handler reads 4, the rest must be skipped. */
static void
test_have_trailing(void)
{
	struct rig r;
	uint8_t b[4 + 7] = {0};

	printf("test_have_trailing\n"); fflush(stdout);
	rig_open(&r, NULL, 0);
	put32(b, 5);
	memset(b + 4, 0xee, 7);
	send_msg(r.fd, BT_HAVE, b, sizeof(b));
	expect_marker(&r, "have trailing");
	CHECK(r.other_haves == 1, "HAVE(5) not delivered");
	rig_close(&r);
}

/* REQUEST with trailing bytes. */
static void
test_request_trailing(void)
{
	struct rig r;
	uint8_t b[12 + 5];

	printf("test_request_trailing\n"); fflush(stdout);
	rig_open(&r, NULL, 0);
	put32(b, 3);
	put32(b + 4, 16384);
	put32(b + 8, 16384);
	memset(b + 12, 0xee, 5);
	send_msg(r.fd, BT_REQUEST, b, sizeof(b));
	expect_marker(&r, "request trailing");
	CHECK(r.requests == 1 && r.req[0] == 3 && r.req[1] == 16384 &&
	    r.req[2] == 16384, "REQUEST fields wrong");
	rig_close(&r);
}

/* HAVE too short for its field: a protocol error, never a read into the
 * next message. */
static void
test_have_short(void)
{
	struct rig r;
	uint8_t b[2] = { 0, 1 };

	printf("test_have_short\n"); fflush(stdout);
	rig_open(&r, NULL, 0);
	send_msg(r.fd, BT_HAVE, b, sizeof(b));
	send_have(r.fd, MARKER_PIECE);
	run(&r);
	CHECK(r.error, "short HAVE accepted");
	CHECK(r.other_haves == 0, "short HAVE delivered a piece");
	CHECK(!r.have_marker, "short HAVE swallowed the next message");
	rig_close(&r);
}

/* PORT with trailing bytes. */
static void
test_port_trailing(void)
{
	struct rig r;
	uint8_t b[2 + 3] = { 0x1a, 0xe1, 1, 2, 3 };

	printf("test_port_trailing\n"); fflush(stdout);
	rig_open(&r, NULL, 0);
	send_msg(r.fd, BT_PORT, b, sizeof(b));
	expect_marker(&r, "port trailing");
	rig_close(&r);
}

/* Unknown message id with a body. */
static void
test_unknown_message(void)
{
	struct rig r;
	uint8_t b[3000];

	printf("test_unknown_message\n"); fflush(stdout);
	rig_open(&r, NULL, 0);
	memset(b, 0xab, sizeof(b));
	send_msg(r.fd, 99, b, sizeof(b));
	expect_marker(&r, "unknown message");
	rig_close(&r);
}

/* Unknown extended message id. */
static void
test_unknown_extension(void)
{
	struct rig r;
	uint8_t b[1 + 2000];

	printf("test_unknown_extension\n"); fflush(stdout);
	rig_open(&r, NULL, 0);
	b[0] = 42;
	memset(b + 1, 0xcd, sizeof(b) - 1);
	send_msg(r.fd, BT_EXTENSION, b, sizeof(b));
	expect_marker(&r, "unknown extension");
	rig_close(&r);
}

/* Build an extension handshake well over 1024 bytes: a long "v" string and
 * a padding key, with ut_pex after them so it is only seen if the whole
 * dictionary was parsed. */
static size_t
big_handshake(char *buf, size_t cap)
{
	char pad[3000];
	memset(pad, 'x', sizeof(pad));
	int n = snprintf(buf, cap, "d1:md6:ut_pexi7ee4:pad_%zu:%.*s1:v4:Testee",
	    sizeof(pad), (int)sizeof(pad), pad);
	CHECK(n > 0 && (size_t)n < cap, "snprintf");
	return (size_t)n;
}

/* A large extension handshake as the first message. */
static void
test_big_ext_handshake_first(void)
{
	struct rig r;
	char hs[4096];
	size_t n = big_handshake(hs, sizeof(hs));

	printf("test_big_ext_handshake_first\n"); fflush(stdout);
	rig_open(&r, hs, n);
	CHECK(mbt_peer_ut_pex_id(r.peer) == 7, "ut_pex id from big handshake: %u",
	    mbt_peer_ut_pex_id(r.peer));
	expect_marker(&r, "big ext handshake first");
	rig_close(&r);
}

/* A large extension handshake again, in the ACTIVE state. */
static void
test_big_ext_handshake_again(void)
{
	struct rig r;
	char hs[4096];
	uint8_t m[1 + 4096];
	size_t n = big_handshake(hs, sizeof(hs));

	printf("test_big_ext_handshake_again\n"); fflush(stdout);
	rig_open(&r, NULL, 0);
	m[0] = 0;
	memcpy(m + 1, hs, n);
	send_msg(r.fd, BT_EXTENSION, m, 1 + n);
	expect_marker(&r, "big ext handshake again");
	CHECK(mbt_peer_ut_pex_id(r.peer) == 7, "ut_pex id from repeat handshake: %u",
	    mbt_peer_ut_pex_id(r.peer));
	rig_close(&r);
}

/* A malformed extension handshake is ignored, not fatal, and consumed. */
static void
test_bad_ext_handshake(void)
{
	struct rig r;
	uint8_t m[1 + 20];

	printf("test_bad_ext_handshake\n"); fflush(stdout);
	rig_open(&r, NULL, 0);
	m[0] = 0;
	memset(m + 1, 'z', 20);
	send_msg(r.fd, BT_EXTENSION, m, sizeof(m));
	expect_marker(&r, "bad ext handshake");
	rig_close(&r);
}

static const struct {
	const char *name;
	void (*fn)(void);
} tests[] = {
	{ "have_trailing", test_have_trailing },
	{ "request_trailing", test_request_trailing },
	{ "have_short", test_have_short },
	{ "port_trailing", test_port_trailing },
	{ "unknown_message", test_unknown_message },
	{ "unknown_extension", test_unknown_extension },
	{ "big_ext_handshake_first", test_big_ext_handshake_first },
	{ "big_ext_handshake_again", test_big_ext_handshake_again },
	{ "bad_ext_handshake", test_bad_ext_handshake },
};

/* With no argument run every case; with a name, only that one. */
int
main(int argc, char **argv)
{
	signal(SIGPIPE, SIG_IGN);
	for (size_t i = 0; i < sizeof(tests) / sizeof(tests[0]); i++)
		if (argc < 2 || strcmp(argv[1], tests[i].name) == 0)
			tests[i].fn();
	printf("all peer wire tests passed\n");
	return 0;
}
