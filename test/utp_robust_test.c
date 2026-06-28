// SPDX-License-Identifier: ISC
/*
 * uTP robustness tests: large one-way transfers under fault injection that
 * the basic utp_test harness cannot produce -- deep reorder (packets far out
 * of order) and tail loss (a specific high-seq packet dropped until
 * retransmitted).
 *
 * These target utp.md items 1 (window / SACK-range / reorder-capacity
 * consistency) and 1b (retransmit must cover the actual hole, not just
 * send_una). They are EXPECTED TO HANG/FAIL against the current
 * implementation and to pass after the window-unification fix; until then this
 * binary is marked an expected failure in meson (should_fail).
 *
 * Uses a TX hook to deliver packets directly via mbt_utp_recv(), so delivery
 * is deterministic with no real network I/O.
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

#define PIPE_HOLD_MAX 64
struct held_pkt { uint8_t buf[20 + 1450]; size_t len; };

struct test_pipe {
	struct mbt_utp_ctx *peer;
	struct sockaddr_in  src_addr;
	int duplicate;

	/* deep reorder: hold up to `deep` DATA packets, then release reversed */
	int             deep;
	struct held_pkt held[PIPE_HOLD_MAX];
	int             held_n;
	struct mbt_ev_timer *flush_timer;  /* deferred reverse-flush of held[] */

	/* tail loss: drop DATA with this exact seq the first drop_seq_times
	 * times it is seen (then let retransmits through). 0 times = off. */
	int drop_seq;
	int drop_seq_times;
};

static struct mbt_ev *g_ev;

static void
pipe_deliver(struct test_pipe *p, const void *buf, size_t len)
{
	mbt_utp_recv(p->peer, buf, len,
	             (struct sockaddr *)&p->src_addr, sizeof(p->src_addr));
	if (p->duplicate)
		mbt_utp_recv(p->peer, buf, len,
		             (struct sockaddr *)&p->src_addr, sizeof(p->src_addr));
}

/* deferred reverse-flush of held deep-reorder packets */
static void
pipe_flush_cb(struct mbt_ev_timer *t, void *arg)
{
	struct test_pipe *p = arg;
	p->flush_timer = NULL;
	mbt_ev_timer_free(t);
	for (int i = p->held_n - 1; i >= 0; i--)
		pipe_deliver(p, p->held[i].buf, p->held[i].len);
	p->held_n = 0;
}

static int
pipe_hook(struct mbt_utp_ctx *ctx, const void *buf, size_t len,
          const struct sockaddr *dst, socklen_t dstlen, void *arg)
{
	(void)ctx; (void)dst; (void)dstlen;
	struct test_pipe *p = arg;
	uint8_t type = PKT_TYPE(buf);

	if (p->drop_seq_times > 0 && type == ST_DATA &&
	    pkt_seq(buf) == (uint16_t)p->drop_seq) {
		p->drop_seq_times--;
		return 0; /* tail-loss: swallow this seq */
	}

	if (p->deep && type == ST_DATA) {
		if (p->held_n < p->deep && p->held_n < PIPE_HOLD_MAX) {
			struct held_pkt *h = &p->held[p->held_n++];
			memcpy(h->buf, buf, len);
			h->len = len;
			/* ensure held packets always drain, even at end of stream:
			 * schedule a deferred reverse-flush. delivering reversed
			 * within each batch produces deep out-of-order arrival with
			 * no permanently-stranded tail. */
			if (!p->flush_timer)
				mbt_ev_timer_new(g_ev, &p->flush_timer, 0,
				                 pipe_flush_cb, p);
			return 0;
		}
		/* buffer full: flush held packets in reverse, then this one */
		for (int i = p->held_n - 1; i >= 0; i--)
			pipe_deliver(p, p->held[i].buf, p->held[i].len);
		p->held_n = 0;
		pipe_deliver(p, buf, len);
		return 0;
	}

	pipe_deliver(p, buf, len);
	return 0;
}

static void
make_loopback_addr(struct sockaddr_in *a, uint16_t port)
{
	memset(a, 0, sizeof(*a));
	a->sin_family = AF_INET;
	a->sin_port   = htons(port);
	a->sin_addr.s_addr = htonl(INADDR_LOOPBACK);
}

#define OW_TOTAL (256 * 1024)   /* well over one window of data */
static int ow_received;

static void
timeout_cb(struct mbt_ev_timer *t, void *arg)
{
	(void)t; (void)arg;
	fprintf(stderr, "test timed out! ow_received=%d / %d\n", ow_received, OW_TOTAL);
	mbt_ev_stop(g_ev);
}

/* ---- one-way transfer harness ---- */

static uint8_t  ow_recv_buf[OW_TOTAL];
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
run_oneway(const char *name, struct test_pipe *c2s_cfg)
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
	uint16_t cp = mbt_utp_ctx_port(client_ctx);

	struct test_pipe c2s = *c2s_cfg, s2c = {0};
	make_loopback_addr(&c2s.src_addr, cp);
	c2s.peer = server_ctx;
	make_loopback_addr(&s2c.src_addr, sp);
	s2c.peer = client_ctx;

	mbt_utp_ctx_set_tx_hook(client_ctx, pipe_hook, &c2s);
	mbt_utp_ctx_set_tx_hook(server_ctx, pipe_hook, &s2c);

	struct sockaddr_in dst;
	make_loopback_addr(&dst, sp);

	struct mbt_conn *client_conn = NULL;
	rv = mbt_utp_connect(client_ctx, &client_conn,
	                     (struct sockaddr *)&dst, sizeof(dst),
	                     NULL, ow_client_eventcb, &st);
	assert(rv == 0);

	struct mbt_ev_timer *to = NULL;
	mbt_ev_timer_new(g_ev, &to, 15000000, timeout_cb, NULL);

	mbt_ev_run(g_ev);
	mbt_ev_timer_free(to);
	if (c2s.flush_timer) mbt_ev_timer_free(c2s.flush_timer);
	if (s2c.flush_timer) mbt_ev_timer_free(s2c.flush_timer);

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
	/* hold 16 DATA packets then release reversed. depth must stay below the
	 * receiver's window (default 32) -- a conforming sender never has more
	 * than a window in flight, so holding >= window would just stall the
	 * sender (and this harness) waiting for acks we are holding. 16-deep
	 * reversed is still far past the adjacent-swap the old harness did. */
	struct test_pipe cfg = {0};
	cfg.deep = 16;
	run_oneway("deep_reorder", &cfg);
}

static void
test_tail_loss(void)
{
	/* drop one specific mid/high seq twice, forcing the sender to identify
	 * and retransmit a hole that is not at send_una. */
	struct test_pipe cfg = {0};
	cfg.drop_seq = 50;
	cfg.drop_seq_times = 2;
	run_oneway("tail_loss", &cfg);
}

int
main(void)
{
	/* MBT_UTP_TRACE=1 enables uTP debug+trace to stderr for diagnosis. */
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
