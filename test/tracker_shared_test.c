// SPDX-License-Identifier: ISC
/*
 * test/tracker_shared_test.c — verify UDP + HTTP share a peer store
 *
 * Creates a shared store, attaches both UDP and HTTP servers.
 * Seeder announces via UDP tracker → leecher discovers via UDP.
 * Verifies the shared store reflects both.
 */
#include <assert.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "mbt/sha1_compat.h"

#include "mbt/event.h"
#include "mbt/session.h"
#include "mbt/torrent.h"
#include "mbt/storage.h"
#include "mbt/tracker_server.h"
#include "mbt/be.h"
#include "mbt/hex.h"

#define PIECE_LEN 16384
#define NUM_PIECES 4
#define TOTAL_LEN (PIECE_LEN * NUM_PIECES)

static struct mbt_ev *ev;
static int download_complete;

static void on_complete(struct mbt_torrent *t, void *arg)
{ (void)t; (void)arg; download_complete = 1; mbt_ev_stop(ev); }

static void timeout_cb(struct mbt_ev_timer *timer, void *arg)
{ (void)timer; (void)arg; mbt_ev_stop(ev); }

int main(void)
{
	int rv;
	uint8_t test_data[TOTAL_LEN];
	for (int i = 0; i < TOTAL_LEN; i++) test_data[i] = (i * 37 + 1) & 0xff;

	uint8_t pieces_hash[NUM_PIECES * 20];
	for (int i = 0; i < NUM_PIECES; i++) {
		SHA1_CTX ctx; SHA1Init(&ctx);
		SHA1Update(&ctx, test_data + i * PIECE_LEN, PIECE_LEN);
		SHA1Final(pieces_hash + i * 20, &ctx);
	}

	_cleanup_be_ struct mbt_be_value *root = NULL, *info = NULL;
	mbt_be_dict_new(&root);
	mbt_be_dict_new(&info);
	mbt_be_dict_append(root, "info", info);
	mbt_be_dict_append_number(info, "length", TOTAL_LEN);
	mbt_be_dict_append_string(info, "name", "shared_test.bin", 15);
	mbt_be_dict_append_number(info, "piece length", PIECE_LEN);
	mbt_be_dict_append_string(info, "pieces", pieces_hash, sizeof(pieces_hash));

	mbt_ev_new_default(&ev);

	/* Create shared store */
	struct mbt_tracker_store *store;
	rv = mbt_tracker_store_new(&store); assert(rv == 0);

	/* UDP tracker using shared store */
	struct mbt_tracker_server *udp;
	rv = mbt_tracker_server_new(&udp, ev, 0, store); assert(rv == 0);
	uint16_t udp_port = mbt_tracker_server_port(udp);

	/* HTTP tracker using same shared store */
	struct mbt_tracker_http *http;
	rv = mbt_tracker_http_new(&http, ev, 0, store); assert(rv == 0);

	/* Now encode the torrent with the actual tracker port */
	char announce[64];
	snprintf(announce, sizeof(announce), "udp://127.0.0.1:%hu/announce", udp_port);
	mbt_be_dict_append_string(root, "announce", announce, strlen(announce));

	unsigned char *encoded; size_t enc_len;
	assert(mbt_be_value_encode(root, &encoded, &enc_len) == 0);

	/* Seeder announces via UDP */
	struct mbt_storage_memory *mem1 = mbt_storage_memory_new();
	mbt_storage_memory_put_file(mem1, 0, test_data, TOTAL_LEN);
	struct mbt_session_config cfg1 = {
		.ev = ev, .storage_ops = mbt_storage_memory_ops(),
		.storage_ctx = mem1, .listen_port = 0,
	};
	struct mbt_session *s1;
	rv = mbt_session_new(&s1, &cfg1); assert(rv == 0);
	rv = mbt_session_add_torrent_buf(s1, encoded, enc_len); assert(rv == 0);

	/* Leecher also announces via UDP (same tracker) */
	struct mbt_storage_memory *mem2 = mbt_storage_memory_new();
	struct mbt_session_config cfg2 = {
		.ev = ev, .storage_ops = mbt_storage_memory_ops(),
		.storage_ctx = mem2, .listen_port = 0,
	};
	struct mbt_session *s2;
	rv = mbt_session_new(&s2, &cfg2); assert(rv == 0);
	rv = mbt_session_add_torrent_buf(s2, encoded, enc_len); assert(rv == 0);

	struct mbt_torrent *t2 = mbt_session_torrent_first(s2);
	struct mbt_torrent_callbacks cb = { .on_complete = on_complete };
	mbt_torrent_set_callbacks(t2, &cb, NULL);

	struct mbt_ev_timer *to;
	mbt_ev_timer_new(ev, &to, 10 * 1000000ULL, timeout_cb, NULL);

	rv = mbt_ev_run(ev);

	assert(download_complete == 1);

	/* Verify shared store has both peers */
	assert(mbt_tracker_store_num_torrents(store) == 1);
	assert(mbt_tracker_store_num_peers(store) >= 2);

	/* HTTP server sees same data (shared store) */
	assert(mbt_tracker_http_num_torrents(http) == 1);
	assert(mbt_tracker_http_num_peers(http) >= 2);

	fprintf(stderr, "tracker_shared_test: PASS (store: %d torrents, %d peers, shared across UDP+HTTP)\n",
	        mbt_tracker_store_num_torrents(store), mbt_tracker_store_num_peers(store));

	free(encoded);
	mbt_ev_timer_free(to);
	mbt_session_free(s1);
	mbt_session_free(s2);
	mbt_tracker_server_free(udp);
	mbt_tracker_http_free(http);
	mbt_tracker_store_free(store);
	mbt_ev_destroy(ev);
	mbt_storage_memory_destroy(mem1);
	mbt_storage_memory_destroy(mem2);
	return 0;
}
