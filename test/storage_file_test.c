// SPDX-License-Identifier: ISC
/*
 * test/storage_file_test.c — exercise mbt_storage_file through the ops interface
 */
#include <assert.h>
#include <string.h>
#include <stdlib.h>
#include <unistd.h>
#include <stdio.h>
#include <sha1.h>
#include <stdint.h>

#include "mbt/storage.h"
#include "mbt/be.h"
#include "mbt/event.h"
#include "mbt/session.h"

#define PIECE_LEN 64
#define NUM_PIECES 4
#define TOTAL_LEN (PIECE_LEN * NUM_PIECES)

/* Build a .torrent, create session with file storage, write+read pieces */
int main(void)
{
	int rv;
	char tmpdir[] = "/tmp/mbt_sf_XXXXXX";
	assert(mkdtemp(tmpdir));

	/* Build fake torrent metadata */
	uint8_t data[TOTAL_LEN];
	for (int i = 0; i < TOTAL_LEN; i++) data[i] = i & 0xff;

	uint8_t pieces_hash[NUM_PIECES * 20];
	for (int i = 0; i < NUM_PIECES; i++) {
		SHA1_CTX ctx;
		SHA1Init(&ctx);
		SHA1Update(&ctx, data + i * PIECE_LEN, PIECE_LEN);
		SHA1Final(pieces_hash + i * 20, &ctx);
	}

	_cleanup_be_ struct mbt_be_value *root = NULL, *info = NULL;
	mbt_be_dict_new(&root);
	mbt_be_dict_new(&info);
	mbt_be_dict_append(root, "info", info);
	mbt_be_dict_append_number(info, "length", TOTAL_LEN);
	mbt_be_dict_append_string(info, "name", "sf_test.bin", 11);
	mbt_be_dict_append_number(info, "piece length", PIECE_LEN);
	mbt_be_dict_append_string(info, "pieces", pieces_hash, sizeof(pieces_hash));

	unsigned char *encoded;
	size_t enc_len;
	assert(mbt_be_value_encode(root, &encoded, &enc_len) == 0);

	/* Write .torrent file */
	char tpath[256];
	snprintf(tpath, sizeof(tpath), "%s/test.torrent", tmpdir);
	FILE *fp = fopen(tpath, "wb");
	fwrite(encoded, 1, enc_len, fp);
	fclose(fp);
	free(encoded);

	/* Create session with file storage */
	struct mbt_ev *ev;
#ifdef HAVE_SD_EVENT
	rv = mbt_ev_sd_new(&ev);
#elif defined(HAVE_LIBEVENT)
	rv = mbt_ev_libevent_new(&ev);
#endif
	assert(rv == 0);

	struct mbt_session_config cfg = {
		.ev = ev,
		.default_storage = mbt_storage_file_new(tmpdir),
		.listen_port = 0,
	};
	struct mbt_session *session;
	rv = mbt_session_new(&session, &cfg);
	assert(rv == 0);

	rv = mbt_session_add_torrent_file(session, tpath, NULL);
	assert(rv == 0);

	struct mbt_torrent *t = mbt_session_torrent_first(session);
	assert(t);

	/* Write pieces through ops (must open first to populate file table) */
	struct mbt_storage stor = mbt_storage_file_new(tmpdir);
	const struct mbt_storage_ops *ops = stor.ops;
	void *sctx = stor.ctx;
	rv = ops->open(sctx, t);
	assert(rv == 0);
	for (int i = 0; i < NUM_PIECES; i++) {
		rv = ops->write_piece(sctx, i, data + i * PIECE_LEN, PIECE_LEN);
		assert(rv == 0);
	}

	/* Read back and verify */
	uint8_t buf[PIECE_LEN];
	for (int i = 0; i < NUM_PIECES; i++) {
		rv = ops->read_piece(sctx, i, buf, PIECE_LEN);
		assert(rv == 0);
		assert(memcmp(buf, data + i * PIECE_LEN, PIECE_LEN) == 0);
	}
	ops->close(sctx);
	mbt_storage_file_free(&stor);

	fprintf(stderr, "storage_file_test: PASS\n");

	/* Cleanup */
	mbt_session_free(session);
	mbt_ev_destroy(ev);

	char fpath[256];
	snprintf(fpath, sizeof(fpath), "%s/sf_test.bin", tmpdir);
	unlink(fpath);
	unlink(tpath);
	rmdir(tmpdir);
	return 0;
}
