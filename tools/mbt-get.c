// SPDX-License-Identifier: ISC
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <signal.h>
#include <unistd.h>
#include <time.h>
#include <err.h>

#include <curl/curl.h>

#include "mbt/event.h"
#include "mbt/infohash.h"
#include "mbt/session.h"
#include "mbt/torrent.h"
#include "mbt/tracker.h"
#include "mbt/log.h"
#include "mbt/be.h"
#include "mbt/hex.h"

#include "mbt/sha1_compat.h"

static struct mbt_session *session;
static int seed_mode;

/* --- curl HTTP callback (async-shaped, blocks internally) --- */

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
curl_http_fn(struct mbt_http_request *req, mbt_http_done_fn done_cb, void *done_arg, void *arg)
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

	struct mbt_http_response resp = {0};
	if (rc == CURLE_OK) {
		resp.data = buf.data;
		resp.len = buf.len;
	} else {
		free(buf.data);
	}
	done_cb(&resp, done_arg);
	return 0;
}

/* --- Callbacks --- */

#define RATE_SLOTS 10  /* 1 slot per second, 10s window */

static uint64_t rate_slots[RATE_SLOTS];
static int rate_idx;
static uint64_t rate_last_sec;

static uint64_t
now_ms(void)
{
	struct timespec ts;
	clock_gettime(CLOCK_MONOTONIC, &ts);
	return (uint64_t)ts.tv_sec * 1000 + ts.tv_nsec / 1000000;
}

static float
rate_kbps(void)
{
	uint64_t total = 0;
	for (int i = 0; i < RATE_SLOTS; i++)
		total += rate_slots[i];
	return (float)total / RATE_SLOTS / 1024.0f; /* KB/s */
}

static void
on_piece(struct mbt_torrent *t, uint32_t piece, void *arg)
{
	(void)arg;
	(void)piece;

	uint32_t plen = mbt_torrent_piece_length(t);
	uint64_t now = now_ms();
	uint64_t sec = now / 1000;

	/* Advance slots if time moved forward */
	while (rate_last_sec && sec > rate_last_sec) {
		rate_last_sec++;
		rate_idx = (rate_idx + 1) % RATE_SLOTS;
		rate_slots[rate_idx] = 0;
	}
	if (!rate_last_sec)
		rate_last_sec = sec;

	rate_slots[rate_idx] += plen;

	fprintf(stderr, "\r%.1f%% [%u/%u pieces, %d peers] %.0f KB/s   ",
	        mbt_torrent_progress(t) * 100.0f,
	        mbt_torrent_have_count(t),
	        mbt_torrent_piece_count(t),
	        mbt_torrent_peer_count(t),
	        rate_kbps());
}

static void
on_complete(struct mbt_torrent *t, void *arg)
{
	(void)arg;
	fprintf(stderr, "\ndownload complete: %s\n", mbt_torrent_name(t));
	if (!seed_mode)
		mbt_session_stop(session);
	else
		fprintf(stderr, "seeding...\n");
}

static void
on_signal(int sig)
{
	(void)sig;
	fprintf(stderr, "\ninterrupted, shutting down...\n");
	mbt_session_stop(session);
}

static void
usage(const char *prog)
{
	fprintf(stderr, "usage: %s [-dDPSL] [-l port] [-o dir] [-p max_peers] [-r KB/s] [-s strategy] <file.torrent | magnet:?...>\n", prog);
	fprintf(stderr, "  -d            enable debug logging\n");
	fprintf(stderr, "  -D            enable DHT\n");
	fprintf(stderr, "  -P            enable PCP port mapping\n");
	fprintf(stderr, "  -l port       listen port (default: 6881)\n");
	fprintf(stderr, "  -o dir        download directory (default: .)\n");
	fprintf(stderr, "  -p N          max peers per torrent (default: 30)\n");
	fprintf(stderr, "  -r KB/s       download rate limit (default: unlimited)\n");
	fprintf(stderr, "  -s strategy   piece picker: sequential (default) or random\n");
	fprintf(stderr, "  -S            seed after download completes\n");
	exit(1);
}

int
main(int argc, char *argv[])
{
	int rv, ch;
	const char *download_dir = ".";
	int debug = 0;
	int no_lsd = 0;
	int use_dht = 0;
	int use_pcp = 0;
	int max_peers = 0;
	int listen_port = 6881;
	uint64_t dl_rate = 0;
	mbt_picker_fn picker = mbt_picker_sequential;

	while ((ch = getopt(argc, argv, "dhDPSLl:o:p:r:s:")) != -1) {
		switch (ch) {
		case 'd':
			debug = 1;
			break;
		case 'S':
			seed_mode = 1;
			break;
		case 'L':
			no_lsd = 1;
			break;
		case 'D':
			use_dht = 1;
			break;
		case 'P':
			use_pcp = 1;
			break;
		case 'l':
			listen_port = atoi(optarg);
			break;
		case 'o':
			download_dir = optarg;
			break;
		case 'p':
			max_peers = atoi(optarg);
			break;
		case 'r':
			dl_rate = (uint64_t)atoi(optarg) * 1024;
			break;
		case 's':
			if (strcmp(optarg, "random") == 0)
				picker = mbt_picker_random;
			else if (strcmp(optarg, "sequential") == 0)
				picker = mbt_picker_sequential;
			else {
				fprintf(stderr, "unknown picker: %s (use sequential or random)\n", optarg);
				return 1;
			}
			break;
		case 'h':
		default:
			usage(argv[0]);
		}
	}

	argc -= optind;
	argv += optind;

	if (argc < 1)
		usage(argv[-optind]);

	signal(SIGPIPE, SIG_IGN);
	signal(SIGINT, on_signal);
	signal(SIGTERM, on_signal);
	curl_global_init(CURL_GLOBAL_DEFAULT);

	mbt_log_set(mbt_log_stderr, NULL);
	if (debug)
		mbt_log_set_debug(MBT_LOG_SESSION | MBT_LOG_TORRENT | MBT_LOG_PEER | MBT_LOG_DHT);

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
	cfg.picker = picker;
	cfg.listen_port = listen_port;
	cfg.http = curl_http_fn;
	cfg.enable_pcp = use_pcp;
	cfg.download_rate = dl_rate;
	if (max_peers)
		cfg.max_peers = max_peers;

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

	if (!no_lsd)
		mbt_session_enable_lsd(session);
	if (use_dht)
		mbt_session_enable_dht(session);

	rv = mbt_session_run(session);

	mbt_session_free(session);
	mbt_ev_destroy(cfg.ev);
	curl_global_cleanup();
	return rv < 0 ? 1 : 0;
}
