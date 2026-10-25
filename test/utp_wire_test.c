// SPDX-License-Identifier: ISC
/*
 * uTP wire-level receive tests: a hand-built remote peer feeds DATA packets
 * straight into mbt_utp_recv(), so the test controls payload sizes, sequence
 * numbers and arrival order exactly. Our own sender never produces payloads
 * above UTP_MSS, never wraps seq_nr in a short test and never reorders, so
 * the cases here (oversize payloads, wraparound, window edges, header
 * extensions) are the shapes a foreign stack such as libtorrent can put on
 * the wire and our loopback tests cannot.
 *
 * Every case ends by comparing the reassembled byte stream with what was
 * sent: a decoder bug shows up as a short or corrupted stream.
 */

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <errno.h>
#include <unistd.h>
#include <sys/socket.h>
#include <sys/time.h>
#include <netinet/in.h>
#include <arpa/inet.h>

#include "mbt/event.h"
#include "mbt/utp.h"
#include "mbt/conn.h"
#include "mbt/log.h"

#define CHECK(c, ...) do { if (!(c)) { \
	fprintf(stderr, "FAIL %s:%d: ", __FILE__, __LINE__); \
	fprintf(stderr, __VA_ARGS__); fprintf(stderr, "\n"); exit(1); } } while (0)

#define HDR 20
#define ST_DATA  0
#define ST_STATE 2
#define ST_SYN   4
#define WINDOW   32          /* default receive window, packets */
#define STREAM_MAX (1 << 20)

struct wire {
	struct mbt_ev *ev;
	struct mbt_utp_ctx *ctx;
	struct mbt_conn *conn;       /* server side of the connection */
	int raw;                     /* the remote peer's UDP socket */
	struct sockaddr_in peer;     /* its address, the source of every packet */
	uint16_t conn_id;            /* id the server expects on our DATA */
	uint16_t syn_seq;            /* seq_nr of our SYN */
	uint16_t srv_seq;            /* seq_nr the server put in its STATE */
	uint8_t  sent[STREAM_MAX];   /* stream the peer has sent so far */
	size_t   sent_len;
	uint8_t  got[STREAM_MAX];    /* stream the server has read so far */
	size_t   got_len;
};

static void
accept_cb(struct mbt_utp_ctx *ctx, struct mbt_conn *conn,
          const struct sockaddr *addr, socklen_t addrlen, void *arg)
{
	struct wire *w = arg;
	(void)ctx; (void)addr; (void)addrlen;
	w->conn = conn;
}

static void
put16(uint8_t *p, uint16_t v)
{
	p[0] = (uint8_t)(v >> 8);
	p[1] = (uint8_t)v;
}

static void
put32(uint8_t *p, uint32_t v)
{
	put16(p, (uint16_t)(v >> 16));
	put16(p + 2, (uint16_t)v);
}

/* Build one packet in buf; ext_len > 0 adds a SACK extension of that many
 * bytes (multiple of 4) between the header and the payload. */
static size_t
build(uint8_t *buf, int type, uint16_t conn_id, uint16_t seq, uint16_t ack,
      size_t ext_len, const uint8_t *payload, size_t plen)
{
	size_t off = HDR;

	memset(buf, 0, HDR);
	buf[0] = (uint8_t)((type << 4) | 1);
	buf[1] = ext_len ? 1 : 0;
	put16(buf + 2, conn_id);
	put32(buf + 4, 1000);
	put32(buf + 12, 1 << 20);
	put16(buf + 16, seq);
	put16(buf + 18, ack);
	if (ext_len) {
		buf[off] = 0;           /* no further extension */
		buf[off + 1] = (uint8_t)ext_len;
		memset(buf + off + 2, 0xa5, ext_len);
		off += 2 + ext_len;
	}
	if (plen)
		memcpy(buf + off, payload, plen);
	return off + plen;
}

static void
feed(struct wire *w, const uint8_t *pkt, size_t len)
{
	int rv = mbt_utp_recv(w->ctx, pkt, len,
	    (struct sockaddr *)&w->peer, sizeof(w->peer));
	CHECK(rv == 0, "mbt_utp_recv returned %d", rv);
}

/* Read whatever the server connection holds into w->got. */
static void
drain(struct wire *w)
{
	uint8_t tmp[4096];
	ssize_t n;

	CHECK(w->conn != NULL, "no connection accepted");
	while ((n = mbt_conn_read(w->conn, tmp, sizeof(tmp))) > 0) {
		CHECK(w->got_len + (size_t)n <= STREAM_MAX, "stream overflow");
		memcpy(w->got + w->got_len, tmp, (size_t)n);
		w->got_len += (size_t)n;
	}
}

static void
wire_open(struct wire *w, uint16_t syn_seq)
{
	struct sockaddr_in a;
	socklen_t al = sizeof(a);
	struct timeval tv = { .tv_sec = 2 };
	uint8_t pkt[HDR + 64], rx[2048];
	uint16_t port;
	ssize_t n;
	size_t len;
	int rv;

	memset(w, 0, sizeof(*w));
	rv = mbt_ev_new_default(&w->ev);
	CHECK(rv == 0, "ev_new");
	rv = mbt_utp_ctx_new(&w->ctx, w->ev, 0, accept_cb, w);
	CHECK(rv == 0, "utp_ctx_new");
	port = mbt_utp_ctx_port(w->ctx);

	w->raw = socket(AF_INET, SOCK_DGRAM, 0);
	CHECK(w->raw >= 0, "socket");
	setsockopt(w->raw, SOL_SOCKET, SO_RCVTIMEO, &tv, sizeof(tv));
	memset(&a, 0, sizeof(a));
	a.sin_family = AF_INET;
	a.sin_addr.s_addr = htonl(INADDR_LOOPBACK);
	CHECK(bind(w->raw, (struct sockaddr *)&a, sizeof(a)) == 0, "bind");
	CHECK(getsockname(w->raw, (struct sockaddr *)&w->peer, &al) == 0,
	    "getsockname");

	w->syn_seq = syn_seq;
	w->conn_id = 0x1234;     /* SYN carries X; DATA to the acceptor uses X+1 */
	len = build(pkt, ST_SYN, w->conn_id, syn_seq, 0, 0, NULL, 0);
	feed(w, pkt, len);

	/* The acceptor answers with a STATE; its seq_nr is what we ack. */
	memset(&a, 0, sizeof(a));
	al = sizeof(a);
	n = recvfrom(w->raw, rx, sizeof(rx), 0, (struct sockaddr *)&a, &al);
	CHECK(n >= HDR, "no STATE reply (%zd)", n);
	CHECK((rx[0] >> 4) == ST_STATE, "reply is not a STATE");
	w->srv_seq = (uint16_t)((rx[16] << 8) | rx[17]);
	w->conn_id++;
	CHECK(w->conn != NULL, "accept callback did not fire");
	(void)port;
}

static void
wire_close(struct wire *w)
{
	if (w->conn)
		mbt_conn_free(w->conn);
	mbt_utp_ctx_free(w->ctx);
	mbt_ev_destroy(w->ev);
	close(w->raw);
}

/* Deterministic payload: byte at stream offset i. */
static uint8_t
pat(size_t i)
{
	return (uint8_t)(i * 31 + 7);
}

/*
 * Send the chunk with index k (k = 0 is the first DATA packet, seq =
 * syn_seq + 1). The chunk covers stream bytes [off, off + len). Chunks must
 * be defined by the caller consistently: sizes[] gives every chunk length.
 */
struct plan {
	size_t n;
	size_t *size;
	size_t *off;
};

static void
plan_make(struct plan *p, size_t n, size_t (*sizefn)(size_t k))
{
	size_t off = 0;

	p->n = n;
	p->size = calloc(n, sizeof(size_t));
	p->off = calloc(n, sizeof(size_t));
	CHECK(p->size && p->off, "calloc");
	for (size_t k = 0; k < n; k++) {
		p->size[k] = sizefn(k);
		p->off[k] = off;
		off += p->size[k];
	}
}

static void
plan_free(struct plan *p)
{
	free(p->size);
	free(p->off);
}

static void
send_chunk(struct wire *w, const struct plan *p, size_t k, size_t ext_len)
{
	uint8_t payload[2048], pkt[HDR + 2 + 64 + 2048];
	uint16_t seq = (uint16_t)(w->syn_seq + 1 + k);
	size_t len;

	CHECK(p->size[k] <= sizeof(payload), "chunk too big");
	for (size_t i = 0; i < p->size[k]; i++)
		payload[i] = pat(p->off[k] + i);
	len = build(pkt, ST_DATA, w->conn_id, seq,
	    (uint16_t)(w->srv_seq - 1), ext_len, payload, p->size[k]);
	feed(w, pkt, len);
}

/* Expected stream: the first `chunks` chunks of the plan, in order. */
static void
expect_stream(struct wire *w, const struct plan *p, size_t chunks,
              const char *what)
{
	size_t want = 0;

	for (size_t k = 0; k < chunks; k++)
		want += p->size[k];
	drain(w);
	CHECK(w->got_len == want, "%s: stream is %zu bytes, want %zu", what,
	    w->got_len, want);
	for (size_t i = 0; i < want; i++)
		CHECK(w->got[i] == pat(i), "%s: byte %zu is %02x, want %02x",
		    what, i, w->got[i], pat(i));
}

static size_t size_1450(size_t k) { (void)k; return 1450; }
static size_t size_1000(size_t k) { (void)k; return 1000; }
static size_t size_500(size_t k) { (void)k; return 500; }

/* A payload above our own MSS, held back by a gap, then released by the
 * missing packet: every byte of the buffered one must survive. */
static void
test_oversize_out_of_order(void)
{
	struct wire w;
	struct plan p;

	printf("test_oversize_out_of_order\n"); fflush(stdout);
	wire_open(&w, 100);
	plan_make(&p, 3, size_1450);
	send_chunk(&w, &p, 1, 0);
	send_chunk(&w, &p, 2, 0);
	send_chunk(&w, &p, 0, 0);
	expect_stream(&w, &p, 3, "oversize ooo");
	plan_free(&p);
	wire_close(&w);
}

/* The same payload size in order, as the control: this path never clipped. */
static void
test_oversize_in_order(void)
{
	struct wire w;
	struct plan p;

	printf("test_oversize_in_order\n"); fflush(stdout);
	wire_open(&w, 200);
	plan_make(&p, 3, size_1450);
	for (size_t k = 0; k < 3; k++)
		send_chunk(&w, &p, k, 0);
	expect_stream(&w, &p, 3, "oversize in order");
	plan_free(&p);
	wire_close(&w);
}

/* 16-bit seq_nr wraps in the middle of a reordered burst. */
static void
test_seq_wrap_reorder(void)
{
	struct wire w;
	struct plan p;

	printf("test_seq_wrap_reorder\n"); fflush(stdout);
	wire_open(&w, 0xfffd);     /* DATA seqs ffff? no: fffe, ffff, 0, 1 */
	plan_make(&p, 4, size_1000);
	send_chunk(&w, &p, 3, 0);
	send_chunk(&w, &p, 2, 0);
	send_chunk(&w, &p, 1, 0);
	send_chunk(&w, &p, 0, 0);
	expect_stream(&w, &p, 4, "wrap reorder");
	plan_free(&p);
	wire_close(&w);
}

/* Fill the whole reorder window (every slot but the next expected one), then
 * the missing packet: the cascade has to deliver all of them in order. */
static void
test_full_window_cascade(void)
{
	struct wire w;
	struct plan p;

	printf("test_full_window_cascade\n"); fflush(stdout);
	wire_open(&w, 300);
	plan_make(&p, WINDOW + 10, size_500);
	for (size_t k = 1; k < WINDOW; k++)
		send_chunk(&w, &p, k, 0);
	send_chunk(&w, &p, 0, 0);
	expect_stream(&w, &p, WINDOW, "full window");
	plan_free(&p);
	wire_close(&w);
}

/* A packet beyond the reorder window must be dropped, not parked in a slot it
 * aliases; once the sender (here: us) resends it in range the stream is
 * intact and holds no junk. */
static void
test_beyond_window(void)
{
	struct wire w;
	struct plan p;
	size_t total = WINDOW + 10;

	printf("test_beyond_window\n"); fflush(stdout);
	wire_open(&w, 400);
	plan_make(&p, total, size_500);
	/* chunk WINDOW + 5 is ahead of ack_nr by WINDOW + 6: out of range */
	send_chunk(&w, &p, WINDOW + 5, 0);
	for (size_t k = 0; k < total; k++)
		send_chunk(&w, &p, k, 0);
	expect_stream(&w, &p, total, "beyond window");
	plan_free(&p);
	wire_close(&w);
}

/* DATA carrying a SACK extension: the payload starts after the extension
 * chain, in order and out of order. */
static void
test_data_with_extension(void)
{
	struct wire w;
	struct plan p;

	printf("test_data_with_extension\n"); fflush(stdout);
	wire_open(&w, 500);
	plan_make(&p, 4, size_1000);
	send_chunk(&w, &p, 0, 4);
	send_chunk(&w, &p, 2, 4);
	send_chunk(&w, &p, 3, 8);
	send_chunk(&w, &p, 1, 4);
	expect_stream(&w, &p, 4, "extension");
	plan_free(&p);
	wire_close(&w);
}

/* Duplicates and retransmits of already delivered or already buffered
 * packets must not add bytes. */
static void
test_duplicates(void)
{
	struct wire w;
	struct plan p;

	printf("test_duplicates\n"); fflush(stdout);
	wire_open(&w, 600);
	plan_make(&p, 4, size_1000);
	send_chunk(&w, &p, 2, 0);
	send_chunk(&w, &p, 2, 0);     /* duplicate of a buffered packet */
	send_chunk(&w, &p, 0, 0);
	send_chunk(&w, &p, 0, 0);     /* duplicate of a delivered packet */
	send_chunk(&w, &p, 1, 0);
	send_chunk(&w, &p, 3, 0);
	send_chunk(&w, &p, 3, 0);
	expect_stream(&w, &p, 4, "duplicates");
	plan_free(&p);
	wire_close(&w);
}

static uint32_t rng_state = 12345;

static uint32_t
rng(void)
{
	rng_state = rng_state * 1103515245u + 12345u;
	return (rng_state >> 16) & 0x7fff;
}

static size_t
size_random(size_t k)
{
	(void)k;
	return 1 + rng() % 1450;
}

/* Random sizes up to 1450, shuffled within groups that fit the window, with
 * the app draining between groups; seq_nr wraps part way through. */
static void
test_random_shuffle(void)
{
	enum { GROUP = 16, GROUPS = 40 };
	struct wire w;
	struct plan p;
	size_t order[GROUP];

	printf("test_random_shuffle\n"); fflush(stdout);
	wire_open(&w, 0xffe0);
	plan_make(&p, GROUP * GROUPS, size_random);
	for (size_t g = 0; g < GROUPS; g++) {
		for (size_t i = 0; i < GROUP; i++)
			order[i] = g * GROUP + i;
		for (size_t i = GROUP - 1; i > 0; i--) {
			size_t j = rng() % (i + 1), t = order[i];
			order[i] = order[j];
			order[j] = t;
		}
		for (size_t i = 0; i < GROUP; i++)
			send_chunk(&w, &p, order[i], (i % 3) ? 0 : 4);
		drain(&w);
	}
	expect_stream(&w, &p, GROUP * GROUPS, "random shuffle");
	plan_free(&p);
	wire_close(&w);
}

static size_t size_1600(size_t k) { (void)k; return 1600; }

/* A payload above the receive cap, arriving ahead of a gap, is dropped whole
 * and unacked. The stream that follows is intact and holds no clipped copy. */
static void
test_beyond_recv_cap(void)
{
	struct wire w;
	struct plan big, p;

	printf("test_beyond_recv_cap\n"); fflush(stdout);
	wire_open(&w, 700);
	plan_make(&big, 3, size_1600);
	plan_make(&p, 3, size_1000);
	send_chunk(&w, &big, 2, 0);    /* seq 703, 1600 bytes: over the cap */
	for (size_t k = 0; k < 3; k++)
		send_chunk(&w, &p, k, 0);
	expect_stream(&w, &p, 3, "beyond recv cap");
	plan_free(&big);
	plan_free(&p);
	wire_close(&w);
}

static const struct {
	const char *name;
	void (*fn)(void);
} tests[] = {
	{ "oversize_in_order", test_oversize_in_order },
	{ "oversize_out_of_order", test_oversize_out_of_order },
	{ "seq_wrap_reorder", test_seq_wrap_reorder },
	{ "full_window_cascade", test_full_window_cascade },
	{ "beyond_window", test_beyond_window },
	{ "data_with_extension", test_data_with_extension },
	{ "duplicates", test_duplicates },
	{ "beyond_recv_cap", test_beyond_recv_cap },
	{ "random_shuffle", test_random_shuffle },
};

/* With no argument run every case; with a name, only that one (a failing
 * case exits the process, so this is how to see the others). */
int
main(int argc, char **argv)
{
	if (getenv("MBT_UTP_TRACE")) {
		mbt_log_set(mbt_log_stderr, NULL);
		mbt_log_set_debug(MBT_LOG_UTP);
		mbt_log_set_trace(MBT_LOG_UTP);
	}
	for (size_t i = 0; i < sizeof(tests) / sizeof(tests[0]); i++)
		if (argc < 2 || strcmp(argv[1], tests[i].name) == 0)
			tests[i].fn();
	printf("all utp wire tests passed\n");
	return 0;
}
