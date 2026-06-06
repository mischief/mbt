#include <assert.h>
#include <stdio.h>
#include <string.h>
#include <unistd.h>
#include <fcntl.h>

#include "event.h"

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

	write(pfd[1], "hello", 5);

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

int
main(void)
{
	struct mbt_ev *ev;
	int rv;

#ifdef HAVE_SD_EVENT
	rv = mbt_ev_sd_new(&ev);
	assert(rv == 0);
	test_backend(ev, "sd-event");
#endif

#ifdef HAVE_LIBEVENT
	rv = mbt_ev_libevent_new(&ev);
	assert(rv == 0);
	test_backend(ev, "libevent");
#endif

	return 0;
}
