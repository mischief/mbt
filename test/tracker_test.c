// SPDX-License-Identifier: ISC
#include <stdio.h>
#include <string.h>
#include <signal.h>
#include <err.h>

#include "mbt/event.h"
#include "mbt/infohash.h"
#include "mbt/tracker.h"
#include "mbt/log.h"

static struct mbt_ev *ev;

static void
on_peer(struct mbt_infohash *ih, const char *addr, uint16_t port, void *arg)
{
	(void)ih;
	(void)arg;
	printf("peer: %s:%hu\n", addr, port);
}

static void
on_done(struct mbt_infohash *ih, int err, int interval, void *arg)
{
	(void)ih;
	(void)arg;
	if (err < 0)
		fprintf(stderr, "tracker error: %s\n", strerror(-err));
	else
		fprintf(stderr, "tracker done, interval=%ds\n", interval);
	mbt_ev_stop(ev);
}

int
main(void)
{
	struct mbt_infohash *ih = NULL;
	int rv;
	uint8_t peer_id[20];
	memcpy(peer_id, "-MB0001-testtrack!!", 20);

	signal(SIGPIPE, SIG_IGN);

	mbt_log_set(mbt_log_stderr, NULL);
	mbt_log_set_debug(MBT_LOG_SESSION);

	rv = mbt_ev_new_default(&ev);
	if (rv < 0)
		errx(1, "mbt_ev_new: %s", strerror(-rv));

	rv = mbt_infohash_new(&ih, MBT_INFOHASH_SHA1, "6F4370DF4304609A8793CE2B59178DCC8FEBF5E2");
	if (rv < 0)
		errx(1, "infohash: %s", strerror(-rv));

	struct mbt_tracker_request req = {
		.ev = ev,
		.ih = ih,
		.peer_id = peer_id,
		.listen_port = 6881,
		.left = 702545920,
		.peer_cb = on_peer,
		.done_cb = on_done,
	};

	rv = mbt_tracker_udp_announce("udp://tracker.opentrackr.org:1337/announce", &req);
	if (rv < 0)
		errx(1, "announce: %s", strerror(-rv));

	rv = mbt_ev_run(ev);

	mbt_infohash_unref(ih);
	mbt_ev_destroy(ev);
	return rv < 0 ? 1 : 0;
}
