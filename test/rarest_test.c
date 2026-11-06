// SPDX-License-Identifier: ISC
/*
 * test/rarest_test.c — verify rarest-first picker prefers rare pieces
 *
 * Setup: 8 pieces. Seeder A has all 8. Seeder B has only pieces 0-3.
 * Pieces 4-7 are "rarer" (only A has them, availability=1).
 * Pieces 0-3 are "common" (both A and B have them, availability=2).
 *
 * The picker is random until the torrent has a piece, so the leecher starts
 * with piece 0. A min_peers above the peer count stops all requests, and the
 * test calls the picker itself, so nothing here depends on timing.
 */
#include <assert.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sha1.h>
#include <stdint.h>

#include "mbt/event.h"
#include "mbt/session.h"
#include "mbt/torrent.h"
#include "mbt/storage.h"
#include "mbt/picker.h"
#include "mbt/be.h"
#include "mbt/hex.h"
#include "mbt/bitmap.h"
#include "mbt/peer.h"
#include "test_util.h"

struct mbt_ev_timer;
struct mbt_torrent;

#define PIECE_LEN 16384
#define NUM_PIECES 8
#define TOTAL_LEN (PIECE_LEN * NUM_PIECES)

static struct mbt_ev *ev;
#define WAIT_STEPS 1000          /* 10 seconds at 10 ms */

int main(void)
{
	uint8_t data[TOTAL_LEN];
	for (int i = 0; i < TOTAL_LEN; i++) data[i] = (i * 37 + 7) & 0xff;

	uint8_t pieces_hash[NUM_PIECES * 20];
	for (int i = 0; i < NUM_PIECES; i++) {
		SHA1_CTX ctx; SHA1Init(&ctx);
		SHA1Update(&ctx, data + i * PIECE_LEN, PIECE_LEN);
		SHA1Final(pieces_hash + i * 20, &ctx);
	}

	/* Build .torrent */
	_cleanup_be_ struct mbt_be_value *root = NULL, *info = NULL;
	mbt_be_dict_new(&root);
	mbt_be_dict_new(&info);
	mbt_be_dict_append(root, "info", info);
	mbt_be_dict_append_number(info, "length", TOTAL_LEN);
	mbt_be_dict_append_string(info, "name", "rarest_test.bin", 15);
	mbt_be_dict_append_number(info, "piece length", PIECE_LEN);
	mbt_be_dict_append_string(info, "pieces", pieces_hash, sizeof(pieces_hash));

	unsigned char *encoded; size_t enc_len;
	mbt_be_value_encode(root, &encoded, &enc_len);

	mbt_ev_new_default(&ev);

	/* Seeder A: has all pieces */
	struct mbt_storage_memory *memA = mbt_storage_memory_new();
	mbt_storage_memory_put_file(memA, 0, data, TOTAL_LEN);
	struct mbt_session_config cfgA = {
		.ev = ev, .default_storage = mbt_storage_memory_storage(memA), .listen_port = 0,
	};
	struct mbt_session *sA;
	mbt_session_new(&sA, &cfgA);
	mbt_session_add_torrent_buf(sA, encoded, enc_len, NULL, NULL);
	test_wait_verified(ev, mbt_session_torrent_first(sA));

	/* Seeder B: has only pieces 0-3 (first half) */
	struct mbt_storage_memory *memB = mbt_storage_memory_new();
	mbt_storage_memory_put_file(memB, 0, data, PIECE_LEN * 4);
	struct mbt_session_config cfgB = {
		.ev = ev, .default_storage = mbt_storage_memory_storage(memB), .listen_port = 0,
	};
	struct mbt_session *sB;
	mbt_session_new(&sB, &cfgB);
	mbt_session_add_torrent_buf(sB, encoded, enc_len, NULL, NULL);
	test_wait_verified(ev, mbt_session_torrent_first(sB));

	/* Leecher: it has piece 0, and it never reaches min_peers. */
	struct mbt_storage_memory *memL = mbt_storage_memory_new();
	struct mbt_session_config cfgL = {
		.ev = ev, .default_storage = mbt_storage_memory_storage(memL), .listen_port = 0,
		.picker = mbt_picker_rarest,
		.max_pieces_per_peer = 1,
		.min_peers_before_download = 3,
	};
	struct mbt_session *sL;
	mbt_session_new(&sL, &cfgL);
	mbt_session_add_torrent_buf(sL, encoded, enc_len, NULL, NULL);

	struct mbt_torrent *tL = mbt_session_torrent_first(sL);
	mbt_storage_memory_put_file(memL, 0, data, PIECE_LEN);
	test_wait_verified(ev, tL);
	assert(mbt_torrent_have_count(tL) == 1);

	/* Add both peers now — piece_count is known, bitfields processed immediately */
	struct mbt_infohash *ih = mbt_torrent_infohash(tL);
	mbt_session_add_peer(sL, ih, "127.0.0.1", mbt_session_listen_port(sA), MBT_SOURCE_PEX, 0);
	mbt_session_add_peer(sL, ih, "127.0.0.1", mbt_session_listen_port(sB), MBT_SOURCE_PEX, 0);

	for (int i = 0; i < WAIT_STEPS && mbt_torrent_piece_avail(tL, 1) < 2; i++)
		mbt_ev_step(ev, 10);
	assert(mbt_torrent_peer_count(tL) == 2);
	for (int i = 1; i < NUM_PIECES; i++)
		assert(mbt_torrent_piece_avail(tL, (uint32_t)i) == (i < 4 ? 2 : 1));

	for (int i = 0; i < 2; i++) {
		struct mbt_peer *p = mbt_torrent_peer_at(tL, i);
		int full = mbt_bitmap_count(mbt_peer_bitfield(p)) == NUM_PIECES;
		int piece = mbt_picker_rarest(tL, p, NULL);

		/* A offers rare pieces, so it gets one. B has only common ones. */
		assert(full ? piece >= 4 : piece >= 1 && piece < 4);
	}
	assert(mbt_torrent_have_count(tL) == 1);

	fprintf(stderr, "rarest_test: PASS\n");

	free(encoded);
	mbt_session_free(sA);
	mbt_session_free(sB);
	mbt_session_free(sL);
	mbt_ev_destroy(ev);
	mbt_storage_memory_destroy(memA);
	mbt_storage_memory_destroy(memB);
	mbt_storage_memory_destroy(memL);
	return 0;
}
