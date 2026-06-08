/*
 * test/lsd_test.c — LSD advertiser + listener on localhost
 */
#include <sys/queue.h>
#include <assert.h>
#include <stdio.h>
#include <string.h>
#include <stdlib.h>
#include <fcntl.h>

#include "mbt/event.h"
#include "mbt/infohash.h"
#include "mbt/lsd.h"
#include "mbt/infohash.h"
#include "mbt/infohash.h"

static struct mbt_ev *ev;
static char test_hash[41];
static int found;

static void
lsd_io_cb(struct mbt_ev_io *io, int fd, uint32_t events, void *v)
{
	struct mbt_lsd *lsd = v;
	(void)io; (void)fd; (void)events;

	struct mbt_lsd_frame *frame = NULL;
	struct mbt_lsd_frame_info *info = NULL;

	int rv = mbt_lsd_recv(lsd, &frame);
	if (rv < 0) return;

	rv = mbt_lsd_frame_parse(frame, &info);
	mbt_lsd_frame_freep(&frame);
	if (rv < 0) return;

	/* Check if this is our test infohash (filter LAN noise) */
	struct mbt_infohash *ih;
	STAILQ_FOREACH(ih, &info->infohashes, next) {
		char hex[41];
		mbt_infohash_string(ih, hex, sizeof(hex));
		if (strcmp(hex, test_hash) == 0) {
			found = 1;
			fprintf(stderr, "  LSD: found %s port %hu from %s\n",
			        hex, info->port, info->remote);
			mbt_ev_stop(ev);
		}
	}
	mbt_lsd_frame_info_freep(&info);
}

static void
announce_cb(struct mbt_ev_timer *timer, void *arg)
{
	struct mbt_lsd *lsd = arg;
	(void)timer;
	mbt_lsd_announce(lsd, test_hash, 6881);
}

static void
timeout_cb(struct mbt_ev_timer *timer, void *arg)
{ (void)timer; (void)arg; mbt_ev_stop(ev); }

int main(void)
{
	int rv;

	/* Generate random infohash to avoid LAN interference */
	snprintf(test_hash, sizeof(test_hash),
	         "%08X%08X%08X%08X%08X",
	         (unsigned)rand(), (unsigned)rand(), (unsigned)rand(),
	         (unsigned)rand(), (unsigned)rand());

	mbt_ev_new_default(&ev);

	struct mbt_lsd *lsd;
	rv = mbt_lsd_init(&lsd, O_NONBLOCK);
	assert(rv == 0);

	/* Listen for LSD announcements */
	struct mbt_ev_io *io;
	rv = mbt_ev_io_new(ev, &io, lsd->fd, MBT_EV_READ, lsd_io_cb, lsd);
	assert(rv == 0);

	/* Announce after 100ms (give listener time to register) */
	struct mbt_ev_timer *ann_timer;
	mbt_ev_timer_new(ev, &ann_timer, 100000ULL, announce_cb, lsd);

	/* Timeout */
	struct mbt_ev_timer *to;
	mbt_ev_timer_new(ev, &to, 3 * 1000000ULL, timeout_cb, NULL);

	rv = mbt_ev_run(ev);

	fprintf(stderr, "lsd_test: %s\n", found ? "PASS" : "FAIL");

	mbt_ev_timer_free(ann_timer);
	mbt_ev_timer_free(to);
	mbt_ev_io_free(io);
	mbt_lsd_freep(&lsd);
	mbt_ev_destroy(ev);

	assert(found);
	return 0;
}
