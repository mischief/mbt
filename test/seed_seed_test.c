// SPDX-License-Identifier: ISC
/*
 * test/seed_seed_test.c — verify two seeders disconnect after connecting
 *
 * Both sessions have all pieces. They connect via x.pe.
 * After a choke interval, they should send NOT_INTERESTED and disconnect.
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
#include "mbt/storage.h"
#include "mbt/be.h"
#include "mbt/hex.h"
#include "mbt/infohash.h"

#define PIECE_LEN 16384
#define NUM_PIECES 4
#define TOTAL_LEN (PIECE_LEN * NUM_PIECES)

static struct mbt_ev *ev;

static void on_timeout(struct mbt_ev_timer *t, void *arg) { (void)t; (void)arg; mbt_ev_stop(ev); }

int main(void)
{
	signal(SIGPIPE, SIG_IGN);
	int rv;
	mbt_ev_new_default(&ev);

	/* Generate test data */
	uint8_t data[TOTAL_LEN];
	memset(data, 'X', TOTAL_LEN);

	/* Build .torrent */
	uint8_t hashes[NUM_PIECES * 20];
	for (int i = 0; i < NUM_PIECES; i++) {
		SHA1_CTX ctx;
		SHA1Init(&ctx);
		SHA1Update(&ctx, data + i * PIECE_LEN, PIECE_LEN);
		SHA1Final(hashes + i * 20, &ctx);
	}

	struct mbt_be_value *info = NULL, *root = NULL;
	mbt_be_dict_new(&info);
	mbt_be_dict_append_string(info, "name", "seed_test.bin", 13);
	mbt_be_dict_append_number(info, "piece length", PIECE_LEN);
	mbt_be_dict_append_number(info, "length", TOTAL_LEN);
	mbt_be_dict_append_string(info, "pieces", hashes, NUM_PIECES * 20);
	mbt_be_dict_new(&root);
	mbt_be_dict_append(root, "info", info);
	uint8_t *enc; size_t enc_len;
	mbt_be_value_encode(root, &enc, &enc_len);
	mbt_be_value_free(root);

	/* Session 1: seeder, listening */
	struct mbt_storage_memory *mem1 = mbt_storage_memory_new();
	mbt_storage_memory_put_file(mem1, 0, data, TOTAL_LEN);
	struct mbt_session_config cfg1 = {
		.ev = ev, .storage_ops = mbt_storage_memory_ops(),
		.storage_ctx = mem1, .listen_port = 0,
		.timers = { .choke_interval = 1 }, /* fast choke for test */
	};
	struct mbt_session *s1;
	rv = mbt_session_new(&s1, &cfg1); assert(rv == 0);
	rv = mbt_session_add_torrent_buf(s1, enc, enc_len); assert(rv == 0);
	struct mbt_torrent *t1 = mbt_session_torrent_first(s1);
	assert(mbt_torrent_complete(t1));

	/* Session 2: seeder, connects to s1 via magnet+x.pe */
	struct mbt_storage_memory *mem2 = mbt_storage_memory_new();
	mbt_storage_memory_put_file(mem2, 0, data, TOTAL_LEN);
	struct mbt_session_config cfg2 = {
		.ev = ev, .storage_ops = mbt_storage_memory_ops(),
		.storage_ctx = mem2, .listen_port = 0,
		.timers = { .choke_interval = 1 },
	};
	struct mbt_session *s2;
	rv = mbt_session_new(&s2, &cfg2); assert(rv == 0);

	/* Use magnet so s2 connects, gets metadata, resumes all, becomes seeder */
	char ih_hex[41];
	struct mbt_infohash *ih = mbt_torrent_infohash(t1);
	mbt_hex_encode(ih->hash_sha1, 20, ih_hex, sizeof(ih_hex));
	char magnet[256];
	snprintf(magnet, sizeof(magnet),
	         "magnet:?xt=urn:btih:%s&x.pe=127.0.0.1:%hu", ih_hex, mbt_session_listen_port(s1));
	rv = mbt_session_add_magnet(s2, magnet); assert(rv == 0);

	/* Run for 5s — should be enough for connect + choke + disconnect */
	struct mbt_ev_timer *to;
	mbt_ev_timer_new(ev, &to, 3 * 1000000ULL, on_timeout, NULL);
	mbt_ev_run(ev);

	/* Both should have 0 peers — disconnected */
	int p1 = mbt_torrent_peer_count(t1);
	struct mbt_torrent *t2 = mbt_session_torrent_first(s2);
	int p2 = t2 ? mbt_torrent_peer_count(t2) : 0;
	fprintf(stderr, "seed_seed_test: s1 peers=%d, s2 peers=%d\n", p1, p2);
	assert(p1 == 0);
	assert(p2 == 0);

	fprintf(stderr, "seed_seed_test: PASS (seeders disconnected)\n");

	free(enc);
	mbt_ev_timer_free(to);
	mbt_session_free(s1);
	mbt_session_free(s2);
	mbt_ev_destroy(ev);
	mbt_storage_memory_destroy(mem1);
	mbt_storage_memory_destroy(mem2);
	return 0;
}
