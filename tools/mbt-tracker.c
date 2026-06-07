/*
 * mbt-tracker: standalone UDP tracker daemon
 * Usage: mbt-tracker [-p port]
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
	uint16_t port = 6969;

	while ((ch = getopt(argc, argv, "p:")) != -1) {
		switch (ch) {
		case 'p': port = atoi(optarg); break;
		default:
			fprintf(stderr, "usage: mbt-tracker [-p port]\n");
			return 1;
		}
	}

	mbt_log_set(mbt_log_stderr, NULL);
	mbt_log_set_debug(0xff);

	signal(SIGINT, on_signal);
	signal(SIGTERM, on_signal);

	mbt_ev_new_default(&ev);

	struct mbt_tracker_server *ts;
	rv = mbt_tracker_server_new(&ts, ev, port);
	if (rv < 0) errx(1, "tracker: %s", strerror(-rv));

	fprintf(stderr, "mbt-tracker running on UDP port %hu\n", port);

	mbt_ev_run(ev);

	fprintf(stderr, "shutdown: %d torrents, %d peers tracked\n",
	        mbt_tracker_server_num_torrents(ts),
	        mbt_tracker_server_num_peers(ts));

	mbt_tracker_server_free(ts);
	mbt_ev_destroy(ev);
	return 0;
}
