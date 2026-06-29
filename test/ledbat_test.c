// SPDX-License-Identifier: ISC
/*
 * LEDBAT congestion-control unit test. Steps the algorithm in isolation (no
 * sockets, no event loop) so we can assert the cwnd trajectory directly --
 * the thing that was painful to observe through the networked uTP stack.
 */
#include <assert.h>
#include <stdio.h>
#include <stdlib.h>
#include <stdint.h>

#include "ledbat.h"

static int g_trace;   /* MBT_LEDBAT_TRACE=1 -> print cwnd each step */

#define MSS   1400u
#define MINW  MSS
#define MAXW  (1024u * MSS)   /* generous ceiling so ramp isn't ceiling-limited */
#define INIT  (10u * MSS)     /* IW10 */

/*
 * Simulate a steady transfer where the sender keeps the window full: each RTT,
 * one window's worth of data is acked. We model that as `cwnd/MSS` acks per
 * RTT, each acking one MSS, with flightsize = the window outstanding at the
 * start of the RTT. Delay stays below TARGET (off_target positive) so cwnd
 * should grow. Returns final cwnd after `rtts` rounds.
 */
static uint32_t
sim_full_window(uint32_t their_delay_us, int rtts)
{
	struct ledbat l;
	ledbat_init(&l, INIT, MINW, MAXW, MSS);
	uint64_t now = 0;
	for (int r = 0; r < rtts; r++) {
		uint32_t window = l.cwnd;          /* what we send this RTT */
		uint32_t flight = window;          /* fully in flight */
		int acks = window / MSS;
		if (acks < 1) acks = 1;
		for (int a = 0; a < acks; a++) {
			/* flightsize decreases as acks drain the window within the RTT */
			ledbat_on_ack(&l, their_delay_us, MSS, flight, now);
			if (flight > MSS) flight -= MSS;
		}
		if (g_trace && (r < 20 || r % 50 == 0))
			fprintf(stderr, "  rtt %3d: cwnd=%u (%u pkts)\n", r, l.cwnd, l.cwnd / MSS);
		now += 1000;  /* ~1ms RTT */
	}
	return l.cwnd;
}

/* What we observed on the LAN: tiny flightsize per ack (pipe drained by the
 * time each ack is processed). Does cwnd still grow? */
static uint32_t
sim_drained(uint32_t their_delay_us, int acks)
{
	struct ledbat l;
	ledbat_init(&l, INIT, MINW, MAXW, MSS);
	uint64_t now = 0;
	for (int a = 0; a < acks; a++) {
		ledbat_on_ack(&l, their_delay_us, MSS, /*flightsize*/ MSS, now);
		now += 100;
	}
	return l.cwnd;
}

int
main(void)
{
	g_trace = getenv("MBT_LEDBAT_TRACE") != NULL;

	/* 1. Ramp: with delay below target and a full window, cwnd must climb
	 *    well above the initial window over many RTTs. */
	uint32_t after = sim_full_window(/*delay*/ 0, /*rtts*/ 500);
	fprintf(stderr, "ramp (full window, 500 rtts): cwnd=%u (init=%u, ceiling=%u)\n", after, INIT, MAXW);
	assert(after > INIT * 4);   /* should have grown substantially */

	/* 2. Yield: with delay ABOVE target (queuing), cwnd must not grow; it
	 *    should back off toward the floor. */
	struct ledbat l;
	ledbat_init(&l, INIT, MINW, MAXW, MSS);
	/* Establish a low base delay (uncongested), then raise queuing delay well
	 * above it -- LEDBAT measures delay relative to the base, so this is the
	 * actual congestion signal. */
	uint64_t t = 0;
	for (int a = 0; a < 20; a++, t += 1000)
		ledbat_on_ack(&l, /*base*/ 1000, MSS, INIT, t);
	uint32_t cwnd_before = l.cwnd;
	for (int a = 0; a < 200; a++, t += 1000) {
		ledbat_on_ack(&l, /*base + 200ms queuing*/ 201000, MSS, INIT, t);
		if (g_trace && a < 15)
			fprintf(stderr, "  yield step %3d: cwnd=%u\n", a, l.cwnd);
	}
	fprintf(stderr, "yield: cwnd %u -> %u under +200ms queuing\n", cwnd_before, l.cwnd);
	assert(l.cwnd < cwnd_before);   /* must back off under real congestion */

	/* 3. Loss halving. */
	ledbat_init(&l, 100 * MSS, MINW, MAXW, MSS);
	ledbat_on_loss(&l);
	printf("loss: cwnd=%u (expect ~%u)\n", l.cwnd, 50 * MSS);
	assert(l.cwnd == 50 * MSS);

	/* 4. Timeout resets to 1 MSS. */
	ledbat_on_timeout(&l);
	printf("timeout: cwnd=%u (expect %u)\n", l.cwnd, MSS);
	assert(l.cwnd == MSS);

	/* 5. The LAN case: tiny flightsize per ack. This reproduces the field
	 *    pinning -- documents current behaviour. No assertion: the purpose is
	 *    to record what cwnd ends up at when flightsize is always 1 MSS. */
	uint32_t drained = sim_drained(0, 1000);
	printf("drained (flightsize=1 MSS, 1000 acks): cwnd=%u\n", drained);

	/*
	 * 6. Slow-start reaches a high window in a LOGARITHMIC number of RTTs.
	 *    With exponential growth (doubling per RTT) from IW10, a 768-pkt
	 *    target should take ~log2(768/10) ~= 7 RTTs, vs ~750 for additive.
	 *    Assert we get there in well under 20 RTTs.
	 */
	{
		struct ledbat sl;
		ledbat_init(&sl, INIT, MINW, MAXW, MSS);
		uint64_t now = 0;
		int rtts_to_target = -1;
		for (int r = 0; r < 40; r++) {
			uint32_t window = sl.cwnd;
			int acks = window / MSS;
			if (acks < 1) acks = 1;
			for (int a = 0; a < acks; a++)
				ledbat_on_ack(&sl, /*delay*/ 0, MSS, window, now);
			if (g_trace && r < 15)
				fprintf(stderr, "  ss rtt %2d: cwnd=%u (%u pkts) ss=%d\n",
				        r, sl.cwnd, sl.cwnd / MSS, sl.slow_start);
			if (rtts_to_target < 0 && sl.cwnd >= 768 * MSS)
				rtts_to_target = r + 1;
			now += 1000;
		}
		fprintf(stderr, "slow-start: reached 768 pkts in %d rtts (cwnd=%u)\n",
		        rtts_to_target, sl.cwnd);
		assert(rtts_to_target > 0 && rtts_to_target < 20);
	}

	/*
	 * 7. Slow-start EXITS when queuing delay reaches TARGET, handing off to
	 *    the additive controller (no more doubling). Establish a low base,
	 *    then feed delay >= TARGET and confirm slow_start clears and the
	 *    window stops exploding.
	 */
	{
		struct ledbat sl;
		ledbat_init(&sl, INIT, MINW, MAXW, MSS);
		uint64_t now = 0;
		/* low base delay, a few exponential steps */
		for (int a = 0; a < 5; a++, now += 1000)
			ledbat_on_ack(&sl, /*base*/ 2000, MSS, sl.cwnd, now);
		assert(sl.slow_start == 1);
		uint32_t before = sl.cwnd;
		/* now queuing delay == base + TARGET -> our_delay == TARGET -> exit */
		ledbat_on_ack(&sl, 2000 + LEDBAT_TARGET_US, MSS, sl.cwnd, now);
		fprintf(stderr, "slow-start exit: ss=%d ssthresh=%u cwnd %u->%u\n",
		        sl.slow_start, sl.ssthresh, before, sl.cwnd);
		assert(sl.slow_start == 0);        /* exited slow-start */
		assert(sl.ssthresh == before);     /* threshold pinned at exit window */
	}

	/*
	 * 8. Loss during slow-start exits it and sets ssthresh = cwnd/2.
	 */
	{
		struct ledbat sl;
		ledbat_init(&sl, 100 * MSS, MINW, MAXW, MSS);
		assert(sl.slow_start == 1);
		ledbat_on_loss(&sl);
		assert(sl.slow_start == 0);
		assert(sl.ssthresh == 50 * MSS);
		assert(sl.cwnd == 50 * MSS);
		printf("loss-in-ss: ssthresh=%u cwnd=%u\n", sl.ssthresh, sl.cwnd);
	}

	/*
	 * 9. RTT estimator: first sample sets baseline; subsequent samples
	 *    apply exponential smoothing. Also verify the RTO floor at 500 ms.
	 */
	{
		struct ledbat sl;
		ledbat_init(&sl, INIT, MINW, MAXW, MSS);
		assert(sl.rtt_us == 0);
		assert(sl.rtt_var_us == 0);
		/* First sample: 10 ms RTT */
		ledbat_on_rtt_sample(&sl, 10000);
		assert(sl.rtt_us == 10000);
		assert(sl.rtt_var_us == 5000);      /* sample / 2 */
		assert(sl.timeout_ms == 500);       /* 10+5*4=30ms -> floors to 500 */
		/* Second sample: 14 ms -> delta = |10000 - 14000| = 4000 */
		ledbat_on_rtt_sample(&sl, 14000);
		/* rtt_var += (4000 - 5000)/4 = 5000 - 250 = 4750 */
		assert(sl.rtt_var_us == 4750);
		/* rtt += (14000 - 10000)/8 = 10000 + 500 = 10500 */
		assert(sl.rtt_us == 10500);
		/* Third sample: 8 ms */
		ledbat_on_rtt_sample(&sl, 8000);
		assert(sl.rtt_us > 0);
		assert(sl.rtt_var_us > 0);
		/* RTO = (rtt + var*4)/1000, must stay >= 500 */
		assert(sl.timeout_ms >= 500);
		/* Large RTT: feed several samples to let 1/8 smoothing converge.
		 * Use 800ms RTT so rto = rtt + 4*var exceeds the 500ms floor.
		 * (With 200ms RTT, var converges to 0 and rto floors at 500 forever.) */
		for (int i = 0; i < 20; i++)
			ledbat_on_rtt_sample(&sl, 800000);  /* 800 ms */
		assert(sl.rtt_us > 500000);           /* smoothed RTT > 500 ms */
		assert(sl.timeout_ms > 500);          /* RTO above floor */
		fprintf(stderr, "rtt_estimator: rtt=%d var=%d rto=%d\n",
		        sl.rtt_us, sl.rtt_var_us, sl.timeout_ms);
	}

	/*
	 * 10. Base-delay reset: after LEDBAT_BASE_HIST_S (120 s) the base delay
	 *     expires and a newer (lower) delay sample can replace it.
	 */
	{
		struct ledbat sl;
		ledbat_init(&sl, INIT, MINW, MAXW, MSS);
		uint64_t now = 0;
		/* Establish a 100 ms base delay */
		for (int a = 0; a < 10; a++, now += 1000)
			ledbat_on_ack(&sl, 100000, MSS, INIT, now);
		assert(sl.base_delay_us == 100000);
		/* Advance 121 s (> 120 s window); now a lower delay should replace base */
		now += (uint64_t)(LEDBAT_BASE_HIST_S + 1) * 1000000;
		ledbat_on_ack(&sl, 5000, MSS, INIT, now);
		assert(sl.base_delay_us == 5000);
		fprintf(stderr, "base_reset: base_delay %u -> %u after 121s gap\n",
		        100000, sl.base_delay_us);
	}

	/*
	 * 11. Loss at min_cwnd: cwnd stays at floor, but ssthresh still updates.
	 */
	{
		struct ledbat sl;
		ledbat_init(&sl, MSS, MINW, MAXW, MSS);  /* init == min */
		assert(sl.cwnd == MSS);
		ledbat_on_loss(&sl);
		assert(sl.cwnd == MSS);         /* clamped at floor */
		assert(sl.ssthresh == MSS);     /* ssthresh = MSS/2 -> clamped to min */
		assert(sl.slow_start == 0);
		printf("loss-at-min: cwnd=%u ssthresh=%u (both == min)\n",
		       sl.cwnd, sl.ssthresh);
	}

	/*
	 * 12. Timeout at min_cwnd: re-enters slow-start, cwnd stays at floor.
	 */
	{
		struct ledbat sl;
		ledbat_init(&sl, MSS, MINW, MAXW, MSS);
		ledbat_on_timeout(&sl);
		assert(sl.cwnd == MSS);         /* clamped at floor */
		assert(sl.slow_start == 1);     /* re-enter slow-start */
		assert(sl.ssthresh == MSS);     /* half of MSS -> clamped to min */
		printf("timeout-at-min: cwnd=%u ss=%d ssthresh=%u\n",
		       sl.cwnd, sl.slow_start, sl.ssthresh);
	}

	/*
	 * 13. Additive steady-state convergence: after slow-start exits, with
	 *     zero queuing delay, cwnd should grow by ~1 MSS per RTT. Skip
	 *     slow-start (it would just hit max_cwnd) and start in additive mode.
	 */
	{
		struct ledbat sl;
		ledbat_init(&sl, INIT, MINW, MAXW, MSS);
		sl.slow_start = 0;              /* test additive directly */
		sl.ssthresh = INIT;
		uint64_t now = 0;
		for (int r = 0; r < 100; r++) {
			uint32_t window = sl.cwnd;
			int acks = window / MSS;
			if (acks < 1) acks = 1;
			for (int a = 0; a < acks; a++)
				ledbat_on_ack(&sl, 0, MSS, window, now);
			now += 1000;
		}
		uint32_t additive_growth = sl.cwnd - INIT;
		/* With GAIN=1 and zero delay, each RTT adds ~1 MSS. Over 100 RTTs
		 * we expect at least 20 MSS of growth (generous lower bound) but
		 * not an explosion. */
		fprintf(stderr, "additive: cwnd=%u growth=%u (%u pkts)\n",
		        sl.cwnd, additive_growth, additive_growth / MSS);
		assert(additive_growth >= 20 * MSS);
		assert(additive_growth <= 200 * MSS);
	}

	/*
	 * 14. Multiple losses in succession: each loss halves ssthresh (TCP-style).
	 *     Verify the halving chain converges to min_cwnd floor. Use a start
	 *     value that cleanly halves: 64 MSS -> 32 -> 16 -> 8 -> 4 -> 2 -> 1.
	 */
	{
		struct ledbat sl;
		ledbat_init(&sl, 64 * MSS, MINW, MAXW, MSS);
		sl.slow_start = 0;
		sl.ssthresh = 64 * MSS;
		uint32_t expect[] = {32, 16, 8, 4, 2, 1};  /* MSS units */
		for (int k = 0; k < 6; k++) {
			ledbat_on_loss(&sl);
			uint32_t exp_cwnd = expect[k] * MSS;
			if (exp_cwnd < MINW) exp_cwnd = MINW;
			if (g_trace)
				fprintf(stderr, "  loss %d: cwnd=%u ssthresh=%u (expect %u)\n",
				        k + 1, sl.cwnd, sl.ssthresh, exp_cwnd);
			assert(sl.ssthresh == exp_cwnd);
			assert(sl.cwnd == exp_cwnd);
		}
		/* After 6 halvings from 64 MSS: at floor */
		assert(sl.cwnd == MINW);
		assert(sl.ssthresh == MINW);
		printf("multi-loss: 6 consecutive losses from 64 MSS -> cwnd=%u (floor)\n",
		       sl.cwnd);
	}

	/*
	 * 15. Slow-start re-enters after reaching ssthresh following a loss:
	 *     timeout sets ss=1, then ramp should exit ss when hitting ssthresh.
	 */
	{
		struct ledbat sl;
		ledbat_init(&sl, 100 * MSS, MINW, MAXW, MSS);
		ledbat_on_timeout(&sl);  /* cwnd = MSS, ssthresh = 50 MSS, ss = 1 */
		assert(sl.slow_start == 1);
		assert(sl.ssthresh == 50 * MSS);
		assert(sl.cwnd == MSS);
		/* Ramp exponentially until ssthresh */
		uint64_t now = 0;
		for (int r = 0; r < 20; r++) {
			uint32_t window = sl.cwnd;
			int acks = window / MSS;
			if (acks < 1) acks = 1;
			for (int a = 0; a < acks; a++)
				ledbat_on_ack(&sl, 0, MSS, window, now);
			if (sl.slow_start == 0) break;
			now += 1000;
		}
		/* Should have exited slow-start at or before ssthresh */
		fprintf(stderr, "ss-reenter: exited ss=%d cwnd=%u ssthresh=%u\n",
		        sl.slow_start, sl.cwnd, sl.ssthresh);
		assert(sl.slow_start == 0);
		assert(sl.cwnd >= sl.ssthresh);
	}

	printf("ledbat tests done\n");
	return 0;
}
