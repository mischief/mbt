// SPDX-License-Identifier: ISC
/*
 * uTP throughput / counter-wrap test: push more than 4 GiB one-way over real
 * loopback UDP through a single uTP connection. The send/recv byte counters
 * (sbuf_*, rbuf_*) are 32-bit and monotonic, so they wrap at 2^32 (~4 GiB) on
 * a long-lived connection. A wrap-unsafe comparison in the send path used to
 * wedge the stream at exactly that boundary (observed in the field at
 * ~4000 MB). This drives a transfer past the wrap and asserts it keeps going.
 *
 * No data verification (we can't hold 4 GiB): the server just counts bytes.
 * Progress is printed every GiB to keep output sparse. Slow by design (uTP
 * loopback throughput is modest); run with a generous timeout.
 *
 * Set MBT_UTP_GB to override the target size in GiB (default 5).
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

static struct mbt_ev *g_ev;

#define GiB (1024ULL * 1024 * 1024)
#define CHUNK 16384

static uint64_t g_target;          /* bytes to transfer */
static uint64_t g_recv;            /* bytes received by server */
static uint64_t g_next_report;     /* next GiB boundary to print */
static struct mbt_conn *g_server_conn;

static void
server_readcb(struct mbt_conn *c, void *arg)
{
	(void)arg;
	uint8_t tmp[CHUNK];
	ssize_t n;
	while ((n = mbt_conn_read(c, tmp, sizeof(tmp))) > 0) {
		g_recv += (uint64_t)n;
		if (g_recv >= g_next_report) {
			printf("  recv %llu MB\n",
			       (unsigned long long)(g_recv / (1024 * 1024)));
			fflush(stdout);
			g_next_report += GiB;
		}
	}
	if (g_recv >= g_target)
		mbt_ev_stop(g_ev);
}

static void
server_accept(struct mbt_utp_ctx *ctx, struct mbt_conn *conn,
              const struct sockaddr *addr, socklen_t addrlen, void *arg)
{
	(void)ctx; (void)addr; (void)addrlen; (void)arg;
	g_server_conn = conn;
	mbt_conn_set_callbacks(conn, server_readcb, NULL, NULL);
	mbt_conn_set_watermark(conn, 1);
}

struct client { uint8_t buf[CHUNK]; uint64_t sent; };

static void client_writecb(struct mbt_conn *c, void *arg);

static void
client_pump(struct mbt_conn *c, struct client *st)
{
	while (st->sent < g_target) {
		size_t chunk = sizeof(st->buf);
		if ((uint64_t)chunk > g_target - st->sent)
			chunk = (size_t)(g_target - st->sent);
		ssize_t n = mbt_conn_write(c, st->buf, chunk);
		if (n == -ENOBUFS) {
			mbt_conn_set_writecb(c, client_writecb, st);
			return;
		}
		if (n < 0)
			return;
		st->sent += (uint64_t)n;
	}
}

static void
client_writecb(struct mbt_conn *c, void *arg)
{
	client_pump(c, arg);
}

static void
client_eventcb(struct mbt_conn *c, int err, void *arg)
{
	struct client *st = arg;
	if (err < 0) { mbt_ev_stop(g_ev); return; }
	client_pump(c, st);
}

static void
make_loopback_addr(struct sockaddr_in *a, uint16_t port)
{
	memset(a, 0, sizeof(*a));
	a->sin_family = AF_INET;
	a->sin_port   = htons(port);
	a->sin_addr.s_addr = htonl(INADDR_LOOPBACK);
}

static void
timeout_cb(struct mbt_ev_timer *t, void *arg)
{
	(void)t; (void)arg;
	fprintf(stderr, "throughput test timed out at recv=%llu MB (target %llu MB)\n",
	        (unsigned long long)(g_recv / (1024 * 1024)),
	        (unsigned long long)(g_target / (1024 * 1024)));
	mbt_ev_stop(g_ev);
}

int
main(void)
{
	if (getenv("MBT_UTP_TRACE")) {
		mbt_log_set(mbt_log_stderr, NULL);
		mbt_log_set_debug(MBT_LOG_UTP);
		mbt_log_set_trace(MBT_LOG_UTP);
	}

	unsigned gb = 5;
	const char *env = getenv("MBT_UTP_GB");
	if (env) { unsigned v = (unsigned)atoi(env); if (v) gb = v; }
	g_target      = (uint64_t)gb * GiB;
	g_recv        = 0;
	g_next_report = GiB;
	g_server_conn = NULL;

	printf("utp throughput: pushing %u GiB one-way (crosses the 4 GiB counter wrap)\n", gb);
	fflush(stdout);

	int rv = mbt_ev_new_default(&g_ev);
	assert(rv == 0);

	struct mbt_utp_ctx *server_ctx = NULL, *client_ctx = NULL;
	rv = mbt_utp_ctx_new(&server_ctx, g_ev, 0, server_accept, NULL);
	assert(rv == 0);
	rv = mbt_utp_ctx_new(&client_ctx, g_ev, 0, NULL, NULL);
	assert(rv == 0);

	uint16_t sp = mbt_utp_ctx_port(server_ctx);

	struct client *st = calloc(1, sizeof(*st));
	assert(st);
	for (size_t i = 0; i < sizeof(st->buf); i++)
		st->buf[i] = (uint8_t)(i * 31 + 7);

	struct sockaddr_in dst;
	make_loopback_addr(&dst, sp);

	struct mbt_conn *client_conn = NULL;
	rv = mbt_utp_connect(client_ctx, &client_conn,
	                     (struct sockaddr *)&dst, sizeof(dst),
	                     NULL, client_eventcb, st);
	assert(rv == 0);

	/* Generous wall-clock cap (overridable). The point is to detect a wedge,
	 * not to benchmark; if it stops making progress it will sit idle until
	 * this fires. */
	struct mbt_ev_timer *to = NULL;
	mbt_ev_timer_new(g_ev, &to, 600ULL * 1000000ULL, timeout_cb, NULL);

	mbt_ev_run(g_ev);
	mbt_ev_timer_free(to);

	printf("final: recv %llu MB of %llu MB\n",
	       (unsigned long long)(g_recv / (1024 * 1024)),
	       (unsigned long long)(g_target / (1024 * 1024)));

	int ok = (g_recv >= g_target);

	mbt_conn_free(client_conn);
	if (g_server_conn) mbt_conn_free(g_server_conn);
	mbt_utp_ctx_free(client_ctx);
	mbt_utp_ctx_free(server_ctx);
	mbt_ev_destroy(g_ev);
	free(st);

	if (!ok) {
		fprintf(stderr, "FAIL: transfer wedged before completion\n");
		return 1;
	}
	printf("PASS utp throughput (crossed 4 GiB wrap)\n");
	return 0;
}
