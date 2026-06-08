// SPDX-License-Identifier: ISC
/*
 * mbt-tracker: standalone UDP + HTTP tracker daemon
 * Usage: mbt-tracker [-u udp_port] [-h http_port]
 */
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <signal.h>
#include <unistd.h>
#include <err.h>

#include "mbt/event.h"
#include "mbt/tracker_server.h"
#include "mbt/log.h"

static struct mbt_ev *ev;

static void on_signal(int sig) { (void)sig; mbt_ev_stop(ev); }

int main(int argc, char *argv[])
{
	int rv, ch;
	uint16_t udp_port = 6969;
	uint16_t http_port = 6969;
	int use_udp = 0, use_http = 0;

	while ((ch = getopt(argc, argv, "u:h:")) != -1) {
		switch (ch) {
		case 'u': udp_port = atoi(optarg); use_udp = 1; break;
		case 'h': http_port = atoi(optarg); use_http = 1; break;
		default:
			fprintf(stderr, "usage: mbt-tracker [-u udp_port] [-h http_port]\n");
			return 1;
		}
	}

	/* Default: both on port 6969 */
	if (!use_udp && !use_http) { use_udp = 1; use_http = 1; }

	mbt_log_set(mbt_log_stderr, NULL);
	mbt_log_set_debug(MBT_LOG_ALL);

	signal(SIGINT, on_signal);
	signal(SIGTERM, on_signal);

	mbt_ev_new_default(&ev);

	struct mbt_tracker_server *udp = NULL;
	struct mbt_tracker_http *http = NULL;

	if (use_udp) {
		rv = mbt_tracker_server_new(&udp, ev, udp_port, NULL);
		if (rv < 0) errx(1, "UDP tracker: %s", strerror(-rv));
		fprintf(stderr, "UDP tracker on port %hu\n", udp_port);
	}

	if (use_http) {
		rv = mbt_tracker_http_new(&http, ev, http_port, NULL);
		if (rv < 0) errx(1, "HTTP tracker: %s", strerror(-rv));
		fprintf(stderr, "HTTP tracker on port %hu\n", http_port);
	}

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

	mbt_ev_destroy(ev);
	return 0;
}
