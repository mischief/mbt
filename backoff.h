// SPDX-License-Identifier: ISC
#ifndef MBT_BACKOFF_H
#define MBT_BACKOFF_H

#include <stdint.h>
#include <stdlib.h>

/*
 * Exponential backoff: a delay that grows to a cap, with a random spread.
 * This object only computes delays. The caller owns the timer.
 * A spread can take a delay above max_us by jitter_pct.
 */
struct mbt_backoff {
	uint64_t initial_us, max_us, cur_us;
	unsigned growth_pct;  /* 200 doubles the delay at each step */
	unsigned jitter_pct;  /* 25 spreads a delay by plus or minus 25% */
	unsigned attempts;    /* delays given since the last reset */
};

static inline void
mbt_backoff_init(struct mbt_backoff *b, uint64_t initial_us, uint64_t max_us,
                 unsigned growth_pct, unsigned jitter_pct)
{
	*b = (struct mbt_backoff){.initial_us = initial_us, .max_us = max_us,
	    .growth_pct = growth_pct, .jitter_pct = jitter_pct};
}

static inline void
mbt_backoff_reset(struct mbt_backoff *b)
{
	b->attempts = 0;
}

/* Spans above this do not fit arc4random_uniform. */
#define MBT_BACKOFF_MAX_SPAN (UINT32_MAX / 2 - 1)

/* The next delay in microseconds. */
static inline uint64_t
mbt_backoff_next(struct mbt_backoff *b)
{
	uint64_t span;

	if (b->attempts == 0)
		b->cur_us = b->initial_us;
	else if (b->cur_us / 100 >= UINT64_MAX / b->growth_pct)
		b->cur_us = b->max_us;
	else
		b->cur_us = b->cur_us / 100 * b->growth_pct +
		            b->cur_us % 100 * b->growth_pct / 100;
	if (b->cur_us > b->max_us)
		b->cur_us = b->max_us;
	b->attempts++;
	span = b->cur_us / 100 * b->jitter_pct;
	if (span > MBT_BACKOFF_MAX_SPAN)
		span = MBT_BACKOFF_MAX_SPAN;
	if (span == 0)
		return b->cur_us;
	return b->cur_us - span + arc4random_uniform((uint32_t)span * 2 + 1);
}

#endif
