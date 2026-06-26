// SPDX-License-Identifier: ISC
#include <assert.h>
#include <stdio.h>
#include <time.h>
#include <stdint.h>

#include "mbt/ratelimit.h"

static void
sleep_usec(uint64_t usec)
{
	struct timespec ts = { .tv_sec = usec / 1000000, .tv_nsec = (usec % 1000000) * 1000 };
	nanosleep(&ts, NULL);
}

static void
test_new_free(void)
{
	struct mbt_ratelimit *rl = NULL;
	int rv = mbt_ratelimit_new(&rl, 1000, 500);
	assert(rv == 0);
	assert(rl != NULL);
	assert(mbt_ratelimit_rate(rl) == 1000);
	assert(mbt_ratelimit_burst(rl) == 500);
	mbt_ratelimit_free(rl);

	/* burst defaults to rate if 0 */
	rv = mbt_ratelimit_new(&rl, 2000, 0);
	assert(rv == 0);
	assert(mbt_ratelimit_burst(rl) == 2000);
	mbt_ratelimit_free(rl);
}

static void
test_unlimited(void)
{
	/* rate=0 means unlimited */
	struct mbt_ratelimit *rl;
	mbt_ratelimit_new(&rl, 0, 0);

	assert(mbt_ratelimit_allow(rl, 999999999) == true);
	assert(mbt_ratelimit_delay(rl, 999999999) == 0);
	assert(mbt_ratelimit_tokens(rl) == UINT64_MAX);

	mbt_ratelimit_free(rl);

	/* NULL limiter also unlimited */
	assert(mbt_ratelimit_allow(NULL, 100) == true);
	assert(mbt_ratelimit_delay(NULL, 100) == 0);
	assert(mbt_ratelimit_tokens(NULL) == UINT64_MAX);
	mbt_ratelimit_consume(NULL, 100); /* no crash */
	mbt_ratelimit_free(NULL);         /* no crash */
}

static void
test_allow_basic(void)
{
	/* 1000 bytes/sec, burst=1000 (starts full) */
	struct mbt_ratelimit *rl;
	mbt_ratelimit_new(&rl, 1000, 1000);

	/* Should allow up to burst */
	assert(mbt_ratelimit_allow(rl, 500) == true);
	assert(mbt_ratelimit_tokens(rl) == 500);
	assert(mbt_ratelimit_allow(rl, 500) == true);
	assert(mbt_ratelimit_tokens(rl) == 0);

	/* Should deny — no tokens left */
	assert(mbt_ratelimit_allow(rl, 1) == false);

	mbt_ratelimit_free(rl);
}

static void
test_refill(void)
{
	/* 10000 bytes/sec, burst=10000 */
	struct mbt_ratelimit *rl;
	mbt_ratelimit_new(&rl, 10000, 10000);

	/* Drain all tokens */
	mbt_ratelimit_consume(rl, 10000);
	assert(mbt_ratelimit_tokens(rl) == 0);

	/* Wait 100ms → should refill ~1000 tokens */
	sleep_usec(100000);

	uint64_t t = mbt_ratelimit_tokens(rl);
	assert(t >= 800 && t <= 1200); /* allow ±20% for scheduling */

	mbt_ratelimit_free(rl);
}

static void
test_delay(void)
{
	/* 1000 bytes/sec, burst=1000 */
	struct mbt_ratelimit *rl;
	mbt_ratelimit_new(&rl, 1000, 1000);

	/* Drain */
	mbt_ratelimit_consume(rl, 1000);

	/* Need 100 bytes → delay = 100/1000 * 1e6 = 100000 usec */
	uint64_t d = mbt_ratelimit_delay(rl, 100);
	assert(d >= 90000 && d <= 110000);

	/* Need 500 bytes → delay = 500000 usec */
	d = mbt_ratelimit_delay(rl, 500);
	assert(d >= 490000 && d <= 510000);

	/* If tokens available, delay=0 */
	sleep_usec(200000); /* refill ~200 tokens */
	d = mbt_ratelimit_delay(rl, 100);
	assert(d == 0);

	mbt_ratelimit_free(rl);
}

static void
test_consume(void)
{
	struct mbt_ratelimit *rl;
	mbt_ratelimit_new(&rl, 1000, 1000);

	mbt_ratelimit_consume(rl, 700);
	assert(mbt_ratelimit_tokens(rl) == 300);

	/* Over-consume clamps to 0 */
	mbt_ratelimit_consume(rl, 500);
	assert(mbt_ratelimit_tokens(rl) == 0);

	mbt_ratelimit_free(rl);
}

static void
test_set_rate(void)
{
	struct mbt_ratelimit *rl;
	mbt_ratelimit_new(&rl, 1000, 1000);

	mbt_ratelimit_consume(rl, 1000);
	mbt_ratelimit_set_rate(rl, 2000); /* double the rate */
	assert(mbt_ratelimit_rate(rl) == 2000);

	/* Wait 100ms → refill at 2000/s → ~200 tokens */
	sleep_usec(100000);
	uint64_t t = mbt_ratelimit_tokens(rl);
	assert(t >= 150 && t <= 250);

	mbt_ratelimit_free(rl);
}

static void
test_set_burst(void)
{
	struct mbt_ratelimit *rl;
	mbt_ratelimit_new(&rl, 1000, 1000);

	/* Tokens start at burst (1000) */
	assert(mbt_ratelimit_tokens(rl) == 1000);

	/* Reduce burst → tokens clamped */
	mbt_ratelimit_set_burst(rl, 500);
	assert(mbt_ratelimit_burst(rl) == 500);
	assert(mbt_ratelimit_tokens(rl) == 500);

	/* Increase burst → tokens stay, room to grow */
	mbt_ratelimit_set_burst(rl, 2000);
	assert(mbt_ratelimit_tokens(rl) == 500);

	mbt_ratelimit_free(rl);
}

static void
test_burst_cap(void)
{
	/* Tokens never exceed burst even with long idle */
	struct mbt_ratelimit *rl;
	mbt_ratelimit_new(&rl, 100000, 1000);

	mbt_ratelimit_consume(rl, 500);
	sleep_usec(200000); /* refill 20000 tokens at 100000/s, but capped at burst=1000 */

	assert(mbt_ratelimit_tokens(rl) == 1000);

	mbt_ratelimit_free(rl);
}

static void
test_large_request(void)
{
	/* Request larger than burst: delay should be calculated correctly */
	struct mbt_ratelimit *rl;
	mbt_ratelimit_new(&rl, 1000, 500);

	/* Bucket is full at 500. Request 1000 → deficit=500 → delay=500ms */
	uint64_t d = mbt_ratelimit_delay(rl, 1000);
	assert(d >= 490000 && d <= 510000);

	mbt_ratelimit_free(rl);
}

int
main(void)
{
	test_new_free();
	test_unlimited();
	test_allow_basic();
	test_refill();
	test_delay();
	test_consume();
	test_set_rate();
	test_set_burst();
	test_burst_cap();
	test_large_request();

	fprintf(stderr, "ratelimit_test: PASS (10 tests)\n");
	return 0;
}
