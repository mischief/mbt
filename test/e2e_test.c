// SPDX-License-Identifier: ISC
/*
 * test/e2e_test.c — end-to-end transfer between two sessions over localhost
 *
 * Session 1 (seeder): memory storage pre-filled with test data, listening
 * Session 2 (leecher): connects via magnet with x.pe pointing at session 1
 * Both share one event loop. Validates data + metadata match after transfer.
 */
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <assert.h>
#include <unistd.h>
#include <time.h>

#include <sha1.h>

#include "mbt/mbt.h"
#include "mbt/be.h"
#include "mbt/hex.h"
#include "mbt/infohash.h"

#define PIECE_LEN 16384
#define NUM_PIECES 8
#define TOTAL_LEN (PIECE_LEN * NUM_PIECES)

static struct mbt_ev *ev;
static int download_complete;

static void
on_complete(struct mbt_torrent *t, void *arg)
{
	(void)t; (void)arg;
	download_complete = 1;
	mbt_ev_stop(ev);
}

/* Generate deterministic test data */
static void
fill_test_data(uint8_t *buf, size_t len)
{
	srand(42);
	for (size_t i = 0; i < len; i++)
		buf[i] = rand() & 0xff;
}

/* Build .torrent in memory, return encoded buffer and hex infohash */
static unsigned char *
build_torrent(const uint8_t *data, char *hex_out, size_t hex_sz, size_t *out_len)
{
	uint8_t pieces_hash[NUM_PIECES * 20];
	for (int i = 0; i < NUM_PIECES; i++) {
		SHA1_CTX ctx;
		SHA1Init(&ctx);
		SHA1Update(&ctx, data + i * PIECE_LEN, PIECE_LEN);
		SHA1Final(pieces_hash + i * 20, &ctx);
	}

	struct mbt_be_value *root = NULL, *info = NULL;
	mbt_be_dict_new(&root);
	mbt_be_dict_new(&info);
	mbt_be_dict_append(root, "info", info);
	mbt_be_dict_append_number(info, "length", TOTAL_LEN);
	mbt_be_dict_append_string(info, "name", "e2e_test.bin", 12);
	mbt_be_dict_append_number(info, "piece length", PIECE_LEN);
	mbt_be_dict_append_string(info, "pieces", pieces_hash, sizeof(pieces_hash));

	unsigned char *encoded;
	size_t enc_len;
	assert(mbt_be_value_encode(root, &encoded, &enc_len) == 0);

	/* Compute infohash */
	unsigned char *info_start = memmem(encoded, enc_len, "4:infod", 7);
	assert(info_start);
	info_start += 6;
	struct mbt_be_value *tmp = NULL;
	int ilen = mbt_be_value_parse(info_start, enc_len - (info_start - encoded), &tmp);
	assert(ilen > 0);
	mbt_be_value_free(tmp);

	SHA1_CTX ctx;
	uint8_t digest[20];
	SHA1Init(&ctx);
	SHA1Update(&ctx, info_start, ilen);
	SHA1Final(digest, &ctx);
	mbt_hex_encode(digest, 20, hex_out, hex_sz);

	mbt_be_value_free(root);
	*out_len = enc_len;
	return encoded;
}

int
main(void)
{
	int rv;
	uint8_t test_data[TOTAL_LEN];
	fill_test_data(test_data, TOTAL_LEN);

	/* Shared event loop */
	mbt_log_set(mbt_log_stderr, NULL);
	mbt_log_set_debug(MBT_LOG_ALL);

#ifdef HAVE_SD_EVENT
	rv = mbt_ev_sd_new(&ev);
#elif defined(HAVE_LIBEVENT)
	rv = mbt_ev_libevent_new(&ev);
#endif
	assert(rv == 0);

	/* Build .torrent */
	char infohash_hex[41];
	size_t torrent_len;
	unsigned char *torrent_buf = build_torrent(test_data, infohash_hex, sizeof(infohash_hex), &torrent_len);
	fprintf(stderr, "e2e_test: infohash=%s\n", infohash_hex);

	/* --- Session 1 (seeder) --- */
	struct mbt_storage_memory *mem1 = mbt_storage_memory_new();

	/* Pre-fill storage with test data BEFORE adding torrent (so resume verifies) */
	rv = mbt_storage_memory_put_file(mem1, 0, test_data, TOTAL_LEN);
	assert(rv == 0);
	struct mbt_session_config cfg1 = {
		.ev = ev,
		.storage_ops = mbt_storage_memory_ops(),
		.storage_ctx = mem1,
		.listen_port = 16881,
	};
	struct mbt_session *s1;
	rv = mbt_session_new(&s1, &cfg1);
	assert(rv == 0);

	rv = mbt_session_add_torrent_buf(s1, torrent_buf, torrent_len);
	assert(rv == 0);

	/* Pre-fill seeder storage with test data (piece by piece) */
	struct mbt_torrent *t1 = mbt_session_torrent_first(s1);
	assert(t1);
	assert(mbt_torrent_complete(t1) == 1); /* resume verified all pieces */

	/* --- Session 2 (leecher) --- */
	struct mbt_storage_memory *mem2 = mbt_storage_memory_new();
	struct mbt_session_config cfg2 = {
		.ev = ev,
		.storage_ops = mbt_storage_memory_ops(),
		.storage_ctx = mem2,
		.listen_port = 16882,
	};
	struct mbt_session *s2;
	rv = mbt_session_new(&s2, &cfg2);
	assert(rv == 0);

	/* Add magnet pointing at seeder */
	char magnet[256];
	snprintf(magnet, sizeof(magnet),
	         "magnet:?xt=urn:btih:%s&x.pe=127.0.0.1:16881", infohash_hex);
	rv = mbt_session_add_magnet(s2, magnet);
	assert(rv == 0);

	struct mbt_torrent *t2 = mbt_session_torrent_first(s2);
	assert(t2);

	struct mbt_torrent_callbacks cb = { .on_complete = on_complete };
	mbt_torrent_set_callbacks(t2, &cb, NULL);

	/* --- Run event loop (drives both sessions) --- */
	fprintf(stderr, "e2e_test: entering event loop\n");
	rv = mbt_ev_run(ev);
	fprintf(stderr, "e2e_test: event loop exited rv=%d\n", rv);

	/* --- Verify --- */
	assert(download_complete == 1);
	assert(mbt_torrent_complete(t2) == 1);
	assert(mbt_torrent_have_count(t2) == NUM_PIECES);

	/* Verify data matches */
	uint8_t *dl_data = mbt_storage_memory_data(mem2);
	assert(dl_data != NULL);
	assert(memcmp(dl_data, test_data, TOTAL_LEN) == 0);

	fprintf(stderr, "e2e_test: PASS (%d pieces transferred over localhost, data verified)\n", NUM_PIECES);

	/* Cleanup */
	free(torrent_buf);
	mbt_session_free(s1);
	mbt_session_free(s2);
	mbt_ev_destroy(ev);
	mbt_storage_memory_destroy(mem1);
	mbt_storage_memory_destroy(mem2);
	return 0;
}
