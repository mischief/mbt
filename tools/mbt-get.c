#include <stdio.h>
#include <string.h>
#include <signal.h>
#include <err.h>

#include "event.h"
#include "infohash.h"
#include "session.h"
#include "torrent.h"
#include "log.h"
#include "be.h"
#include "hex.h"

#include <sha1.h>

static struct mbt_session *session;

static void
on_piece(struct mbt_torrent *t, uint32_t piece, void *arg)
{
	(void)arg;
	(void)piece;
	float pct = mbt_torrent_progress(t) * 100.0f;
	fprintf(stderr, "\r%.1f%%", pct);
}

static void
on_complete(struct mbt_torrent *t, void *arg)
{
	(void)arg;
	fprintf(stderr, "\ndownload complete: %s\n", mbt_torrent_name(t));
	mbt_session_stop(session);
}

int
main(int argc, char *argv[])
{
	int rv;
	const char *torrent_file;
	const char *download_dir = ".";

	if (argc < 2) {
		fprintf(stderr, "usage: %s <file.torrent> [download-dir]\n", argv[0]);
		return 1;
	}

	torrent_file = argv[1];
	if (argc > 2)
		download_dir = argv[2];

	signal(SIGPIPE, SIG_IGN);

	mbt_log_set(mbt_log_stderr, NULL);
	mbt_log_set_debug(MBT_LOG_SESSION | MBT_LOG_TORRENT);

	rv = mbt_session_default(&session, download_dir);
	if (rv < 0)
		errx(1, "session: %s", strerror(-rv));

	rv = mbt_session_add_torrent_file(session, torrent_file);
	if (rv < 0)
		errx(1, "add_torrent_file: %s", strerror(-rv));

	/* Find the torrent to set callbacks — parse infohash from file */
	{
		FILE *fp = fopen(torrent_file, "rb");
		unsigned char *buf = malloc(4*1024*1024);
		size_t len = fread(buf, 1, 4*1024*1024, fp);
		fclose(fp);

		struct be_value *be = NULL;
		be_value_parse(buf, len, &be);
		free(buf);

		struct be_value *info = be_find(be, "info");
		unsigned char *enc; size_t elen;
		be_value_encode(info, &enc, &elen);

		SHA1_CTX ctx; uint8_t digest[20]; char hex[41];
		SHA1Init(&ctx);
		SHA1Update(&ctx, enc, elen);
		SHA1Final(digest, &ctx);
		mbintohex(digest, 20, hex, sizeof(hex));
		free(enc);
		be_value_free(be);

		struct mbt_infohash *ih;
		mbt_infohash_new(&ih, MBT_INFOHASH_SHA1, hex);

		struct mbt_torrent *t = mbt_session_find_torrent(session, ih);
		if (t) {
			struct mbt_torrent_callbacks cb = {
				.on_piece_complete = on_piece,
				.on_complete = on_complete,
			};
			mbt_torrent_set_callbacks(t, &cb, NULL);
			fprintf(stderr, "downloading: %s [%s]\n", mbt_torrent_name(t), hex);
		}

		mbt_infohash_unref(ih);
	}

	mbt_session_enable_lsd(session);

	rv = mbt_session_run(session);

	mbt_session_free(session);
	return rv < 0 ? 1 : 0;
}
