// SPDX-License-Identifier: ISC
#include <stdlib.h>
#include <errno.h>
#include <systemd/sd-event.h>
#include <stdint.h>
#include <sys/epoll.h>
#include <time.h>

#include "mbt/event.h"
#include "ev_sd_internal.h"

struct mbt_ev_io;
struct mbt_ev_timer;

struct ev_sd_io {
	struct mbt_ev *ev;
	sd_event_source *src;
	mbt_ev_io_cb cb;
	void *arg;
	int fd;
};

struct ev_sd_timer {
	struct mbt_ev *ev;
	sd_event_source *src;
	mbt_ev_timer_cb cb;
	void *arg;
};

static uint32_t
to_epoll(uint32_t flags)
{
	uint32_t e = 0;
	if (flags & MBT_EV_READ)  e |= EPOLLIN;
	if (flags & MBT_EV_WRITE) e |= EPOLLOUT;
	return e;
}

static uint32_t
from_epoll(uint32_t e)
{
	uint32_t f = 0;
	if (e & EPOLLIN)  f |= MBT_EV_READ;
	if (e & EPOLLOUT) f |= MBT_EV_WRITE;
	return f;
}

static int
sd_io_handler(sd_event_source *s, int fd, uint32_t revents, void *v)
{
	struct ev_sd_io *io = v;
	io->cb((struct mbt_ev_io *)io, fd, from_epoll(revents), io->arg);
	return 0;
}

static int
sd_io_new(struct mbt_ev *ev, struct mbt_ev_io **out, int fd, uint32_t events,
          mbt_ev_io_cb cb, void *arg)
{
	struct ev_sd *s = (struct ev_sd *)ev;
	struct ev_sd_io *io;
	int rv;

	io = calloc(1, sizeof(*io));
	if (!io)
		return -ENOMEM;

	io->ev = ev;
	io->cb = cb;
	io->arg = arg;
	io->fd = fd;

	rv = sd_event_add_io(s->loop, &io->src, fd, to_epoll(events), sd_io_handler, io);
	if (rv < 0) {
		free(io);
		return rv;
	}

	*out = (struct mbt_ev_io *)io;
	return 0;
}

static int
sd_io_mod(struct mbt_ev_io *handle, uint32_t events)
{
	struct ev_sd_io *io = (struct ev_sd_io *)handle;
	return sd_event_source_set_io_events(io->src, to_epoll(events));
}

static void
sd_io_free(struct mbt_ev_io *handle)
{
	struct ev_sd_io *io = (struct ev_sd_io *)handle;
	sd_event_source_unref(io->src);
	free(io);
}

static int
sd_timer_handler(sd_event_source *s, uint64_t usec, void *v)
{
	struct ev_sd_timer *t = v;
	t->cb((struct mbt_ev_timer *)t, t->arg);
	return 0;
}

static int
sd_timer_new(struct mbt_ev *ev, struct mbt_ev_timer **out, uint64_t usec,
             mbt_ev_timer_cb cb, void *arg)
{
	struct ev_sd *s = (struct ev_sd *)ev;
	struct ev_sd_timer *t;
	uint64_t now;
	int rv;

	t = calloc(1, sizeof(*t));
	if (!t)
		return -ENOMEM;

	t->ev = ev;
	t->cb = cb;
	t->arg = arg;

	rv = sd_event_now(s->loop, CLOCK_MONOTONIC, &now);
	if (rv < 0) {
		free(t);
		return rv;
	}

	rv = sd_event_add_time(s->loop, &t->src, CLOCK_MONOTONIC,
	                       now + usec, 0, sd_timer_handler, t);
	if (rv < 0) {
		free(t);
		return rv;
	}

	*out = (struct mbt_ev_timer *)t;
	return 0;
}

static int
sd_timer_mod(struct mbt_ev_timer *handle, uint64_t usec)
{
	struct ev_sd_timer *t = (struct ev_sd_timer *)handle;
	struct ev_sd *s = (struct ev_sd *)t->ev;
	uint64_t now;
	int rv;

	rv = sd_event_now(s->loop, CLOCK_MONOTONIC, &now);
	if (rv < 0)
		return rv;

	rv = sd_event_source_set_time(t->src, now + usec);
	if (rv < 0)
		return rv;

	return sd_event_source_set_enabled(t->src, SD_EVENT_ONESHOT);
}

static void
sd_timer_free(struct mbt_ev_timer *handle)
{
	struct ev_sd_timer *t = (struct ev_sd_timer *)handle;
	sd_event_source_unref(t->src);
	free(t);
}

static int
sd_run(struct mbt_ev *ev)
{
	struct ev_sd *s = (struct ev_sd *)ev;
	return sd_event_loop(s->loop);
}

static void
sd_step(struct mbt_ev *ev, int timeout_ms)
{
	struct ev_sd *s = (struct ev_sd *)ev;
	uint64_t usec = timeout_ms < 0 ? UINT64_MAX : (uint64_t)timeout_ms * 1000;
	sd_event_run(s->loop, usec);
}

static void
sd_stop(struct mbt_ev *ev)
{
	struct ev_sd *s = (struct ev_sd *)ev;
	sd_event_exit(s->loop, 0);
}

static void
sd_destroy(struct mbt_ev *ev)
{
	struct ev_sd *s = (struct ev_sd *)ev;
	if (s->owned)
		sd_event_unref(s->loop);
	free(s);
}

static const struct mbt_ev_ops sd_ops = {
	.io_new    = sd_io_new,
	.io_mod    = sd_io_mod,
	.io_free   = sd_io_free,
	.timer_new = sd_timer_new,
	.timer_mod = sd_timer_mod,
	.timer_free = sd_timer_free,
	.run       = sd_run,
	.step      = sd_step,
	.stop      = sd_stop,
	.destroy   = sd_destroy,
};

int
mbt_ev_sd_new(struct mbt_ev **out)
{
	struct ev_sd *s;
	int rv;

	s = calloc(1, sizeof(*s));
	if (!s)
		return -ENOMEM;

	rv = sd_event_default(&s->loop);
	if (rv < 0) {
		free(s);
		return rv;
	}

	s->owned = 1;
	s->base.ops = &sd_ops;
	*out = &s->base;
	return 0;
}

int
mbt_ev_sd_new_from(struct mbt_ev **out, sd_event *loop)
{
	struct ev_sd *s;

	s = calloc(1, sizeof(*s));
	if (!s)
		return -ENOMEM;

	s->loop = sd_event_ref(loop);
	s->base.ops = &sd_ops;
	*out = &s->base;
	return 0;
}
