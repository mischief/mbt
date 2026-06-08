// SPDX-License-Identifier: ISC
/*
 * test/magnet_flow_test.c — magnet → tracker → metadata → download
 *
 * Seeder has the torrent. Leecher starts with only a magnet URI + tracker.
 * Must fetch metadata via BEP 9, then download pieces.
 */
#include <assert.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include <sha1.h>

#include "mbt/event.h"
#include "mbt/session.h"
#include "mbt/torrent.h"
#include "mbt/storage.h"
#include "mbt/tracker_server.h"
#include "mbt/be.h"
#include "mbt/hex.h"
#include "mbt/log.h"

#define PIECE_LEN 16384
#define NUM_PIECES 4
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
	for (int i = 0; i < TOTAL_LEN; i++) test_data[i] = (i * 13 + 7) & 0xff;

	/* Compute piece hashes + build .torrent with tracker */
	uint8_t pieces_hash[NUM_PIECES * 20];
	for (int i = 0; i < NUM_PIECES; i++) {
		SHA1_CTX ctx; SHA1Init(&ctx);
		SHA1Update(&ctx, test_data + i * PIECE_LEN, PIECE_LEN);
		SHA1Final(pieces_hash + i * 20, &ctx);
	}

	struct mbt_be_value *root = NULL, *info = NULL;
	mbt_be_dict_new(&root);
	mbt_be_dict_append_string(root, "announce", "udp://127.0.0.1:17969/announce", 30);
	mbt_be_dict_new(&info);
	mbt_be_dict_append(root, "info", info);
	mbt_be_dict_append_number(info, "length", TOTAL_LEN);
	mbt_be_dict_append_string(info, "name", "magnet_test.bin", 15);
	mbt_be_dict_append_number(info, "piece length", PIECE_LEN);
	mbt_be_dict_append_string(info, "pieces", pieces_hash, sizeof(pieces_hash));

	unsigned char *encoded; size_t enc_len;
	assert(mbt_be_value_encode(root, &encoded, &enc_len) == 0);

	/* Compute infohash */
	unsigned char *info_start = memmem(encoded, enc_len, "4:infod", 7);
	if (!info_start) info_start = memmem(encoded, enc_len, "4:info", 6);
	assert(info_start); info_start += 6;
	struct mbt_be_value *tmp = NULL;
	int ilen = mbt_be_value_parse(info_start, enc_len - (info_start - encoded), &tmp);
	assert(ilen > 0); mbt_be_value_free(tmp);
	SHA1_CTX ctx; uint8_t digest[20]; char hex[41];
	SHA1Init(&ctx); SHA1Update(&ctx, info_start, ilen); SHA1Final(digest, &ctx);
	mbt_hex_encode(digest, 20, hex, sizeof(hex));

	mbt_be_value_free(root);

	mbt_ev_new_default(&ev);

	/* Tracker */
	struct mbt_tracker_server *tracker;
	rv = mbt_tracker_server_new(&tracker, ev, 17969, NULL); assert(rv == 0);

	/* Seeder */
	struct mbt_storage_memory *mem1 = mbt_storage_memory_new();
	mbt_storage_memory_put_file(mem1, 0, test_data, TOTAL_LEN);
	struct mbt_session_config cfg1 = {
		.ev = ev, .storage_ops = mbt_storage_memory_ops(),
		.storage_ctx = mem1, .listen_port = 17891,
	};
	struct mbt_session *s1;
	rv = mbt_session_new(&s1, &cfg1); assert(rv == 0);
	rv = mbt_session_add_torrent_buf(s1, encoded, enc_len); assert(rv == 0);

	/* Leecher — magnet only, no .torrent */
	struct mbt_storage_memory *mem2 = mbt_storage_memory_new();
	struct mbt_session_config cfg2 = {
		.ev = ev, .storage_ops = mbt_storage_memory_ops(),
		.storage_ctx = mem2, .listen_port = 17892,
	};
	struct mbt_session *s2;
	rv = mbt_session_new(&s2, &cfg2); assert(rv == 0);

	char magnet[256];
	snprintf(magnet, sizeof(magnet),
	         "magnet:?xt=urn:btih:%s&tr=udp://127.0.0.1:17969/announce", hex);
	rv = mbt_session_add_magnet(s2, magnet); assert(rv == 0);

	struct mbt_torrent *t2 = mbt_session_torrent_first(s2);
	struct mbt_torrent_callbacks cb = { .on_complete = on_complete };
	mbt_torrent_set_callbacks(t2, &cb, NULL);

	struct mbt_ev_timer *to;
	mbt_ev_timer_new(ev, &to, 10 * 1000000ULL, timeout_cb, NULL);

	rv = mbt_ev_run(ev);

	assert(download_complete == 1);
	uint8_t *dl = mbt_storage_memory_data(mem2);
	assert(dl && memcmp(dl, test_data, TOTAL_LEN) == 0);

	fprintf(stderr, "magnet_flow_test: PASS\n");

	free(encoded);
	mbt_ev_timer_free(to);
	mbt_session_free(s1);
	mbt_session_free(s2);
	mbt_tracker_server_free(tracker);
	mbt_ev_destroy(ev);
	mbt_storage_memory_destroy(mem1);
	mbt_storage_memory_destroy(mem2);
	return 0;
}
