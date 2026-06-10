// SPDX-License-Identifier: ISC
#ifndef MBT_RATELIMIT_H
#define MBT_RATELIMIT_H

#include "export.h"
#include <stddef.h>
#include <stdint.h>
#include <stdbool.h>

/*
 * Token bucket rate limiter.
 *
 * Tokens refill at `rate` bytes per second, up to a maximum of `burst` tokens.
 * A transfer of N bytes requires N tokens. If insufficient tokens are
 * available, the caller can query the delay until enough accumulate.
 *
 * A rate of 0 means unlimited (all operations succeed immediately).
 */
struct mbt_ratelimit;

/* Create a rate limiter. rate=bytes/sec, burst=max tokens (0 rate = unlimited). */
MBT_API int mbt_ratelimit_new(struct mbt_ratelimit **out, uint64_t rate, uint64_t burst);
MBT_API void mbt_ratelimit_free(struct mbt_ratelimit *rl);

/* Can n bytes be consumed right now? Consumes tokens if true. */
MBT_API bool mbt_ratelimit_allow(struct mbt_ratelimit *rl, size_t n);

/* How many microseconds until n bytes become available? 0 = now. */
MBT_API uint64_t mbt_ratelimit_delay(struct mbt_ratelimit *rl, size_t n);

/* Consume n bytes unconditionally (call after delay has elapsed). */
MBT_API void mbt_ratelimit_consume(struct mbt_ratelimit *rl, size_t n);

/* Current available tokens. */
MBT_API uint64_t mbt_ratelimit_tokens(struct mbt_ratelimit *rl);

/* Adjust parameters at runtime. */
MBT_API void mbt_ratelimit_set_rate(struct mbt_ratelimit *rl, uint64_t rate);
MBT_API void mbt_ratelimit_set_burst(struct mbt_ratelimit *rl, uint64_t burst);
MBT_API uint64_t mbt_ratelimit_rate(struct mbt_ratelimit *rl);
MBT_API uint64_t mbt_ratelimit_burst(struct mbt_ratelimit *rl);

#endif
