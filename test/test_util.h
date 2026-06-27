// SPDX-License-Identifier: ISC
#ifndef MBT_TEST_UTIL_H
#define MBT_TEST_UTIL_H

#include <mbt/torrent.h>
#include <mbt/event.h>

/* Pump the event loop until async SHA1 verify completes. */
static inline void
test_wait_verified(struct mbt_ev *ev, struct mbt_torrent *t)
{
	while (mbt_torrent_state(t) == MBT_TORRENT_VERIFYING)
		mbt_ev_step(ev, 10);
}

#endif
