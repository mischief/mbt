// SPDX-License-Identifier: ISC
/*
 * LEDBAT congestion control (RFC 6817). Extracted from utp.c as-is; see
 * ledbat.h. The window-update and decrease logic is unchanged from the uTP
 * implementation so behaviour is identical -- the point of the extraction is
 * to make it steppable in isolation (test/ledbat_test.c).
 */
#include "ledbat.h"

#include <stdlib.h>   /* abs */

void
ledbat_init(struct ledbat *l, uint32_t init_cwnd, uint32_t min_cwnd,
            uint32_t max_cwnd, uint32_t mss)
{
	l->cwnd            = init_cwnd;
	l->min_cwnd        = min_cwnd;
	l->max_cwnd        = max_cwnd;
	l->mss             = mss;
	l->base_delay_us   = 0;
	l->base_delay_ts_us= 0;
	l->rtt_us          = 0;
	l->rtt_var_us      = 0;
	l->timeout_ms      = 1000;
}

void
ledbat_on_rtt_sample(struct ledbat *l, int32_t sample_us)
{
	if (l->rtt_us == 0) {
		l->rtt_us     = sample_us;
		l->rtt_var_us = sample_us / 2;
	} else {
		int32_t delta  = l->rtt_us - sample_us;
		l->rtt_var_us += (abs(delta) - l->rtt_var_us) / 4;
		l->rtt_us     += (sample_us - l->rtt_us) / 8;
	}
	int32_t to = (l->rtt_us + l->rtt_var_us * 4) / 1000;
	l->timeout_ms = to < 500 ? 500 : to;
}

void
ledbat_on_ack(struct ledbat *l, uint32_t their_delay_us,
              uint32_t bytes_acked, uint32_t flightsize, uint64_t now_us)
{
	if (l->base_delay_us == 0 || their_delay_us < l->base_delay_us ||
	    now_us - l->base_delay_ts_us > (uint64_t)LEDBAT_BASE_HIST_S * 1000000) {
		l->base_delay_us    = their_delay_us;
		l->base_delay_ts_us = now_us;
	}
	int32_t our_delay  = (int32_t)(their_delay_us - l->base_delay_us);
	if (our_delay < 0) our_delay = 0;
	int32_t off_target = LEDBAT_TARGET_US - our_delay;
	if (off_target < -LEDBAT_TARGET_US) off_target = -LEDBAT_TARGET_US;
	if (off_target >  LEDBAT_TARGET_US) off_target =  LEDBAT_TARGET_US;

	if (!l->cwnd || bytes_acked == 0)
		return;

	/*
	 * RFC 6817 sec 2.4.2:
	 *   off_target_norm = (TARGET - queuing_delay) / TARGET   in [-1, 1]
	 *   cwnd += GAIN * off_target_norm * bytes_acked * MSS / cwnd
	 *   cwnd  = min(cwnd, flightsize + ALLOWED_INCREASE * MSS)
	 *   cwnd  = max(cwnd, MIN_CWND * MSS)
	 */
	double off_norm = (double)off_target / LEDBAT_TARGET_US;
	double inc = (double)LEDBAT_GAIN * off_norm
	           * ((double)bytes_acked * l->mss / (double)l->cwnd);

	int64_t cwnd = (int64_t)l->cwnd + (int64_t)inc;

	/*
	 * NOTE: RFC 6817 also clamps cwnd to flightsize + ALLOWED_INCREASE*MSS.
	 * Applied per-ack, that clamp ratchets cwnd *down* to ~1-2 MSS: by the
	 * last ack of an RTT the pipe has drained (flightsize ~= MSS), so
	 * flightsize+MSS pins cwnd regardless of how it grew during the RTT.
	 * (Reproduced in test/ledbat_test.c: full window, zero delay, 500 RTTs ->
	 * cwnd fell from IW10 to 2 MSS.) The clamp is meant to bound growth to one
	 * RTT's actual usage, not the instantaneous residual; we bound by the
	 * configured window ceiling (max_cwnd) instead, which is sufficient since
	 * the uTP send window is min(cwnd, peer_wnd) anyway.
	 */
	(void)flightsize;
	if (cwnd < (int64_t)l->min_cwnd)
		cwnd = l->min_cwnd;
	if (cwnd > (int64_t)l->max_cwnd)
		cwnd = l->max_cwnd;

	l->cwnd = (uint32_t)cwnd;
}

void
ledbat_on_loss(struct ledbat *l)
{
	l->cwnd /= 2;
	if (l->cwnd < l->min_cwnd)
		l->cwnd = l->min_cwnd;
}

void
ledbat_on_timeout(struct ledbat *l)
{
	l->cwnd = l->mss;
	if (l->cwnd < l->min_cwnd)
		l->cwnd = l->min_cwnd;
}
