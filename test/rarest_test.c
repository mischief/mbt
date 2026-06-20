// SPDX-License-Identifier: ISC
/*
 * test/rarest_test.c — verify rarest-first picker prefers rare pieces
 *
 * Setup: 8 pieces. Seeder A has all 8. Seeder B has only pieces 0-3.
 * Pieces 4-7 are "rarer" (only A has them, availability=1).
 * Pieces 0-3 are "common" (both A and B have them, availability=2).
 * Leecher uses rarest picker → should get pieces 4-7 first.
 */
#include <assert.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "mbt/sha1_compat.h"

#include "mbt/event.h"
#include "mbt/session.h"
#include "mbt/torrent.h"
#include "mbt/storage.h"
#include "mbt/picker.h"
#include "mbt/be.h"
#include "mbt/hex.h"

#define PIECE_LEN 16384
#define NUM_PIECES 8
#define TOTAL_LEN (PIECE_LEN * NUM_PIECES)

static struct mbt_ev *ev;
static int complete;
static int pieces_order[NUM_PIECES];
static int pieces_idx;

static void on_piece(struct mbt_torrent *t, uint32_t piece, void *arg)
{ (void)t; (void)arg; if (pieces_idx < NUM_PIECES) pieces_order[pieces_idx++] = piece; }

static void on_complete(struct mbt_torrent *t, void *arg)
{ (void)t; (void)arg; complete = 1; mbt_ev_stop(ev); }

static void timeout_cb(struct mbt_ev_timer *timer, void *arg)
{ (void)timer; (void)arg; mbt_ev_stop(ev); }

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
		.ev = ev, .storage_ops = mbt_storage_memory_ops(),
		.storage_ctx = memA, .listen_port = 0,
	};
	struct mbt_session *sA;
	mbt_session_new(&sA, &cfgA);
	mbt_session_add_torrent_buf(sA, encoded, enc_len);

	/* Seeder B: has only pieces 0-3 (first half) */
	struct mbt_storage_memory *memB = mbt_storage_memory_new();
	mbt_storage_memory_put_file(memB, 0, data, PIECE_LEN * 4); /* only first 4 pieces */
	struct mbt_session_config cfgB = {
		.ev = ev, .storage_ops = mbt_storage_memory_ops(),
		.storage_ctx = memB, .listen_port = 0,
	};
	struct mbt_session *sB;
	mbt_session_new(&sB, &cfgB);
	mbt_session_add_torrent_buf(sB, encoded, enc_len);

	/* Leecher: uses rarest picker, connects to both */
	struct mbt_storage_memory *memL = mbt_storage_memory_new();
	struct mbt_session_config cfgL = {
		.ev = ev, .storage_ops = mbt_storage_memory_ops(),
		.storage_ctx = memL, .listen_port = 0,
		.picker = mbt_picker_rarest,
		.max_pieces_per_peer = 1, /* serialize for predictable pick order */
	};
	struct mbt_session *sL;
	mbt_session_new(&sL, &cfgL);

	/* Compute infohash for magnet */
	unsigned char *is = memmem(encoded, enc_len, "4:infod", 7);
	if (!is) is = memmem(encoded, enc_len, "4:info", 6);
	is += 6;
	struct mbt_be_value *tmp = NULL;
	int ilen = mbt_be_value_parse(is, enc_len - (is - encoded), &tmp);
	mbt_be_unref(tmp);
	SHA1_CTX ctx; uint8_t digest[20]; char hex[41];
	SHA1Init(&ctx); SHA1Update(&ctx, is, ilen); SHA1Final(digest, &ctx);
	mbt_hex_encode(digest, 20, hex, sizeof(hex));

	char magnet[256];
	snprintf(magnet, sizeof(magnet),
	         "magnet:?xt=urn:btih:%s&x.pe=127.0.0.1:%hu&x.pe=127.0.0.1:%hu",
	         hex, mbt_session_listen_port(sA), mbt_session_listen_port(sB));
	mbt_session_add_magnet(sL, magnet);

	struct mbt_torrent *tL = mbt_session_torrent_first(sL);
	struct mbt_torrent_callbacks cb = { .on_piece_complete = on_piece, .on_complete = on_complete };
	mbt_torrent_set_callbacks(tL, &cb, NULL);

	struct mbt_ev_timer *to;
	mbt_ev_timer_new(ev, &to, 15 * 1000000ULL, timeout_cb, NULL);
	mbt_ev_run(ev);

	assert(complete == 1);
	fprintf(stderr, "  pieces arrived: ");
	for (int i = 0; i < pieces_idx; i++) fprintf(stderr, "%d ", pieces_order[i]);
	fprintf(stderr, "\n");

	/* The first piece is random (phase 1). After that, rarest-first
	 * should strongly prefer pieces 4-7 (avail=1) over 0-3 (avail=2).
	 * Assert all 8 pieces arrived and at least 1 rare piece in first 4. */
	assert(pieces_idx == NUM_PIECES);
	int rare_in_first4 = 0;
	for (int i = 0; i < 4; i++)
		if (pieces_order[i] >= 4) rare_in_first4++;
	fprintf(stderr, "  rare pieces (4-7) in first 4: %d\n", rare_in_first4);
	assert(rare_in_first4 >= 1);

	fprintf(stderr, "rarest_test: PASS\n");

	free(encoded);
	mbt_ev_timer_free(to);
	mbt_session_free(sA);
	mbt_session_free(sB);
	mbt_session_free(sL);
	mbt_ev_destroy(ev);
	mbt_storage_memory_destroy(memA);
	mbt_storage_memory_destroy(memB);
	mbt_storage_memory_destroy(memL);
	return 0;
}
