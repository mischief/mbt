// SPDX-License-Identifier: ISC
/*
 * mbt-dht: standalone DHT lookup tool
 * Usage: mbt-dht <infohash_hex>
 */
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>
#include <err.h>

#include "mbt/event.h"
#include "mbt/dht.h"
#include "mbt/infohash.h"
#include "mbt/log.h"

static struct mbt_ev *ev;
static int peer_count;

static void
on_peer(const char *addr, uint16_t port, void *arg)
{
	(void)arg;
	printf("%s:%hu\n", addr, port);
	peer_count++;
}

static void
on_done(void *arg)
{
	(void)arg;
	fprintf(stderr, "lookup done: %d peers found\n", peer_count);
	mbt_ev_stop(ev);
}

int
main(int argc, char *argv[])
{
	int rv, ch;

	while ((ch = getopt(argc, argv, "h")) != -1) {
		switch (ch) {
		case 'h':
		default:
			fprintf(stderr, "usage: mbt-dht <infohash_hex>\n");
			return ch == 'h' ? 0 : 1;
		}
	}
	argc -= optind;
	argv += optind;

	if (argc < 1) {
		fprintf(stderr, "usage: mbt-dht <infohash_hex>\n");
		return 1;
	}

	mbt_log_set(mbt_log_stderr, NULL);
	mbt_log_set_debug(MBT_LOG_DHT);

	struct mbt_infohash *ih;
	rv = mbt_infohash_new(&ih, MBT_INFOHASH_SHA1, argv[0]);
	if (rv < 0)
		errx(1, "invalid infohash: %s", argv[0]);

#ifdef HAVE_SD_EVENT
	rv = mbt_ev_sd_new(&ev);
#elif defined(HAVE_LIBEVENT)
	rv = mbt_ev_libevent_new(&ev);
#endif
	if (rv < 0) errx(1, "event: %s", strerror(-rv));

	struct mbt_dht *dht;
	rv = mbt_dht_new(&dht, ev, 6881);
	if (rv < 0) errx(1, "dht: %s", strerror(-rv));

	/* Bootstrap from well-known nodes */
	mbt_dht_bootstrap(dht, "router.bittorrent.com", 6881);
	mbt_dht_bootstrap(dht, "dht.transmissionbt.com", 6881);

	/* Start lookup (bootstrap nodes are in routing table as seeds) */
	rv = mbt_dht_get_peers(dht, ih, on_peer, on_done, NULL);
	if (rv < 0) errx(1, "get_peers: %s", strerror(-rv));

	rv = mbt_ev_run(ev);

	fprintf(stderr, "routing table: %d nodes\n", mbt_dht_node_count(dht));

	mbt_dht_free(dht);
	mbt_infohash_unref(ih);
	mbt_ev_destroy(ev);
	return peer_count > 0 ? 0 : 1;
}
