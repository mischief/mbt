#include <stdio.h>
#include <signal.h>
#include <string.h>
#include <err.h>
#include <sysexits.h>

#include "event.h"
#include "infohash.h"
#include "session.h"
#include "torrent.h"

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
	struct infohash *ih = NULL;
	struct mbt_torrent *t;
	int rv;

	const char *hexstr = "C8295CE630F2064F08440DB1534E4992CFE4862A";
	const char *peer_addr = "192.168.0.10";
	uint16_t peer_port = 28888;

	signal(SIGPIPE, SIG_IGN);

	rv = mbt_session_default(&s, ".");
	if (rv < 0)
		errx(1, "mbt_session_default: %s", strerror(-rv));

	rv = infohash_new(&ih, INFOHASH_SHA1, hexstr);
	if (rv < 0)
		errx(1, "infohash_new: %s", strerror(-rv));

	rv = mbt_session_add_infohash(s, ih);
	if (rv < 0)
		errx(1, "mbt_session_add_infohash: %s", strerror(-rv));

	t = mbt_session_find_torrent(s, ih);
	if (!t)
		errx(1, "torrent not found after add");

	struct mbt_torrent_callbacks tcb = {
		.on_piece_complete = on_piece_complete,
		.on_complete = on_complete,
	};
	mbt_torrent_set_callbacks(t, &tcb, s);

	/* Enable LSD so we also discover peers automatically */
	mbt_session_enable_lsd(s);

	/* Connect directly to known peer */
	rv = mbt_session_add_peer(s, ih, peer_addr, peer_port);
	if (rv < 0)
		errx(1, "mbt_session_add_peer: %s", strerror(-rv));

	fprintf(stderr, "connecting to %s:%hu for %s\n", peer_addr, peer_port, hexstr);

	rv = mbt_session_run(s);

	infohash_unref(ih);
	mbt_session_free(s);

	return rv < 0 ? 1 : 0;
}
