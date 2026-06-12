// SPDX-License-Identifier: ISC
/*
 * resolve_cares.c — async DNS via c-ares (event-loop integrated)
 */
#include <stdlib.h>
#include <string.h>
#include <errno.h>
#include <netdb.h>
#include <stdio.h>
#include <ares.h>

#include "mbt/resolve.h"
#include "mbt/event.h"

struct mbt_resolve {
	struct mbt_ev *ev;
	ares_channel channel;
	struct mbt_ev_io *io;
	int fd;
	uint16_t port;
	mbt_resolve_cb cb;
	void *arg;
	int cancelled;
	int done;
};

static void
ares_cb(void *arg, int status, int timeouts, struct ares_addrinfo *res)
{
	struct mbt_resolve *r = arg;
	(void)timeouts;

	r->done = 1;
	if (r->cancelled) {
		if (res) ares_freeaddrinfo(res);
		return;
	}

	if (status != ARES_SUCCESS || !res || !res->nodes) {
		r->cb(NULL, 0, -EHOSTUNREACH, r->arg);
	} else {
		struct sockaddr_storage ss = {0};
		socklen_t len = res->nodes->ai_addrlen;
		memcpy(&ss, res->nodes->ai_addr, len);

		if (ss.ss_family == AF_INET)
			((struct sockaddr_in *)&ss)->sin_port = htons(r->port);
		else if (ss.ss_family == AF_INET6)
			((struct sockaddr_in6 *)&ss)->sin6_port = htons(r->port);

		r->cb(&ss, len, 0, r->arg);
	}
	if (res) ares_freeaddrinfo(res);
}

static void
resolve_io_cb(struct mbt_ev_io *io, int fd, uint32_t events, void *v)
{
	struct mbt_resolve *r = v;
	(void)io; (void)events;

	ares_socket_t rfd = (events & MBT_EV_READ) ? fd : ARES_SOCKET_BAD;
	ares_socket_t wfd = (events & MBT_EV_WRITE) ? fd : ARES_SOCKET_BAD;
	ares_process_fd(r->channel, rfd, wfd);

	if (r->done) {
		mbt_ev_io_free(r->io);
		ares_destroy(r->channel);
		free(r);
	}
}

int
mbt_resolve(struct mbt_ev *ev, const char *host, uint16_t port,
            mbt_resolve_cb cb, void *arg, struct mbt_resolve **out)
{
	struct mbt_resolve *r = calloc(1, sizeof(*r));
	if (!r) { cb(NULL, 0, -ENOMEM, arg); return 0; }

	r->ev = ev;
	r->port = port;
	r->cb = cb;
	r->arg = arg;

	int rv = ares_init(&r->channel);
	if (rv != ARES_SUCCESS) {
		free(r);
		cb(NULL, 0, -ENOMEM, arg);
		return 0;
	}

	struct ares_addrinfo_hints hints = { .ai_socktype = SOCK_DGRAM };
	ares_getaddrinfo(r->channel, host, NULL, &hints, ares_cb, r);

	/* Get the fd c-ares wants us to poll */
	ares_socket_t socks[ARES_GETSOCK_MAXNUM];
	int bitmask = ares_getsock(r->channel, socks, ARES_GETSOCK_MAXNUM);

	if (ARES_GETSOCK_READABLE(bitmask, 0) || ARES_GETSOCK_WRITABLE(bitmask, 0)) {
		r->fd = socks[0];
		uint32_t ev_flags = 0;
		if (ARES_GETSOCK_READABLE(bitmask, 0)) ev_flags |= MBT_EV_READ;
		if (ARES_GETSOCK_WRITABLE(bitmask, 0)) ev_flags |= MBT_EV_WRITE;
		mbt_ev_io_new(ev, &r->io, r->fd, ev_flags, resolve_io_cb, r);
	}

	if (out) *out = r;
	return 0;
}

void
mbt_resolve_cancel(struct mbt_resolve *r)
{
	if (!r) return;
	r->cancelled = 1;
	ares_cancel(r->channel);
	if (r->io) mbt_ev_io_free(r->io);
	ares_destroy(r->channel);
	free(r);
}
