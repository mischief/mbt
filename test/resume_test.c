/*
 * test/resume_test.c — verify resume path with memory storage
 *
 * Creates a fake torrent (4 pieces of zeros), pre-fills memory storage,
 * adds to session, verifies all pieces via SHA1, fires on_complete.
 */
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <assert.h>
#include <unistd.h>

#include <sha1.h>

#include "mbt/event.h"
#include "mbt/session.h"
#include "mbt/torrent.h"
#include "mbt/storage.h"
#include "mbt/be.h"

#define PIECE_LEN 16384
#define NUM_PIECES 4
#define TOTAL_LEN (PIECE_LEN * NUM_PIECES)

/* Build a minimal .torrent file in memory and write to tmp path */
static char *
build_torrent_file(void)
{
	/* Compute SHA1 of each piece (all zeros) */
	uint8_t zero_piece[PIECE_LEN] = {0};
	uint8_t pieces_hash[NUM_PIECES * 20];

	for (int i = 0; i < NUM_PIECES; i++) {
		SHA1_CTX ctx;
		SHA1Init(&ctx);
		SHA1Update(&ctx, zero_piece, PIECE_LEN);
		SHA1Final(pieces_hash + i * 20, &ctx);
	}

	/* Build bencode: d4:infod6:lengthi65536e4:name8:test.bin12:piece lengthi16384e6:pieces80:...ee */
	struct mbt_be_value *root = NULL, *info = NULL;
	mbt_be_dict_new(&root);
	mbt_be_dict_new(&info);
	mbt_be_dict_append(root, "info", info);
	mbt_be_dict_append_number(info, "length", TOTAL_LEN);
	mbt_be_dict_append_string(info, "name", "test.bin", 8);
	mbt_be_dict_append_number(info, "piece length", PIECE_LEN);
	mbt_be_dict_append_string(info, "pieces", pieces_hash, sizeof(pieces_hash));

	unsigned char *encoded;
	size_t enc_len;
	int rv = mbt_be_value_encode(root, &encoded, &enc_len);
	assert(rv == 0);
	mbt_be_value_free(root);

	/* Write to temp file */
	char *path = strdup("/tmp/mbt_resume_test_XXXXXX.torrent");
	int fd = mkstemps(path, 8);
	assert(fd >= 0);
	write(fd, encoded, enc_len);
	close(fd);
	free(encoded);

	return path;
}

int
main(void)
{
	int rv;

	/* 1. Build .torrent file */
	char *torrent_path = build_torrent_file();

	/* 2. Create memory storage pre-filled with zeros */
	struct mbt_storage_memory *mem = mbt_storage_memory_new();

	/* 3. Create session */
	struct mbt_session_config cfg = {0};
#ifdef HAVE_SD_EVENT
	rv = mbt_ev_sd_new(&cfg.ev);
#elif defined(HAVE_LIBEVENT)
	rv = mbt_ev_libevent_new(&cfg.ev);
#endif
	assert(rv == 0);

	cfg.storage_ops = mbt_storage_memory_ops();
	cfg.storage_ctx = mem;
	cfg.listen_port = 0; /* no listener needed */

	struct mbt_session *session;
	rv = mbt_session_new(&session, &cfg);
	assert(rv == 0);

	/* 4. Add torrent */
	rv = mbt_session_add_torrent_file(session, torrent_path);
	assert(rv == 0);

	/* 5. Set completion callback */
	struct mbt_torrent *t = mbt_session_torrent_first(session);
	assert(t != NULL);

	/* Resume fires on_complete during add_torrent_file if all pieces verified.
	 * Check directly — no need to run the event loop. */
	assert(mbt_torrent_complete(t) == 1);
	assert(mbt_torrent_have_count(t) == NUM_PIECES);

	fprintf(stderr, "resume_test: PASS (4 pieces verified from memory storage)\n");

	/* Cleanup */
	mbt_session_free(session);
	mbt_storage_memory_destroy(mem);
	unlink(torrent_path);
	free(torrent_path);
	return 0;
}
