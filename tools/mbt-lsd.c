/*
 * mbt-lsd: listen for Local Service Discovery announcements
 * and automatically start downloading any discovered torrents.
 */
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>
#include <err.h>

#include "mbt/event.h"
#include "mbt/infohash.h"
#include "mbt/session.h"
#include "mbt/storage.h"
#include "mbt/log.h"

static int
on_lsd(struct mbt_session *s, struct mbt_infohash *ih,
       const char *addr, uint16_t port, void *arg)
{
	(void)s; (void)arg;
	char hex[MBT_MBT_INFOHASH_SHA1HEXLEN + 1];
	mbt_infohash_string(ih, hex, sizeof(hex));
	printf("LSD: %s from %s:%hu\n", hex, addr, port);
	return 1; /* accept: add torrent and connect */
}

int
main(int argc, char *argv[])
{
	int rv, c;
	const char *outdir = ".";

	while ((c = getopt(argc, argv, "do:")) != -1) {
		switch (c) {
		case 'd':
			mbt_log_set(mbt_log_stderr, NULL);
			mbt_log_set_debug(MBT_LOG_ALL);
			break;
		case 'o':
			outdir = optarg;
			break;
		default:
			fprintf(stderr, "Usage: %s [-d] [-o dir]\n", argv[0]);
			return 1;
		}
	}

	struct mbt_session_config cfg = {0};

#ifdef HAVE_SD_EVENT
	rv = mbt_ev_sd_new(&cfg.ev);
#elif defined(HAVE_LIBEVENT)
	rv = mbt_ev_libevent_new(&cfg.ev);
#else
#error "No event backend configured"
#endif
	if (rv < 0)
		errx(1, "mbt_ev_new: %s", strerror(-rv));

	cfg.storage_ops = mbt_storage_file_ops();
	cfg.storage_ctx = (void *)outdir;

	struct mbt_session *session;
	rv = mbt_session_new(&session, &cfg);
	if (rv < 0)
		errx(1, "mbt_session_new: %s", strerror(-rv));

	struct mbt_session_callbacks cb = {
		.on_lsd_announce = on_lsd,
	};
	mbt_session_set_callbacks(session, &cb);

	printf("Listening for LSD announcements (download to %s)\n", outdir);

	rv = mbt_session_run(session);
	if (rv < 0)
		errx(1, "mbt_session_run: %s", strerror(-rv));

	mbt_session_free(session);
	return 0;
}
