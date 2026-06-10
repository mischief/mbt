// SPDX-License-Identifier: ISC
/*
 * test/snub_test.c — verify anti-snubbing works
 *
 * Seeder A: has all pieces, responds normally.
 * Seeder B: has all pieces, never responds to requests (snubbed).
 * Leecher: snub_timeout=2s, should snub B and complete via A.
 */
#include <assert.h>
#include <stdio.h>
#include <string.h>
#include <stdlib.h>

#include <sha1.h>

#include "mbt/event.h"
#include "mbt/session.h"
#include "mbt/torrent.h"
#include "mbt/storage.h"
#include "mbt/picker.h"
#include "mbt/be.h"
#include "mbt/hex.h"

#define PIECE_LEN 16384
#define NUM_PIECES 4
#define TOTAL_LEN (PIECE_LEN * NUM_PIECES)

static struct mbt_ev *ev;
static int complete;

static void on_complete(struct mbt_torrent *t, void *arg)
{ (void)t; (void)arg; complete = 1; mbt_ev_stop(ev); }

static void timeout_cb(struct mbt_ev_timer *t, void *a)
{ (void)t; (void)a; mbt_ev_stop(ev); }

/*
 * "Dead seeder" — we use a normal session but with a storage backend
 * that returns data for have_piece (so it advertises HAVE_ALL) but
 * read_piece returns nothing (so REQUEST is never fulfilled).
 */
static int dead_open(void *ctx, struct mbt_torrent *t)
{ (void)ctx;(void)t; return 0; }
static void dead_close(void *ctx) { (void)ctx; }
static int dead_have(void *ctx, uint32_t p) { (void)ctx;(void)p; return 1; }
static int dead_read(void *ctx, uint32_t p, void *buf, size_t len)
{ (void)ctx;(void)p;(void)buf;(void)len; return -1; /* never deliver */ }
static int dead_write(void *ctx, uint32_t p, const void *d, size_t l)
{ (void)ctx;(void)p;(void)d;(void)l; return 0; }

static const struct mbt_storage_ops dead_ops = {
	.open = dead_open, .close = dead_close,
	.have_piece = dead_have, .read_piece = dead_read, .write_piece = dead_write,
};

int main(void)
{
	uint8_t data[TOTAL_LEN];
	for (int i = 0; i < TOTAL_LEN; i++) data[i] = (i * 41 + 3) & 0xff;

	uint8_t pieces_hash[NUM_PIECES * 20];
	for (int i = 0; i < NUM_PIECES; i++) {
		SHA1_CTX ctx; SHA1Init(&ctx);
		SHA1Update(&ctx, data + i * PIECE_LEN, PIECE_LEN);
		SHA1Final(pieces_hash + i * 20, &ctx);
	}

	struct mbt_be_value *root = NULL, *info = NULL;
	mbt_be_dict_new(&root);
	mbt_be_dict_new(&info);
	mbt_be_dict_append(root, "info", info);
	mbt_be_dict_append_number(info, "length", TOTAL_LEN);
	mbt_be_dict_append_string(info, "name", "snub_test.bin", 13);
	mbt_be_dict_append_number(info, "piece length", PIECE_LEN);
	mbt_be_dict_append_string(info, "pieces", pieces_hash, sizeof(pieces_hash));

	unsigned char *encoded; size_t enc_len;
	mbt_be_value_encode(root, &encoded, &enc_len);
	mbt_be_value_free(root);

	mbt_ev_new_default(&ev);

	/* Seeder A: working */
	struct mbt_storage_memory *memA = mbt_storage_memory_new();
	mbt_storage_memory_put_file(memA, 0, data, TOTAL_LEN);
	struct mbt_session_config cfgA = {
		.ev = ev, .storage_ops = mbt_storage_memory_ops(),
		.storage_ctx = memA, .listen_port = 19401,
	};
	struct mbt_session *sA;
	mbt_session_new(&sA, &cfgA);
	mbt_session_add_torrent_buf(sA, encoded, enc_len);

	/* Seeder B: dead (advertises pieces but never sends data) */
	struct mbt_session_config cfgB = {
		.ev = ev, .storage_ops = &dead_ops,
		.storage_ctx = NULL, .listen_port = 19402,
	};
	struct mbt_session *sB;
	mbt_session_new(&sB, &cfgB);
	mbt_session_add_torrent_buf(sB, encoded, enc_len);

	/* Leecher: short snub timeout */
	struct mbt_storage_memory *memL = mbt_storage_memory_new();
	struct mbt_session_config cfgL = {
		.ev = ev, .storage_ops = mbt_storage_memory_ops(),
		.storage_ctx = memL, .listen_port = 19403,
		.timers = { .snub_timeout = 2, .request_timeout = 3 },
	};
	struct mbt_session *sL;
	mbt_session_new(&sL, &cfgL);

	/* Compute magnet */
	unsigned char *is = memmem(encoded, enc_len, "4:infod", 7);
	if (!is) is = memmem(encoded, enc_len, "4:info", 6);
	is += 6;
	struct mbt_be_value *tmp = NULL;
	int ilen = mbt_be_value_parse(is, enc_len - (is - encoded), &tmp);
	mbt_be_value_free(tmp);
	SHA1_CTX ctx; uint8_t digest[20]; char hex[41];
	SHA1Init(&ctx); SHA1Update(&ctx, is, ilen); SHA1Final(digest, &ctx);
	mbt_hex_encode(digest, 20, hex, sizeof(hex));

	char magnet[256];
	snprintf(magnet, sizeof(magnet),
	         "magnet:?xt=urn:btih:%s&x.pe=127.0.0.1:19401&x.pe=127.0.0.1:19402", hex);
	mbt_session_add_magnet(sL, magnet);

	struct mbt_torrent *tL = mbt_session_torrent_first(sL);
	struct mbt_torrent_callbacks cb = { .on_complete = on_complete };
	mbt_torrent_set_callbacks(tL, &cb, NULL);

	struct mbt_ev_timer *to;
	mbt_ev_timer_new(ev, &to, 15 * 1000000ULL, timeout_cb, NULL);
	mbt_ev_run(ev);

	assert(complete == 1);
	fprintf(stderr, "snub_test: PASS (completed despite dead peer)\n");

	free(encoded);
	mbt_ev_timer_free(to);
	mbt_session_free(sA);
	mbt_session_free(sB);
	mbt_session_free(sL);
	mbt_ev_destroy(ev);
	mbt_storage_memory_destroy(memA);
	mbt_storage_memory_destroy(memL);
	return 0;
}
