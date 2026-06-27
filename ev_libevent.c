// SPDX-License-Identifier: ISC
#include <stdlib.h>
#include <errno.h>

#ifdef HAVE_LIBEVENT1
#include <event.h>
/* libevent 1.x shims */
#define evutil_socket_t int
static inline struct event *
event_new(struct event_base *base, int fd, short flags,
          void (*cb)(int, short, void *), void *arg)
{
	struct event *e = malloc(sizeof(*e));
	if (!e) return NULL;
	event_set(e, fd, flags, cb, arg);
	event_base_set(base, e);
	return e;
}
static inline void event_free(struct event *e) { free(e); }
static inline struct event *
evtimer_new(struct event_base *base, void (*cb)(int, short, void *), void *arg)
{
	struct event *e = malloc(sizeof(*e));
	if (!e) return NULL;
	evtimer_set(e, cb, arg);
	event_base_set(base, e);
	return e;
}
#else
#include <event2/event.h>
#endif

#include "mbt/event.h"

#include "ev_lev_internal.h"

struct ev_lev_io {
	struct mbt_ev *ev;
	struct event *e;
	mbt_ev_io_cb cb;
	void *arg;
	int fd;
};

struct ev_lev_timer {
	struct mbt_ev *ev;
	struct event *e;
	mbt_ev_timer_cb cb;
	void *arg;
};

static short
to_lev(uint32_t flags)
{
	short e = EV_PERSIST;
	if (flags & MBT_EV_READ)  e |= EV_READ;
	if (flags & MBT_EV_WRITE) e |= EV_WRITE;
	return e;
}

static uint32_t
from_lev(short e)
{
	uint32_t f = 0;
	if (e & EV_READ)  f |= MBT_EV_READ;
	if (e & EV_WRITE) f |= MBT_EV_WRITE;
	return f;
}

static void
lev_io_handler(evutil_socket_t fd, short what, void *v)
{
	struct ev_lev_io *io = v;
	io->cb((struct mbt_ev_io *)io, fd, from_lev(what), io->arg);
}

static int
lev_io_new(struct mbt_ev *ev, struct mbt_ev_io **out, int fd, uint32_t events,
           mbt_ev_io_cb cb, void *arg)
{
	struct ev_lev *l = (struct ev_lev *)ev;
	struct ev_lev_io *io;

	io = calloc(1, sizeof(*io));
	if (!io)
		return -ENOMEM;

	io->ev = ev;
	io->cb = cb;
	io->arg = arg;
	io->fd = fd;

	io->e = event_new(l->base_ev, fd, to_lev(events), lev_io_handler, io);
	if (!io->e) {
		free(io);
		return -ENOMEM;
	}

	if (event_add(io->e, NULL) < 0) {
		event_free(io->e);
		free(io);
		return -EIO;
	}

	*out = (struct mbt_ev_io *)io;
	return 0;
}

static int
lev_io_mod(struct mbt_ev_io *handle, uint32_t events)
{
	struct ev_lev_io *io = (struct ev_lev_io *)handle;
	struct ev_lev *l = (struct ev_lev *)io->ev;

	event_del(io->e);
	event_free(io->e);

	io->e = event_new(l->base_ev, io->fd, to_lev(events), lev_io_handler, io);
	if (!io->e)
		return -ENOMEM;

	if (event_add(io->e, NULL) < 0) {
		event_free(io->e);
		io->e = NULL;
		return -EIO;
	}

	return 0;
}

static void
lev_io_free(struct mbt_ev_io *handle)
{
	struct ev_lev_io *io = (struct ev_lev_io *)handle;
	event_del(io->e);
	event_free(io->e);
	free(io);
}

static void
lev_timer_handler(evutil_socket_t fd, short what, void *v)
{
	(void)fd;
	(void)what;
	struct ev_lev_timer *t = v;
	t->cb((struct mbt_ev_timer *)t, t->arg);
}

static int
lev_timer_new(struct mbt_ev *ev, struct mbt_ev_timer **out, uint64_t usec,
              mbt_ev_timer_cb cb, void *arg)
{
	struct ev_lev *l = (struct ev_lev *)ev;
	struct ev_lev_timer *t;
	struct timeval tv;

	t = calloc(1, sizeof(*t));
	if (!t)
		return -ENOMEM;

	t->ev = ev;
	t->cb = cb;
	t->arg = arg;

	t->e = evtimer_new(l->base_ev, lev_timer_handler, t);
	if (!t->e) {
		free(t);
		return -ENOMEM;
	}

	tv.tv_sec = usec / 1000000;
	tv.tv_usec = usec % 1000000;

	if (evtimer_add(t->e, &tv) < 0) {
		event_free(t->e);
		free(t);
		return -EIO;
	}

	*out = (struct mbt_ev_timer *)t;
	return 0;
}

static int
lev_timer_mod(struct mbt_ev_timer *handle, uint64_t usec)
{
	struct ev_lev_timer *t = (struct ev_lev_timer *)handle;
	struct timeval tv;

	tv.tv_sec = usec / 1000000;
	tv.tv_usec = usec % 1000000;

	if (evtimer_add(t->e, &tv) < 0)
		return -EIO;

	return 0;
}

static void
lev_timer_free(struct mbt_ev_timer *handle)
{
	struct ev_lev_timer *t = (struct ev_lev_timer *)handle;
	evtimer_del(t->e);
	event_free(t->e);
	free(t);
}

static int
lev_run(struct mbt_ev *ev)
{
	struct ev_lev *l = (struct ev_lev *)ev;
	return event_base_dispatch(l->base_ev);
}

static void
lev_step(struct mbt_ev *ev, int timeout_ms)
{
	struct ev_lev *l = (struct ev_lev *)ev;
	if (timeout_ms >= 0) {
		struct timeval tv = {
			.tv_sec  = timeout_ms / 1000,
			.tv_usec = (timeout_ms % 1000) * 1000,
		};
		event_base_loopexit(l->base_ev, &tv);
	}
	event_base_loop(l->base_ev, EVLOOP_ONCE);
}

static void
lev_stop(struct mbt_ev *ev)
{
	struct ev_lev *l = (struct ev_lev *)ev;
	event_base_loopbreak(l->base_ev);
}

static void
lev_destroy(struct mbt_ev *ev)
{
	struct ev_lev *l = (struct ev_lev *)ev;
	if (l->owned)
		event_base_free(l->base_ev);
	free(l);
}

static const struct mbt_ev_ops lev_ops = {
	.io_new    = lev_io_new,
	.io_mod    = lev_io_mod,
	.io_free   = lev_io_free,
	.timer_new = lev_timer_new,
	.timer_mod = lev_timer_mod,
	.timer_free = lev_timer_free,
	.run       = lev_run,
	.step      = lev_step,
	.stop      = lev_stop,
	.destroy   = lev_destroy,
};

int
mbt_ev_libevent_new(struct mbt_ev **out)
{
	struct ev_lev *l;

	l = calloc(1, sizeof(*l));
	if (!l)
		return -ENOMEM;

	l->base_ev = event_base_new();
	if (!l->base_ev) {
		free(l);
		return -ENOMEM;
	}

	l->owned = 1;
	l->base.ops = &lev_ops;
	*out = &l->base;
	return 0;
}

int
mbt_ev_libevent_new_from(struct mbt_ev **out, struct event_base *base)
{
	struct ev_lev *l;

	l = calloc(1, sizeof(*l));
	if (!l)
		return -ENOMEM;

	l->base_ev = base;
	l->base.ops = &lev_ops;
	*out = &l->base;
	return 0;
}
