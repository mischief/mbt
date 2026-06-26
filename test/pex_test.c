// SPDX-License-Identifier: ISC
// Test: PEX (BEP 11) peer exchange
//
// Topology:
//   seeder   — has all pieces, both leechers connect to it
//   leecher B — connects only to seeder initially
//   leecher C — connects only to seeder initially
//
// After pex_interval fires, seeder sends PEX to B listing C (and vice versa).
// B should initiate an outbound connect to C. We stop after a fixed delay and
// assert each leecher has seen more than 1 peer (seeder + the other leecher).
#include <assert.h>
#include <stdio.h>
#include <string.h>
#include <stdlib.h>
#include <signal.h>
#include <sha1.h>
#include <stdint.h>

#include "mbt/event.h"
#include "mbt/session.h"
#include "mbt/torrent.h"
#include "mbt/storage.h"
#include "mbt/be.h"
#include "mbt/hex.h"
#include "mbt/log.h"

struct mbt_ev_timer;

#define PIECE_LEN  16384
#define NUM_PIECES 128
#define TOTAL_LEN  (PIECE_LEN * NUM_PIECES)

static struct mbt_ev *ev;

static void stop_cb(struct mbt_ev_timer *t, void *a)
{ (void)t; (void)a; mbt_ev_stop(ev); }

int main(int argc, char *argv[])
{
	int debug = argc > 1 && strcmp(argv[1], "-d") == 0;
	signal(SIGPIPE, SIG_IGN);

	if (debug) {
		mbt_log_set(mbt_log_stderr, NULL);
		mbt_log_set_debug(MBT_LOG_ALL);
	}

	/* Build a small torrent in memory */
	uint8_t data[TOTAL_LEN];
	for (int i = 0; i < TOTAL_LEN; i++) data[i] = (i * 97 + 31) & 0xff;

	uint8_t pieces_hash[NUM_PIECES * 20];
	for (int i = 0; i < NUM_PIECES; i++) {
		SHA1_CTX ctx; SHA1Init(&ctx);
		SHA1Update(&ctx, data + i * PIECE_LEN, PIECE_LEN);
		SHA1Final(pieces_hash + i * 20, &ctx);
	}

	_cleanup_be_ struct mbt_be_value *root = NULL, *info = NULL;
	mbt_be_dict_new(&root);
	mbt_be_dict_new(&info);
	mbt_be_dict_append_number(info, "length", TOTAL_LEN);
	mbt_be_dict_append_string(info, "name", "pex_test.bin", 12);
	mbt_be_dict_append_number(info, "piece length", PIECE_LEN);
	mbt_be_dict_append_string(info, "pieces", pieces_hash, sizeof(pieces_hash));
	mbt_be_dict_append(root, "info", info);

	unsigned char *encoded; size_t enc_len;
	mbt_be_value_encode(root, &encoded, &enc_len);

	/* Compute infohash */
	unsigned char *is = memmem(encoded, enc_len, "4:infod", 7);
	if (!is) is = memmem(encoded, enc_len, "4:info", 6);
	is += 6;
	_cleanup_be_ struct mbt_be_value *tmp = NULL;
	int ilen = mbt_be_value_parse(is, enc_len - (is - encoded), &tmp);
	SHA1_CTX hctx; uint8_t digest[20]; char hex[41];
	SHA1Init(&hctx); SHA1Update(&hctx, is, ilen); SHA1Final(digest, &hctx);
	mbt_hex_encode(digest, 20, hex, sizeof(hex));

	mbt_ev_new_default(&ev);

	/* Seeder: pex_interval=1s, maintain_interval=1s so connects fire quickly */
	struct mbt_storage_memory *smem = mbt_storage_memory_new();
	mbt_storage_memory_put_file(smem, 0, data, TOTAL_LEN);
	struct mbt_session_config scfg = {
		.ev = ev,
		.default_storage = mbt_storage_memory_storage(smem),
		.listen_port = 0,
		.max_peers = 10,
		.timers = { .pex_interval = 1, .maintain_interval = 1 },
		.upload_rate = 256 * 1024, /* 256 KB/s: 2MB takes ~8s, PEX fires at 1s */
	};
	struct mbt_session *seeder;
	mbt_session_new(&seeder, &scfg);
	mbt_session_add_torrent_buf(seeder, encoded, enc_len, NULL);
	uint16_t seeder_port = mbt_session_listen_port(seeder);

	/* Leecher B: connects only to seeder */
	struct mbt_storage_memory *bmem = mbt_storage_memory_new();
	struct mbt_session_config bcfg = {
		.ev = ev,
		.default_storage = mbt_storage_memory_storage(bmem),
		.listen_port = 0,
		.max_peers = 10,
		.timers = { .pex_interval = 1, .maintain_interval = 1 },
	};
	struct mbt_session *leecher_b;
	mbt_session_new(&leecher_b, &bcfg);
	char magnet_b[256];
	snprintf(magnet_b, sizeof(magnet_b),
	         "magnet:?xt=urn:btih:%s&x.pe=127.0.0.1:%hu", hex, seeder_port);
	mbt_session_add_magnet(leecher_b, magnet_b, NULL);
	struct mbt_torrent *tb = mbt_session_torrent_first(leecher_b);

	/* Leecher C: connects only to seeder */
	struct mbt_storage_memory *cmem = mbt_storage_memory_new();
	struct mbt_session_config ccfg = {
		.ev = ev,
		.default_storage = mbt_storage_memory_storage(cmem),
		.listen_port = 0,
		.max_peers = 10,
		.timers = { .pex_interval = 1, .maintain_interval = 1 },
	};
	struct mbt_session *leecher_c;
	mbt_session_new(&leecher_c, &ccfg);
	char magnet_c[256];
	snprintf(magnet_c, sizeof(magnet_c),
	         "magnet:?xt=urn:btih:%s&x.pe=127.0.0.1:%hu", hex, seeder_port);
	mbt_session_add_magnet(leecher_c, magnet_c, NULL);
	struct mbt_torrent *tc = mbt_session_torrent_first(leecher_c);

	/* Run for 5s: rate-limited seeder at 256KB/s means leechers are still
	 * downloading when PEX fires at 1s, giving them time to connect to each other */
	struct mbt_ev_timer *stop;
	mbt_ev_timer_new(ev, &stop, 5 * 1000000ULL, stop_cb, NULL);
	mbt_ev_run(ev);
	mbt_ev_timer_free(stop);

	int b_peers = mbt_torrent_peer_count(tb);
	int c_peers = mbt_torrent_peer_count(tc);
	fprintf(stderr, "pex_test: leecher B peers=%d, leecher C peers=%d\n",
	        b_peers, c_peers);
	assert(b_peers > 1 && "B should have connected to C via PEX");
	assert(c_peers > 1 && "C should have connected to B via PEX");

	fprintf(stderr, "pex_test: PASS\n");
	free(encoded);

	mbt_session_free(seeder);
	mbt_session_free(leecher_b);
	mbt_session_free(leecher_c);
	mbt_storage_memory_destroy(smem);
	mbt_storage_memory_destroy(bmem);
	mbt_storage_memory_destroy(cmem);
	mbt_ev_destroy(ev);
	return 0;
}
