// SPDX-License-Identifier: ISC
// Test: BEP 44 — two DHT nodes, put+get immutable item
#include <assert.h>
#include <string.h>
#include <stdio.h>
#include <sha1.h>
#include <stdint.h>

#include "mbt/dht.h"
#include "mbt/event.h"

struct mbt_ev_timer;

static int got_item;
static uint8_t got_value[64];
static size_t got_vlen;

static void
item_cb(const uint8_t *value, size_t vlen, int64_t seq,
        const uint8_t *k, void *arg)
{
	(void)arg; (void)seq; (void)k;
	got_item = 1;
	got_vlen = vlen;
	if (vlen <= sizeof(got_value))
		memcpy(got_value, value, vlen);
}

static int stop_count;
static struct mbt_ev *ev;

static void
tick_cb(struct mbt_ev_timer *timer, void *v)
{
	(void)v;
	stop_count++;
	if (stop_count >= 20) {
		mbt_ev_stop(ev);
	} else {
		mbt_ev_timer_mod(timer, 100000ULL);
	}
}

int main(void)
{
	int rv;

	rv = mbt_ev_new_default(&ev);
	assert(rv == 0);

	/* Create two DHT nodes on localhost */
	struct mbt_dht *a, *b;
	rv = mbt_dht_new(&a, ev, 0); assert(rv == 0);
	rv = mbt_dht_new(&b, ev, 0); assert(rv == 0);

	/* Bootstrap B → A */
	rv = mbt_dht_bootstrap(b, "127.0.0.1", mbt_dht_port(a));
	assert(rv == 0);

	/* Put an immutable item on A */
	const char *value = "12:Hello World!";
	rv = mbt_dht_put_immutable(a, value, strlen(value), NULL, NULL);
	assert(rv == 0);

	/* Compute expected target */
	uint8_t target[20];
	SHA1_CTX ctx;
	SHA1Init(&ctx);
	SHA1Update(&ctx, (const uint8_t *)value, strlen(value));
	SHA1Final(target, &ctx);

	/* Get from A directly (local) */
	got_item = 0;
	rv = mbt_dht_get_immutable(a, target, item_cb, NULL);
	assert(rv == 0);
	assert(got_item == 1);
	assert(got_vlen == strlen(value));
	assert(memcmp(got_value, value, got_vlen) == 0);

	/* Run event loop briefly to let B bootstrap */
	struct mbt_ev_timer *tick;
	mbt_ev_timer_new(ev, &tick, 100000ULL, tick_cb, NULL); /* 100ms ticks */
	mbt_ev_run(ev);

	/* B should now be able to query A via get message */
	/* For now, test that local put+get works correctly */

	fprintf(stderr, "bep44_test: PASS (immutable put+get, local storage)\n");

	mbt_dht_free(a);
	mbt_dht_free(b);
	mbt_ev_destroy(ev);
	return 0;
}
