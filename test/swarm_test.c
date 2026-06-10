// SPDX-License-Identifier: ISC
/*
 * test/swarm_test.c — parameterized swarm scaling test
 *
 * Runs N seeders + M leechers on one event loop, all on localhost.
 * Verifies all leechers complete. Exposes scaling issues (fd limits,
 * memory growth, timer storms).
 *
 * Usage: swarm_test [--seeders N] [--leechers M] [--pieces P]
 * Defaults: 2 seeders, 5 leechers, 8 pieces
 */
#include <assert.h>
#include <stdio.h>
#include <string.h>
#include <stdlib.h>
#include <getopt.h>

#include <sha1.h>

#include "mbt/event.h"
#include "mbt/session.h"
#include "mbt/torrent.h"
#include "mbt/storage.h"
#include "mbt/picker.h"
#include "mbt/be.h"
#include "mbt/hex.h"

#define PIECE_LEN 16384
#define BASE_PORT 19700

static struct mbt_ev *ev;
static int completions;
static int target_completions;

static void on_complete(struct mbt_torrent *t, void *arg)
{
	(void)t; (void)arg;
	completions++;
	if (completions >= target_completions)
		mbt_ev_stop(ev);
}

static void timeout_cb(struct mbt_ev_timer *t, void *a)
{ (void)t; (void)a; mbt_ev_stop(ev); }

int main(int argc, char *argv[])
{
	int num_seeders = 2;
	int num_leechers = 5;
	int num_pieces = 8;

	static struct option longopts[] = {
		{"seeders", required_argument, NULL, 's'},
		{"leechers", required_argument, NULL, 'l'},
		{"pieces", required_argument, NULL, 'p'},
		{"help", no_argument, NULL, 'h'},
		{NULL, 0, NULL, 0}
	};
	int ch;
	while ((ch = getopt_long(argc, argv, "s:l:p:h", longopts, NULL)) != -1) {
		switch (ch) {
		case 's': num_seeders = atoi(optarg); break;
		case 'l': num_leechers = atoi(optarg); break;
		case 'p': num_pieces = atoi(optarg); break;
		case 'h':
		default:
			fprintf(stderr, "usage: swarm_test [--seeders N] [--leechers M] [--pieces P]\n");
			return ch == 'h' ? 0 : 1;
		}
	}

	target_completions = num_leechers;
	int total_len = PIECE_LEN * num_pieces;
	uint8_t *data = malloc(total_len);
	for (int i = 0; i < total_len; i++) data[i] = (i * 97 + 31) & 0xff;

	uint8_t *pieces_hash = malloc(num_pieces * 20);
	for (int i = 0; i < num_pieces; i++) {
		SHA1_CTX ctx; SHA1Init(&ctx);
		SHA1Update(&ctx, data + i * PIECE_LEN, PIECE_LEN);
		SHA1Final(pieces_hash + i * 20, &ctx);
	}

	struct mbt_be_value *root = NULL, *info = NULL;
	mbt_be_dict_new(&root);
	mbt_be_dict_new(&info);
	mbt_be_dict_append(root, "info", info);
	mbt_be_dict_append_number(info, "length", total_len);
	mbt_be_dict_append_string(info, "name", "swarm_test.bin", 14);
	mbt_be_dict_append_number(info, "piece length", PIECE_LEN);
	mbt_be_dict_append_string(info, "pieces", pieces_hash, num_pieces * 20);

	unsigned char *encoded; size_t enc_len;
	mbt_be_value_encode(root, &encoded, &enc_len);
	mbt_be_value_free(root);

	/* Compute infohash */
	unsigned char *is = memmem(encoded, enc_len, "4:infod", 7);
	if (!is) is = memmem(encoded, enc_len, "4:info", 6);
	is += 6;
	struct mbt_be_value *tmp = NULL;
	int ilen = mbt_be_value_parse(is, enc_len - (is - encoded), &tmp);
	mbt_be_value_free(tmp);
	SHA1_CTX hctx; uint8_t digest[20]; char hex[41];
	SHA1Init(&hctx); SHA1Update(&hctx, is, ilen); SHA1Final(digest, &hctx);
	mbt_hex_encode(digest, 20, hex, sizeof(hex));

	mbt_ev_new_default(&ev);

	/* Build peer list string for magnet */
	char peers[4096] = "";
	for (int i = 0; i < num_seeders; i++) {
		char tmp2[64];
		snprintf(tmp2, sizeof(tmp2), "&x.pe=127.0.0.1:%d", BASE_PORT + i);
		strncat(peers, tmp2, sizeof(peers) - strlen(peers) - 1);
	}

	/* Create seeders */
	struct mbt_session **seeders = calloc(num_seeders, sizeof(*seeders));
	struct mbt_storage_memory **smem = calloc(num_seeders, sizeof(*smem));
	for (int i = 0; i < num_seeders; i++) {
		smem[i] = mbt_storage_memory_new();
		mbt_storage_memory_put_file(smem[i], 0, data, total_len);
		struct mbt_session_config cfg = {
			.ev = ev, .storage_ops = mbt_storage_memory_ops(),
			.storage_ctx = smem[i], .listen_port = BASE_PORT + i,
		};
		mbt_session_new(&seeders[i], &cfg);
		mbt_session_add_torrent_buf(seeders[i], encoded, enc_len);
	}

	/* Create leechers */
	struct mbt_session **leechers = calloc(num_leechers, sizeof(*leechers));
	struct mbt_storage_memory **lmem = calloc(num_leechers, sizeof(*lmem));
	for (int i = 0; i < num_leechers; i++) {
		lmem[i] = mbt_storage_memory_new();
		struct mbt_session_config cfg = {
			.ev = ev, .storage_ops = mbt_storage_memory_ops(),
			.storage_ctx = lmem[i], .listen_port = BASE_PORT + num_seeders + i,
		};
		mbt_session_new(&leechers[i], &cfg);

		char magnet[8192];
		snprintf(magnet, sizeof(magnet), "magnet:?xt=urn:btih:%s%s", hex, peers);
		mbt_session_add_magnet(leechers[i], magnet);

		struct mbt_torrent *t = mbt_session_torrent_first(leechers[i]);
		struct mbt_torrent_callbacks cb = { .on_complete = on_complete };
		mbt_torrent_set_callbacks(t, &cb, NULL);
	}

	fprintf(stderr, "swarm: %d seeders, %d leechers, %d pieces (%d KB)\n",
	        num_seeders, num_leechers, num_pieces, total_len / 1024);

	struct mbt_ev_timer *to;
	mbt_ev_timer_new(ev, &to, 60 * 1000000ULL, timeout_cb, NULL);
	mbt_ev_run(ev);

	fprintf(stderr, "swarm: %d/%d leechers completed\n", completions, num_leechers);
	assert(completions == num_leechers);
	fprintf(stderr, "swarm_test: PASS\n");

	mbt_ev_timer_free(to);
	for (int i = 0; i < num_seeders; i++) {
		mbt_session_free(seeders[i]);
		mbt_storage_memory_destroy(smem[i]);
	}
	for (int i = 0; i < num_leechers; i++) {
		mbt_session_free(leechers[i]);
		mbt_storage_memory_destroy(lmem[i]);
	}
	free(seeders); free(leechers); free(smem); free(lmem);
	free(encoded); free(data); free(pieces_hash);
	mbt_ev_destroy(ev);
	return 0;
}
