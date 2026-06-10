// SPDX-License-Identifier: ISC
/*
 * test/ipv6_test.c — verify peer-to-peer and tracker work over IPv6 (::1)
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
#include "mbt/tracker_server.h"
#include "mbt/dht.h"
#include "mbt/infohash.h"

#define PIECE_LEN 16384
#define NUM_PIECES 4
#define TOTAL_LEN (PIECE_LEN * NUM_PIECES)

static struct mbt_ev *ev;
static int complete;

static void on_complete(struct mbt_torrent *t, void *arg)
{ (void)t; (void)arg; complete = 1; mbt_ev_stop(ev); }

static void timeout_cb(struct mbt_ev_timer *t, void *a)
{ (void)t; (void)a; mbt_ev_stop(ev); }

static void
build_torrent(uint8_t *data, unsigned char **encoded, size_t *enc_len, char *hex)
{
	for (int i = 0; i < TOTAL_LEN; i++) data[i] = (i * 43 + 7) & 0xff;

	uint8_t pieces_hash[NUM_PIECES * 20];
	for (int i = 0; i < NUM_PIECES; i++) {
		SHA1_CTX ctx; SHA1Init(&ctx);
		SHA1Update(&ctx, data + i * PIECE_LEN, PIECE_LEN);
		SHA1Final(pieces_hash + i * 20, &ctx);
	}

	struct mbt_be_value *root = NULL, *info = NULL;
	mbt_be_dict_new(&root);
	mbt_be_dict_new(&info);
	mbt_be_dict_append(root, "info", info);
	mbt_be_dict_append_number(info, "length", TOTAL_LEN);
	mbt_be_dict_append_string(info, "name", "ipv6_test.bin", 13);
	mbt_be_dict_append_number(info, "piece length", PIECE_LEN);
	mbt_be_dict_append_string(info, "pieces", pieces_hash, sizeof(pieces_hash));

	mbt_be_value_encode(root, encoded, enc_len);
	mbt_be_value_free(root);

	unsigned char *is = memmem(*encoded, *enc_len, "4:infod", 7);
	if (!is) is = memmem(*encoded, *enc_len, "4:info", 6);
	is += 6;
	struct mbt_be_value *tmp = NULL;
	int ilen = mbt_be_value_parse(is, *enc_len - (is - *encoded), &tmp);
	mbt_be_value_free(tmp);
	SHA1_CTX ctx; uint8_t digest[20];
	SHA1Init(&ctx); SHA1Update(&ctx, is, ilen); SHA1Final(digest, &ctx);
	mbt_hex_encode(digest, 20, hex, 41);
}

static void
test_peer_to_peer_v6(void)
{
	uint8_t data[TOTAL_LEN];
	unsigned char *encoded; size_t enc_len; char hex[41];
	build_torrent(data, &encoded, &enc_len, hex);

	mbt_ev_new_default(&ev);

	/* Seeder on ::1 */
	struct mbt_storage_memory *memS = mbt_storage_memory_new();
	mbt_storage_memory_put_file(memS, 0, data, TOTAL_LEN);
	struct mbt_session_config cfgS = {
		.ev = ev, .storage_ops = mbt_storage_memory_ops(),
		.storage_ctx = memS, .listen_port = 19801,
	};
	struct mbt_session *sS;
	mbt_session_new(&sS, &cfgS);
	mbt_session_add_torrent_buf(sS, encoded, enc_len);

	/* Leecher connects via IPv6 */
	struct mbt_storage_memory *memL = mbt_storage_memory_new();
	struct mbt_session_config cfgL = {
		.ev = ev, .storage_ops = mbt_storage_memory_ops(),
		.storage_ctx = memL, .listen_port = 19802,
	};
	struct mbt_session *sL;
	mbt_session_new(&sL, &cfgL);

	char magnet[256];
	snprintf(magnet, sizeof(magnet), "magnet:?xt=urn:btih:%s&x.pe=[::1]:19801", hex);
	mbt_session_add_magnet(sL, magnet);

	struct mbt_torrent *tL = mbt_session_torrent_first(sL);
	struct mbt_torrent_callbacks cb = { .on_complete = on_complete };
	mbt_torrent_set_callbacks(tL, &cb, NULL);

	complete = 0;
	struct mbt_ev_timer *to;
	mbt_ev_timer_new(ev, &to, 10 * 1000000ULL, timeout_cb, NULL);
	mbt_ev_run(ev);

	assert(complete == 1);
	fprintf(stderr, "  peer-to-peer over IPv6: PASS\n");

	free(encoded);
	mbt_ev_timer_free(to);
	mbt_session_free(sS);
	mbt_session_free(sL);
	mbt_ev_destroy(ev);
	mbt_storage_memory_destroy(memS);
	mbt_storage_memory_destroy(memL);
}

static void
test_tracker_v6(void)
{
	uint8_t data[TOTAL_LEN];
	unsigned char *encoded; size_t enc_len; char hex[41];
	build_torrent(data, &encoded, &enc_len, hex);

	mbt_ev_new_default(&ev);

	/* Tracker on ::1 port 19899 */
	struct mbt_tracker_server *tracker;
	int rv = mbt_tracker_server_new(&tracker, ev, 19899, NULL);
	assert(rv == 0);

	/* Seeder */
	struct mbt_storage_memory *memS = mbt_storage_memory_new();
	mbt_storage_memory_put_file(memS, 0, data, TOTAL_LEN);

	/* Build .torrent with IPv6 tracker announce URL */
	struct mbt_be_value *root = NULL, *info = NULL;
	mbt_be_dict_new(&root);
	mbt_be_dict_new(&info);
	mbt_be_dict_append(root, "info", info);
	mbt_be_dict_append_number(info, "length", TOTAL_LEN);
	mbt_be_dict_append_string(info, "name", "ipv6_test.bin", 13);
	mbt_be_dict_append_number(info, "piece length", PIECE_LEN);

	uint8_t pieces_hash[NUM_PIECES * 20];
	for (int i = 0; i < NUM_PIECES; i++) {
		SHA1_CTX ctx; SHA1Init(&ctx);
		SHA1Update(&ctx, data + i * PIECE_LEN, PIECE_LEN);
		SHA1Final(pieces_hash + i * 20, &ctx);
	}
	mbt_be_dict_append_string(info, "pieces", pieces_hash, sizeof(pieces_hash));
	mbt_be_dict_append_string(root, "announce", "udp://[::1]:19899/announce", 26);

	unsigned char *enc2; size_t enc2_len;
	mbt_be_value_encode(root, &enc2, &enc2_len);
	mbt_be_value_free(root);

	struct mbt_session_config cfgS = {
		.ev = ev, .storage_ops = mbt_storage_memory_ops(),
		.storage_ctx = memS, .listen_port = 19803,
		.timers = { .tracker_interval = 5 },
	};
	struct mbt_session *sS;
	mbt_session_new(&sS, &cfgS);
	mbt_session_add_torrent_buf(sS, enc2, enc2_len);

	/* Leecher via tracker */
	struct mbt_storage_memory *memL = mbt_storage_memory_new();
	struct mbt_session_config cfgL = {
		.ev = ev, .storage_ops = mbt_storage_memory_ops(),
		.storage_ctx = memL, .listen_port = 19804,
		.timers = { .tracker_interval = 5 },
	};
	struct mbt_session *sL;
	mbt_session_new(&sL, &cfgL);

	char magnet[256];
	snprintf(magnet, sizeof(magnet),
	         "magnet:?xt=urn:btih:%s&tr=udp://[::1]:19899/announce", hex);
	mbt_session_add_magnet(sL, magnet);

	struct mbt_torrent *tL = mbt_session_torrent_first(sL);
	struct mbt_torrent_callbacks cb = { .on_complete = on_complete };
	mbt_torrent_set_callbacks(tL, &cb, NULL);

	complete = 0;
	struct mbt_ev_timer *to;
	mbt_ev_timer_new(ev, &to, 15 * 1000000ULL, timeout_cb, NULL);
	mbt_ev_run(ev);

	assert(complete == 1);
	fprintf(stderr, "  tracker over IPv6: PASS\n");

	free(encoded); free(enc2);
	mbt_ev_timer_free(to);
	mbt_tracker_server_free(tracker);
	mbt_session_free(sS);
	mbt_session_free(sL);
	mbt_ev_destroy(ev);
	mbt_storage_memory_destroy(memS);
	mbt_storage_memory_destroy(memL);
}

static void test_dht_v6(void);
int main(void)
{
	test_peer_to_peer_v6();
	test_tracker_v6();
	test_dht_v6();
	fprintf(stderr, "ipv6_test: PASS\n");
	return 0;
}

static struct mbt_dht *dht_a, *dht_b, *dht_c;
static int dht_found;

static void dht_on_peer(const char *addr, uint16_t port, void *arg)
{ (void)arg; fprintf(stderr, "  DHT found peer: %s:%hu\n", addr, port); dht_found = 1; }

static void dht_on_done(void *arg) { (void)arg; mbt_ev_stop(ev); }

static void dht_do_lookup(struct mbt_ev_timer *timer, void *arg)
{
	(void)timer; (void)arg;
	struct mbt_infohash *ih;
	mbt_infohash_new(&ih, MBT_INFOHASH_SHA1, "BBBBBBBBBBBBBBBBBBBBBBBBBBBBBBBBBBBBBBBB");
	mbt_dht_get_peers(dht_c, ih, dht_on_peer, dht_on_done, NULL);
	mbt_infohash_unref(ih);
}

static void dht_a_done(void *arg)
{
	(void)arg;
	/* After get_peers completes, A has tokens — now announce */
	struct mbt_infohash *ih;
	mbt_infohash_new(&ih, MBT_INFOHASH_SHA1, "BBBBBBBBBBBBBBBBBBBBBBBBBBBBBBBBBBBBBBBB");
	mbt_dht_announce(dht_a, ih, 7777);
	mbt_infohash_unref(ih);
}

static void dht_do_announce(struct mbt_ev_timer *timer, void *arg)
{
	(void)timer; (void)arg;
	struct mbt_infohash *ih;
	mbt_infohash_new(&ih, MBT_INFOHASH_SHA1, "BBBBBBBBBBBBBBBBBBBBBBBBBBBBBBBBBBBBBBBB");
	mbt_dht_get_peers(dht_a, ih, NULL, dht_a_done, NULL);
	mbt_infohash_unref(ih);
}

static void
test_dht_v6(void)
{
	mbt_ev_new_default(&ev);

	/* 3 DHT nodes on ::1 with distinct IDs */
	mbt_dht_new(&dht_a, ev, 10101);
	mbt_dht_new(&dht_b, ev, 10102);
	mbt_dht_new(&dht_c, ev, 10103);
	uint8_t id_a[20] = {0x11}; mbt_dht_set_node_id(dht_a, id_a);
	uint8_t id_b[20] = {0x22}; mbt_dht_set_node_id(dht_b, id_b);
	uint8_t id_c[20] = {0x33}; mbt_dht_set_node_id(dht_c, id_c);

	/* Bootstrap over IPv6 */
	mbt_dht_bootstrap(dht_a, "::1", 10102);
	mbt_dht_bootstrap(dht_b, "::1", 10101);
	mbt_dht_bootstrap(dht_b, "::1", 10103);
	mbt_dht_bootstrap(dht_c, "::1", 10102);

	/* A announces after 1s, C looks up after 2s */
	struct mbt_ev_timer *t1, *t2, *to;
	mbt_ev_timer_new(ev, &t1, 1000000ULL, dht_do_announce, NULL);
	mbt_ev_timer_new(ev, &t2, 4000000ULL, dht_do_lookup, NULL);
	mbt_ev_timer_new(ev, &to, 10000000ULL, timeout_cb, NULL);

	dht_found = 0;
	mbt_ev_run(ev);

	assert(dht_found == 1);
	fprintf(stderr, "  DHT over IPv6: PASS\n");

	mbt_ev_timer_free(t1);
	mbt_ev_timer_free(t2);
	mbt_ev_timer_free(to);
	mbt_dht_free(dht_a);
	mbt_dht_free(dht_b);
	mbt_dht_free(dht_c);
	mbt_ev_destroy(ev);
}
