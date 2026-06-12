// SPDX-License-Identifier: ISC
/*
 * resolve_asr.c — async DNS via OpenBSD asr_run (event-loop integrated)
 */
#include <sys/types.h>
#include <sys/socket.h>
#include <netinet/in.h>
#include <stdlib.h>
#include <string.h>
#include <errno.h>
#include <netdb.h>
#include <asr.h>

#include "mbt/resolve.h"
#include "mbt/event.h"

struct mbt_resolve {
	struct mbt_ev *ev;
	struct mbt_ev_io *io;
	struct mbt_ev_timer *timer;
	struct asr_query *query;
	uint16_t port;
	mbt_resolve_cb cb;
	void *arg;
};

static void resolve_continue(struct mbt_resolve *r);

static void
resolve_io_cb(struct mbt_ev_io *io, int fd, uint32_t events, void *v)
{
	(void)io; (void)fd; (void)events;
	struct mbt_resolve *r = v;
	if (r->io) { mbt_ev_io_free(r->io); r->io = NULL; }
	if (r->timer) { mbt_ev_timer_free(r->timer); r->timer = NULL; }
	resolve_continue(r);
}

static void
resolve_timer_cb(struct mbt_ev_timer *t, void *v)
{
	(void)t;
	struct mbt_resolve *r = v;
	if (r->io) { mbt_ev_io_free(r->io); r->io = NULL; }
	if (r->timer) { mbt_ev_timer_free(r->timer); r->timer = NULL; }
	resolve_continue(r);
}

static int
gai_to_errno(int gai_err)
{
	switch (gai_err) {
	case EAI_NONAME:  return -ENOENT;
	case EAI_AGAIN:   return -EAGAIN;
	case EAI_MEMORY:  return -ENOMEM;
	case EAI_FAIL:    return -EIO;
	default:          return -EIO;
	}
}

static void
resolve_continue(struct mbt_resolve *r)
{
	struct asr_result ar;
	int rv = asr_run(r->query, &ar);

	if (rv == 0) {
		/* Not done yet — watch fd or set timeout */
		uint32_t flags = 0;
		if (ar.ar_cond == ASR_WANT_READ) flags = MBT_EV_READ;
		else if (ar.ar_cond == ASR_WANT_WRITE) flags = MBT_EV_WRITE;

		if (ar.ar_fd >= 0 && flags)
			mbt_ev_io_new(r->ev, &r->io, ar.ar_fd, flags, resolve_io_cb, r);
		if (ar.ar_timeout >= 0)
			mbt_ev_timer_new(r->ev, &r->timer,
			                 (uint64_t)ar.ar_timeout * 1000, resolve_timer_cb, r);
		return;
	}

	/* Done */
	r->query = NULL;
	if (ar.ar_gai_errno != 0 || !ar.ar_addrinfo) {
		r->cb(NULL, 0, gai_to_errno(ar.ar_gai_errno), r->arg);
	} else {
		struct sockaddr_storage ss = {0};
		socklen_t len = ar.ar_addrinfo->ai_addrlen;
		memcpy(&ss, ar.ar_addrinfo->ai_addr, len);
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
	struct addrinfo hints = { .ai_socktype = SOCK_DGRAM };
	struct asr_query *q = getaddrinfo_async(host, NULL, &hints, NULL);
	if (!q) { cb(NULL, 0, -ENOMEM, arg); return 0; }

	struct mbt_resolve *r = calloc(1, sizeof(*r));
	if (!r) { asr_abort(q); cb(NULL, 0, -ENOMEM, arg); return 0; }

	r->ev = ev;
	r->query = q;
	r->port = port;
	r->cb = cb;
	r->arg = arg;

	resolve_continue(r);

	if (out) *out = r;
	return 0;
}

void
mbt_resolve_cancel(struct mbt_resolve *r)
{
	if (!r) return;
	if (r->io) mbt_ev_io_free(r->io);
	if (r->timer) mbt_ev_timer_free(r->timer);
	if (r->query) asr_abort(r->query);
	free(r);
}
