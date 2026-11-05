// SPDX-License-Identifier: ISC
/* BEP 47: padding files transfer as zeros and never reach the disk. */
#include <assert.h>
#include <sha1.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>
#include <time.h>
#include <unistd.h>

#include "mbt/be.h"
#include "mbt/event.h"
#include "mbt/session.h"
#include "mbt/storage.h"
#include "mbt/torrent.h"
#include "test_util.h"

#define PIECE_LEN 16384
#define A_LEN 20000
#define PAD_LEN (PIECE_LEN * 2 - A_LEN)
#define B_LEN 10000
#define TOTAL_LEN (A_LEN + PAD_LEN + B_LEN)
#define NUM_PIECES 3
#define WAIT_SEC 20

static uint8_t stream[TOTAL_LEN];
static char tmpdir[] = "/tmp/mbt_pad_XXXXXX";

static void
add_file(struct mbt_be_value *files, const char *dir, const char *name, int len, int pad)
{
	struct mbt_be_value *file, *path, *c;

	assert(mbt_be_dict_new(&file) == 0);
	assert(mbt_be_list_new(&path) == 0);
	if (dir) {
		assert(mbt_be_string_new(&c, dir, strlen(dir)) == 0);
		mbt_be_list_append(path, c);
		mbt_be_unref(c);
	}
	assert(mbt_be_string_new(&c, name, strlen(name)) == 0);
	mbt_be_list_append(path, c);
	mbt_be_unref(c);
	assert(mbt_be_dict_append_number(file, "length", len) == 0);
	assert(mbt_be_dict_append(file, "path", path) == 0);
	if (pad)
		assert(mbt_be_dict_append_string(file, "attr", "p", 1) == 0);
	mbt_be_list_append(files, file);
	mbt_be_unref(path);
	mbt_be_unref(file);
}

static void
make_torrent(unsigned char **enc, size_t *enc_len)
{
	uint8_t hashes[NUM_PIECES * 20];
	struct mbt_be_value *root, *info, *files;

	for (int i = 0; i < NUM_PIECES; i++) {
		size_t off = (size_t)i * PIECE_LEN;
		size_t n = TOTAL_LEN - off < PIECE_LEN ? TOTAL_LEN - off : PIECE_LEN;
		SHA1_CTX ctx;

		SHA1Init(&ctx);
		SHA1Update(&ctx, stream + off, n);
		SHA1Final(hashes + i * 20, &ctx);
	}
	assert(mbt_be_dict_new(&info) == 0);
	assert(mbt_be_dict_append_string(info, "name", "tor", 3) == 0);
	assert(mbt_be_dict_append_number(info, "piece length", PIECE_LEN) == 0);
	assert(mbt_be_dict_append_string(info, "pieces", hashes, sizeof(hashes)) == 0);
	assert(mbt_be_list_new(&files) == 0);
	add_file(files, NULL, "a.bin", A_LEN, 0);
	add_file(files, ".pad", "12768", PAD_LEN, 1);
	add_file(files, NULL, "b.bin", B_LEN, 0);
	assert(mbt_be_dict_append(info, "files", files) == 0);
	mbt_be_unref(files);
	assert(mbt_be_dict_new(&root) == 0);
	assert(mbt_be_dict_append(root, "info", info) == 0);
	mbt_be_unref(info);
	assert(mbt_be_value_encode(root, enc, enc_len) == 0);
	mbt_be_unref(root);
}

static struct mbt_session *
session_on(struct mbt_ev *ev, const char *sub)
{
	char p[256];

	snprintf(p, sizeof(p), "%s/%s", tmpdir, sub);
	assert(mkdir(p, 0755) == 0);
	struct mbt_storage *st = mbt_storage_file_new(p);
	assert(st);
	struct mbt_session_config cfg = {.ev = ev, .default_storage = st};
	struct mbt_session *s;

	assert(mbt_session_new(&s, &cfg) == 0);
	mbt_storage_unref(st);
	return s;
}

static void
write_seed(const char *name, const uint8_t *data, size_t len)
{
	char p[256];
	FILE *fp;

	snprintf(p, sizeof(p), "%s/seed/tor", tmpdir);
	mkdir(p, 0755);
	snprintf(p, sizeof(p), "%s/seed/tor/%s", tmpdir, name);
	fp = fopen(p, "wb");
	assert(fp);
	assert(fwrite(data, 1, len, fp) == len);
	fclose(fp);
}

static void
check_leech(const char *name, const uint8_t *want, size_t len)
{
	static uint8_t got[TOTAL_LEN];
	char p[256];
	struct stat sb;
	FILE *fp;

	snprintf(p, sizeof(p), "%s/leech/tor/%s", tmpdir, name);
	assert(stat(p, &sb) == 0);
	assert((size_t)sb.st_size == len);
	fp = fopen(p, "rb");
	assert(fp);
	assert(fread(got, 1, len, fp) == len);
	fclose(fp);
	assert(memcmp(got, want, len) == 0);
}

int
main(void)
{
	struct mbt_ev *ev;
	unsigned char *enc;
	size_t enc_len;
	char p[300];
	struct stat sb;

	for (int i = 0; i < TOTAL_LEN; i++)
		stream[i] = (uint8_t)(i * 13 + 5);
	memset(stream + A_LEN, 0, PAD_LEN);
	make_torrent(&enc, &enc_len);

	assert(mkdtemp(tmpdir));
	assert(mbt_ev_new_default(&ev) == 0);
	struct mbt_session *seed = session_on(ev, "seed");
	struct mbt_session *leech = session_on(ev, "leech");
	write_seed("a.bin", stream, A_LEN);
	write_seed("b.bin", stream + A_LEN + PAD_LEN, B_LEN);

	assert(mbt_session_add_torrent_buf(seed, enc, enc_len, NULL, NULL) == 0);
	struct mbt_torrent *st = mbt_session_torrent_first(seed);
	assert(st);
	test_wait_verified(ev, st);
	assert(mbt_torrent_complete(st));
	assert(mbt_torrent_file_count(st) == 3);
	assert(mbt_torrent_file(st, 1)->pad);

	assert(mbt_session_add_torrent_buf(leech, enc, enc_len, NULL, NULL) == 0);
	struct mbt_torrent *lt = mbt_session_torrent_first(leech);
	assert(lt);
	test_wait_verified(ev, lt);
	assert(mbt_session_add_peer(leech, mbt_torrent_infohash(lt), "127.0.0.1",
	    mbt_session_listen_port(seed), MBT_SOURCE_MANUAL, 0) == 0);

	time_t end = time(NULL) + WAIT_SEC;
	while (!mbt_torrent_complete(lt)) {
		assert(time(NULL) < end);
		test_ev_step(ev, 10);
	}

	check_leech("a.bin", stream, A_LEN);
	check_leech("b.bin", stream + A_LEN + PAD_LEN, B_LEN);
	snprintf(p, sizeof(p), "%s/leech/tor/.pad", tmpdir);
	assert(stat(p, &sb) < 0);

	mbt_session_free(leech);
	mbt_session_free(seed);
	mbt_ev_destroy(ev);
	free(enc);
	snprintf(p, sizeof(p), "rm -rf %s", tmpdir);
	assert(system(p) == 0);
	fprintf(stderr, "storage_pad_test: PASS\n");
	return 0;
}
