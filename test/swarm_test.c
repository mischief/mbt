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
#include <signal.h>
#include <getopt.h>
#include <sha1.h>
#include <stdint.h>
#include <unistd.h>

#include "mbt/event.h"
#include "mbt/session.h"
#include "mbt/torrent.h"
#include "mbt/storage.h"
#include "mbt/be.h"
#include "mbt/hex.h"
#include "mbt/tracker_server.h"
#include "mbt/log.h"

struct mbt_ev_timer;
struct mbt_torrent;

#define PIECE_LEN 16384

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
	signal(SIGPIPE, SIG_IGN);

	int num_seeders = 2;
	int num_leechers = 5;
	int num_pieces = 8;
	int piece_kib = 16;
	int use_tracker = 0;
	int disable_tcp = 0;
	int disable_utp = 0;
	int choke_interval = 0;  /* 0 = library default (10s) */

	static struct option longopts[] = {
		{"seeders",     required_argument, NULL, 's'},
		{"leechers",    required_argument, NULL, 'l'},
		{"pieces",      required_argument, NULL, 'p'},
		{"piece-size",  required_argument, NULL, 'b'},
		{"tracker",     no_argument,       NULL, 't'},
		{"disable-tcp", no_argument,       NULL, 'T'},
		{"disable-utp", no_argument,       NULL, 'U'},
		{"debug",       no_argument,       NULL, 'd'},
		{"trace",       no_argument,       NULL, 'R'},  /* trace level, all components */
		{"choke-interval", required_argument, NULL, 'C'},  /* seconds; 0=default */
		{"help",        no_argument,       NULL, 'h'},
		{NULL, 0, NULL, 0}
	};
	int ch;
	int debug = 0;
	int trace = 0;
	while ((ch = getopt_long(argc, argv, "s:l:p:b:tTUdh", longopts, NULL)) != -1) {
		switch (ch) {
		case 's': num_seeders = atoi(optarg); break;
		case 'l': num_leechers = atoi(optarg); break;
		case 'p': num_pieces = atoi(optarg); break;
		case 'b': piece_kib = atoi(optarg); break;
		case 't': use_tracker = 1; break;
		case 'T': disable_tcp = 1; break;
		case 'U': disable_utp = 1; break;
		case 'd': debug = 1; break;
		case 'R': trace = 1; break;
		case 'C': choke_interval = atoi(optarg); break;
		case 'h':
		default:
			fprintf(stderr,
			        "usage: swarm_test [--seeders N] [--leechers M] [--pieces P]\n"
			        "                  [--piece-size KiB] [--tracker]\n"
			        "                  [--disable-tcp] [--disable-utp]\n"
			        "                  [--debug] [--trace] [--choke-interval S]\n");
			return ch == 'h' ? 0 : 1;
		}
	}
	if (disable_tcp && disable_utp) {
		fprintf(stderr, "swarm_test: --disable-tcp and --disable-utp are mutually exclusive\n");
		return 1;
	}

	target_completions = num_leechers;
	if (debug || trace) {
		mbt_log_set(mbt_log_stderr, NULL);
		if (debug)
			mbt_log_set_debug(MBT_LOG_ALL);
		if (trace)
			mbt_log_set_trace(MBT_LOG_ALL);  /* trace level, all components */
	}
	int piece_len = piece_kib * 1024;
	int total_len = piece_len * num_pieces;
	uint8_t *data = malloc(total_len);
	for (int i = 0; i < total_len; i++) data[i] = (i * 97 + 31) & 0xff;

	uint8_t *pieces_hash = malloc(num_pieces * 20);
	for (int i = 0; i < num_pieces; i++) {
		SHA1_CTX ctx; SHA1Init(&ctx);
		SHA1Update(&ctx, data + i * piece_len, piece_len);
		SHA1Final(pieces_hash + i * 20, &ctx);
	}

	_cleanup_be_ struct mbt_be_value *root = NULL, *info = NULL;
	mbt_be_dict_new(&root);
	mbt_be_dict_new(&info);
	mbt_be_dict_append(root, "info", info);
	mbt_be_dict_append_number(info, "length", total_len);
	mbt_be_dict_append_string(info, "name", "swarm_test.bin", 14);
	mbt_be_dict_append_number(info, "piece length", piece_len);
	mbt_be_dict_append_string(info, "pieces", pieces_hash, num_pieces * 20);

	unsigned char *encoded; size_t enc_len;

	mbt_ev_new_default(&ev);

	/* Optional tracker */
	struct mbt_tracker_server *tracker = NULL;
	uint16_t tracker_port = 0;
	if (use_tracker) {
		int rv = mbt_tracker_server_new(&tracker, ev, 0, NULL);
		if (rv < 0) { fprintf(stderr, "tracker: %s\n", strerror(-rv)); return 1; }
		tracker_port = mbt_tracker_server_port(tracker);
	}

	if (use_tracker) {
		char announce[128];
		snprintf(announce, sizeof(announce), "udp://127.0.0.1:%hu/announce", tracker_port);
		mbt_be_dict_append_string(root, "announce", announce, strlen(announce));
	}

	mbt_be_value_encode(root, &encoded, &enc_len);

	/* Compute infohash */
	unsigned char *is = memmem(encoded, enc_len, "4:infod", 7);
	if (!is) is = memmem(encoded, enc_len, "4:info", 6);
	is += 6;
	struct mbt_be_value *tmp = NULL;
	int ilen = mbt_be_value_parse(is, enc_len - (is - encoded), &tmp);
	mbt_be_unref(tmp);
	SHA1_CTX hctx; uint8_t digest[20]; char hex[41];
	SHA1Init(&hctx); SHA1Update(&hctx, is, ilen); SHA1Final(digest, &hctx);
	mbt_hex_encode(digest, 20, hex, sizeof(hex));

	/* Create seeders */
	struct mbt_session **seeders = calloc(num_seeders, sizeof(*seeders));
	struct mbt_storage_memory **smem = calloc(num_seeders, sizeof(*smem));
	for (int i = 0; i < num_seeders; i++) {
		smem[i] = mbt_storage_memory_new();
		mbt_storage_memory_put_file(smem[i], 0, data, total_len);
		struct mbt_session_config cfg = {
			.ev = ev, .default_storage = mbt_storage_memory_storage(smem[i]), .listen_port = 0,
			.max_peers = num_leechers + num_seeders,
			.timers = { .tracker_interval = 2, .request_timeout = 2, .choke_interval = choke_interval },
			.disable_tcp = disable_tcp,
			.disable_utp = disable_utp,
		};
		mbt_session_new(&seeders[i], &cfg);
		mbt_session_add_torrent_buf(seeders[i], encoded, enc_len, NULL);
	}

	/* Build peer list string for magnet (only used when no tracker) */
	char peers[4096] = "";
	if (!use_tracker) {
		for (int i = 0; i < num_seeders; i++) {
			char tmp2[64];
			snprintf(tmp2, sizeof(tmp2), "&x.pe=127.0.0.1:%hu", mbt_session_listen_port(seeders[i]));
			strncat(peers, tmp2, sizeof(peers) - strlen(peers) - 1);
		}
	}

	/* Create leechers */
	struct mbt_session **leechers = calloc(num_leechers, sizeof(*leechers));
	struct mbt_storage_memory **lmem = calloc(num_leechers, sizeof(*lmem));
	for (int i = 0; i < num_leechers; i++) {
		lmem[i] = mbt_storage_memory_new();
		struct mbt_session_config cfg = {
			.ev = ev, .default_storage = mbt_storage_memory_storage(lmem[i]), .listen_port = 0,
			.timers = { .tracker_interval = 2, .request_timeout = 2, .choke_interval = choke_interval },
			.disable_tcp = disable_tcp,
			.disable_utp = disable_utp,
		};
		mbt_session_new(&leechers[i], &cfg);

		char magnet[8192];
		if (use_tracker)
			snprintf(magnet, sizeof(magnet),
			         "magnet:?xt=urn:btih:%s&tr=udp://127.0.0.1:%hu/announce",
			         hex, tracker_port);
		else
			snprintf(magnet, sizeof(magnet), "magnet:?xt=urn:btih:%s%s", hex, peers);
		mbt_session_add_magnet(leechers[i], magnet, NULL);

		struct mbt_torrent *t = mbt_session_torrent_first(leechers[i]);
		struct mbt_torrent_callbacks cb = { .on_complete = on_complete };
		mbt_torrent_set_callbacks(t, &cb, NULL);
	}

	fprintf(stderr, "swarm: %d seeders, %d leechers, %d pieces (%d KB, %d KiB/piece)%s [%s]\n",
	        num_seeders, num_leechers, num_pieces, total_len / 1024, piece_kib,
	        use_tracker ? " [tracker]" : " [direct]",
	        disable_tcp ? "utp-only" : disable_utp ? "tcp-only" : "mixed");

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
	if (tracker) mbt_tracker_server_free(tracker);
	free(encoded); free(data); free(pieces_hash);
	mbt_ev_destroy(ev);
	return 0;
}
