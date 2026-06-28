// SPDX-License-Identifier: ISC
/*
 * Comprehensive uTP test suite.
 *
 * Uses TX hooks to bypass real UDP: packets are delivered directly via
 * mbt_utp_recv(), making tests deterministic with no real network I/O.
 */

#include <assert.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>
#include <netinet/in.h>
#include <arpa/inet.h>

#include "mbt/event.h"
#include "mbt/utp.h"
#include "mbt/conn.h"

/* packet type nibble (for TX hook filtering) */
#define PKT_TYPE(buf) (((const uint8_t *)(buf))[0] >> 4)
#define ST_DATA  0

/* ---- test_pipe: delivers packets directly between two uTP contexts ---- */

struct test_pipe {
	struct mbt_utp_ctx  *peer;         /* deliver packets to this ctx */
	struct sockaddr_in   src_addr;     /* pretend source address */

	/* fault injection */
	int  drop_nth;        /* drop every Nth DATA packet (0 = none) */
	int  pkt_data_count;  /* count of DATA packets seen */
	int  duplicate;       /* deliver each packet twice */

	/* reorder: buffer one, deliver after next */
	int     reorder;
	uint8_t pending[20 + 1450]; /* UTP_HEADER_SIZE + UTP_MAX_PACKET_SIZE */
	size_t  pending_len;
};

static int
pipe_hook(struct mbt_utp_ctx *ctx, const void *buf, size_t len,
          const struct sockaddr *dst, socklen_t dstlen, void *arg)
{
	(void)ctx; (void)dst; (void)dstlen;
	struct test_pipe *p = arg;
	uint8_t type = PKT_TYPE(buf);

	if (p->drop_nth && type == ST_DATA) {
		p->pkt_data_count++;
		if (p->pkt_data_count % p->drop_nth == 0)
			return 0; /* drop */
	}

	if (p->reorder && type == ST_DATA) {
		if (!p->pending_len) {
			memcpy(p->pending, buf, len);
			p->pending_len = len;
			return 0; /* buffer, don't deliver yet */
		} else {
			/* deliver current first (reversed order), then pending */
			mbt_utp_recv(p->peer, buf, len,
			             (struct sockaddr *)&p->src_addr, sizeof(p->src_addr));
			if (p->duplicate)
				mbt_utp_recv(p->peer, buf, len,
				             (struct sockaddr *)&p->src_addr, sizeof(p->src_addr));
			mbt_utp_recv(p->peer, p->pending, p->pending_len,
			             (struct sockaddr *)&p->src_addr, sizeof(p->src_addr));
			p->pending_len = 0;
			return 0;
		}
	}

	mbt_utp_recv(p->peer, buf, len,
	             (struct sockaddr *)&p->src_addr, sizeof(p->src_addr));
	if (p->duplicate)
		mbt_utp_recv(p->peer, buf, len,
		             (struct sockaddr *)&p->src_addr, sizeof(p->src_addr));
	return 0;
}

/* Flush any pending buffered packet in a reorder pipe */
static void
pipe_flush(struct test_pipe *p)
{
	if (p->pending_len) {
		mbt_utp_recv(p->peer, p->pending, p->pending_len,
		             (struct sockaddr *)&p->src_addr, sizeof(p->src_addr));
		p->pending_len = 0;
	}
}

/* ---- common test scaffolding ---- */

static struct mbt_ev *g_ev;

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

	/* Install cross-connected TX hooks */
	struct test_pipe c2s = {0}, s2c = {0};
	make_loopback_addr(&c2s.src_addr, client_port);
	c2s.peer = server_ctx;
	make_loopback_addr(&s2c.src_addr, server_port);
	s2c.peer = client_ctx;

	mbt_utp_ctx_set_tx_hook(client_ctx, pipe_hook, &c2s);
	mbt_utp_ctx_set_tx_hook(server_ctx, pipe_hook, &s2c);

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

	struct test_pipe c2s = {0}, s2c = {0};
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

	struct test_pipe c2s = {0}, s2c = {0};
	make_loopback_addr(&c2s.src_addr, cp);
	c2s.peer      = server_ctx;
	c2s.duplicate = 1;   /* duplicate all client->server packets */
	make_loopback_addr(&s2c.src_addr, sp);
	s2c.peer      = client_ctx;
	s2c.duplicate = 1;

	mbt_utp_ctx_set_tx_hook(client_ctx, pipe_hook, &c2s);
	mbt_utp_ctx_set_tx_hook(server_ctx, pipe_hook, &s2c);

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

	struct test_pipe c2s = {0}, s2c = {0};
	make_loopback_addr(&c2s.src_addr, cp);
	c2s.peer     = server_ctx;
	c2s.drop_nth = 4;   /* drop every 4th DATA packet */
	make_loopback_addr(&s2c.src_addr, sp);
	s2c.peer = client_ctx;

	mbt_utp_ctx_set_tx_hook(client_ctx, pipe_hook, &c2s);
	mbt_utp_ctx_set_tx_hook(server_ctx, pipe_hook, &s2c);

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

	pipe_flush(&c2s);

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

	struct test_pipe c2s = {0}, s2c = {0};
	make_loopback_addr(&c2s.src_addr, cp);
	c2s.peer    = server_ctx;
	c2s.reorder = 1;   /* swap pairs of DATA packets */
	make_loopback_addr(&s2c.src_addr, sp);
	s2c.peer = client_ctx;

	mbt_utp_ctx_set_tx_hook(client_ctx, pipe_hook, &c2s);
	mbt_utp_ctx_set_tx_hook(server_ctx, pipe_hook, &s2c);

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

	/* Flush any pending reordered packet */
	pipe_flush(&c2s);

	mbt_ev_timer_free(to);

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

	struct test_pipe c2s = {0}, s2c = {0};
	make_loopback_addr(&c2s.src_addr, cp);
	c2s.peer = server_ctx;
	make_loopback_addr(&s2c.src_addr, sp);
	s2c.peer = client_ctx;

	mbt_utp_ctx_set_tx_hook(client_ctx, pipe_hook, &c2s);
	mbt_utp_ctx_set_tx_hook(server_ctx, pipe_hook, &s2c);

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

	printf("all utp tests passed\n");
	return 0;
}
