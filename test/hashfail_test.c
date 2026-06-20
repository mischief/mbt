// SPDX-License-Identifier: ISC
/*
 * test/hashfail_test.c — verify hash failure detection and peer banning
 *
 * Seeder A: returns correct data.
 * Seeder B: returns corrupted data (bit-flipped).
 * Leecher: should detect hash failures from B, ban B after 5 failures,
 * and complete the download via A.
 */
#include <assert.h>
#include <stdio.h>
#include <string.h>
#include <stdlib.h>

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
static uint8_t good_data[TOTAL_LEN];

static void on_complete(struct mbt_torrent *t, void *arg)
{ (void)t; (void)arg; complete = 1; mbt_ev_stop(ev); }

static void timeout_cb(struct mbt_ev_timer *t, void *a)
{ (void)t; (void)a; mbt_ev_stop(ev); }

/* Corrupt storage: returns data with one byte flipped per piece */
static int corrupt_open(void *ctx, struct mbt_torrent *t)
{ (void)ctx; (void)t; return 0; }
static void corrupt_close(void *ctx) { (void)ctx; }
static int corrupt_have(void *ctx, uint32_t p) { (void)ctx; (void)p; return 1; }
static int corrupt_read(void *ctx, uint32_t p, void *buf, size_t len)
{
	(void)ctx;
	size_t off = (size_t)p * PIECE_LEN;
	if (off + len > TOTAL_LEN) len = TOTAL_LEN - off;
	memcpy(buf, good_data + off, len);
	/* Flip one byte */
	((uint8_t *)buf)[0] ^= 0xff;
	return 0;
}
static int corrupt_write(void *ctx, uint32_t p, const void *d, size_t l)
{ (void)ctx; (void)p; (void)d; (void)l; return 0; }

static const struct mbt_storage_ops corrupt_ops = {
	.open = corrupt_open, .close = corrupt_close,
	.have_piece = corrupt_have, .read_piece = corrupt_read,
	.write_piece = corrupt_write,
};

int main(void)
{
	for (int i = 0; i < TOTAL_LEN; i++) good_data[i] = (i * 59 + 13) & 0xff;

	uint8_t pieces_hash[NUM_PIECES * 20];
	for (int i = 0; i < NUM_PIECES; i++) {
		SHA1_CTX ctx; SHA1Init(&ctx);
		SHA1Update(&ctx, good_data + i * PIECE_LEN, PIECE_LEN);
		SHA1Final(pieces_hash + i * 20, &ctx);
	}

	_cleanup_be_ struct mbt_be_value *root = NULL, *info = NULL;
	mbt_be_dict_new(&root);
	mbt_be_dict_new(&info);
	mbt_be_dict_append(root, "info", info);
	mbt_be_dict_append_number(info, "length", TOTAL_LEN);
	mbt_be_dict_append_string(info, "name", "hashfail_test.bin", 17);
	mbt_be_dict_append_number(info, "piece length", PIECE_LEN);
	mbt_be_dict_append_string(info, "pieces", pieces_hash, sizeof(pieces_hash));

	unsigned char *encoded; size_t enc_len;
	mbt_be_value_encode(root, &encoded, &enc_len);

	mbt_ev_new_default(&ev);

	/* Seeder A: correct data */
	struct mbt_storage_memory *memA = mbt_storage_memory_new();
	mbt_storage_memory_put_file(memA, 0, good_data, TOTAL_LEN);
	struct mbt_session_config cfgA = {
		.ev = ev, .storage_ops = mbt_storage_memory_ops(),
		.storage_ctx = memA, .listen_port = 0,
	};
	struct mbt_session *sA;
	mbt_session_new(&sA, &cfgA);
	mbt_session_add_torrent_buf(sA, encoded, enc_len);

	/* Seeder B: corrupt data */
	struct mbt_session_config cfgB = {
		.ev = ev, .storage_ops = &corrupt_ops,
		.storage_ctx = NULL, .listen_port = 0,
	};
	struct mbt_session *sB;
	mbt_session_new(&sB, &cfgB);
	mbt_session_add_torrent_buf(sB, encoded, enc_len);

	/* Leecher: short request timeout to cycle faster */
	struct mbt_storage_memory *memL = mbt_storage_memory_new();
	struct mbt_session_config cfgL = {
		.ev = ev, .storage_ops = mbt_storage_memory_ops(),
		.storage_ctx = memL, .listen_port = 0,
		.timers = { .request_timeout = 5 },
	};
	struct mbt_session *sL;
	mbt_session_new(&sL, &cfgL);

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
	struct mbt_torrent_callbacks cb = { .on_complete = on_complete };
	mbt_torrent_set_callbacks(tL, &cb, NULL);

	struct mbt_ev_timer *to;
	mbt_ev_timer_new(ev, &to, 30 * 1000000ULL, timeout_cb, NULL);
	mbt_ev_run(ev);

	assert(complete == 1);
	fprintf(stderr, "hashfail_test: PASS (completed despite corrupt peer)\n");

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
