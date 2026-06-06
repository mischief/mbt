#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <signal.h>
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
	curl_global_init(CURL_GLOBAL_DEFAULT);

	mbt_log_set(mbt_log_stderr, NULL);
	mbt_log_set_debug(MBT_LOG_SESSION | MBT_LOG_TORRENT);

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

	rv = mbt_session_add_torrent_file(session, torrent_file);
	if (rv < 0)
		errx(1, "add_torrent: %s", strerror(-rv));

	/* Set callbacks on the torrent */
	struct mbt_torrent *t = mbt_session_torrent_first(session);
	if (t) {
		struct mbt_torrent_callbacks cb = {
			.on_piece_complete = on_piece,
			.on_complete = on_complete,
		};
		mbt_torrent_set_callbacks(t, &cb, NULL);
		fprintf(stderr, "downloading: %s\n", mbt_torrent_name(t) ? mbt_torrent_name(t) : "(unknown)");
	}

	mbt_session_enable_lsd(session);

	rv = mbt_session_run(session);

	mbt_session_free(session);
	curl_global_cleanup();
	return rv < 0 ? 1 : 0;
}
