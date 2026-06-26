// SPDX-License-Identifier: ISC
#include <stdlib.h>
#include <time.h>
#include <stdint.h>

#include "mbt/ratelimit.h"

struct mbt_ratelimit {
	uint64_t rate;      /* bytes per second (0 = unlimited) */
	uint64_t burst;     /* max tokens */
	uint64_t tokens;    /* current tokens (scaled by 1e6 for usec precision) */
	uint64_t last_usec; /* last refill timestamp in usec */
};

static uint64_t
now_usec(void)
{
	struct timespec ts;
	clock_gettime(CLOCK_MONOTONIC, &ts);
	return (uint64_t)ts.tv_sec * 1000000ULL + (uint64_t)ts.tv_nsec / 1000;
}

static void
refill(struct mbt_ratelimit *rl)
{
	if (rl->rate == 0)
		return;

	uint64_t now = now_usec();
	uint64_t elapsed = now - rl->last_usec;
	rl->last_usec = now;

	/* tokens += rate * elapsed / 1e6 */
	uint64_t added = rl->rate * elapsed / 1000000ULL;
	rl->tokens += added;
	if (rl->tokens > rl->burst)
		rl->tokens = rl->burst;
}

int
mbt_ratelimit_new(struct mbt_ratelimit **out, uint64_t rate, uint64_t burst)
{
	struct mbt_ratelimit *rl = calloc(1, sizeof(*rl));
	if (!rl)
		return -1;

	rl->rate = rate;
	rl->burst = burst ? burst : rate; /* default burst = 1 second */
	rl->tokens = rl->burst;           /* start full */
	rl->last_usec = now_usec();
	*out = rl;
	return 0;
}

void
mbt_ratelimit_free(struct mbt_ratelimit *rl)
{
	free(rl);
}

bool
mbt_ratelimit_allow(struct mbt_ratelimit *rl, size_t n)
{
	if (!rl || rl->rate == 0)
		return true;

	refill(rl);

	if (rl->tokens >= n) {
		rl->tokens -= n;
		return true;
	}
	return false;
}

uint64_t
mbt_ratelimit_delay(struct mbt_ratelimit *rl, size_t n)
{
	if (!rl || rl->rate == 0)
		return 0;

	refill(rl);

	if (rl->tokens >= n)
		return 0;

	uint64_t deficit = n - rl->tokens;
	/* usec = deficit * 1e6 / rate */
	return deficit * 1000000ULL / rl->rate;
}

void
mbt_ratelimit_consume(struct mbt_ratelimit *rl, size_t n)
{
	if (!rl || rl->rate == 0)
		return;

	refill(rl);

	if (rl->tokens >= n)
		rl->tokens -= n;
	else
		rl->tokens = 0;
}

uint64_t
mbt_ratelimit_tokens(struct mbt_ratelimit *rl)
{
	if (!rl || rl->rate == 0)
		return UINT64_MAX;

	refill(rl);
	return rl->tokens;
}

void
mbt_ratelimit_set_rate(struct mbt_ratelimit *rl, uint64_t rate)
{
	if (!rl) return;
	refill(rl); /* settle tokens at old rate first */
	rl->rate = rate;
}

void
mbt_ratelimit_set_burst(struct mbt_ratelimit *rl, uint64_t burst)
{
	if (!rl) return;
	rl->burst = burst;
	if (rl->tokens > burst)
		rl->tokens = burst;
}

uint64_t
mbt_ratelimit_rate(struct mbt_ratelimit *rl)
{
	return rl ? rl->rate : 0;
}

uint64_t
mbt_ratelimit_burst(struct mbt_ratelimit *rl)
{
	return rl ? rl->burst : 0;
}
