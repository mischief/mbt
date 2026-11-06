// SPDX-License-Identifier: ISC
/* A torrent transfer driven only by poll on the mbt_ev_watch descriptor. */
#include <assert.h>
#include <errno.h>
#include <poll.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>
#include <unistd.h>

#include "mbt/event.h"
#include "mbt/infohash.h"
#include "mbt/metainfo.h"
#include "mbt/session.h"
#include "mbt/storage.h"
#include "mbt/torrent.h"

#define PIECE_LEN 16384
#define NUM_PIECES 64
#define TOTAL_LEN (PIECE_LEN * NUM_PIECES)
#define IDLE_POLL_MS 2000        /* a missed wakeup costs this much */
#define LIMIT_SEC 10
#define SKIP 77

static uint8_t data[TOTAL_LEN];

/* One round, as a program with its own loop does it. */
static int
round_once(struct mbt_ev *ev)
{
	struct pollfd p = {.events = POLLIN};
	int ms = -1;

	p.fd = mbt_ev_watch(ev, &ms);
	if (p.fd < 0)
		return p.fd;
	(void)poll(&p, 1, ms >= 0 && ms < IDLE_POLL_MS ? ms : IDLE_POLL_MS);
	mbt_ev_step(ev, 0);
	return 0;
}

static void
transfer(struct mbt_ev *ev, const unsigned char *enc, size_t enc_len, int use_utp)
{
	struct mbt_storage_memory *sm = mbt_storage_memory_new();
	struct mbt_storage_memory *lm = mbt_storage_memory_new();
	struct mbt_session_config cfg = {.ev = ev, .disable_utp = !use_utp, .disable_tcp = use_utp,
	    .default_storage = mbt_storage_memory_storage(sm)};
	struct mbt_session *seed, *leech;
	struct mbt_torrent *st, *lt;
	time_t start;

	assert(sm && lm);
	assert(mbt_session_new(&seed, &cfg) == 0);
	cfg.default_storage = mbt_storage_memory_storage(lm);
	assert(mbt_session_new(&leech, &cfg) == 0);
	assert(mbt_session_add_torrent_buf(seed, enc, enc_len, NULL, &st) == 0);
	assert(mbt_storage_memory_put_file(sm, 0, data, TOTAL_LEN) == 0);
	assert(mbt_session_add_torrent_buf(leech, enc, enc_len, NULL, &lt) == 0);
	start = time(NULL);
	while (mbt_torrent_have_count(st) < NUM_PIECES) {
		assert(round_once(ev) == 0);
		assert(time(NULL) - start < LIMIT_SEC);
	}
	assert(mbt_session_add_peer(leech, mbt_torrent_infohash(lt), "127.0.0.1",
	                            mbt_session_listen_port(seed), MBT_SOURCE_MANUAL, 0) == 0);
	while (!mbt_torrent_complete(lt)) {
		assert(round_once(ev) == 0);
		assert(time(NULL) - start < LIMIT_SEC);
	}
	assert(memcmp(mbt_storage_memory_data(lm), data, TOTAL_LEN) == 0);
	mbt_session_free(leech);
	mbt_session_free(seed);
	mbt_storage_memory_destroy(lm);
	mbt_storage_memory_destroy(sm);
}

int
main(void)
{
	char path[] = "/tmp/mbt_watch_XXXXXX";
	struct mbt_metainfo *mi;
	struct mbt_ev *ev;
	unsigned char *enc;
	size_t enc_len;
	int ms, fd = mkstemp(path);

	assert(fd >= 0);
	for (size_t i = 0; i < sizeof(data); i++)
		data[i] = (uint8_t)(i * 31 + i / 251);
	assert(write(fd, data, sizeof(data)) == (ssize_t)sizeof(data));
	close(fd);
	assert(mbt_metainfo_new(&mi) == 0);
	assert(mbt_metainfo_set_content(mi, path, PIECE_LEN) == 0);
	assert(mbt_metainfo_encode(mi, &enc, &enc_len) == 0);
	mbt_metainfo_free(mi);
	unlink(path);

	assert(mbt_ev_new_default(&ev) == 0);
	if (mbt_ev_watch(ev, &ms) == -ENOTSUP) {
		puts("loop_watch: no watch descriptor on this backend");
		return SKIP;
	}
	transfer(ev, enc, enc_len, 0);
	transfer(ev, enc, enc_len, 1);
	mbt_ev_destroy(ev);
	free(enc);
	puts("loop_watch: ok");
	return 0;
}
