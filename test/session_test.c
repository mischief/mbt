#include <stdio.h>
#include <signal.h>
#include <string.h>
#include <err.h>

#include "event.h"
#include "infohash.h"
#include "session.h"
#include "torrent.h"
#include "log.h"

static void
on_piece_complete(struct mbt_torrent *t, uint32_t piece, void *arg)
{
	fprintf(stderr, "piece %u complete (%.1f%%)\n", piece, mbt_torrent_progress(t) * 100.0f);
}

static void
on_complete(struct mbt_torrent *t, void *arg)
{
	struct mbt_session *s = arg;
	fprintf(stderr, "download complete!\n");
	mbt_session_stop(s);
}

int
main(int argc, char *argv[])
{
	struct mbt_session *s = NULL;
	struct mbt_torrent *t;
	struct infohash *ih;
	int rv;

	const char *hexstr = "6F4370DF4304609A8793CE2B59178DCC8FEBF5E2";

	signal(SIGPIPE, SIG_IGN);

	mbt_log_set(mbt_log_stderr, NULL);
	mbt_log_set_debug(MBT_LOG_SESSION | MBT_LOG_TORRENT);

	rv = mbt_session_default(&s, ".");
	if (rv < 0)
		errx(1, "mbt_session_default: %s", strerror(-rv));

	/* Add torrent by infohash (metadata will come from peer via BEP 9) */
	rv = infohash_new(&ih, INFOHASH_SHA1, hexstr);
	if (rv < 0)
		errx(1, "infohash_new: %s", strerror(-rv));

	rv = mbt_session_add_infohash(s, ih);
	if (rv < 0)
		errx(1, "add_infohash: %s", strerror(-rv));

	t = mbt_session_find_torrent(s, ih);
	struct mbt_torrent_callbacks tcb = {
		.on_piece_complete = on_piece_complete,
		.on_complete = on_complete,
	};
	mbt_torrent_set_callbacks(t, &tcb, s);

	/* Connect to known peers */
	mbt_session_add_peer(s, ih, "192.168.0.10", 28888);    /* sokoban */

	infohash_unref(ih);

	fprintf(stderr, "downloading %s from 2 peers...\n", hexstr);

	rv = mbt_session_run(s);

	mbt_session_free(s);
	return rv < 0 ? 1 : 0;
}
