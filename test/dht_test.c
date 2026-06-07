/*
 * test/dht_test.c — self-contained DHT test on localhost
 *
 * 3 nodes on localhost. A announces to B, C looks up via B and finds A.
 */
#include <assert.h>
#include <stdio.h>
#include <string.h>
#include <unistd.h>

#include "mbt/event.h"
#include "mbt/dht.h"
#include "mbt/infohash.h"

#define TEST_HASH "AAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAA"

static struct mbt_ev *ev;
static struct mbt_dht *node_a, *node_b, *node_c;
static int found_peer;

static void on_peer(const char *addr, uint16_t port, void *arg)
{
	(void)arg;
	fprintf(stderr, "  C found peer: %s:%hu\n", addr, port);
	found_peer = 1;
}

static void on_lookup_done(void *arg)
{
	(void)arg;
	mbt_ev_stop(ev);
}

static void do_lookup(struct mbt_ev_timer *timer, void *arg)
{
	(void)timer; (void)arg;
	struct mbt_infohash *ih;
	mbt_infohash_new(&ih, MBT_INFOHASH_SHA1, TEST_HASH);
	fprintf(stderr, "  C: starting lookup\n");
	mbt_dht_get_peers(node_c, ih, on_peer, on_lookup_done, NULL);
	mbt_infohash_unref(ih);
}

static void do_announce_after_getpeers(void *arg)
{
	(void)arg;
	struct mbt_infohash *ih;
	mbt_infohash_new(&ih, MBT_INFOHASH_SHA1, TEST_HASH);
	fprintf(stderr, "  A: announcing (have tokens now)\n");
	mbt_dht_announce(node_a, ih, 6881);
	mbt_infohash_unref(ih);
}

static void do_announce(struct mbt_ev_timer *timer, void *arg)
{
	(void)timer; (void)arg;
	struct mbt_infohash *ih;
	mbt_infohash_new(&ih, MBT_INFOHASH_SHA1, TEST_HASH);
	fprintf(stderr, "  A: get_peers (to collect tokens)\n");
	mbt_dht_get_peers(node_a, ih, NULL, do_announce_after_getpeers, NULL);
	mbt_infohash_unref(ih);
}

static void timeout_cb(struct mbt_ev_timer *timer, void *arg)
{
	(void)timer; (void)arg;
	mbt_ev_stop(ev);
}

int main(void)
{
	int rv;
	struct mbt_ev_timer *t1, *t2, *t3;

	mbt_ev_new_default(&ev);

	rv = mbt_dht_new(&node_a, ev, 10001); assert(rv == 0);
	rv = mbt_dht_new(&node_b, ev, 10002); assert(rv == 0);
	rv = mbt_dht_new(&node_c, ev, 10003); assert(rv == 0);

	/* Deterministic IDs */
	uint8_t id_a[20] = {0x10};
	uint8_t id_b[20] = {0x80};
	uint8_t id_c[20] = {0xF0};
	mbt_dht_set_node_id(node_a, id_a);
	mbt_dht_set_node_id(node_b, id_b);
	mbt_dht_set_node_id(node_c, id_c);

	/* Everyone bootstraps from everyone (small network) */
	mbt_dht_bootstrap(node_a, "127.0.0.1", 10002);
	mbt_dht_bootstrap(node_b, "127.0.0.1", 10001);
	mbt_dht_bootstrap(node_b, "127.0.0.1", 10003);
	mbt_dht_bootstrap(node_c, "127.0.0.1", 10002);

	/* t=0.5s: A starts get_peers+announce sequence */
	mbt_ev_timer_new(ev, &t1, 500000ULL, do_announce, NULL);
	/* t=3s: C looks up (give announce time to complete) */
	mbt_ev_timer_new(ev, &t2, 3000000ULL, do_lookup, NULL);
	/* t=8s: safety timeout */
	mbt_ev_timer_new(ev, &t3, 8000000ULL, timeout_cb, NULL);

	rv = mbt_ev_run(ev);

	fprintf(stderr, "dht_test: %s (found_peer=%d, nodes: A=%d B=%d C=%d)\n",
	        found_peer ? "PASS" : "FAIL", found_peer,
	        mbt_dht_node_count(node_a),
	        mbt_dht_node_count(node_b),
	        mbt_dht_node_count(node_c));

	mbt_ev_timer_free(t1);
	mbt_ev_timer_free(t2);
	mbt_ev_timer_free(t3);
	mbt_dht_free(node_a);
	mbt_dht_free(node_b);
	mbt_dht_free(node_c);
	mbt_ev_destroy(ev);

	assert(found_peer);
	return 0;
}
