// SPDX-License-Identifier: ISC
#include <limits.h>
#include <stdlib.h>
#include <stdint.h>

#include "mbt/picker.h"
#include "internal.h"
#include "mbt/torrent.h"
#include "mbt/bitmap.h"
#include "mbt/peer.h"

struct mbt_torrent;

int
mbt_picker_sequential(struct mbt_torrent *t, void *peer, void *ctx)
{
	(void)ctx;
	struct mbt_bitmap *peer_bits = mbt_peer_bitfield((struct mbt_peer *)peer);

	if (!peer_bits)
		return -1;

	/* Find first piece we don't have that this peer does */
	uint32_t bits = mbt_torrent_piece_count(t); if (bits == 0) return -1;
	for (size_t i = 0; i < bits; i++) {
		if (mbt_bitmap_isset(peer_bits, i) > 0
		    && !mbt_torrent_has_piece(t, i)
		    && !mbt_torrent_piece_fully_requested(t, i))
			return (int)i;
	}

	return -1;
}

int
mbt_picker_random(struct mbt_torrent *t, void *peer, void *ctx)
{
	(void)ctx;
	struct mbt_bitmap *peer_bits = mbt_peer_bitfield((struct mbt_peer *)peer);

	if (!peer_bits)
		return -1;

	uint32_t bits = mbt_torrent_piece_count(t); if (bits == 0) return -1;
	if (bits == 0)
		return -1;

	/* Start at random offset, scan forward */
	size_t start = mbt_random_uniform(bits);
	for (size_t i = 0; i < bits; i++) {
		size_t idx = (start + i) % bits;
		if (mbt_bitmap_isset(peer_bits, idx) > 0
		    && !mbt_torrent_has_piece(t, idx)
		    && !mbt_torrent_piece_fully_requested(t, idx))
			return (int)idx;
	}

	return -1;
}

int
mbt_picker_rarest(struct mbt_torrent *t, void *peer, void *ctx)
{
	(void)ctx;
	struct mbt_bitmap *peer_bits = mbt_peer_bitfield((struct mbt_peer *)peer);
	if (!peer_bits) return -1;

	uint32_t bits = mbt_torrent_piece_count(t);
	if (bits == 0) return -1;

	/* Phase 1: random first piece (until we have at least one) */
	if (mbt_torrent_have_count(t) == 0)
		return mbt_picker_random(t, peer, ctx);

	/* Phase 3: endgame — allow duplicate requests */
	/* (handled by drive_peer when endgame flag is set) */

	/* Phase 2: rarest first — use precomputed per-piece availability counters */
	int best = -1;
	int best_avail = INT_MAX;
	int ties = 0;

	for (uint32_t i = 0; i < bits; i++) {
		if (mbt_bitmap_isset(peer_bits, i) <= 0)
			continue;
		if (mbt_torrent_has_piece(t, i))
			continue;
		if (mbt_torrent_piece_fully_requested(t, i))
			continue;

		int avail = mbt_torrent_piece_avail(t, i);
		if (avail < best_avail) {
			best_avail = avail;
			best = (int)i;
			ties = 1;
		} else if (avail == best_avail) {
			ties++;
			if (mbt_random_uniform(ties) == 0)
				best = (int)i;
		}
	}

	return best;
}
