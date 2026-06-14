// SPDX-License-Identifier: ISC
// mbt-dhtd — persistent DHT node (bootstrap, run forever, log queries)
#include <signal.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>

#include "mbt/mbt.h"
#include "mbt/dht.h"

static void usage(const char *prog)
{
	fprintf(stderr, "usage: %s [-p port] [-b host:port] [-d]\n", prog);
	fprintf(stderr, "  -p port       DHT listen port (default: 6881)\n");
	fprintf(stderr, "  -b host:port  bootstrap node (default: router.bittorrent.com:6881)\n");
	fprintf(stderr, "  -d            enable debug logging\n");
	exit(1);
}

int main(int argc, char *argv[])
{
	int port = 6881, debug = 0, rv, ch;
	const char *bootstrap = "router.bittorrent.com:6881";

	while ((ch = getopt(argc, argv, "p:b:dh")) != -1) {
		switch (ch) {
		case 'p': port = atoi(optarg); break;
		case 'b': bootstrap = optarg; break;
		case 'd': debug = 1; break;
		default: usage(argv[0]);
		}
	}

	signal(SIGPIPE, SIG_IGN);

	mbt_log_set(mbt_log_stderr, NULL);
	if (debug)
		mbt_log_set_debug(MBT_LOG_ALL);

	struct mbt_ev *ev;
	rv = mbt_ev_new_default(&ev);
	if (rv < 0) { fprintf(stderr, "ev_new: %s\n", strerror(-rv)); return 1; }

	struct mbt_dht *dht;
	rv = mbt_dht_new(&dht, ev, port);
	if (rv < 0) { fprintf(stderr, "dht_new: %s\n", strerror(-rv)); return 1; }

	/* Parse bootstrap host:port */
	char host[256];
	uint16_t bport = 6881;
	const char *colon = strrchr(bootstrap, ':');
	if (colon) {
		snprintf(host, sizeof(host), "%.*s", (int)(colon - bootstrap), bootstrap);
		bport = atoi(colon + 1);
	} else {
		snprintf(host, sizeof(host), "%s", bootstrap);
	}

	mbt_dht_bootstrap(dht, host, bport);
	fprintf(stderr, "DHT node running on port %d, bootstrap %s:%hu\n", port, host, bport);
	fprintf(stderr, "routing table: %d nodes\n", mbt_dht_node_count(dht));

	rv = mbt_ev_run(ev);

	mbt_dht_free(dht);
	mbt_ev_destroy(ev);
	return 0;
}
