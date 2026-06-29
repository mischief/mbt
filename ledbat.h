// SPDX-License-Identifier: ISC
#ifndef MBT_LEDBAT_H
#define MBT_LEDBAT_H

/*
 * LEDBAT congestion control (RFC 6817) as used by uTP (BEP 29), extracted from
 * utp.c so the algorithm can be stepped in isolation by a unit test. Internal;
 * not part of the public API.
 *
 * The state here is the congestion-control math only: the window (cwnd, which
 * uTP calls max_window), the one-way base-delay estimate, and the RTT/timeout
 * estimator. Sequence-number bookkeeping (send_una, dup-ack counting, the
 * once-per-loss-episode guard) stays in utp.c, which calls ledbat_on_loss()
 * when it decides a loss episode has begun.
 *
 * RFC 6817's controller (sec 2.4.2) is additive-increase only: it ramps at
 * ~+1 MSS/RTT, which takes hundreds of RTTs to reach the bandwidth-delay
 * product on a fast/low-RTT path. The RFC mentions slow-start only as an
 * optional MAY; libutp (the BEP 29 reference) adds a TCP-style exponential
 * slow-start on top, which is what lets real uTP peers reach line rate
 * quickly. We do the same: grow cwnd by bytes_acked per ack (doubling per RTT)
 * until queuing delay first reaches TARGET (or a loss/timeout), then fall into
 * the RFC additive controller for the polite steady state.
 *
 * Units: bytes for windows, microseconds for delay/RTT, milliseconds for the
 * timeout. now_us is a monotonic clock in microseconds (the caller's).
 */

#include <stdint.h>

/* Tunables (RFC 6817 sec 2.5 names in comments). */
#define LEDBAT_TARGET_US      100000  /* TARGET: max accepted queuing delay */
#define LEDBAT_GAIN           1       /* GAIN, MUST be <= 1 */
#define LEDBAT_ALLOWED_INC    1       /* ALLOWED_INCREASE, in MSS */
#define LEDBAT_BASE_HIST_S    120     /* base-delay history window, seconds */

struct ledbat {
	uint32_t cwnd;            /* congestion window, bytes (uTP max_window) */
	uint32_t min_cwnd;        /* floor, bytes (>= 1 MSS) */
	uint32_t max_cwnd;        /* ceiling, bytes (configured window) */
	uint32_t mss;             /* segment size, bytes */

	uint32_t base_delay_us;   /* sliding-min one-way delay baseline */
	uint64_t base_delay_ts_us;/* when base_delay was last (re)set */

	int      slow_start;      /* 1 while ramping exponentially (TCP-style) */
	uint32_t ssthresh;        /* cwnd at which to leave slow-start, bytes */

	int32_t  rtt_us;
	int32_t  rtt_var_us;
	int32_t  timeout_ms;      /* derived RTO, >= 500 ms */
};

/* Initialize. init/min/max are byte windows; mss is the segment size. */
void ledbat_init(struct ledbat *l, uint32_t init_cwnd, uint32_t min_cwnd,
                 uint32_t max_cwnd, uint32_t mss);

/*
 * Apply an acknowledgement. their_delay_us is the peer's reported one-way
 * delay sample (uTP timestamp_difference_microseconds); bytes_acked is the
 * data this ack newly acknowledges; flightsize is the bytes outstanding before
 * this ack. Updates cwnd per RFC 6817 sec 2.4.2. now_us tracks base-delay age.
 */
void ledbat_on_ack(struct ledbat *l, uint32_t their_delay_us,
                   uint32_t bytes_acked, uint32_t flightsize, uint64_t now_us);

/* RTT estimator update from a single (un-retransmitted) sample. */
void ledbat_on_rtt_sample(struct ledbat *l, int32_t sample_us);

/* Loss episode: multiplicative decrease (cwnd = max(cwnd/2, min_cwnd)). */
void ledbat_on_loss(struct ledbat *l);

/* Congestion timeout (no ACKs within RTO): cwnd = 1 MSS. */
void ledbat_on_timeout(struct ledbat *l);

#endif
