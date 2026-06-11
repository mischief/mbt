// SPDX-License-Identifier: ISC
/*
 * test/bitfield_test.c — verify BITFIELD + HAVE exchange between peers
 *
 * Setup: 8 pieces. Session 1 has pieces 0-3 (50%). Session 2 has pieces 0-5 (75%).
 * After connecting, session 2 should see session 1 at 50% via BITFIELD.
 * Session 1 downloads pieces 4-5 from session 2, sends HAVE for each.
 * Session 2 should then see session 1 at 75%.
 * Session 1 reaches 75% (pieces 0-5), matching session 2.
 */
#include <assert.h>
#include <stdio.h>
#include <string.h>
#include <stdlib.h>
#include <signal.h>

#include "mbt/sha1_compat.h"
#include "mbt/event.h"
#include "mbt/session.h"
#include "mbt/torrent.h"
#include "mbt/peer.h"
#include "mbt/storage.h"
#include "mbt/metainfo.h"
#include "mbt/be.h"
#include "mbt/hex.h"
#include "mbt/bitmap.h"
#include "mbt/log.h"
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
}

static void
on_timeout(struct mbt_ev_timer *timer, void *arg)
{
	(void)timer; (void)arg;
	mbt_ev_stop(ev);
}

/* Stop shortly after s1 gets its pieces from s2 */
static struct mbt_torrent *check_torrent;
static void
on_check(struct mbt_ev_timer *timer, void *arg)
{
	(void)arg;
	if (check_torrent && mbt_torrent_have_count(check_torrent) >= 6)
		mbt_ev_stop(ev);
	else
		mbt_ev_timer_mod(timer, 100000);
}

int main(void)
{
	signal(SIGPIPE, SIG_IGN);
	int rv;

	mbt_ev_new_default(&ev);

	/* Generate test data */
	uint8_t data[TOTAL_LEN];
	for (int i = 0; i < NUM_PIECES; i++)
		memset(data + i * PIECE_LEN, 'A' + i, PIECE_LEN);

	/* Build .torrent (all 8 pieces) */
	uint8_t hashes[NUM_PIECES * 20];
	for (int i = 0; i < NUM_PIECES; i++) {
		SHA1_CTX ctx;
		SHA1Init(&ctx);
		SHA1Update(&ctx, data + i * PIECE_LEN, PIECE_LEN);
		SHA1Final(hashes + i * 20, &ctx);
	}

	/* Encode minimal torrent */
	struct mbt_be_value *info = NULL, *root = NULL;
	mbt_be_dict_new(&info);
	mbt_be_dict_append_string(info, "name", "bitfield_test.bin", 17);
	mbt_be_dict_append_number(info, "piece length", PIECE_LEN);
	mbt_be_dict_append_number(info, "length", TOTAL_LEN);
	mbt_be_dict_append_string(info, "pieces", hashes, NUM_PIECES * 20);

	mbt_be_dict_new(&root);
	mbt_be_dict_append(root, "info", info);

	uint8_t *enc; size_t enc_len;
	mbt_be_value_encode(root, &enc, &enc_len);
	mbt_be_value_free(root);

	/* Session 2: has pieces 0-5 (75%), must start first (listener) */
	struct mbt_storage_memory *mem2 = mbt_storage_memory_new();
	mbt_storage_memory_put_file(mem2, 0, data, 6 * PIECE_LEN);

	struct mbt_session_config cfg2 = {
		.ev = ev, .storage_ops = mbt_storage_memory_ops(),
		.storage_ctx = mem2, .listen_port = 0,
	};
	struct mbt_session *s2;
	rv = mbt_session_new(&s2, &cfg2); assert(rv == 0);
	rv = mbt_session_add_torrent_buf(s2, enc, enc_len); assert(rv == 0);

	struct mbt_torrent *t2 = mbt_session_torrent_first(s2);
	assert(t2);
	assert(mbt_torrent_have_count(t2) == 6); /* 75% */

	/* Session 1: starts with magnet (no metadata), has 4 pieces on disk */
	struct mbt_storage_memory *mem1 = mbt_storage_memory_new();
	mbt_storage_memory_put_file(mem1, 0, data, 4 * PIECE_LEN);

	struct mbt_session_config cfg1 = {
		.ev = ev, .storage_ops = mbt_storage_memory_ops(),
		.storage_ctx = mem1, .listen_port = 0,
		.max_pieces_per_peer = 4,
	};
	struct mbt_session *s1;
	rv = mbt_session_new(&s1, &cfg1); assert(rv == 0);

	/* Add as magnet pointing at s2, using same infohash */
	char ih_hex[41];
	struct mbt_infohash *ih2 = mbt_torrent_infohash(t2);
	mbt_hex_encode(ih2->hash_sha1, 20, ih_hex, sizeof(ih_hex));
	char magnet[256];
	snprintf(magnet, sizeof(magnet),
	         "magnet:?xt=urn:btih:%s&x.pe=127.0.0.1:%hu", ih_hex, mbt_session_listen_port(s2));
	rv = mbt_session_add_magnet(s1, magnet); assert(rv == 0);

	struct mbt_torrent *t1 = mbt_session_torrent_first(s1);
	assert(t1);
	assert(mbt_torrent_have_count(t1) == 0);
	check_torrent = t1;
	assert(mbt_torrent_have_count(t2) == 6); /* 75% */

	/* Session 1 wants to complete — set callbacks */
	struct mbt_torrent_callbacks cb = { .on_complete = on_complete };
	mbt_torrent_set_callbacks(t1, &cb, NULL);

	/* s1 connects to s2 via x.pe in magnet */

	/* Run with timeout */
	struct mbt_ev_timer *to;
	mbt_ev_timer_new(ev, &to, 5 * 1000000ULL, on_timeout, NULL);
	struct mbt_ev_timer *check;
	mbt_ev_timer_new(ev, &check, 100000, on_check, NULL);
	mbt_ev_run(ev);

	/* Verify: s1 downloaded pieces 4-5 from s2 */
	fprintf(stderr, "s1 have: %u/%u\n", mbt_torrent_have_count(t1), (uint32_t)NUM_PIECES);
	assert(mbt_torrent_have_count(t1) == 6); /* got pieces 4,5 from s2 */

	/* Verify: s2 sees s1 at 75% (4 original + 2 downloaded = 6 pieces) */
	int s2_peers = mbt_torrent_peer_count(t2);
	fprintf(stderr, "s2 peers: %d\n", s2_peers);
	assert(s2_peers >= 1);

	struct mbt_peer_stats ps;
	rv = mbt_torrent_peer_stats(t2, 0, &ps);
	assert(rv == 0);
	fprintf(stderr, "s2 sees s1: done_pct=%d%%\n", ps.done_pct);
	assert(ps.done_pct == 75); /* 6/8 = 75% */

	fprintf(stderr, "bitfield_test: PASS\n");

	free(enc);
	mbt_ev_timer_free(to);
	mbt_ev_timer_free(check);
	mbt_session_free(s1);
	mbt_session_free(s2);
	mbt_ev_destroy(ev);
	mbt_storage_memory_destroy(mem1);
	mbt_storage_memory_destroy(mem2);
	return 0;
}
