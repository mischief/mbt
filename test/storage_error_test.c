// SPDX-License-Identifier: ISC
/* Storage failures stop the torrent and reach the caller. */
#include <assert.h>
#include <errno.h>
#include <sha1.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>

#include "mbt/be.h"
#include "mbt/event.h"
#include "mbt/infohash.h"
#include "mbt/session.h"
#include "mbt/storage.h"
#include "mbt/torrent.h"
#include "test_util.h"

#define PIECE_LEN 16384
#define NUM_PIECES 4
#define TOTAL_LEN (PIECE_LEN * NUM_PIECES)
#define GOOD_WRITES 2
#define WAIT_SEC 20

static uint8_t data[TOTAL_LEN];
static int writes;
static int completes;
static int errors;
static int last_error;
static char handle;

static int
fail_open(void *ctx, struct mbt_torrent *t, struct mbt_storage_handle **out)
{
	(void)ctx; (void)t; (void)out;
	return -EACCES;
}

static int
full_open(void *ctx, struct mbt_torrent *t, struct mbt_storage_handle **out)
{
	(void)t;
	*out = ctx;
	return 0;
}

static int
full_write(struct mbt_storage_handle *h, uint32_t p, const void *d, size_t l)
{
	(void)h; (void)p; (void)d; (void)l;
	return writes++ < GOOD_WRITES ? 0 : -ENOSPC;
}

static int
full_read(struct mbt_storage_handle *h, uint32_t p, void *buf, size_t len)
{
	(void)h; (void)p; (void)buf; (void)len;
	return -EIO;
}

static void
full_close(struct mbt_storage_handle *h)
{
	(void)h;
}

static const struct mbt_storage_ops full_ops = {
	.open = full_open, .write_piece = full_write, .read_piece = full_read,
	.close = full_close,
};
static const struct mbt_storage_ops noopen_ops = {
	.open = fail_open, .write_piece = full_write, .read_piece = full_read,
	.close = full_close,
};

static void
on_complete(struct mbt_session *s, struct mbt_torrent *t, void *arg)
{
	(void)s; (void)t; (void)arg;
	completes++;
}

static void
on_error(struct mbt_session *s, struct mbt_torrent *t, int err, void *arg)
{
	(void)s; (void)t; (void)arg;
	errors++;
	last_error = err;
}

static void
make_torrent(unsigned char **enc, size_t *enc_len)
{
	uint8_t hashes[NUM_PIECES * 20];
	struct mbt_be_value *root, *info;

	for (int i = 0; i < TOTAL_LEN; i++)
		data[i] = (uint8_t)(i * 11 + 3);
	for (int i = 0; i < NUM_PIECES; i++) {
		SHA1_CTX ctx;

		SHA1Init(&ctx);
		SHA1Update(&ctx, data + i * PIECE_LEN, PIECE_LEN);
		SHA1Final(hashes + i * 20, &ctx);
	}
	assert(mbt_be_dict_new(&info) == 0);
	assert(mbt_be_dict_append_string(info, "name", "e.bin", 5) == 0);
	assert(mbt_be_dict_append_number(info, "piece length", PIECE_LEN) == 0);
	assert(mbt_be_dict_append_number(info, "length", TOTAL_LEN) == 0);
	assert(mbt_be_dict_append_string(info, "pieces", hashes, sizeof(hashes)) == 0);
	assert(mbt_be_dict_new(&root) == 0);
	assert(mbt_be_dict_append(root, "info", info) == 0);
	mbt_be_unref(info);
	assert(mbt_be_value_encode(root, enc, enc_len) == 0);
	mbt_be_unref(root);
}

static struct mbt_session *
session_on(struct mbt_ev *ev, struct mbt_storage *st)
{
	struct mbt_session_config cfg = {.ev = ev, .default_storage = st};
	struct mbt_session_callbacks cb = {
		.on_torrent_complete = on_complete, .on_torrent_error = on_error,
	};
	struct mbt_session *s;

	assert(mbt_session_new(&s, &cfg) == 0);
	mbt_session_set_callbacks(s, &cb);
	return s;
}

static void
wait_error(struct mbt_ev *ev)
{
	time_t end = time(NULL) + WAIT_SEC;

	errors = 0;
	while (errors == 0) {
		assert(time(NULL) < end);
		test_ev_step(ev, 10);
	}
}

/* A failed open from a .torrent fails the add and leaves no torrent. */
static void
test_add_open_fails(struct mbt_ev *ev, const unsigned char *enc, size_t enc_len)
{
	struct mbt_storage *st = mbt_storage_new(&noopen_ops, &handle);
	struct mbt_session *s = session_on(ev, st);

	assert(mbt_session_add_torrent_buf(s, enc, enc_len, NULL) == -EACCES);
	assert(mbt_session_torrent_count(s) == 0);
	mbt_session_free(s);
	mbt_storage_unref(st);
}

/* A failed open after a metadata fetch reports through on_torrent_error. */
static void
test_magnet_open_fails(struct mbt_ev *ev, struct mbt_session *seed, struct mbt_torrent *st)
{
	struct mbt_storage *bad = mbt_storage_new(&noopen_ops, &handle);
	struct mbt_session *s = session_on(ev, bad);
	char hex[41], uri[128];

	mbt_infohash_string(mbt_torrent_infohash(st), hex, sizeof(hex));
	snprintf(uri, sizeof(uri), "magnet:?xt=urn:btih:%s&x.pe=127.0.0.1:%hu", hex,
	    mbt_session_listen_port(seed));
	assert(mbt_session_add_magnet(s, uri, NULL) == 0);
	wait_error(ev);
	assert(last_error == -EACCES);
	assert(mbt_torrent_state(mbt_session_torrent_first(s)) == MBT_TORRENT_ERROR);
	mbt_session_free(s);
	mbt_storage_unref(bad);
}

/* A failed write stops the torrent, and the piece does not count. */
static void
test_write_fails(struct mbt_ev *ev, struct mbt_session *seed,
    const unsigned char *enc, size_t enc_len)
{
	struct mbt_storage *full = mbt_storage_new(&full_ops, &handle);
	struct mbt_session *s = session_on(ev, full);

	assert(mbt_session_add_torrent_buf(s, enc, enc_len, NULL) == 0);
	struct mbt_torrent *t = mbt_session_torrent_first(s);
	test_wait_verified(ev, t);
	assert(mbt_session_add_peer(s, mbt_torrent_infohash(t), "127.0.0.1",
	    mbt_session_listen_port(seed), MBT_SOURCE_MANUAL, 0) == 0);
	wait_error(ev);
	assert(last_error == -ENOSPC);
	assert(mbt_torrent_error(t) == -ENOSPC);
	assert(mbt_torrent_state(t) == MBT_TORRENT_ERROR);
	assert(mbt_torrent_have_count(t) < NUM_PIECES);
	assert(!mbt_torrent_complete(t));
	mbt_session_free(s);
	mbt_storage_unref(full);
}

int
main(void)
{
	struct mbt_ev *ev;
	unsigned char *enc;
	size_t enc_len;

	make_torrent(&enc, &enc_len);
	assert(mbt_ev_new_default(&ev) == 0);

	test_add_open_fails(ev, enc, enc_len);

	struct mbt_storage_memory *mem = mbt_storage_memory_new();
	mbt_storage_memory_put_file(mem, 0, data, TOTAL_LEN);
	struct mbt_session *seed = session_on(ev, mbt_storage_memory_storage(mem));
	assert(mbt_session_add_torrent_buf(seed, enc, enc_len, NULL) == 0);
	struct mbt_torrent *st = mbt_session_torrent_first(seed);
	test_wait_verified(ev, st);
	assert(mbt_torrent_complete(st));
	completes = 0;

	test_magnet_open_fails(ev, seed, st);
	test_write_fails(ev, seed, enc, enc_len);
	assert(completes == 0);

	mbt_session_free(seed);
	mbt_storage_memory_destroy(mem);
	mbt_ev_destroy(ev);
	free(enc);
	fprintf(stderr, "storage_error_test: PASS\n");
	return 0;
}
