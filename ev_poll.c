// SPDX-License-Identifier: ISC
// ev_poll.c — pollable event loop backend (caller drives with poll(2))
#include <stdlib.h>
#include <errno.h>
#include <poll.h>
#include <time.h>
#include <stdint.h>
#include <sys/queue.h>

#include "mbt/compat.h"
#include "mbt/event.h"

struct mbt_ev_io;
struct mbt_ev_timer;

struct poll_io {
	struct mbt_ev *ev;
	int fd;
	uint32_t events;
	mbt_ev_io_cb cb;
	void *arg;
	int dead;
	SLIST_ENTRY(poll_io) entry;
};

struct poll_timer {
	struct mbt_ev *ev;
	uint64_t fire_at_us;
	mbt_ev_timer_cb cb;
	void *arg;
	int dead;
	SLIST_ENTRY(poll_timer) entry;
};

struct poll_ev {
	struct mbt_ev base;
	SLIST_HEAD(, poll_io) ios;
	SLIST_HEAD(, poll_timer) timers;
	int dispatching;
	int running;
};

static uint64_t
now_us(void)
{
	struct timespec ts;
	clock_gettime(CLOCK_MONOTONIC, &ts);
	return (uint64_t)ts.tv_sec * 1000000 + ts.tv_nsec / 1000;
}

static int
poll_io_new(struct mbt_ev *ev, struct mbt_ev_io **out, int fd, uint32_t events,
            mbt_ev_io_cb cb, void *arg)
{
	struct poll_ev *pev = (struct poll_ev *)ev;
	struct poll_io *io = calloc(1, sizeof(*io));
	if (!io) return -ENOMEM;
	io->ev = ev;
	io->fd = fd;
	io->events = events;
	io->cb = cb;
	io->arg = arg;
	SLIST_INSERT_HEAD(&pev->ios, io, entry);
	*out = (struct mbt_ev_io *)io;
	return 0;
}

static int
poll_io_mod(struct mbt_ev_io *io_base, uint32_t events)
{
	struct poll_io *io = (struct poll_io *)io_base;
	io->events = events;
	return 0;
}

static void
poll_io_free(struct mbt_ev_io *io_base)
{
	struct poll_io *io = (struct poll_io *)io_base;
	struct poll_ev *pev = (struct poll_ev *)io->ev;
	if (pev->dispatching) {
		io->dead = 1;
		return;
	}
	SLIST_REMOVE(&pev->ios, io, poll_io, entry);
	free(io);
}

static int
poll_timer_new(struct mbt_ev *ev, struct mbt_ev_timer **out, uint64_t usec,
              mbt_ev_timer_cb cb, void *arg)
{
	struct poll_ev *pev = (struct poll_ev *)ev;
	struct poll_timer *t = calloc(1, sizeof(*t));
	if (!t) return -ENOMEM;
	t->ev = ev;
	t->fire_at_us = now_us() + usec;
	t->cb = cb;
	t->arg = arg;
	SLIST_INSERT_HEAD(&pev->timers, t, entry);
	*out = (struct mbt_ev_timer *)t;
	return 0;
}

static int
poll_timer_mod(struct mbt_ev_timer *timer, uint64_t usec)
{
	struct poll_timer *t = (struct poll_timer *)timer;
	t->fire_at_us = now_us() + usec;
	t->dead = 0;
	return 0;
}

static void
poll_timer_free(struct mbt_ev_timer *timer)
{
	struct poll_timer *t = (struct poll_timer *)timer;
	struct poll_ev *pev = (struct poll_ev *)t->ev;
	if (pev->dispatching) {
		t->dead = 1;
		return;
	}
	SLIST_REMOVE(&pev->timers, t, poll_timer, entry);
	free(t);
}

static void
poll_reap(struct poll_ev *pev)
{
	struct poll_io *io, *io_tmp;
	SLIST_FOREACH_SAFE(io, &pev->ios, entry, io_tmp) {
		if (io->dead) {
			SLIST_REMOVE(&pev->ios, io, poll_io, entry);
			free(io);
		}
	}
	struct poll_timer *t, *t_tmp;
	SLIST_FOREACH_SAFE(t, &pev->timers, entry, t_tmp) {
		if (t->dead) {
			SLIST_REMOVE(&pev->timers, t, poll_timer, entry);
			free(t);
		}
	}
}

int mbt_ev_io_count(struct mbt_ev *ev);

static void
poll_step(struct mbt_ev *ev, int timeout_ms)
{
	int need = mbt_ev_io_count(ev);
	struct pollfd *fds = malloc((need + 1) * sizeof(*fds));
	if (!fds) return;
	int n = mbt_ev_pollfd(ev, fds, need + 1);
	int ms = mbt_ev_timeout_ms(ev);
	if (timeout_ms >= 0 && (ms < 0 || ms > timeout_ms)) ms = timeout_ms;
	poll(fds, n, ms);
	mbt_ev_dispatch(ev, fds, n);
	free(fds);
}

static int
poll_run(struct mbt_ev *ev)
{
	struct poll_ev *pev = (struct poll_ev *)ev;
	pev->running = 1;
	while (pev->running)
		poll_step(ev, -1);
	return 0;
}

static void
poll_stop(struct mbt_ev *ev)
{
	struct poll_ev *pev = (struct poll_ev *)ev;
	pev->running = 0;
}

static void
poll_destroy(struct mbt_ev *ev)
{
	struct poll_ev *pev = (struct poll_ev *)ev;
	struct poll_io *io, *io_tmp;
	SLIST_FOREACH_SAFE(io, &pev->ios, entry, io_tmp)
		free(io);
	struct poll_timer *t, *t_tmp;
	SLIST_FOREACH_SAFE(t, &pev->timers, entry, t_tmp)
		free(t);
	free(pev);
}

static const struct mbt_ev_ops poll_ops = {
	.io_new     = poll_io_new,
	.io_mod     = poll_io_mod,
	.io_free    = poll_io_free,
	.timer_new  = poll_timer_new,
	.timer_mod  = poll_timer_mod,
	.timer_free = poll_timer_free,
	.run        = poll_run,
	.step       = poll_step,
	.stop       = poll_stop,
	.destroy    = poll_destroy,
};

int
mbt_ev_new_pollable(struct mbt_ev **out)
{
	struct poll_ev *pev = calloc(1, sizeof(*pev));
	if (!pev) return -ENOMEM;
	pev->base.ops = &poll_ops;
	SLIST_INIT(&pev->ios);
	SLIST_INIT(&pev->timers);
	*out = &pev->base;
	return 0;
}

int
mbt_ev_io_count(struct mbt_ev *ev)
{
	struct poll_ev *pev = (struct poll_ev *)ev;
	int n = 0;
	struct poll_io *io;
	SLIST_FOREACH(io, &pev->ios, entry)
		if (!io->dead) n++;
	return n;
}

int
mbt_ev_pollfd(struct mbt_ev *ev, struct pollfd *fds, int max)
{
	struct poll_ev *pev = (struct poll_ev *)ev;
	int n = 0;
	struct poll_io *io;
	SLIST_FOREACH(io, &pev->ios, entry) {
		if (n >= max) break;
		if (io->dead) continue;
		fds[n].fd = io->fd;
		fds[n].events = 0;
		if (io->events & MBT_EV_READ) fds[n].events |= POLLIN;
		if (io->events & MBT_EV_WRITE) fds[n].events |= POLLOUT;
		fds[n].revents = 0;
		n++;
	}
	return n;
}

int
mbt_ev_timeout_ms(struct mbt_ev *ev)
{
	struct poll_ev *pev = (struct poll_ev *)ev;
	uint64_t now = now_us();
	uint64_t earliest = UINT64_MAX;
	struct poll_timer *t;

	SLIST_FOREACH(t, &pev->timers, entry) {
		if (t->dead) continue;
		if (t->fire_at_us <= now) return 0;
		if (t->fire_at_us < earliest) earliest = t->fire_at_us;
	}

	if (earliest == UINT64_MAX) return -1;
	uint64_t diff = earliest - now;
	return (int)((diff + 999) / 1000);
}

void
mbt_ev_dispatch(struct mbt_ev *ev, struct pollfd *fds, int nfds)
{
	struct poll_ev *pev = (struct poll_ev *)ev;
	pev->dispatching = 1;

	/* Fire timers first so UI/housekeeping isn't starved by heavy I/O */
	uint64_t now = now_us();
	struct poll_timer *t;
	SLIST_FOREACH(t, &pev->timers, entry) {
		if (!t->dead && t->fire_at_us <= now)
			t->cb((struct mbt_ev_timer *)t, t->arg);
	}

	int i = 0;
	struct poll_io *io;
	SLIST_FOREACH(io, &pev->ios, entry) {
		if (i >= nfds) break;
		if (io->dead) { i++; continue; }
		uint32_t fired = 0;
		if (fds[i].revents & (POLLIN | POLLHUP | POLLERR)) fired |= MBT_EV_READ;
		if (fds[i].revents & POLLOUT) fired |= MBT_EV_WRITE;
		if (fired)
			io->cb((struct mbt_ev_io *)io, io->fd, fired, io->arg);
		i++;
	}

	pev->dispatching = 0;
	poll_reap(pev);
}
