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
	 *    pinning -- documents current behaviour. */
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

	printf("ledbat tests done\n");
	return 0;
}
