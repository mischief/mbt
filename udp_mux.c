// SPDX-License-Identifier: ISC
#include <errno.h>
#include <stdint.h>
#include <stdlib.h>
#include <string.h>
#include <sys/socket.h>
#include <arpa/inet.h>

#include "udp_mux.h"
#include "net.h"
#include "mbt/log.h"
#include "mbt/ratelimit.h"

/*
 * Many uTP peers are multiplexed onto one shared UDP socket, so datagrams pile
 * up while we service others. Reading one datagram per event-loop wakeup is
 * ~3x slower on a busy swarm than draining a batch each wakeup -- the dominant
 * cost is the event-loop iteration (epoll + timers + dispatch), not the recv
 * syscall, so we want to clear the backlog in as few wakeups as possible.
 *
 * UDP_RECV_BATCH is how many datagrams we drain per wakeup. This is NOT a
 * free knob: on a 2-seeder/5-leecher uTP swarm there is a sharp cliff around
 * 13-15 datagrams/wakeup -- below it throughput collapses to the
 * one-per-wakeup case (~61s to complete), at/above it we get the win (~19-22s).
 * 16 sits just past the cliff. Do not lower it.
 *
 * (We measured recvmmsg(2) here too; at the same per-wakeup count it was
 * identical to this recvfrom loop -- the win is the draining, not syscall
 * batching -- so we kept the loop: one buffer, no feature detection, portable.)
 */
#define UDP_RECV_BATCH 16

struct mbt_udp_mux {
	struct mbt_listener listener;
	mbt_udp_handler utp_h;
	void *utp_arg;
	mbt_udp_handler dht_h;
	void *dht_arg;
	struct mbt_ratelimit *trunc_rl;  /* throttles the oversized-datagram warning */
};

/* uTP first byte == (type<<4)|version, version==1, type 0..4 (ST_DATA..ST_SYN). */
static int
looks_like_utp(uint8_t b)
{
	return (b & 0x0f) == 1 && (b >> 4) <= 4;
}

/* An oversized datagram was truncated into our buffer; log (rate-limited) and
 * drop it -- feeding a truncated packet to the uTP/DHT parsers would corrupt
 * the stream. uTP retransmits, DHT retries, so dropping is safe. */
static void
udp_mux_truncated(struct mbt_udp_mux *m, const struct sockaddr_storage *src)
{
	if (mbt_ratelimit_allow(m->trunc_rl, 10)) {  /* rate=1/s, cost 10 => 1/10s */
		char addr[INET6_ADDRSTRLEN + 8];  /* "[<ipv6>]:port" */
		mbt_log_warn(MBT_LOG_UTP,
		    "udp_mux: datagram from %s larger than %d bytes; truncated and dropped",
		    mbt_sockaddr_str(src, addr, sizeof(addr)), UDP_RECV_MAX);
	}
}

/* Demux one datagram on its first byte and hand it to the right consumer. */
static void
udp_mux_dispatch(struct mbt_udp_mux *m, const uint8_t *buf, size_t n,
                 struct sockaddr *src, socklen_t srclen)
{
	if (n == 0)
		return;
	if (looks_like_utp(buf[0])) {
		if (m->utp_h)
			m->utp_h(buf, n, src, srclen, m->utp_arg);
	} else if (buf[0] == 'd') {
		if (m->dht_h)
			m->dht_h(buf, n, src, srclen, m->dht_arg);
	}
	/* unrecognized: drop */
}

static void
udp_mux_recv(struct mbt_ev_io *io, int fd, uint32_t events, void *arg)
{
	(void)io; (void)events;
	struct mbt_udp_mux *m = arg;

	/* Drain up to UDP_RECV_BATCH datagrams per wakeup. MSG_TRUNC makes recvfrom
	 * report the true datagram length, so a >buffer datagram returns n > sizeof
	 * and we detect+drop it. epoll is level-triggered, so any remainder
	 * re-fires us immediately -- a flood on one socket can't starve the loop. */
	uint8_t buf[UDP_RECV_MAX];
	for (int i = 0; i < UDP_RECV_BATCH; i++) {
		struct sockaddr_storage src;
		socklen_t srclen = sizeof(src);
		ssize_t n = recvfrom(fd, buf, sizeof(buf), MSG_DONTWAIT | MSG_TRUNC,
		                     (struct sockaddr *)&src, &srclen);
		if (n <= 0)
			return;
		if ((size_t)n > sizeof(buf)) {
			udp_mux_truncated(m, &src);
			continue;
		}
		udp_mux_dispatch(m, buf, (size_t)n, (struct sockaddr *)&src, srclen);
	}
}

int
mbt_udp_mux_new(struct mbt_udp_mux **out, struct mbt_ev *ev, uint16_t port)
{
	struct mbt_udp_mux *m = calloc(1, sizeof(*m));
	if (!m)
		return -ENOMEM;

	/* 1 token/s, burst 10; each warning costs 10 tokens (see udp_mux_truncated)
	 * => at most 1 warning / 10s. burst >= cost so tokens can accumulate. */
	if (mbt_ratelimit_new(&m->trunc_rl, 1, 10) < 0) {
		free(m);
		return -ENOMEM;
	}

	int rv = mbt_listen_udp(&m->listener, ev, port, udp_mux_recv, m);
	if (rv < 0) {
		mbt_ratelimit_free(m->trunc_rl);
		free(m);
		return rv;
	}
	*out = m;
	return 0;
}

void
mbt_udp_mux_set_utp(struct mbt_udp_mux *m, mbt_udp_handler h, void *arg)
{
	m->utp_h = h;
	m->utp_arg = arg;
}

void
mbt_udp_mux_set_dht(struct mbt_udp_mux *m, mbt_udp_handler h, void *arg)
{
	m->dht_h = h;
	m->dht_arg = arg;
}

int
mbt_udp_mux_sendto(struct mbt_udp_mux *m, const void *buf, size_t len,
                   const struct sockaddr *dst, socklen_t dstlen)
{
	int fd = (dst->sa_family == AF_INET6) ? m->listener.fd6 : m->listener.fd4;
	if (fd < 0)
		return -EAFNOSUPPORT;
	ssize_t n = sendto(fd, buf, len, 0, dst, dstlen);
	return n < 0 ? -errno : 0;
}

uint16_t
mbt_udp_mux_port(struct mbt_udp_mux *m)
{
	return m->listener.port;
}

void
mbt_udp_mux_free(struct mbt_udp_mux *m)
{
	if (!m)
		return;
	mbt_listener_close(&m->listener);
	mbt_ratelimit_free(m->trunc_rl);
	free(m);
}
