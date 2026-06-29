// SPDX-License-Identifier: ISC
/*
 * TCP throughput control test: push >4 GiB one-way through the TCP mbt_conn
 * path (conn_sd / conn_*), over a loopback TCP socket, using the same
 * event loop, app-level read/write, and progress accounting as the uTP
 * throughput test. It is a CONTROL for utp_throughput_test: comparing the two
 * isolates uTP's per-packet protocol overhead (per-packet ACK/send syscalls)
 * from the cost common to both paths (event loop, clocksource, the kernel
 * socket copies).
 *
 * The TCP conn buffers writes internally without bound (never returns
 * -ENOBUFS), so we pace the writer by capping bytes in flight (sent - recv)
 * and resuming from a 0ms timer -- otherwise we would balloon userspace memory.
 *
 * 'slow' suite, excluded from the default run. MBT_TCP_GB overrides size (5).
 */
#include <assert.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <errno.h>
#include <unistd.h>
#include <netinet/in.h>
#include <arpa/inet.h>
#include <sys/socket.h>
#include <fcntl.h>

#include "mbt/event.h"
#include "mbt/conn.h"

static struct mbt_ev *g_ev;

#define GiB (1024ULL * 1024 * 1024)
#define CHUNK 16384
#define INFLIGHT_CAP (4 * 1024 * 1024)   /* cap userspace buffering */

static uint64_t g_target;
static uint64_t g_recv;
static uint64_t g_next_report;
static struct mbt_conn *g_cli, *g_srv;
static uint8_t g_buf[CHUNK];
static uint64_t g_sent;
static struct mbt_ev_timer *g_pump_timer;
static int g_ack_mode;        /* 1 = receiver writes a 1-byte ack per chunk */
static uint64_t g_acked;      /* bytes the sender has seen acked (ack_mode) */
static size_t g_chunk = CHUNK; /* per-write size; shrink to ~MSS to match uTP */

static void pump(void);

static void
pump_timer_cb(struct mbt_ev_timer *t, void *arg)
{
	(void)arg;
	mbt_ev_timer_free(t);
	g_pump_timer = NULL;
	pump();
}

static void
pump(void)
{
	while (g_sent < g_target) {
		/* In ack mode the writer is clocked by acks (like uTP): don't get
		 * more than INFLIGHT_CAP ahead of acked bytes. In plain mode, cap how
		 * far ahead of the receiver we buffer. */
		uint64_t ahead = g_ack_mode ? (g_sent - g_acked) : (g_sent - g_recv);
		if (ahead >= INFLIGHT_CAP) {
			if (!g_pump_timer)
				mbt_ev_timer_new(g_ev, &g_pump_timer, 1000, pump_timer_cb, NULL);
			return;
		}
		size_t chunk = g_chunk;
		if ((uint64_t)chunk > g_target - g_sent)
			chunk = (size_t)(g_target - g_sent);
		ssize_t n = mbt_conn_write(g_cli, g_buf, chunk);
		if (n < 0) {
			if (n == -EAGAIN || n == -ENOBUFS) {
				if (!g_pump_timer)
					mbt_ev_timer_new(g_ev, &g_pump_timer, 1000, pump_timer_cb, NULL);
				return;
			}
			mbt_ev_stop(g_ev);
			return;
		}
		g_sent += (uint64_t)n;
	}
}

static void
srv_readcb(struct mbt_conn *c, void *arg)
{
	(void)arg;
	uint8_t tmp[CHUNK];
	ssize_t n;
	while ((n = mbt_conn_read(c, tmp, sizeof(tmp))) > 0) {
		uint64_t before = g_recv;
		g_recv += (uint64_t)n;
		if (g_ack_mode) {
			/* One ack byte per CHUNK boundary crossed -- mirrors uTP sending
			 * one ack datagram per received data packet (one write() each). */
			uint64_t acks = (g_recv / g_chunk) - (before / g_chunk);
			uint8_t a = 1;
			for (uint64_t i = 0; i < acks; i++)
				mbt_conn_write(c, &a, 1);
		}
		if (g_recv >= g_next_report) {
			printf("  recv %llu MB\n",
			       (unsigned long long)(g_recv / (1024 * 1024)));
			fflush(stdout);
			g_next_report += GiB;
		}
	}
	if (g_recv >= g_target) { mbt_ev_stop(g_ev); return; }
	if (!g_ack_mode)
		pump();  /* plain mode: receiver drain unblocks the writer */
}

/* ack-mode: sender drains the 1-byte acks and is clocked by them */
static void
cli_readcb(struct mbt_conn *c, void *arg)
{
	(void)arg;
	uint8_t tmp[4096];
	ssize_t n;
	while ((n = mbt_conn_read(c, tmp, sizeof(tmp))) > 0)
		g_acked += (uint64_t)n * g_chunk;  /* each ack byte = one chunk acked */
	pump();
}

static void
cli_eventcb(struct mbt_conn *c, int err, void *arg)
{
	(void)c; (void)arg;
	if (err < 0) mbt_ev_stop(g_ev);
}

static void
srv_eventcb(struct mbt_conn *c, int err, void *arg)
{
	(void)c; (void)arg;
	if (err < 0) mbt_ev_stop(g_ev);
}

static void
timeout_cb(struct mbt_ev_timer *t, void *arg)
{
	(void)t; (void)arg;
	fprintf(stderr, "tcp throughput timed out at recv=%llu MB (target %llu MB)\n",
	        (unsigned long long)(g_recv / (1024 * 1024)),
	        (unsigned long long)(g_target / (1024 * 1024)));
	mbt_ev_stop(g_ev);
}

/* connected loopback TCP fd pair via listen/connect/accept */
static int
tcp_pair(int *cli_fd, int *srv_fd)
{
	int ln = socket(AF_INET, SOCK_STREAM, 0);
	if (ln < 0) return -1;
	int one = 1; setsockopt(ln, SOL_SOCKET, SO_REUSEADDR, &one, sizeof(one));
	struct sockaddr_in a; memset(&a, 0, sizeof(a));
	a.sin_family = AF_INET; a.sin_addr.s_addr = htonl(INADDR_LOOPBACK); a.sin_port = 0;
	if (bind(ln, (struct sockaddr *)&a, sizeof(a)) < 0) return -1;
	socklen_t al = sizeof(a);
	if (getsockname(ln, (struct sockaddr *)&a, &al) < 0) return -1;
	if (listen(ln, 1) < 0) return -1;

	int cf = socket(AF_INET, SOCK_STREAM, 0);
	if (cf < 0) return -1;
	/* nonblocking connect on loopback completes effectively immediately */
	if (connect(cf, (struct sockaddr *)&a, sizeof(a)) < 0 && errno != EINPROGRESS)
		return -1;
	int sf = accept(ln, NULL, NULL);
	if (sf < 0) return -1;
	close(ln);
	*cli_fd = cf; *srv_fd = sf;
	return 0;
}

int
main(void)
{
	unsigned gb = 5;
	const char *env = getenv("MBT_TCP_GB");
	if (env) { unsigned v = (unsigned)atoi(env); if (v) gb = v; }
	g_ack_mode = getenv("MBT_TCP_ACK") ? 1 : 0;
	{ const char *cs = getenv("MBT_TCP_CHUNK"); if (cs) { int v = atoi(cs); if (v > 0 && (size_t)v <= sizeof(g_buf)) g_chunk = (size_t)v; } }
	g_target = (uint64_t)gb * GiB;
	g_recv = 0; g_sent = 0; g_acked = 0; g_next_report = GiB;

	for (size_t i = 0; i < sizeof(g_buf); i++)
		g_buf[i] = (uint8_t)(i * 31 + 7);

	printf("tcp throughput: pushing %u GiB one-way (control vs uTP)%s\n", gb,
	       g_ack_mode ? " [ack-clocked]" : "");
	fflush(stdout);

	int rv = mbt_ev_new_default(&g_ev);
	assert(rv == 0);

	int cf, sf;
	rv = tcp_pair(&cf, &sf);
	assert(rv == 0);

	rv = mbt_conn_new(g_ev, &g_srv, sf, srv_readcb, srv_eventcb, NULL);
	assert(rv == 0);
	mbt_conn_set_watermark(g_srv, 1);

	rv = mbt_conn_new(g_ev, &g_cli, cf, g_ack_mode ? cli_readcb : NULL,
	                  cli_eventcb, NULL);
	assert(rv == 0);
	if (g_ack_mode)
		mbt_conn_set_watermark(g_cli, 1);

	struct mbt_ev_timer *to = NULL;
	mbt_ev_timer_new(g_ev, &to, 600ULL * 1000000ULL, timeout_cb, NULL);

	pump();          /* kick off */
	mbt_ev_run(g_ev);
	mbt_ev_timer_free(to);
	if (g_pump_timer) mbt_ev_timer_free(g_pump_timer);

	printf("final: recv %llu MB of %llu MB\n",
	       (unsigned long long)(g_recv / (1024 * 1024)),
	       (unsigned long long)(g_target / (1024 * 1024)));

	int ok = (g_recv >= g_target);
	mbt_conn_free(g_cli);
	mbt_conn_free(g_srv);
	mbt_ev_destroy(g_ev);

	if (!ok) { fprintf(stderr, "FAIL: tcp transfer wedged\n"); return 1; }
	printf("PASS tcp throughput\n");
	return 0;
}
