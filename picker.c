#include <stdlib.h>

#include "picker.h"
#include "torrent.h"
#include "bitmap.h"
#include "peer.h"

int
mbt_picker_sequential(struct mbt_torrent *t, void *peer, void *ctx)
{
	(void)ctx;
	struct mbt_bitmap *peer_bits = mbt_peer_bitfield((struct mbt_peer *)peer);

	if (!peer_bits)
		return -1;

	/* Find first piece we don't have that this peer does */
	size_t bits = mbt_bitmap_bits(peer_bits);
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

	size_t bits = mbt_bitmap_bits(peer_bits);
	if (bits == 0)
		return -1;

	/* Start at random offset, scan forward */
	size_t start = arc4random_uniform(bits);
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
	/* TODO: needs peer availability counts */
	return mbt_picker_random(t, peer, ctx);
}
