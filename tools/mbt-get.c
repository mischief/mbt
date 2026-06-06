#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <signal.h>
#include <unistd.h>
#include <err.h>

#include <curl/curl.h>

#include "event.h"
#include "infohash.h"
#include "session.h"
#include "torrent.h"
#include "tracker.h"
#include "log.h"
#include "be.h"
#include "hex.h"

#include <sha1.h>

static struct mbt_session *session;

/* --- curl HTTP callback --- */

struct curl_buf {
	char *data;
	size_t len;
};

static size_t
curl_write_cb(void *ptr, size_t size, size_t nmemb, void *v)
{
	struct curl_buf *b = v;
	size_t total = size * nmemb;
	char *tmp = realloc(b->data, b->len + total);
	if (!tmp) return 0;
	memcpy(tmp + b->len, ptr, total);
	b->data = tmp;
	b->len += total;
	return total;
}

static int
curl_http_fn(struct mbt_http_request *req, struct mbt_http_response *resp, void *arg)
{
	(void)arg;
	CURL *c = curl_easy_init();
	if (!c) return -1;

	struct curl_buf buf = {0};

	curl_easy_setopt(c, CURLOPT_URL, req->url);
	curl_easy_setopt(c, CURLOPT_WRITEFUNCTION, curl_write_cb);
	curl_easy_setopt(c, CURLOPT_WRITEDATA, &buf);
	curl_easy_setopt(c, CURLOPT_TIMEOUT, 15L);
	curl_easy_setopt(c, CURLOPT_FOLLOWLOCATION, 1L);

	CURLcode rc = curl_easy_perform(c);
	curl_easy_cleanup(c);

	if (rc != CURLE_OK) {
		free(buf.data);
		return -1;
	}

	resp->data = buf.data;
	resp->len = buf.len;
	return 0;
}

/* --- Callbacks --- */

static void
on_piece(struct mbt_torrent *t, uint32_t piece, void *arg)
{
	(void)arg;
	(void)piece;
	fprintf(stderr, "\r%.1f%%", mbt_torrent_progress(t) * 100.0f);
}

static void
on_complete(struct mbt_torrent *t, void *arg)
{
	(void)arg;
	fprintf(stderr, "\ndownload complete: %s\n", mbt_torrent_name(t));
	mbt_session_stop(session);
}

static void
usage(const char *prog)
{
	fprintf(stderr, "usage: %s [-d] [-o dir] <file.torrent | magnet:?...>\n", prog);
	fprintf(stderr, "  -d        enable debug logging\n");
	fprintf(stderr, "  -o dir    download directory (default: .)\n");
	exit(1);
}

int
main(int argc, char *argv[])
{
	int rv, ch;
	const char *download_dir = ".";
	int debug = 0;

	while ((ch = getopt(argc, argv, "do:")) != -1) {
		switch (ch) {
		case 'd':
			debug = 1;
			break;
		case 'o':
			download_dir = optarg;
			break;
		default:
			usage(argv[0]);
		}
	}

	argc -= optind;
	argv += optind;

	if (argc < 1)
		usage(argv[-optind]);

	signal(SIGPIPE, SIG_IGN);
	curl_global_init(CURL_GLOBAL_DEFAULT);

	mbt_log_set(mbt_log_stderr, NULL);
	if (debug)
		mbt_log_set_debug(MBT_LOG_SESSION | MBT_LOG_TORRENT | MBT_LOG_PEER);

	struct mbt_session_config cfg = {0};
#ifdef HAVE_SD_EVENT
	rv = mbt_ev_sd_new(&cfg.ev);
#elif defined(HAVE_LIBEVENT)
	rv = mbt_ev_libevent_new(&cfg.ev);
#endif
	if (rv < 0)
		errx(1, "event: %s", strerror(-rv));

	cfg.storage_ops = mbt_storage_file_ops();
	cfg.storage_ctx = (void *)download_dir;
	cfg.picker = mbt_picker_sequential;
	cfg.listen_port = 6881;
	cfg.http = curl_http_fn;

	rv = mbt_session_new(&session, &cfg);
	if (rv < 0)
		errx(1, "session: %s", strerror(-rv));

	/* Add torrent or magnet */
	if (strncmp(argv[0], "magnet:", 7) == 0) {
		rv = mbt_session_add_magnet(session, argv[0]);
		if (rv < 0)
			errx(1, "add_magnet: %s", strerror(-rv));
	} else {
		rv = mbt_session_add_torrent_file(session, argv[0]);
		if (rv < 0)
			errx(1, "add_torrent: %s", strerror(-rv));
	}

	/* Set callbacks on the torrent */
	struct mbt_torrent *t = mbt_session_torrent_first(session);
	if (t) {
		struct mbt_torrent_callbacks cb = {
			.on_piece_complete = on_piece,
			.on_complete = on_complete,
		};
		mbt_torrent_set_callbacks(t, &cb, NULL);
		fprintf(stderr, "downloading: %s\n", mbt_torrent_name(t) ? mbt_torrent_name(t) : "(metadata pending)");
	}

	mbt_session_enable_lsd(session);

	rv = mbt_session_run(session);

	mbt_session_free(session);
	curl_global_cleanup();
	return rv < 0 ? 1 : 0;
}
