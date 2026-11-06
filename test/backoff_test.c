// SPDX-License-Identifier: ISC
/* Delay sequence, cap, reset, and the bounds of the spread. */
#include <assert.h>
#include <stdio.h>

#include "backoff.h"

static void
test_exact(void)
{
	static const uint64_t want[] = {5, 10, 20, 40, 60, 60};
	struct mbt_backoff b;

	mbt_backoff_init(&b, 5, 60, 200, 0);
	for (unsigned i = 0; i < sizeof(want) / sizeof(want[0]); i++) {
		assert(mbt_backoff_next(&b) == want[i]);
		assert(b.attempts == i + 1);
	}
	mbt_backoff_reset(&b);
	assert(b.attempts == 0);
	assert(mbt_backoff_next(&b) == 5);
}

static void
test_growth(void)
{
	struct mbt_backoff b;

	mbt_backoff_init(&b, 100, 1000, 150, 0);
	assert(mbt_backoff_next(&b) == 100);
	assert(mbt_backoff_next(&b) == 150);
	assert(mbt_backoff_next(&b) == 225);
}

static void
test_large(void)
{
	struct mbt_backoff b;

	/* The cap holds where cur * growth_pct would overflow. */
	mbt_backoff_init(&b, UINT64_MAX / 8, UINT64_MAX, 200, 0);
	for (int i = 0; i < 8; i++)
		assert(mbt_backoff_next(&b) >= UINT64_MAX / 8);
	assert(mbt_backoff_next(&b) == UINT64_MAX);
}

static void
test_jitter(void)
{
	struct mbt_backoff b;
	int differ = 0;

	mbt_backoff_init(&b, 1000000, 1000000, 200, 25);
	for (int i = 0; i < 1000; i++) {
		uint64_t d = mbt_backoff_next(&b);

		assert(d >= 750000 && d <= 1250000);
		differ += d != 1000000;
	}
	assert(differ > 0);
}

int
main(void)
{
	test_exact();
	test_growth();
	test_large();
	test_jitter();
	puts("backoff: ok");
	return 0;
}
