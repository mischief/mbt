// SPDX-License-Identifier: ISC
/*
 * test/resume_test.c — verify resume path with memory storage
 */
#include <stdio.h>
#include <stdlib.h>
#include <assert.h>
#include <sha1.h>
#include <stdint.h>

#include "mbt/event.h"
#include "mbt/session.h"
#include "mbt/torrent.h"
#include "mbt/storage.h"
#include "mbt/be.h"
#include "test_util.h"

#define PIECE_LEN 16384
#define NUM_PIECES 4
#define TOTAL_LEN (PIECE_LEN * NUM_PIECES)

int
main(void)
{
	int rv;

	/* 1. Build .torrent in memory */
	uint8_t zero_piece[PIECE_LEN] = {0};
	uint8_t pieces_hash[NUM_PIECES * 20];
	for (int i = 0; i < NUM_PIECES; i++) {
		SHA1_CTX ctx;
		SHA1Init(&ctx);
		SHA1Update(&ctx, zero_piece, PIECE_LEN);
		SHA1Final(pieces_hash + i * 20, &ctx);
	}

	_cleanup_be_ struct mbt_be_value *root = NULL, *info = NULL;
	mbt_be_dict_new(&root);
	mbt_be_dict_new(&info);
	mbt_be_dict_append(root, "info", info);
	mbt_be_dict_append_number(info, "length", TOTAL_LEN);
	mbt_be_dict_append_string(info, "name", "test.bin", 8);
	mbt_be_dict_append_number(info, "piece length", PIECE_LEN);
	mbt_be_dict_append_string(info, "pieces", pieces_hash, sizeof(pieces_hash));

	unsigned char *encoded;
	size_t enc_len;
	assert(mbt_be_value_encode(root, &encoded, &enc_len) == 0);

	/* 2. Create memory storage pre-filled with zeros */
	struct mbt_storage_memory *mem = mbt_storage_memory_new();

	/* 3. Create session */
	struct mbt_ev *ev;
	mbt_ev_new_default(&ev);

	struct mbt_session_config cfg = {
		.ev = ev,
		.default_storage = mbt_storage_memory_storage(mem),
		.listen_port = 0,
	};
	struct mbt_session *session;
	rv = mbt_session_new(&session, &cfg);
	assert(rv == 0);

	/* 4. Add torrent from buffer */
	rv = mbt_session_add_torrent_buf(session, encoded, enc_len, NULL);
	assert(rv == 0);
	free(encoded);

	/* 5. Check resume verified all pieces */
	struct mbt_torrent *t = mbt_session_torrent_first(session);
	assert(t != NULL);
	test_wait_verified(ev, t);
	assert(mbt_torrent_complete(t) == 1);
	assert(mbt_torrent_have_count(t) == NUM_PIECES);

	fprintf(stderr, "resume_test: PASS (4 pieces verified from memory storage)\n");

	mbt_session_free(session);
	mbt_ev_destroy(ev);
	mbt_storage_memory_destroy(mem);
	return 0;
}
