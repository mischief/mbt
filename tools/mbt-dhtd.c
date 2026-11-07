// SPDX-License-Identifier: ISC
// mbt-dhtd — persistent DHT node (bootstrap, run forever, log queries)
#include <signal.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>
#include <stdint.h>

#include "mbt/dht.h"
#include "mbt/event.h"
#include "mbt/log.h"
#include "tools/args.h"
#include "banned.h"

static void usage(const char *prog)
{
	fprintf(stderr, "usage: %s [-p port] [-b host:port] [-d]\n", prog);
	fprintf(stderr, "  -p port       DHT listen port (default: 6881)\n");
	fprintf(stderr, "  -b host:port  bootstrap node (default: dht.transmissionbt.com:6881)\n");
	fprintf(stderr, "  -d            enable debug logging\n");
	fprintf(stderr, "  -s count      infohashes to store peers for (default: 4096)\n");
	fprintf(stderr, "  -i count      BEP 44 items to store (default: 1024)\n");
	exit(1);
}

int main(int argc, char *argv[])
{
	int port = 6881, debug = 0, max_stored = 4096, max_items = 1024, rv, ch;
	const char *bootstrap = "dht.transmissionbt.com:6881";

	while ((ch = getopt(argc, argv, "p:b:ds:i:h")) != -1) {
		switch (ch) {
		case 'p': port = mbt_arg_int(optarg, 0, UINT16_MAX); break;
		case 'b': bootstrap = optarg; break;
		case 'd': debug = 1; break;
		case 's': max_stored = mbt_arg_int(optarg, 1, 1 << 24); break;
		case 'i': max_items = mbt_arg_int(optarg, 1, 1 << 24); break;
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
	struct mbt_dht_config cfg = {
		.ev = ev, .port = (uint16_t)port, .max_stored = max_stored, .max_items = max_items,
	};

	rv = mbt_dht_new(&dht, &cfg);
	if (rv < 0) { fprintf(stderr, "dht_new: %s\n", strerror(-rv)); return 1; }

#ifdef __OpenBSD__
	if (unveil("/", "") == -1 || pledge("stdio inet dns", NULL) == -1)
		err(1, "sandbox");
#endif

	/* Parse bootstrap host:port */
	char host[256];
	uint16_t bport = 6881;
	const char *colon = strrchr(bootstrap, ':');
	if (colon) {
		snprintf(host, sizeof(host), "%.*s", (int)(colon - bootstrap), bootstrap);
		bport = mbt_arg_int(colon + 1, 1, UINT16_MAX);
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
