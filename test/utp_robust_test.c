// SPDX-License-Identifier: ISC
/*
 * uTP robustness tests: large one-way transfers over REAL loopback UDP, with
 * fault injection on the receive path -- deep reorder (datagrams delivered far
 * out of order) and tail loss (a specific high-seq packet dropped until
 * retransmitted).
 *
 * These run over the same real sendto/recvfrom + event-loop path the swarm and
 * production use (no TX hook, no synchronous in-process delivery). Faults are
 * injected via mbt_utp_ctx_set_rx_hook on the receiver: the hook sees each
 * datagram as it arrives and may drop it, or stash it and re-inject it later
 * (reorder) from a 0ms timer. delivery stays asynchronous, so there is no
 * send->recv->send reentrancy.
 *
 * Targets utp.md items 1 (window/SACK/reorder consistency) and 1b (retransmit
 * the actual hole, not just send_una), plus the congestion fixes.
 */

#include <assert.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <errno.h>
#include <netinet/in.h>
#include <arpa/inet.h>

#include "mbt/event.h"
#include "mbt/utp.h"
#include "mbt/conn.h"
#include "mbt/log.h"

#define PKT_TYPE(buf) (((const uint8_t *)(buf))[0] >> 4)
#define ST_DATA  0

/* seq_nr lives at byte offset 16 in the uTP header, big-endian */
static inline uint16_t
pkt_seq(const void *buf)
{
	const uint8_t *b = buf;
	return (uint16_t)((b[16] << 8) | b[17]);
}

static struct mbt_ev *g_ev;

/* ---- rx fault injector (installed on the receiver ctx) ---- */

#define RX_HOLD_MAX 64
struct held_pkt {
	uint8_t buf[20 + 1500];
	size_t  len;
	struct sockaddr_storage src;
	socklen_t srclen;
};

struct rx_fault {
	struct mbt_utp_ctx *ctx;   /* ctx to re-inject into */

	/* tail loss: drop DATA with this seq the first drop_seq_times times. */
	int drop_seq;
	int drop_seq_times;

	/* deep reorder: hold up to `deep` DATA datagrams, then release reversed
	 * via a 0ms timer. delivering reversed within each batch produces deep
	 * out-of-order arrival; the timer guarantees the tail always drains. */
	int             deep;
	struct held_pkt held[RX_HOLD_MAX];
	int             held_n;
	struct mbt_ev_timer *flush_timer;
	int             reinjecting;   /* guard: pass through during re-injection */
};

static void
rx_reinject(struct rx_fault *f)
{
	f->reinjecting = 1;
	for (int i = f->held_n - 1; i >= 0; i--) {
		struct held_pkt *h = &f->held[i];
		mbt_utp_recv(f->ctx, h->buf, h->len,
		             (struct sockaddr *)&h->src, h->srclen);
	}
	f->held_n = 0;
	f->reinjecting = 0;
}

static void
rx_flush_cb(struct mbt_ev_timer *t, void *arg)
{
	struct rx_fault *f = arg;
	f->flush_timer = NULL;
	mbt_ev_timer_free(t);
	rx_reinject(f);
}

/* return 1 = deliver, 0 = drop/suppress (handled out of band) */
static int
rx_hook(struct mbt_utp_ctx *ctx, const void *buf, size_t len,
        const struct sockaddr *src, socklen_t srclen, void *arg)
{
	struct rx_fault *f = arg;
	uint8_t type = (len >= 20) ? PKT_TYPE(buf) : 0xff;

	/* during re-injection, deliver verbatim -- don't re-hold/re-drop. */
	if (f->reinjecting)
		return 1;

	if (f->drop_seq_times > 0 && type == ST_DATA && len >= 20 &&
	    pkt_seq(buf) == (uint16_t)f->drop_seq) {
		f->drop_seq_times--;
		return 0; /* tail-loss */
	}

	if (f->deep && type == ST_DATA && len <= sizeof(((struct held_pkt *)0)->buf)) {
		if (f->held_n < f->deep && f->held_n < RX_HOLD_MAX) {
			struct held_pkt *h = &f->held[f->held_n++];
			memcpy(h->buf, buf, len);
			h->len = len;
			memcpy(&h->src, src, srclen);
			h->srclen = srclen;
			if (!f->flush_timer)
				mbt_ev_timer_new(g_ev, &f->flush_timer, 0, rx_flush_cb, f);
			return 0; /* hold, re-injected reversed by the timer */
		}
		/* buffer full: flush reversed now, then deliver this one */
		rx_reinject(f);
		return 1;
	}

	return 1;
}

/* ---- common ---- */

static void
timeout_cb(struct mbt_ev_timer *t, void *arg)
{
	(void)t; (void)arg;
	fprintf(stderr, "test timed out!\n");
	mbt_ev_stop(g_ev);
}

static void
make_loopback_addr(struct sockaddr_in *a, uint16_t port)
{
	memset(a, 0, sizeof(*a));
	a->sin_family = AF_INET;
	a->sin_port   = htons(port);
	a->sin_addr.s_addr = htonl(INADDR_LOOPBACK);
}

/* ---- one-way transfer harness ---- */

#define OW_TOTAL (256 * 1024)

static uint8_t  ow_recv_buf[OW_TOTAL];
static int      ow_received;
static struct mbt_conn *ow_server_conn;

static void
ow_server_readcb(struct mbt_conn *c, void *arg)
{
	(void)arg;
	uint8_t tmp[8192];
	ssize_t n;
	while ((n = mbt_conn_read(c, tmp, sizeof(tmp))) > 0) {
		if (ow_received + (int)n <= OW_TOTAL)
			memcpy(ow_recv_buf + ow_received, tmp, (size_t)n);
		ow_received += (int)n;
	}
	if (ow_received >= OW_TOTAL)
		mbt_ev_stop(g_ev);
}

static void
ow_server_accept(struct mbt_utp_ctx *ctx, struct mbt_conn *conn,
                 const struct sockaddr *addr, socklen_t addrlen, void *arg)
{
	(void)ctx; (void)addr; (void)addrlen; (void)arg;
	ow_server_conn = conn;
	mbt_conn_set_callbacks(conn, ow_server_readcb, NULL, NULL);
	mbt_conn_set_watermark(conn, 1);
}

struct ow_client { const uint8_t *data; size_t off; };

static void ow_client_writecb(struct mbt_conn *c, void *arg);

static void
ow_client_pump(struct mbt_conn *c, struct ow_client *st)
{
	while (st->off < OW_TOTAL) {
		size_t chunk = OW_TOTAL - st->off;
		if (chunk > 8192) chunk = 8192;
		ssize_t n = mbt_conn_write(c, st->data + st->off, chunk);
		if (n == -ENOBUFS) {
			mbt_conn_set_writecb(c, ow_client_writecb, st);
			return;
		}
		if (n < 0) return;
		st->off += (size_t)n;
	}
}

static void
ow_client_writecb(struct mbt_conn *c, void *arg)
{
	ow_client_pump(c, arg);
}

static void
ow_client_eventcb(struct mbt_conn *c, int err, void *arg)
{
	struct ow_client *st = arg;
	if (err < 0) { mbt_ev_stop(g_ev); return; }
	ow_client_pump(c, st);
}

static void
run_oneway(const char *name, struct rx_fault *fault)
{
	printf("test_%s\n", name); fflush(stdout);

	ow_received    = 0;
	ow_server_conn = NULL;
	memset(ow_recv_buf, 0, sizeof(ow_recv_buf));

	uint8_t *send_data = malloc(OW_TOTAL);
	for (int i = 0; i < OW_TOTAL; i++)
		send_data[i] = (uint8_t)(i * 31 + 7);
	struct ow_client st = { .data = send_data, .off = 0 };

	int rv = mbt_ev_new_default(&g_ev);
	assert(rv == 0);

	struct mbt_utp_ctx *server_ctx = NULL, *client_ctx = NULL;
	rv = mbt_utp_ctx_new(&server_ctx, g_ev, 0, ow_server_accept, NULL);
	assert(rv == 0);
	rv = mbt_utp_ctx_new(&client_ctx, g_ev, 0, NULL, NULL);
	assert(rv == 0);

	uint16_t sp = mbt_utp_ctx_port(server_ctx);

	/* fault injection on the receiver (server) only */
	fault->ctx = server_ctx;
	mbt_utp_ctx_set_rx_hook(server_ctx, rx_hook, fault);

	struct sockaddr_in dst;
	make_loopback_addr(&dst, sp);

	struct mbt_conn *client_conn = NULL;
	rv = mbt_utp_connect(client_ctx, &client_conn,
	                     (struct sockaddr *)&dst, sizeof(dst),
	                     NULL, ow_client_eventcb, &st);
	assert(rv == 0);

	struct mbt_ev_timer *to = NULL;
	mbt_ev_timer_new(g_ev, &to, 30000000, timeout_cb, NULL);

	mbt_ev_run(g_ev);
	mbt_ev_timer_free(to);
	if (fault->flush_timer) mbt_ev_timer_free(fault->flush_timer);

	assert(ow_received == OW_TOTAL);
	assert(memcmp(ow_recv_buf, send_data, OW_TOTAL) == 0);

	mbt_conn_free(client_conn);
	if (ow_server_conn) mbt_conn_free(ow_server_conn);
	mbt_utp_ctx_free(client_ctx);
	mbt_utp_ctx_free(server_ctx);
	mbt_ev_destroy(g_ev);
	free(send_data);

	printf("PASS %s\n", name); fflush(stdout);
}

static void
test_deep_reorder(void)
{
	/* hold 16 DATA datagrams then release reversed. depth stays below the
	 * receiver's window (default 32) so the sender keeps flowing. */
	struct rx_fault f = {0};
	f.deep = 16;
	run_oneway("deep_reorder", &f);
}

static void
test_tail_loss(void)
{
	/* drop a specific mid/high seq twice, forcing the sender to retransmit
	 * a hole that is not at send_una. */
	struct rx_fault f = {0};
	f.drop_seq = 50;
	f.drop_seq_times = 2;
	run_oneway("tail_loss", &f);
}

int
main(void)
{
	if (getenv("MBT_UTP_TRACE")) {
		mbt_log_set(mbt_log_stderr, NULL);
		mbt_log_set_debug(MBT_LOG_UTP);
		mbt_log_set_trace(MBT_LOG_UTP);
	}
	test_deep_reorder();
	test_tail_loss();
	printf("all utp robustness tests passed\n");
	return 0;
}
