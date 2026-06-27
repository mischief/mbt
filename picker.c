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

struct rarest_ctx {
	struct mbt_torrent *t;
	int best;
	int best_avail;
	int ties;
};

static void
rarest_cb(size_t i, void *arg)
{
	struct rarest_ctx *rc = arg;
	if (mbt_torrent_piece_fully_requested(rc->t, (uint32_t)i))
		return;
	int avail = mbt_torrent_piece_avail(rc->t, (uint32_t)i);
	if (avail < rc->best_avail) {
		rc->best_avail = avail;
		rc->best = (int)i;
		rc->ties = 1;
	} else if (avail == rc->best_avail) {
		rc->ties++;
		if (mbt_random_uniform(rc->ties) == 0)
			rc->best = (int)i;
	}
}

int
mbt_picker_rarest(struct mbt_torrent *t, void *peer, void *ctx)
{
	(void)ctx;
	struct mbt_bitmap *peer_bits = mbt_peer_bitfield((struct mbt_peer *)peer);
	if (!peer_bits) return -1;

	if (mbt_torrent_piece_count(t) == 0) return -1;

	/* Phase 1: random first piece (until we have at least one) */
	if (mbt_torrent_have_count(t) == 0)
		return mbt_picker_random(t, peer, ctx);

	/* Phase 2: rarest first — iterate (peer_bits & ~have) without per-bit calls */
	struct mbt_bitmap *have = mbt_torrent_have_bitmap(t);
	if (!have) return -1;

	struct rarest_ctx rc = { t, -1, INT_MAX, 0 };
	mbt_bitmap_foreach_andnot(peer_bits, have, rarest_cb, &rc);
	return rc.best;
}
