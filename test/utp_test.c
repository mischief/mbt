// SPDX-License-Identifier: ISC
/*
 * Comprehensive uTP test suite.
 *
 * Runs over real loopback UDP (the same sendto/recvfrom + event-loop path as
 * production). Faults (loss, duplication, reorder) are injected on the receive
 * path via mbt_utp_ctx_set_rx_hook, installed on whichever context receives the
 * traffic being faulted.
 */

#include <assert.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <errno.h>
#include <unistd.h>
#include <netinet/in.h>
#include <arpa/inet.h>

#include "mbt/event.h"
#include "mbt/utp.h"
#include "mbt/conn.h"

#define PKT_TYPE(buf) (((const uint8_t *)(buf))[0] >> 4)
#define ST_DATA  0

static struct mbt_ev *g_ev;

/* ---- rx fault injector ---- */

struct rx_fault {
	struct mbt_utp_ctx *ctx;   /* ctx this hook is installed on */

	int drop_nth;        /* drop every Nth DATA datagram (0 = none) */
	int data_count;      /* DATA datagrams seen */
	int duplicate;       /* deliver each DATA datagram twice */

	int reorder;         /* swap adjacent DATA: hold one, release after next */
	uint8_t pend[20 + 1500];
	size_t  pend_len;
	struct sockaddr_storage pend_src;
	socklen_t pend_srclen;
	struct mbt_ev_timer *flush_timer;  /* releases a stranded held packet */

	int reinjecting;     /* guard so re-injected datagrams pass through */
};

static void
rx_reorder_flush_cb(struct mbt_ev_timer *t, void *arg)
{
	struct rx_fault *f = arg;
	f->flush_timer = NULL;
	mbt_ev_timer_free(t);
	if (f->pend_len) {
		f->reinjecting = 1;
		mbt_utp_recv(f->ctx, f->pend, f->pend_len,
		             (struct sockaddr *)&f->pend_src, f->pend_srclen);
		f->reinjecting = 0;
		f->pend_len = 0;
	}
}

static int
rx_hook(struct mbt_utp_ctx *ctx, const void *buf, size_t len,
        const struct sockaddr *src, socklen_t srclen, void *arg)
{
	struct rx_fault *f = arg;
	uint8_t type = (len >= 20) ? PKT_TYPE(buf) : 0xff;

	if (f->reinjecting)
		return 1;

	if (f->drop_nth && type == ST_DATA) {
		f->data_count++;
		if (f->data_count % f->drop_nth == 0)
			return 0; /* drop */
	}

	if (f->reorder && type == ST_DATA && len <= sizeof(f->pend)) {
		if (!f->pend_len) {
			memcpy(f->pend, buf, len);
			f->pend_len = len;
			memcpy(&f->pend_src, src, srclen);
			f->pend_srclen = srclen;
			/* release even if no "next" arrives (e.g. single/last pkt) */
			if (!f->flush_timer)
				mbt_ev_timer_new(g_ev, &f->flush_timer, 0,
				                 rx_reorder_flush_cb, f);
			return 0; /* hold */
		}
		/* deliver this one, then the held one (reversed). */
		if (f->flush_timer) {
			mbt_ev_timer_free(f->flush_timer);
			f->flush_timer = NULL;
		}
		if (f->duplicate)
			mbt_utp_recv(ctx, buf, len, src, srclen);
		f->reinjecting = 1;
		mbt_utp_recv(ctx, f->pend, f->pend_len,
		             (struct sockaddr *)&f->pend_src, f->pend_srclen);
		f->reinjecting = 0;
		f->pend_len = 0;
		return 1; /* the current datagram is delivered normally by caller */
	}

	if (f->duplicate && type == ST_DATA) {
		f->reinjecting = 1;
		mbt_utp_recv(ctx, buf, len, src, srclen);
		f->reinjecting = 0;
	}

	return 1;
}

static void
make_loopback_addr(struct sockaddr_in *a, uint16_t port)
{
	memset(a, 0, sizeof(*a));
	a->sin_family = AF_INET;
	a->sin_port   = htons(port);
	a->sin_addr.s_addr = htonl(INADDR_LOOPBACK);
}

/* Hard timeout to prevent test hangs */
static void
timeout_cb(struct mbt_ev_timer *t, void *arg)
{
	(void)t; (void)arg;
	fprintf(stderr, "test timed out!\n");
	mbt_ev_stop(g_ev);
}

/* ---- test_basic ---- */

#define PAYLOAD     "hello utp"
#define PAYLOAD_LEN 9

static struct mbt_conn *basic_server_conn;
static int basic_server_received;
static int basic_client_received;

static void
basic_server_readcb(struct mbt_conn *c, void *arg)
{
	(void)arg;
	char buf[64];
	ssize_t n = mbt_conn_read(c, buf, sizeof(buf));
	if (n <= 0) return;
	basic_server_received += (int)n;
	mbt_conn_write(c, buf, (size_t)n);
}

static void
basic_server_eventcb(struct mbt_conn *c, int err, void *arg)
{
	(void)c; (void)arg;
	if (err < 0) mbt_ev_stop(g_ev);
}

static void
basic_server_accept(struct mbt_utp_ctx *ctx, struct mbt_conn *conn,
                    const struct sockaddr *addr, socklen_t addrlen, void *arg)
{
	(void)ctx; (void)addr; (void)addrlen; (void)arg;
	basic_server_conn = conn;
	mbt_conn_set_callbacks(conn, basic_server_readcb, basic_server_eventcb, NULL);
	mbt_conn_set_watermark(conn, 1);
}

static void
basic_client_readcb(struct mbt_conn *c, void *arg)
{
	(void)arg;
	char buf[64];
	ssize_t n = mbt_conn_read(c, buf, sizeof(buf));
	if (n <= 0) return;
	basic_client_received += (int)n;
	if (basic_client_received >= PAYLOAD_LEN)
		mbt_ev_stop(g_ev);
}

static void
basic_client_eventcb(struct mbt_conn *c, int err, void *arg)
{
	(void)arg;
	if (err < 0) { mbt_ev_stop(g_ev); return; }
	mbt_conn_write(c, PAYLOAD, PAYLOAD_LEN);
}

static void
test_basic(void)
{
	printf("test_basic\n"); fflush(stdout);

	basic_server_conn     = NULL;
	basic_server_received = 0;
	basic_client_received = 0;

	int rv = mbt_ev_new_default(&g_ev);
	assert(rv == 0);

	struct mbt_utp_ctx *server_ctx = NULL, *client_ctx = NULL;

	rv = mbt_utp_ctx_new(&server_ctx, g_ev, 0, basic_server_accept, NULL);
	assert(rv == 0);
	uint16_t server_port = mbt_utp_ctx_port(server_ctx);
	assert(server_port != 0);

	rv = mbt_utp_ctx_new(&client_ctx, g_ev, 0, NULL, NULL);
	assert(rv == 0);
	uint16_t client_port = mbt_utp_ctx_port(client_ctx);
	(void)client_port;

	/* real loopback UDP, no fault injection */

	struct sockaddr_in dst;
	make_loopback_addr(&dst, server_port);

	struct mbt_conn *client_conn = NULL;
	rv = mbt_utp_connect(client_ctx, &client_conn,
	                     (struct sockaddr *)&dst, sizeof(dst),
	                     basic_client_readcb, basic_client_eventcb, NULL);
	assert(rv == 0);
	mbt_conn_set_watermark(client_conn, 1);

	/* Hard timeout */
	struct mbt_ev_timer *to = NULL;
	mbt_ev_timer_new(g_ev, &to, 3000000, timeout_cb, NULL);

	mbt_ev_run(g_ev);

	mbt_ev_timer_free(to);

	assert(basic_server_received == PAYLOAD_LEN);
	assert(basic_client_received == PAYLOAD_LEN);

	mbt_conn_free(client_conn);
	if (basic_server_conn) mbt_conn_free(basic_server_conn);
	mbt_utp_ctx_free(client_ctx);
	mbt_utp_ctx_free(server_ctx);
	mbt_ev_destroy(g_ev);

	printf("PASS basic\n"); fflush(stdout);
}

/* ---- test_large: 32KB transfer ---- */

#define LARGE_SIZE (32 * 1024)

static struct mbt_conn *large_server_conn;
static uint8_t large_server_buf[LARGE_SIZE];
static int large_server_received;
static int large_client_done;

static void
large_server_readcb(struct mbt_conn *c, void *arg)
{
	(void)arg;
	uint8_t tmp[4096];
	ssize_t n;
	while ((n = mbt_conn_read(c, tmp, sizeof(tmp))) > 0) {
		memcpy(large_server_buf + large_server_received, tmp, (size_t)n);
		large_server_received += (int)n;
	}
	if (large_server_received >= LARGE_SIZE)
		mbt_ev_stop(g_ev);
}

static void
large_server_eventcb(struct mbt_conn *c, int err, void *arg)
{
	(void)c; (void)arg;
	if (err < 0) mbt_ev_stop(g_ev);
}

static void
large_server_accept(struct mbt_utp_ctx *ctx, struct mbt_conn *conn,
                    const struct sockaddr *addr, socklen_t addrlen, void *arg)
{
	(void)ctx; (void)addr; (void)addrlen; (void)arg;
	large_server_conn = conn;
	mbt_conn_set_callbacks(conn, large_server_readcb, large_server_eventcb, NULL);
	mbt_conn_set_watermark(conn, 1);
}

static void
large_client_eventcb(struct mbt_conn *c, int err, void *arg)
{
	uint8_t *data = arg;
	if (err < 0) { mbt_ev_stop(g_ev); return; }
	/* Write 32KB in chunks */
	size_t off = 0;
	while (off < LARGE_SIZE) {
		ssize_t n = mbt_conn_write(c, data + off, LARGE_SIZE - off);
		if (n <= 0) break;
		off += (size_t)n;
	}
	large_client_done = 1;
}

static void
test_large(void)
{
	printf("test_large\n"); fflush(stdout);

	large_server_conn     = NULL;
	large_server_received = 0;
	large_client_done     = 0;
	memset(large_server_buf, 0, sizeof(large_server_buf));

	/* Generate deterministic test data */
	uint8_t send_data[LARGE_SIZE];
	for (int i = 0; i < LARGE_SIZE; i++)
		send_data[i] = (uint8_t)(i * 7 + 13);

	int rv = mbt_ev_new_default(&g_ev);
	assert(rv == 0);

	struct mbt_utp_ctx *server_ctx = NULL, *client_ctx = NULL;
	rv = mbt_utp_ctx_new(&server_ctx, g_ev, 0, large_server_accept, NULL);
	assert(rv == 0);
	rv = mbt_utp_ctx_new(&client_ctx, g_ev, 0, NULL, NULL);
	assert(rv == 0);

	uint16_t sp = mbt_utp_ctx_port(server_ctx);
	uint16_t cp = mbt_utp_ctx_port(client_ctx);
	(void)cp;

	/* real loopback UDP, no fault injection */

	struct sockaddr_in dst;
	make_loopback_addr(&dst, sp);

	struct mbt_conn *client_conn = NULL;
	rv = mbt_utp_connect(client_ctx, &client_conn,
	                     (struct sockaddr *)&dst, sizeof(dst),
	                     NULL, large_client_eventcb, send_data);
	assert(rv == 0);

	struct mbt_ev_timer *to = NULL;
	mbt_ev_timer_new(g_ev, &to, 5000000, timeout_cb, NULL);

	mbt_ev_run(g_ev);

	mbt_ev_timer_free(to);

	assert(large_server_received == LARGE_SIZE);
	assert(memcmp(large_server_buf, send_data, LARGE_SIZE) == 0);

	mbt_conn_free(client_conn);
	if (large_server_conn) mbt_conn_free(large_server_conn);
	mbt_utp_ctx_free(client_ctx);
	mbt_utp_ctx_free(server_ctx);
	mbt_ev_destroy(g_ev);

	printf("PASS large\n"); fflush(stdout);
}

/* ---- test_duplicate: every packet delivered twice ---- */

static struct mbt_conn *dup_server_conn;
static uint8_t dup_server_buf[PAYLOAD_LEN];
static int dup_server_received;

static void
dup_server_readcb(struct mbt_conn *c, void *arg)
{
	(void)arg;
	uint8_t tmp[64];
	ssize_t n = mbt_conn_read(c, tmp, sizeof(tmp));
	if (n <= 0) return;
	memcpy(dup_server_buf + dup_server_received, tmp, (size_t)n);
	dup_server_received += (int)n;
	if (dup_server_received >= PAYLOAD_LEN)
		mbt_ev_stop(g_ev);
}

static void
dup_server_accept(struct mbt_utp_ctx *ctx, struct mbt_conn *conn,
                  const struct sockaddr *addr, socklen_t addrlen, void *arg)
{
	(void)ctx; (void)addr; (void)addrlen; (void)arg;
	dup_server_conn = conn;
	mbt_conn_set_callbacks(conn, dup_server_readcb, NULL, NULL);
	mbt_conn_set_watermark(conn, 1);
}

static void
dup_client_eventcb(struct mbt_conn *c, int err, void *arg)
{
	(void)arg;
	if (err < 0) { mbt_ev_stop(g_ev); return; }
	mbt_conn_write(c, PAYLOAD, PAYLOAD_LEN);
}

static void
test_duplicate(void)
{
	printf("test_duplicate\n"); fflush(stdout);

	dup_server_conn     = NULL;
	dup_server_received = 0;
	memset(dup_server_buf, 0, sizeof(dup_server_buf));

	int rv = mbt_ev_new_default(&g_ev);
	assert(rv == 0);

	struct mbt_utp_ctx *server_ctx = NULL, *client_ctx = NULL;
	rv = mbt_utp_ctx_new(&server_ctx, g_ev, 0, dup_server_accept, NULL);
	assert(rv == 0);
	rv = mbt_utp_ctx_new(&client_ctx, g_ev, 0, NULL, NULL);
	assert(rv == 0);

	uint16_t sp = mbt_utp_ctx_port(server_ctx);
	uint16_t cp = mbt_utp_ctx_port(client_ctx);
	(void)cp;

	struct rx_fault sf = {0}, cf = {0};
	sf.ctx = server_ctx; sf.duplicate = 1;  /* dup c2s arriving at server */
	cf.ctx = client_ctx; cf.duplicate = 1;  /* dup s2c arriving at client */
	mbt_utp_ctx_set_rx_hook(server_ctx, rx_hook, &sf);
	mbt_utp_ctx_set_rx_hook(client_ctx, rx_hook, &cf);

	struct sockaddr_in dst;
	make_loopback_addr(&dst, sp);

	struct mbt_conn *client_conn = NULL;
	rv = mbt_utp_connect(client_ctx, &client_conn,
	                     (struct sockaddr *)&dst, sizeof(dst),
	                     NULL, dup_client_eventcb, NULL);
	assert(rv == 0);

	struct mbt_ev_timer *to = NULL;
	mbt_ev_timer_new(g_ev, &to, 3000000, timeout_cb, NULL);

	mbt_ev_run(g_ev);

	mbt_ev_timer_free(to);

	assert(dup_server_received == PAYLOAD_LEN);
	assert(memcmp(dup_server_buf, PAYLOAD, PAYLOAD_LEN) == 0);

	mbt_conn_free(client_conn);
	if (dup_server_conn) mbt_conn_free(dup_server_conn);
	mbt_utp_ctx_free(client_ctx);
	mbt_utp_ctx_free(server_ctx);
	mbt_ev_destroy(g_ev);

	printf("PASS duplicate\n"); fflush(stdout);
}

/* ---- test_loss: drop every 4th DATA packet ---- */

#define LOSS_SIZE 4096

static struct mbt_conn *loss_server_conn;
static uint8_t loss_server_buf[LOSS_SIZE];
static int loss_server_received;

static void
loss_server_readcb(struct mbt_conn *c, void *arg)
{
	(void)arg;
	uint8_t tmp[512];
	ssize_t n;
	while ((n = mbt_conn_read(c, tmp, sizeof(tmp))) > 0) {
		memcpy(loss_server_buf + loss_server_received, tmp, (size_t)n);
		loss_server_received += (int)n;
	}
	if (loss_server_received >= LOSS_SIZE)
		mbt_ev_stop(g_ev);
}

static void
loss_server_accept(struct mbt_utp_ctx *ctx, struct mbt_conn *conn,
                   const struct sockaddr *addr, socklen_t addrlen, void *arg)
{
	(void)ctx; (void)addr; (void)addrlen; (void)arg;
	loss_server_conn = conn;
	mbt_conn_set_callbacks(conn, loss_server_readcb, NULL, NULL);
	mbt_conn_set_watermark(conn, 1);
}

static void
loss_client_eventcb(struct mbt_conn *c, int err, void *arg)
{
	uint8_t *data = arg;
	if (err < 0) { mbt_ev_stop(g_ev); return; }
	size_t off = 0;
	while (off < LOSS_SIZE) {
		ssize_t n = mbt_conn_write(c, data + off, LOSS_SIZE - off);
		if (n <= 0) break;
		off += (size_t)n;
	}
}

static void
test_loss(void)
{
	printf("test_loss\n"); fflush(stdout);

	loss_server_conn     = NULL;
	loss_server_received = 0;
	memset(loss_server_buf, 0, sizeof(loss_server_buf));

	uint8_t send_data[LOSS_SIZE];
	for (int i = 0; i < LOSS_SIZE; i++)
		send_data[i] = (uint8_t)(i * 3 + 7);

	int rv = mbt_ev_new_default(&g_ev);
	assert(rv == 0);

	struct mbt_utp_ctx *server_ctx = NULL, *client_ctx = NULL;
	rv = mbt_utp_ctx_new(&server_ctx, g_ev, 0, loss_server_accept, NULL);
	assert(rv == 0);
	rv = mbt_utp_ctx_new(&client_ctx, g_ev, 0, NULL, NULL);
	assert(rv == 0);

	uint16_t sp = mbt_utp_ctx_port(server_ctx);
	uint16_t cp = mbt_utp_ctx_port(client_ctx);
	(void)cp;

	struct rx_fault sf = {0};
	sf.ctx = server_ctx; sf.drop_nth = 4;  /* drop every 4th DATA at server */
	mbt_utp_ctx_set_rx_hook(server_ctx, rx_hook, &sf);

	struct sockaddr_in dst;
	make_loopback_addr(&dst, sp);

	struct mbt_conn *client_conn = NULL;
	rv = mbt_utp_connect(client_ctx, &client_conn,
	                     (struct sockaddr *)&dst, sizeof(dst),
	                     NULL, loss_client_eventcb, send_data);
	assert(rv == 0);

	/* Allow up to 5 seconds for RTO retransmits */
	struct mbt_ev_timer *to = NULL;
	mbt_ev_timer_new(g_ev, &to, 5000000, timeout_cb, NULL);

	mbt_ev_run(g_ev);

	mbt_ev_timer_free(to);

	assert(loss_server_received == LOSS_SIZE);
	assert(memcmp(loss_server_buf, send_data, LOSS_SIZE) == 0);

	mbt_conn_free(client_conn);
	if (loss_server_conn) mbt_conn_free(loss_server_conn);
	mbt_utp_ctx_free(client_ctx);
	mbt_utp_ctx_free(server_ctx);
	mbt_ev_destroy(g_ev);

	printf("PASS loss\n"); fflush(stdout);
}

/* ---- test_out_of_order: swap pairs of DATA packets ---- */

static struct mbt_conn *ooo_server_conn;
static uint8_t ooo_server_buf[PAYLOAD_LEN];
static int ooo_server_received;

static void
ooo_server_readcb(struct mbt_conn *c, void *arg)
{
	(void)arg;
	uint8_t tmp[64];
	ssize_t n;
	while ((n = mbt_conn_read(c, tmp, sizeof(tmp))) > 0) {
		memcpy(ooo_server_buf + ooo_server_received, tmp, (size_t)n);
		ooo_server_received += (int)n;
	}
	if (ooo_server_received >= PAYLOAD_LEN)
		mbt_ev_stop(g_ev);
}

static void
ooo_server_accept(struct mbt_utp_ctx *ctx, struct mbt_conn *conn,
                  const struct sockaddr *addr, socklen_t addrlen, void *arg)
{
	(void)ctx; (void)addr; (void)addrlen; (void)arg;
	ooo_server_conn = conn;
	mbt_conn_set_callbacks(conn, ooo_server_readcb, NULL, NULL);
	mbt_conn_set_watermark(conn, 1);
}

static void
ooo_client_eventcb(struct mbt_conn *c, int err, void *arg)
{
	(void)arg;
	if (err < 0) { mbt_ev_stop(g_ev); return; }
	mbt_conn_write(c, PAYLOAD, PAYLOAD_LEN);
}

static void
test_out_of_order(void)
{
	printf("test_out_of_order\n"); fflush(stdout);

	ooo_server_conn     = NULL;
	ooo_server_received = 0;
	memset(ooo_server_buf, 0, sizeof(ooo_server_buf));

	int rv = mbt_ev_new_default(&g_ev);
	assert(rv == 0);

	struct mbt_utp_ctx *server_ctx = NULL, *client_ctx = NULL;
	rv = mbt_utp_ctx_new(&server_ctx, g_ev, 0, ooo_server_accept, NULL);
	assert(rv == 0);
	rv = mbt_utp_ctx_new(&client_ctx, g_ev, 0, NULL, NULL);
	assert(rv == 0);

	uint16_t sp = mbt_utp_ctx_port(server_ctx);
	uint16_t cp = mbt_utp_ctx_port(client_ctx);
	(void)cp;

	struct rx_fault sf = {0};
	sf.ctx = server_ctx; sf.reorder = 1;  /* swap adjacent DATA at server */
	mbt_utp_ctx_set_rx_hook(server_ctx, rx_hook, &sf);

	struct sockaddr_in dst;
	make_loopback_addr(&dst, sp);

	struct mbt_conn *client_conn = NULL;
	rv = mbt_utp_connect(client_ctx, &client_conn,
	                     (struct sockaddr *)&dst, sizeof(dst),
	                     NULL, ooo_client_eventcb, NULL);
	assert(rv == 0);

	struct mbt_ev_timer *to = NULL;
	mbt_ev_timer_new(g_ev, &to, 3000000, timeout_cb, NULL);

	mbt_ev_run(g_ev);

	mbt_ev_timer_free(to);
	if (sf.flush_timer) mbt_ev_timer_free(sf.flush_timer);

	assert(ooo_server_received == PAYLOAD_LEN);
	assert(memcmp(ooo_server_buf, PAYLOAD, PAYLOAD_LEN) == 0);

	mbt_conn_free(client_conn);
	if (ooo_server_conn) mbt_conn_free(ooo_server_conn);
	mbt_utp_ctx_free(client_ctx);
	mbt_utp_ctx_free(server_ctx);
	mbt_ev_destroy(g_ev);

	printf("PASS out_of_order\n"); fflush(stdout);
}

/* ---- test_fin: client sends data then closes; server sees EOF ---- */

static struct mbt_conn *fin_server_conn;
static int fin_server_received;
static int fin_server_got_eof;

static void
fin_server_readcb(struct mbt_conn *c, void *arg)
{
	(void)arg;
	char buf[64];
	ssize_t n = mbt_conn_read(c, buf, sizeof(buf));
	if (n > 0) fin_server_received += (int)n;
}

static void
fin_server_eventcb(struct mbt_conn *c, int err, void *arg)
{
	(void)c; (void)arg;
	if (err == 0) {
		fin_server_got_eof = 1;
		mbt_ev_stop(g_ev);
	} else {
		mbt_ev_stop(g_ev);
	}
}

static void
fin_server_accept(struct mbt_utp_ctx *ctx, struct mbt_conn *conn,
                  const struct sockaddr *addr, socklen_t addrlen, void *arg)
{
	(void)ctx; (void)addr; (void)addrlen; (void)arg;
	fin_server_conn = conn;
	mbt_conn_set_callbacks(conn, fin_server_readcb, fin_server_eventcb, NULL);
	mbt_conn_set_watermark(conn, 1);
}

static struct mbt_conn *fin_client_conn;

static void
fin_client_eventcb(struct mbt_conn *c, int err, void *arg)
{
	(void)arg;
	if (err < 0) { mbt_ev_stop(g_ev); return; }
	mbt_conn_write(c, PAYLOAD, PAYLOAD_LEN);
	/* Free the connection to send FIN */
	fin_client_conn = NULL;
	mbt_conn_free(c);
}

static void
test_fin(void)
{
	printf("test_fin\n"); fflush(stdout);

	fin_server_conn     = NULL;
	fin_server_received = 0;
	fin_server_got_eof  = 0;
	fin_client_conn     = NULL;

	int rv = mbt_ev_new_default(&g_ev);
	assert(rv == 0);

	struct mbt_utp_ctx *server_ctx = NULL, *client_ctx = NULL;
	rv = mbt_utp_ctx_new(&server_ctx, g_ev, 0, fin_server_accept, NULL);
	assert(rv == 0);
	rv = mbt_utp_ctx_new(&client_ctx, g_ev, 0, NULL, NULL);
	assert(rv == 0);

	uint16_t sp = mbt_utp_ctx_port(server_ctx);
	uint16_t cp = mbt_utp_ctx_port(client_ctx);
	(void)cp;

	/* real loopback UDP, no fault injection */

	struct sockaddr_in dst;
	make_loopback_addr(&dst, sp);

	rv = mbt_utp_connect(client_ctx, &fin_client_conn,
	                     (struct sockaddr *)&dst, sizeof(dst),
	                     NULL, fin_client_eventcb, NULL);
	assert(rv == 0);

	struct mbt_ev_timer *to = NULL;
	mbt_ev_timer_new(g_ev, &to, 5000000, timeout_cb, NULL);

	mbt_ev_run(g_ev);

	mbt_ev_timer_free(to);

	assert(fin_server_got_eof == 1);

	if (fin_server_conn) mbt_conn_free(fin_server_conn);
	/* fin_client_conn already freed in eventcb */
	mbt_utp_ctx_free(client_ctx);
	mbt_utp_ctx_free(server_ctx);
	mbt_ev_destroy(g_ev);

	printf("PASS fin\n"); fflush(stdout);
}

/* ---- test_flow_control: transfer 2x rbuf capacity with a slow reader ----
 *
 * The receiver defers draining its rbuf to a timer, letting it fill up.
 * This exercises the flow-control path: sender must pause when peer_wnd=0
 * and resume only after the receiver sends a window update on drain.
 * Without the fix (fake-ACKing dropped packets), data is silently lost
 * and the transfer stalls permanently.
 */

#define FC_TOTAL  (128 * 1024)   /* 2x RBUF_CAP */

static struct mbt_conn *fc_server_conn;
static uint8_t fc_server_buf[FC_TOTAL];
static int fc_server_received;

static void fc_drain_cb(struct mbt_ev_timer *t, void *arg);

static void
fc_server_readcb(struct mbt_conn *c, void *arg)
{
	(void)arg;
	/* Defer the drain: let rbuf stay full so the sender hits peer_wnd=0 */
	struct mbt_ev_timer *dt = NULL;
	mbt_ev_timer_new(g_ev, &dt, 0, fc_drain_cb, c);
	(void)dt;
}

static void
fc_drain_cb(struct mbt_ev_timer *t, void *arg)
{
	struct mbt_conn *c = arg;
	mbt_ev_timer_free(t);

	uint8_t tmp[4096];
	ssize_t n;
	while ((n = mbt_conn_read(c, tmp, sizeof(tmp))) > 0) {
		memcpy(fc_server_buf + fc_server_received, tmp, (size_t)n);
		fc_server_received += (int)n;
	}
	if (fc_server_received >= FC_TOTAL)
		mbt_ev_stop(g_ev);
}

static void
fc_server_accept(struct mbt_utp_ctx *ctx, struct mbt_conn *conn,
                 const struct sockaddr *addr, socklen_t addrlen, void *arg)
{
	(void)ctx; (void)addr; (void)addrlen; (void)arg;
	fc_server_conn = conn;
	mbt_conn_set_callbacks(conn, fc_server_readcb, NULL, NULL);
	mbt_conn_set_watermark(conn, 1);
}

struct fc_client_state {
	uint8_t *data;
	size_t   off;
};

static void fc_client_writecb(struct mbt_conn *c, void *arg);

static void
fc_client_send(struct mbt_conn *c, struct fc_client_state *st)
{
	while (st->off < FC_TOTAL) {
		size_t want = FC_TOTAL - st->off;
		if (want > 16 * 1024) want = 16 * 1024;
		ssize_t n = mbt_conn_write(c, st->data + st->off, want);
		if (n == -ENOBUFS) {
			mbt_conn_set_writecb(c, fc_client_writecb, st);
			return;
		}
		if (n <= 0) { mbt_ev_stop(g_ev); return; }
		st->off += (size_t)n;
	}
}

static void
fc_client_writecb(struct mbt_conn *c, void *arg)
{
	fc_client_send(c, arg);
}

static void
fc_client_eventcb(struct mbt_conn *c, int err, void *arg)
{
	if (err < 0) { mbt_ev_stop(g_ev); return; }
	fc_client_send(c, arg);
}

static void
test_flow_control(void)
{
	printf("test_flow_control\n"); fflush(stdout);

	fc_server_conn     = NULL;
	fc_server_received = 0;
	memset(fc_server_buf, 0, sizeof(fc_server_buf));

	uint8_t *send_data = malloc(FC_TOTAL);
	for (int i = 0; i < FC_TOTAL; i++)
		send_data[i] = (uint8_t)(i * 11 + 3);

	struct fc_client_state fc_st = { .data = send_data, .off = 0 };

	int rv = mbt_ev_new_default(&g_ev);
	assert(rv == 0);

	struct mbt_utp_ctx *server_ctx = NULL, *client_ctx = NULL;
	rv = mbt_utp_ctx_new(&server_ctx, g_ev, 0, fc_server_accept, NULL);
	assert(rv == 0);
	rv = mbt_utp_ctx_new(&client_ctx, g_ev, 0, NULL, NULL);
	assert(rv == 0);

	uint16_t sp = mbt_utp_ctx_port(server_ctx);
	uint16_t cp = mbt_utp_ctx_port(client_ctx);
	(void)cp;

	/* real loopback UDP, no fault injection */

	struct sockaddr_in dst;
	make_loopback_addr(&dst, sp);

	struct mbt_conn *client_conn = NULL;
	rv = mbt_utp_connect(client_ctx, &client_conn,
	                     (struct sockaddr *)&dst, sizeof(dst),
	                     NULL, fc_client_eventcb, &fc_st);
	assert(rv == 0);

	/* Allow time for RTO retransmits if needed */
	struct mbt_ev_timer *to = NULL;
	mbt_ev_timer_new(g_ev, &to, 10000000, timeout_cb, NULL);

	mbt_ev_run(g_ev);

	mbt_ev_timer_free(to);

	assert(fc_server_received == FC_TOTAL);
	assert(memcmp(fc_server_buf, send_data, FC_TOTAL) == 0);

	mbt_conn_free(client_conn);
	if (fc_server_conn) mbt_conn_free(fc_server_conn);
	mbt_utp_ctx_free(client_ctx);
	mbt_utp_ctx_free(server_ctx);
	mbt_ev_destroy(g_ev);
	free(send_data);

	printf("PASS flow_control\n"); fflush(stdout);
}

/* ---- main ---- */

int
main(void)
{
	test_basic();
	test_large();
	test_duplicate();
	test_loss();
	test_out_of_order();
	test_fin();
	test_flow_control();

	printf("all utp tests passed\n");
	return 0;
}
