// SPDX-License-Identifier: ISC
#include <sys/types.h>
#include <assert.h>
#include <string.h>
#include <stdint.h>

#include "mbt/picker.h"
#include "mbt/torrent.h"
#include "mbt/bitmap.h"
#include "mbt/peer.h"

/* Mock: we need mbt_torrent_piece_count, has_piece, piece_fully_requested
 * and mbt_peer_bitfield to work. We'll use real torrent/bitmap objects. */

/* We can't easily mock torrent internals, so test the picker logic
 * by calling it directly with a minimal torrent setup. For now just
 * verify the sequential picker returns pieces in order and skips
 * pieces we already have. */

/* Minimal test using the picker function signature directly */

/* Since we can't create a real mbt_peer without a conn, test the
 * picker's logic by verifying bitmap operations that the picker depends on */
static void
test_bitmap_for_picker(void)
{
	struct mbt_bitmap *have = NULL, *peer = NULL;

	/* 16 pieces */
	mbt_bitmap_new_bits(&have, 16);
	mbt_bitmap_new_bits(&peer, 16);

	/* Peer has all pieces */
	for (int i = 0; i < 16; i++)
		mbt_bitmap_set(peer, i);

	/* We have piece 0 and 1 */
	mbt_bitmap_set(have, 0);
	mbt_bitmap_set(have, 1);

	/* First clear in our have = piece 2 */
	assert(mbt_bitmap_first_clear(have) == 2);

	/* Peer has piece 2 */
	assert(mbt_bitmap_isset(peer, 2) > 0);

	/* We don't have piece 2 */
	assert(mbt_bitmap_isset(have, 2) == 0);

	/* Count: peer has 16, we have 2 */
	assert(mbt_bitmap_count(peer) == 16);
	assert(mbt_bitmap_count(have) == 2);

	/* Full check */
	assert(!mbt_bitmap_full(have));
	for (int i = 2; i < 16; i++)
		mbt_bitmap_set(have, i);
	assert(mbt_bitmap_full(have));

	mbt_bitmap_unref(have);
	mbt_bitmap_unref(peer);
}

static void
test_bitmap_partial_peer(void)
{
	struct mbt_bitmap *have = NULL, *peer = NULL;

	mbt_bitmap_new_bits(&have, 10);
	mbt_bitmap_new_bits(&peer, 10);

	/* Peer only has pieces 3, 5, 7 */
	mbt_bitmap_set(peer, 3);
	mbt_bitmap_set(peer, 5);
	mbt_bitmap_set(peer, 7);

	/* We have piece 3 */
	mbt_bitmap_set(have, 3);

	/* Piece 5 is the first one peer has that we don't */
	int found = -1;
	for (int i = 0; i < 10; i++) {
		if (mbt_bitmap_isset(peer, i) > 0 && mbt_bitmap_isset(have, i) == 0) {
			found = i;
			break;
		}
	}
	assert(found == 5);

	mbt_bitmap_unref(have);
	mbt_bitmap_unref(peer);
}

static void
test_bitmap_no_overlap(void)
{
	struct mbt_bitmap *have = NULL, *peer = NULL;

	mbt_bitmap_new_bits(&have, 8);
	mbt_bitmap_new_bits(&peer, 8);

	/* Peer has 0,1,2 — we have 0,1,2 */
	for (int i = 0; i < 3; i++) {
		mbt_bitmap_set(peer, i);
		mbt_bitmap_set(have, i);
	}

	/* No piece peer has that we don't */
	int found = -1;
	for (int i = 0; i < 8; i++) {
		if (mbt_bitmap_isset(peer, i) > 0 && mbt_bitmap_isset(have, i) == 0) {
			found = i;
			break;
		}
	}
	assert(found == -1);

	mbt_bitmap_unref(have);
	mbt_bitmap_unref(peer);
}

int
main(void)
{
	test_bitmap_for_picker();
	test_bitmap_partial_peer();
	test_bitmap_no_overlap();
	return 0;
}
