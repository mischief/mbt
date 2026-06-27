// SPDX-License-Identifier: ISC
#ifndef MBT_TEST_UTIL_H
#define MBT_TEST_UTIL_H

#include <poll.h>
#include <mbt/torrent.h>
#include <mbt/event.h>

/* Run one event loop iteration with a timeout (ms), then dispatch. */
static inline void
test_ev_step(struct mbt_ev *ev, int timeout_ms)
{
	mbt_ev_step(ev, timeout_ms);
}

/* Pump the event loop until async SHA1 verify completes. */
static inline void
test_wait_verified(struct mbt_ev *ev, struct mbt_torrent *t)
{
	while (mbt_torrent_state(t) == MBT_TORRENT_VERIFYING)
		test_ev_step(ev, 10);
}

/* Pump until at least n peers are connected to the torrent. */
static inline void
test_wait_peers(struct mbt_ev *ev, struct mbt_torrent *t, int n)
{
	while (mbt_torrent_peer_count(t) < n)
		test_ev_step(ev, 10);
}

/* Pump until a piece reaches at least the given availability. */
static inline void
test_wait_avail(struct mbt_ev *ev, struct mbt_torrent *t, uint32_t piece, int avail)
{
	while (mbt_torrent_piece_avail(t, piece) < avail)
		test_ev_step(ev, 10);
}

#endif
