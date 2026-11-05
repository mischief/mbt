// SPDX-License-Identifier: ISC
/* The add calls: returned torrent, repeated adds and .torrent size limits. */
#include <assert.h>
#include <errno.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>

#include "mbt/event.h"
#include "mbt/infohash.h"
#include "mbt/metainfo.h"
#include "mbt/session.h"
#include "mbt/storage.h"
#include "mbt/torrent.h"
#include "test_util.h"

#define PIECE_LEN 16384
#define FILE_LEN 100
#define TRACKER "udp://127.0.0.1:1/announce"
#define BIG_TAIL (5 * 1024 * 1024)
#define OVER_LIMIT (65 * 1024 * 1024)

static char tmpdir[] = "/tmp/mbt_add_XXXXXX";

static void
path_of(char *p, size_t n, const char *name)
{
	snprintf(p, n, "%s/%s", tmpdir, name);
}

/* Encode a torrent of one short file. */
static void
encode_priv(unsigned char **enc, size_t *enc_len, int priv)
{
	static const uint8_t data[FILE_LEN];
	struct mbt_metainfo *mi;
	char path[256];
	FILE *fp;

	path_of(path, sizeof(path), "a.bin");
	fp = fopen(path, "wb");
	assert(fp);
	assert(fwrite(data, 1, sizeof(data), fp) == sizeof(data));
	fclose(fp);
	assert(mbt_metainfo_new(&mi) == 0);
	assert(mbt_metainfo_set_content(mi, path, PIECE_LEN) == 0);
	assert(mbt_metainfo_add_tracker(mi, TRACKER) == 0);
	mbt_metainfo_set_private(mi, priv);
	assert(mbt_metainfo_encode(mi, enc, enc_len) == 0);
	mbt_metainfo_free(mi);
}

static void
encode(unsigned char **enc, size_t *enc_len)
{
	encode_priv(enc, enc_len, 0);
}

/* BEP 27: a private torrent stays on its own trackers. */
static void
test_fallback_private(struct mbt_session *s)
{
	struct mbt_torrent *t = NULL;
	unsigned char *enc;
	size_t enc_len;

	encode_priv(&enc, &enc_len, 1);
	assert(mbt_session_add_torrent_buf(s, enc, enc_len, NULL, &t) == 0);
	assert(mbt_torrent_is_private(t));
	mbt_session_add_tracker_fallbacks(s, t);
	assert(mbt_session_torrent_tracker_count(s, t) == 1);
	assert(mbt_session_remove_torrent(s, mbt_torrent_infohash(t)) == 0);
	free(enc);
}

static void
test_repeat(struct mbt_session *s, const unsigned char *enc, size_t enc_len)
{
	struct mbt_torrent *t = NULL, *again = NULL;

	assert(mbt_session_add_torrent_buf(s, enc, enc_len, NULL, &t) == 0);
	assert(t && mbt_torrent_has_metadata(t));
	assert(t == mbt_session_torrent_first(s));
	assert(mbt_session_torrent_tracker_count(s, t) == 1);

	assert(mbt_session_add_torrent_buf(s, enc, enc_len, NULL, &again) == 0);
	assert(again == t);
	assert(mbt_session_add_infohash(s, mbt_torrent_infohash(t), NULL, &again) == 0);
	assert(again == t);
	assert(mbt_session_torrent_count(s) == 1);
	assert(mbt_session_torrent_tracker_count(s, t) == 1);
	assert(mbt_session_add_torrent_buf(s, enc, enc_len, NULL, NULL) == 0);
	assert(mbt_session_remove_torrent(s, mbt_torrent_infohash(t)) == 0);
}

/* Metainfo after a magnet adds only trackers the magnet did not have. */
static void
test_magnet_then_metainfo(struct mbt_session *s, const unsigned char *enc, size_t enc_len)
{
	struct mbt_metainfo *mi;
	struct mbt_torrent *t = NULL, *again = NULL;
	char hex[41], uri[256];

	assert(mbt_metainfo_parse(&mi, enc, enc_len) == 0);
	mbt_infohash_string(mbt_metainfo_infohash(mi), hex, sizeof(hex));
	mbt_metainfo_free(mi);
	snprintf(uri, sizeof(uri), "magnet:?xt=urn:btih:%s&tr=%s", hex, TRACKER);

	assert(mbt_session_add_magnet(s, uri, NULL, &t) == 0);
	assert(t && !mbt_torrent_has_metadata(t));
	assert(mbt_session_torrent_tracker_count(s, t) == 1);
	assert(mbt_session_add_magnet(s, uri, NULL, &again) == 0);
	assert(again == t);
	assert(mbt_session_torrent_tracker_count(s, t) == 1);

	assert(mbt_session_add_torrent_buf(s, enc, enc_len, NULL, &again) == 0);
	assert(again == t);
	assert(mbt_torrent_has_metadata(t));
	assert(mbt_session_torrent_tracker_count(s, t) == 1);
	assert(mbt_session_remove_torrent(s, mbt_torrent_infohash(t)) == 0);
}

static void
test_read_piece(struct mbt_session *s, struct mbt_ev *ev, const unsigned char *enc, size_t enc_len)
{
	struct mbt_torrent *t = NULL;
	uint8_t buf[FILE_LEN], zero[FILE_LEN] = {0};

	assert(mbt_session_add_torrent_buf(s, enc, enc_len, NULL, &t) == 0);
	assert(mbt_torrent_piece_size(t, 0) == FILE_LEN);
	assert(mbt_torrent_piece_size(t, 1) == 0);
	test_wait_verified(ev, t);
	memset(buf, 0xff, sizeof(buf));
	assert(mbt_torrent_read_piece(t, 0, buf, sizeof(buf)) == 0);
	assert(memcmp(buf, zero, FILE_LEN) == 0);
	assert(mbt_torrent_read_piece(t, 1, buf, sizeof(buf)) == -ERANGE);
	assert(mbt_torrent_read_piece(t, 0, buf, FILE_LEN - 1) == -EMSGSIZE);
	assert(mbt_session_remove_torrent(s, mbt_torrent_infohash(t)) == 0);
}

static void
test_file_limits(struct mbt_session *s)
{
	struct mbt_torrent *t = NULL;
	unsigned char *enc;
	size_t enc_len;
	char path[256];
	FILE *fp;

	path_of(path, sizeof(path), "none.torrent");
	assert(mbt_session_add_torrent_file(s, path, NULL, &t) == -ENOENT);
	assert(t == NULL);

	path_of(path, sizeof(path), "empty.torrent");
	fp = fopen(path, "wb");
	assert(fp);
	fclose(fp);
	assert(mbt_session_add_torrent_file(s, path, NULL, &t) == -EIO);

	/* The parser ignores bytes after the top-level dictionary. */
	encode(&enc, &enc_len);
	path_of(path, sizeof(path), "big.torrent");
	fp = fopen(path, "wb");
	assert(fp);
	assert(fwrite(enc, 1, enc_len, fp) == enc_len);
	assert(ftruncate(fileno(fp), BIG_TAIL) == 0);
	fclose(fp);
	free(enc);
	assert(mbt_session_add_torrent_file(s, path, NULL, &t) == 0);
	assert(t && mbt_torrent_has_metadata(t));
	assert(mbt_session_remove_torrent(s, mbt_torrent_infohash(t)) == 0);

	path_of(path, sizeof(path), "huge.torrent");
	fp = fopen(path, "wb");
	assert(fp);
	assert(ftruncate(fileno(fp), OVER_LIMIT) == 0);
	fclose(fp);
	assert(mbt_session_add_torrent_file(s, path, NULL, NULL) == -EFBIG);
}

int
main(void)
{
	struct mbt_ev *ev;
	struct mbt_session *s;
	unsigned char *enc;
	size_t enc_len;
	char cmd[300];

	assert(mkdtemp(tmpdir));
	assert(mbt_ev_new_default(&ev) == 0);
	struct mbt_storage *st = mbt_storage_file_new(tmpdir);
	assert(st);
	struct mbt_session_config cfg = {.ev = ev, .default_storage = st};
	assert(mbt_session_new(&s, &cfg) == 0);
	mbt_storage_unref(st);

	encode(&enc, &enc_len);
	test_repeat(s, enc, enc_len);
	test_magnet_then_metainfo(s, enc, enc_len);
	test_read_piece(s, ev, enc, enc_len);
	test_file_limits(s);
	test_fallback_private(s);
	free(enc);

	mbt_session_free(s);
	mbt_ev_destroy(ev);
	snprintf(cmd, sizeof(cmd), "rm -rf %s", tmpdir);
	assert(system(cmd) == 0);
	fprintf(stderr, "session_add_test: PASS\n");
	return 0;
}
