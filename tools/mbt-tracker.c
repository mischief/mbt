// SPDX-License-Identifier: ISC
/*
 * mbt-tracker: standalone UDP + HTTP tracker daemon
 * Usage: mbt-tracker [-d] [-u udp_port] [-H http_port] [-i interval] [-e expire] [-m max_torrents]
 */
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <signal.h>
#include <unistd.h>
#include <err.h>
#include <stdint.h>

#include "mbt/event.h"
#include "mbt/tracker_server.h"
#include "mbt/log.h"
#include "tools/args.h"
#include "banned.h"

static struct mbt_ev *ev;

static void on_signal(int sig) { (void)sig; mbt_ev_stop(ev); }

int main(int argc, char *argv[])
{
	int rv, ch;
	uint16_t udp_port = 6969;
	uint16_t http_port = 6969;
	int use_udp = 0, use_http = 0, debug = 0;
	int interval = 0, expire = 0, max_torrents = 1 << 20;
	struct mbt_tracker_store *store;

	while ((ch = getopt(argc, argv, "dhu:H:i:e:m:")) != -1) {
		switch (ch) {
		case 'd': debug = 1; break;
		case 'u': udp_port = mbt_arg_int(optarg, 0, UINT16_MAX); use_udp = 1; break;
		case 'H': http_port = mbt_arg_int(optarg, 0, UINT16_MAX); use_http = 1; break;
		case 'i': interval = mbt_arg_int(optarg, 1, 86400); break;
		case 'e': expire = mbt_arg_int(optarg, 1, 86400 * 7); break;
		case 'm': max_torrents = mbt_arg_int(optarg, 1, 1 << 30); break;
		case 'h':
		default:
			fprintf(stderr, "usage: mbt-tracker [-d] [-u udp_port] [-H http_port] [-i interval] "
			        "[-e expire] [-m max_torrents]\n");
			return ch == 'h' ? 0 : 1;
		}
	}

	/* Default: both on port 6969 */
	if (!use_udp && !use_http) { use_udp = 1; use_http = 1; }

	mbt_log_set(mbt_log_stderr, NULL);
	if (debug)
		mbt_log_set_debug(MBT_LOG_ALL);

	signal(SIGINT, on_signal);
	signal(SIGTERM, on_signal);

	mbt_ev_new_default(&ev);

	if ((rv = mbt_tracker_store_new(&store)) < 0)
		errx(1, "store: %s", strerror(-rv));
	mbt_tracker_store_set_max_torrents(store, max_torrents);
	mbt_tracker_store_set_interval(store, interval);
	mbt_tracker_store_set_expire(store, expire);

	struct mbt_tracker_server *udp = NULL;
	struct mbt_tracker_http *http = NULL;

	if (use_udp) {
		rv = mbt_tracker_server_new(&udp, ev, udp_port, store);
		if (rv < 0) errx(1, "UDP tracker: %s", strerror(-rv));
		fprintf(stderr, "UDP tracker on port %hu\n", udp_port);
	}

	if (use_http) {
		rv = mbt_tracker_http_new(&http, ev, http_port, store);
		if (rv < 0) errx(1, "HTTP tracker: %s", strerror(-rv));
		fprintf(stderr, "HTTP tracker on port %hu\n", http_port);
	}

#ifdef __OpenBSD__
	if (unveil("/", "") == -1 || pledge("stdio inet", NULL) == -1)
		err(1, "sandbox");
#endif

	mbt_ev_run(ev);

	if (udp) {
		fprintf(stderr, "UDP: %d torrents, %d peers\n",
		        mbt_tracker_server_num_torrents(udp), mbt_tracker_server_num_peers(udp));
		mbt_tracker_server_free(udp);
	}
	if (http) {
		fprintf(stderr, "HTTP: %d torrents, %d peers\n",
		        mbt_tracker_http_num_torrents(http), mbt_tracker_http_num_peers(http));
		mbt_tracker_http_free(http);
	}

	mbt_tracker_store_free(store);
	mbt_ev_destroy(ev);
	return 0;
}
