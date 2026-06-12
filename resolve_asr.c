// SPDX-License-Identifier: ISC
/*
 * resolve_asr.c — async DNS via OpenBSD asr_run (event-loop integrated)
 */
#include <stdlib.h>
#include <string.h>
#include <errno.h>
#include <netdb.h>
#include <stdio.h>
#include <asr.h>

#include "mbt/resolve.h"
#include "mbt/event.h"

struct mbt_resolve {
	struct mbt_ev *ev;
	struct mbt_ev_io *io;
	struct asr_query *query;
	uint16_t port;
	mbt_resolve_cb cb;
	void *arg;
	int cancelled;
};

static void
resolve_io_cb(struct mbt_ev_io *io, int fd, uint32_t events, void *v)
{
	struct mbt_resolve *r = v;
	(void)io; (void)fd; (void)events;

	struct asr_result ar;
	int rv = asr_run(r->query, &ar);

	if (rv == 0) {
		/* Still in progress — wait for more IO */
		return;
	}

	/* Complete */
	mbt_ev_io_free(r->io);
	r->io = NULL;
	r->query = NULL;

	if (r->cancelled) {
		if (ar.ar_addrinfo) freeaddrinfo(ar.ar_addrinfo);
		free(r);
		return;
	}

	if (ar.ar_gai_errno != 0 || !ar.ar_addrinfo) {
		r->cb(NULL, 0, -ENOENT, r->arg);
	} else {
		struct sockaddr_storage ss = {0};
		socklen_t len = ar.ar_addrinfo->ai_addrlen;
		memcpy(&ss, ar.ar_addrinfo->ai_addr, len);

		/* Set port */
		if (ss.ss_family == AF_INET)
			((struct sockaddr_in *)&ss)->sin_port = htons(r->port);
		else if (ss.ss_family == AF_INET6)
			((struct sockaddr_in6 *)&ss)->sin6_port = htons(r->port);

		freeaddrinfo(ar.ar_addrinfo);
		r->cb(&ss, len, 0, r->arg);
	}
	free(r);
}

int
mbt_resolve(struct mbt_ev *ev, const char *host, uint16_t port,
            mbt_resolve_cb cb, void *arg, struct mbt_resolve **out)
{
	struct asr_query *q;
	struct addrinfo hints = { .ai_socktype = SOCK_DGRAM };

	q = getaddrinfo_async(host, NULL, &hints, NULL);
	if (!q) {
		cb(NULL, 0, -ENOMEM, arg);
		return 0;
	}

	/* First call to get the fd */
	struct asr_result ar;
	int rv = asr_run(q, &ar);
	if (rv != 0) {
		/* Resolved immediately (cached?) */
		if (ar.ar_gai_errno != 0 || !ar.ar_addrinfo) {
			cb(NULL, 0, -ENOENT, arg);
		} else {
			struct sockaddr_storage ss = {0};
			socklen_t len = ar.ar_addrinfo->ai_addrlen;
			memcpy(&ss, ar.ar_addrinfo->ai_addr, len);
			if (ss.ss_family == AF_INET)
				((struct sockaddr_in *)&ss)->sin_port = htons(port);
			else if (ss.ss_family == AF_INET6)
				((struct sockaddr_in6 *)&ss)->sin6_port = htons(port);
			freeaddrinfo(ar.ar_addrinfo);
			cb(&ss, len, 0, arg);
		}
		if (out) *out = NULL;
		return 0;
	}

	/* Need to wait — register fd with event loop */
	struct mbt_resolve *r = calloc(1, sizeof(*r));
	if (!r) { asr_abort(q); cb(NULL, 0, -ENOMEM, arg); return 0; }

	r->ev = ev;
	r->query = q;
	r->port = port;
	r->cb = cb;
	r->arg = arg;

	int fd = asr_run_cond(q, &ar, NULL); /* get the fd to poll */
	mbt_ev_io_new(ev, &r->io, fd, MBT_EV_READ, resolve_io_cb, r);

	if (out) *out = r;
	return 0;
}

void
mbt_resolve_cancel(struct mbt_resolve *r)
{
	if (!r) return;
	r->cancelled = 1;
	if (r->query) asr_abort(r->query);
	if (r->io) mbt_ev_io_free(r->io);
	free(r);
}
