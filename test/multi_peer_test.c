// SPDX-License-Identifier: ISC
/*
 * test/multi_peer_test.c — download from 3 partial seeders
 *
 * Seeder A has pieces 0,1. Seeder B has pieces 2,3. Seeder C has pieces 4,5.
 * Leecher must combine from all three to complete (6 pieces total).
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
#include "mbt/be.h"
#include "mbt/hex.h"

#define PIECE_LEN 16384
#define NUM_PIECES 6
#define TOTAL_LEN (PIECE_LEN * NUM_PIECES)

static struct mbt_ev *ev;
static int download_complete;

static void on_complete(struct mbt_torrent *t, void *arg)
{ (void)t; (void)arg; download_complete = 1; mbt_ev_stop(ev); }

static void timeout_cb(struct mbt_ev_timer *timer, void *arg)
{ (void)timer; (void)arg; mbt_ev_stop(ev); }

int main(void)
{
	int rv;
	uint8_t test_data[TOTAL_LEN];
	for (int i = 0; i < TOTAL_LEN; i++) test_data[i] = (i * 31 + 5) & 0xff;

	/* Build .torrent */
	uint8_t pieces_hash[NUM_PIECES * 20];
	for (int i = 0; i < NUM_PIECES; i++) {
		SHA1_CTX ctx; SHA1Init(&ctx);
		SHA1Update(&ctx, test_data + i * PIECE_LEN, PIECE_LEN);
		SHA1Final(pieces_hash + i * 20, &ctx);
	}

	struct mbt_be_value *root = NULL, *info = NULL;
	mbt_be_dict_new(&root);
	mbt_be_dict_new(&info);
	mbt_be_dict_append(root, "info", info);
	mbt_be_dict_append_number(info, "length", TOTAL_LEN);
	mbt_be_dict_append_string(info, "name", "multi_test.bin", 14);
	mbt_be_dict_append_number(info, "piece length", PIECE_LEN);
	mbt_be_dict_append_string(info, "pieces", pieces_hash, sizeof(pieces_hash));

	unsigned char *encoded; size_t enc_len;
	assert(mbt_be_value_encode(root, &encoded, &enc_len) == 0);
	mbt_be_value_free(root);

	mbt_ev_new_default(&ev);

	/* Seeder A: pieces 0,1 */
	struct mbt_storage_memory *memA = mbt_storage_memory_new();
	mbt_storage_memory_put_file(memA, 0, test_data, PIECE_LEN * 2);
	struct mbt_session_config cfgA = {
		.ev = ev, .storage_ops = mbt_storage_memory_ops(),
		.storage_ctx = memA, .listen_port = 18001,
	};
	struct mbt_session *sA;
	rv = mbt_session_new(&sA, &cfgA); assert(rv == 0);
	rv = mbt_session_add_torrent_buf(sA, encoded, enc_len); assert(rv == 0);

	/* Seeder B: pieces 2,3 */
	struct mbt_storage_memory *memB = mbt_storage_memory_new();
	mbt_storage_memory_put_file(memB, PIECE_LEN * 2, test_data + PIECE_LEN * 2, PIECE_LEN * 2);
	struct mbt_session_config cfgB = {
		.ev = ev, .storage_ops = mbt_storage_memory_ops(),
		.storage_ctx = memB, .listen_port = 18002,
	};
	struct mbt_session *sB;
	rv = mbt_session_new(&sB, &cfgB); assert(rv == 0);
	rv = mbt_session_add_torrent_buf(sB, encoded, enc_len); assert(rv == 0);

	/* Seeder C: pieces 4,5 */
	struct mbt_storage_memory *memC = mbt_storage_memory_new();
	mbt_storage_memory_put_file(memC, PIECE_LEN * 4, test_data + PIECE_LEN * 4, PIECE_LEN * 2);
	struct mbt_session_config cfgC = {
		.ev = ev, .storage_ops = mbt_storage_memory_ops(),
		.storage_ctx = memC, .listen_port = 18003,
	};
	struct mbt_session *sC;
	rv = mbt_session_new(&sC, &cfgC); assert(rv == 0);
	rv = mbt_session_add_torrent_buf(sC, encoded, enc_len); assert(rv == 0);

	/* Leecher: connects to all three via x.pe */
	struct mbt_storage_memory *memL = mbt_storage_memory_new();
	struct mbt_session_config cfgL = {
		.ev = ev, .storage_ops = mbt_storage_memory_ops(),
		.storage_ctx = memL, .listen_port = 18004,
	};
	struct mbt_session *sL;
	rv = mbt_session_new(&sL, &cfgL); assert(rv == 0);

	/* Compute infohash for magnet */
	unsigned char *info_start = memmem(encoded, enc_len, "4:infod", 7);
	if (!info_start) info_start = memmem(encoded, enc_len, "4:info", 6);
	assert(info_start); info_start += 6;
	struct mbt_be_value *tmp = NULL;
	int ilen = mbt_be_value_parse(info_start, enc_len - (info_start - encoded), &tmp);
	assert(ilen > 0); mbt_be_value_free(tmp);
	SHA1_CTX ctx; uint8_t digest[20]; char hex[41];
	SHA1Init(&ctx); SHA1Update(&ctx, info_start, ilen); SHA1Final(digest, &ctx);
	mbt_hex_encode(digest, 20, hex, sizeof(hex));

	char magnet[256];
	snprintf(magnet, sizeof(magnet),
	         "magnet:?xt=urn:btih:%s&x.pe=127.0.0.1:18001&x.pe=127.0.0.1:18002&x.pe=127.0.0.1:18003", hex);
	rv = mbt_session_add_magnet(sL, magnet); assert(rv == 0);

	struct mbt_torrent *tL = mbt_session_torrent_first(sL);
	struct mbt_torrent_callbacks cb = { .on_complete = on_complete };
	mbt_torrent_set_callbacks(tL, &cb, NULL);

	struct mbt_ev_timer *to;
	mbt_ev_timer_new(ev, &to, 10 * 1000000ULL, timeout_cb, NULL);

	rv = mbt_ev_run(ev);

	assert(download_complete == 1);
	uint8_t *dl = mbt_storage_memory_data(memL);
	assert(dl && memcmp(dl, test_data, TOTAL_LEN) == 0);

	fprintf(stderr, "multi_peer_test: PASS (6 pieces from 3 partial seeders)\n");

	free(encoded);
	mbt_ev_timer_free(to);
	mbt_session_free(sA);
	mbt_session_free(sB);
	mbt_session_free(sC);
	mbt_session_free(sL);
	mbt_ev_destroy(ev);
	mbt_storage_memory_destroy(memA);
	mbt_storage_memory_destroy(memB);
	mbt_storage_memory_destroy(memC);
	mbt_storage_memory_destroy(memL);
	return 0;
}
