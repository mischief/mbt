// SPDX-License-Identifier: ISC
// test/poll_test.c — test pollable event loop with plain poll(2)
#include <assert.h>
#include <stdio.h>
#include <string.h>
#include <unistd.h>
#include <poll.h>
#include <sys/socket.h>

#include "mbt/event.h"

static int read_count;
static int timer_count;
static char read_buf[64];

static void
on_io(struct mbt_ev_io *io, int fd, uint32_t events, void *arg)
{
	(void)io; (void)events; (void)arg;
	ssize_t n = read(fd, read_buf + read_count, sizeof(read_buf) - read_count);
	if (n > 0) read_count += n;
}

static void
on_timer(struct mbt_ev_timer *timer, void *arg)
{
	(void)timer;
	int *count = arg;
	(*count)++;
}

static void
test_io(void)
{
	printf("test_io\n");

	struct mbt_ev *ev;
	int rv = mbt_ev_new_pollable(&ev);
	assert(rv == 0);

	int sv[2];
	rv = socketpair(AF_UNIX, SOCK_STREAM, 0, sv);
	assert(rv == 0);

	struct mbt_ev_io *io;
	rv = mbt_ev_io_new(ev, &io, sv[0], MBT_EV_READ, on_io, NULL);
	assert(rv == 0);

	/* Write from the other end */
	write(sv[1], "hello", 5);
	close(sv[1]);

	/* Caller-driven poll loop */
	struct pollfd fds[8];
	int n = mbt_ev_pollfd(ev, fds, 8);
	assert(n == 1);
	assert(fds[0].fd == sv[0]);
	assert(fds[0].events & POLLIN);

	int ms = mbt_ev_timeout_ms(ev);
	assert(ms == -1); /* no timers */

	rv = poll(fds, n, 100);
	assert(rv > 0);
	mbt_ev_dispatch(ev, fds, n);

	assert(read_count == 5);
	assert(memcmp(read_buf, "hello", 5) == 0);

	mbt_ev_io_free(io);
	close(sv[0]);
	mbt_ev_destroy(ev);
}

static void
test_timer(void)
{
	printf("test_timer\n");

	struct mbt_ev *ev;
	int rv = mbt_ev_new_pollable(&ev);
	assert(rv == 0);

	timer_count = 0;
	struct mbt_ev_timer *t;
	rv = mbt_ev_timer_new(ev, &t, 1000, on_timer, &timer_count); /* 1ms */
	assert(rv == 0);

	int ms = mbt_ev_timeout_ms(ev);
	assert(ms >= 0 && ms <= 2);

	/* Wait for timer */
	struct pollfd fds[1];
	int n = mbt_ev_pollfd(ev, fds, 1);
	assert(n == 0); /* no IO, just timer */
	poll(fds, 0, 5); /* sleep 5ms */
	mbt_ev_dispatch(ev, fds, 0);

	assert(timer_count == 1);

	mbt_ev_timer_free(t);
	mbt_ev_destroy(ev);
}

static void
test_run(void)
{
	printf("test_run\n");

	struct mbt_ev *ev;
	int rv = mbt_ev_new_pollable(&ev);
	assert(rv == 0);

	/* Timer that stops the loop */
	struct mbt_ev_timer *t;
	int count = 0;
	rv = mbt_ev_timer_new(ev, &t, 5000, on_timer, &count); /* 5ms */
	assert(rv == 0);

	/* Use built-in run() — should work with poll backend too */
	/* But we need to stop it, so use a timer that stops */
	/* Instead just verify dispatch works in a manual loop */
	struct pollfd fds[1];
	for (int i = 0; i < 20 && count == 0; i++) {
		int n = mbt_ev_pollfd(ev, fds, 1);
		int ms = mbt_ev_timeout_ms(ev);
		poll(fds, n, ms > 0 ? ms : 1);
		mbt_ev_dispatch(ev, fds, n);
	}

	assert(count >= 1);

	mbt_ev_timer_free(t);
	mbt_ev_destroy(ev);
}

int
main(void)
{
	test_io();
	test_timer();
	test_run();
	printf("all poll tests passed\n");
	return 0;
}
