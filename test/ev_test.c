// SPDX-License-Identifier: ISC
#include <assert.h>
#include <errno.h>
#include <poll.h>
#include <stdio.h>
#include <string.h>
#include <unistd.h>
#include <fcntl.h>
#include <stdint.h>
#include <sys/types.h>

#include "mbt/event.h"

struct mbt_ev_io;
struct mbt_ev_timer;

static int io_fired;
static int timer_fired;

static void
on_io(struct mbt_ev_io *io, int fd, uint32_t events, void *arg)
{
	char buf[16];
	ssize_t n;

	(void)io;
	assert(events & MBT_EV_READ);
	n = read(fd, buf, sizeof(buf));
	assert(n == 5);
	assert(memcmp(buf, "hello", 5) == 0);
	io_fired = 1;

	if (io_fired && timer_fired)
		mbt_ev_stop(arg);
}

static void
on_timer(struct mbt_ev_timer *timer, void *arg)
{
	(void)timer;
	timer_fired = 1;

	if (io_fired && timer_fired)
		mbt_ev_stop(arg);
}

static void
test_backend(struct mbt_ev *ev, const char *name)
{
	struct mbt_ev_io *io;
	struct mbt_ev_timer *tmr;
	int pfd[2];
	int rv;

	io_fired = 0;
	timer_fired = 0;

	rv = pipe(pfd);
	assert(rv == 0);

	fcntl(pfd[0], F_SETFL, fcntl(pfd[0], F_GETFL) | O_NONBLOCK);

	rv = mbt_ev_io_new(ev, &io, pfd[0], MBT_EV_READ, on_io, ev);
	assert(rv == 0);

	rv = mbt_ev_timer_new(ev, &tmr, 10000, on_timer, ev);
	assert(rv == 0);

	assert(write(pfd[1], "hello", 5) == 5);

	rv = mbt_ev_run(ev);

	assert(io_fired);
	assert(timer_fired);

	mbt_ev_io_free(io);
	mbt_ev_timer_free(tmr);
	mbt_ev_destroy(ev);

	close(pfd[0]);
	close(pfd[1]);

	printf("%s: OK\n", name);
}

/* A timer and an fd must wake a poll on the watch descriptor. */
static int watch_hits;

static void
watch_timer_cb(struct mbt_ev_timer *t, void *arg)
{
	(void)arg;
	mbt_ev_timer_free(t);
	watch_hits++;
}

static void
watch_io_cb(struct mbt_ev_io *io, int fd, uint32_t events, void *arg)
{
	char c;

	(void)io; (void)events; (void)arg;
	assert(read(fd, &c, 1) == 1);
	watch_hits++;
}

static void
test_watch(struct mbt_ev *ev)
{
	struct mbt_ev_timer *t;
	struct mbt_ev_io *io;
	int pfd[2], ms = 0, fd;
	struct pollfd p = {.events = POLLIN};

	if ((fd = mbt_ev_watch(ev, &ms)) == -ENOTSUP)
		return;
	assert(fd >= 0 && ms == -1);

	/* The timer is the only event: the fd alone must report it. */
	assert(mbt_ev_timer_new(ev, &t, 20000, watch_timer_cb, NULL) == 0);
	p.fd = mbt_ev_watch(ev, &ms);
	assert(p.fd >= 0);
	(void)poll(&p, 1, ms >= 0 ? ms + 1 : 2000);
	mbt_ev_step(ev, 0);
	assert(watch_hits == 1);

	assert(pipe(pfd) == 0);
	assert(mbt_ev_io_new(ev, &io, pfd[0], MBT_EV_READ, watch_io_cb, NULL) == 0);
	p.fd = mbt_ev_watch(ev, &ms);
	assert(poll(&p, 1, 0) == 0);
	assert(write(pfd[1], "x", 1) == 1);
	assert(poll(&p, 1, 2000) == 1);
	mbt_ev_step(ev, 0);
	assert(watch_hits == 2);
	mbt_ev_io_free(io);
	close(pfd[0]);
	close(pfd[1]);
}

int
main(void)
{
	struct mbt_ev *ev;
	int rv;

#ifdef HAVE_SD_EVENT
	rv = mbt_ev_sd_new(&ev);
	assert(rv == 0);
	test_watch(ev);
	test_backend(ev, "sd-event");
#endif

#ifdef HAVE_LIBEVENT
	rv = mbt_ev_libevent_new(&ev);
	assert(rv == 0);
	test_watch(ev);
	test_backend(ev, "libevent");
#endif

#ifdef HAVE_POLL_BACKEND
	rv = mbt_ev_new_pollable(&ev);
	assert(rv == 0);
	test_watch(ev);
	test_backend(ev, "poll");
#endif

#if !defined(HAVE_SD_EVENT) && !defined(HAVE_LIBEVENT) && !defined(HAVE_POLL_BACKEND)
	(void)ev;
	(void)rv;
#endif

	return 0;
}
