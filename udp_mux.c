// SPDX-License-Identifier: ISC
#include <errno.h>
#include <stdint.h>
#include <stdlib.h>
#include <string.h>

#include "udp_mux.h"
#include "net.h"

struct mbt_udp_mux {
	struct mbt_listener listener;
	mbt_udp_handler utp_h;
	void *utp_arg;
	mbt_udp_handler dht_h;
	void *dht_arg;
};

/* uTP first byte == (type<<4)|version, version==1, type 0..4 (ST_DATA..ST_SYN). */
static int
looks_like_utp(uint8_t b)
{
	return (b & 0x0f) == 1 && (b >> 4) <= 4;
}

static void
udp_mux_recv(struct mbt_ev_io *io, int fd, uint32_t events, void *arg)
{
	(void)io; (void)events;
	struct mbt_udp_mux *m = arg;

	uint8_t buf[65536];
	struct sockaddr_storage src;
	socklen_t srclen = sizeof(src);

	ssize_t n = recvfrom(fd, buf, sizeof(buf), 0,
	                     (struct sockaddr *)&src, &srclen);
	if (n <= 0)
		return;

	/* Demux on the first byte. */
	if (looks_like_utp(buf[0])) {
		if (m->utp_h &&
		    m->utp_h(buf, (size_t)n, (struct sockaddr *)&src, srclen, m->utp_arg))
			return;
	} else if (buf[0] == 'd') {
		if (m->dht_h &&
		    m->dht_h(buf, (size_t)n, (struct sockaddr *)&src, srclen, m->dht_arg))
			return;
	}
	/* unrecognized or unconsumed: drop */
}

int
mbt_udp_mux_new(struct mbt_udp_mux **out, struct mbt_ev *ev, uint16_t port)
{
	struct mbt_udp_mux *m = calloc(1, sizeof(*m));
	if (!m)
		return -ENOMEM;

	int rv = mbt_listen_udp(&m->listener, ev, port, udp_mux_recv, m);
	if (rv < 0) {
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
	free(m);
}
