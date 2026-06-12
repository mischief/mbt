// SPDX-License-Identifier: ISC
/*
 * resolve_cares.c — async DNS via c-ares (event-loop integrated)
 *
 * Uses ARES_OPT_SOCK_STATE_CB so c-ares tells us when to watch fds.
 * Each fd gets an mbt_ev_io. No threads.
 */
#include <stdlib.h>
#include <string.h>
#include <errno.h>
#include <netdb.h>

#include <ares.h>

#include "mbt/resolve.h"
#include "mbt/event.h"

#define MAX_FDS 8

struct mbt_resolve {
	struct mbt_ev *ev;
	ares_channel channel;
	uint16_t port;
	mbt_resolve_cb cb;
	void *arg;
	int done;
	int cancelled;
	struct { int fd; struct mbt_ev_io *io; } fds[MAX_FDS];
	int nfds;
};

static void resolve_process(struct mbt_ev_io *io, int fd, uint32_t events, void *v);

static void
sock_state_cb(void *data, ares_socket_t fd, int readable, int writable)
{
	struct mbt_resolve *r = data;
	if (r->done) return; /* ignore socket changes during teardown */

	int idx = -1;
	for (int i = 0; i < r->nfds; i++)
		if (r->fds[i].fd == (int)fd) { idx = i; break; }

	if (!readable && !writable) {
		if (idx >= 0) {
			mbt_ev_io_free(r->fds[idx].io);
			r->fds[idx] = r->fds[--r->nfds];
		}
		return;
	}

	uint32_t flags = 0;
	if (readable) flags |= MBT_EV_READ;
	if (writable) flags |= MBT_EV_WRITE;

	if (idx >= 0) {
		mbt_ev_io_mod(r->fds[idx].io, flags);
	} else if (r->nfds < MAX_FDS) {
		idx = r->nfds++;
		r->fds[idx].fd = fd;
		mbt_ev_io_new(r->ev, &r->fds[idx].io, fd, flags, resolve_process, r);
	}
}

static void
ares_addrinfo_cb(void *arg, int status, int timeouts, struct ares_addrinfo *res)
{
	struct mbt_resolve *r = arg;
	(void)timeouts;

	r->done = 1; /* prevent sock_state_cb from touching fds during destroy */

	if (r->cancelled) {
		if (res) ares_freeaddrinfo(res);
		return; /* cleanup in resolve_process */
	}

	if (status != ARES_SUCCESS || !res || !res->nodes) {
		int err;
		switch (status) {
		case ARES_ENOTFOUND:  err = -ENOENT; break;
		case ARES_ETIMEOUT:   err = -EAGAIN; break;
		case ARES_ENOMEM:     err = -ENOMEM; break;
		case ARES_ECANCELLED: err = -ECANCELED; break;
		default:              err = -EIO; break;
		}
		r->cb(NULL, 0, err, r->arg);
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
resolve_cleanup(struct mbt_resolve *r)
{
	for (int i = 0; i < r->nfds; i++)
		mbt_ev_io_free(r->fds[i].io);
	ares_destroy(r->channel);
	free(r);
}

static void
resolve_process(struct mbt_ev_io *io, int fd, uint32_t events, void *v)
{
	struct mbt_resolve *r = v;
	(void)io;
	ares_socket_t rfd = (events & MBT_EV_READ) ? fd : ARES_SOCKET_BAD;
	ares_socket_t wfd = (events & MBT_EV_WRITE) ? fd : ARES_SOCKET_BAD;
	ares_process_fd(r->channel, rfd, wfd);

	/* If resolution completed during ares_process_fd, clean up now
	 * (safe — we're past the ares call, io is no longer referenced) */
	if (r->done)
		resolve_cleanup(r);
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

	struct ares_options opts = {
		.sock_state_cb = sock_state_cb,
		.sock_state_cb_data = r,
	};
	int rv = ares_init_options(&r->channel, &opts, ARES_OPT_SOCK_STATE_CB);
	if (rv != ARES_SUCCESS) {
		free(r);
		cb(NULL, 0, -ENOMEM, arg);
		return 0;
	}

	struct ares_addrinfo_hints hints = { .ai_socktype = SOCK_DGRAM };
	ares_getaddrinfo(r->channel, host, NULL, &hints, ares_addrinfo_cb, r);

	/* If resolved synchronously (cached/immediate NXDOMAIN), clean up now */
	if (r->done) {
		ares_destroy(r->channel);
		free(r);
		if (out) *out = NULL;
		return 0;
	}

	if (out) *out = r;
	return 0;
}

void
mbt_resolve_cancel(struct mbt_resolve *r)
{
	if (!r) return;
	r->cancelled = 1;
	r->done = 1;
	ares_cancel(r->channel);
	resolve_cleanup(r);
}
