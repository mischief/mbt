// SPDX-License-Identifier: ISC
/* Transfer totals must keep the work of peers that left. */
#include <assert.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>

#include "mbt/event.h"
#include "mbt/infohash.h"
#include "mbt/metainfo.h"
#include "mbt/session.h"
#include "mbt/storage.h"
#include "mbt/torrent.h"
#include "test_util.h"

#define PIECE_LEN 16384
#define NUM_PIECES 4
#define TOTAL_LEN (PIECE_LEN * NUM_PIECES)
#define WAIT_STEPS 2000          /* 20 seconds at 10 ms */

static uint8_t data[TOTAL_LEN];
static struct mbt_ev *ev;

static struct mbt_session *
new_session(struct mbt_storage_memory *m)
{
	struct mbt_session_config cfg = {.ev = ev, .disable_utp = 1,
	    .default_storage = mbt_storage_memory_storage(m)};
	struct mbt_session *s;

	assert(mbt_session_new(&s, &cfg) == 0);
	return s;
}

int
main(void)
{
	char path[] = "/tmp/mbt_totals_XXXXXX";
	struct mbt_storage_memory *sm = mbt_storage_memory_new();
	struct mbt_storage_memory *lm = mbt_storage_memory_new();
	struct mbt_session *seed, *leech;
	struct mbt_torrent *st, *lt;
	struct mbt_metainfo *mi;
	unsigned char *enc;
	size_t enc_len;
	int fd = mkstemp(path);

	assert(fd >= 0 && sm && lm);
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
	seed = new_session(sm);
	leech = new_session(lm);
	assert(mbt_session_add_torrent_buf(seed, enc, enc_len, NULL, &st) == 0);
	assert(mbt_storage_memory_put_file(sm, 0, data, TOTAL_LEN) == 0);
	assert(mbt_session_add_torrent_buf(leech, enc, enc_len, NULL, &lt) == 0);
	test_wait_verified(ev, st);
	test_wait_verified(ev, lt);
	assert(mbt_torrent_have_count(st) == NUM_PIECES);

	/* One bad piece after verify gives the leecher a hash failure. */
	mbt_storage_memory_data(sm)[PIECE_LEN * 2] ^= 0xff;
	assert(mbt_session_add_peer(leech, mbt_torrent_infohash(lt), "127.0.0.1",
	                            mbt_session_listen_port(seed), MBT_SOURCE_MANUAL, 0) == 0);
	for (int i = 0; i < WAIT_STEPS && mbt_torrent_hash_failures(lt) == 0; i++)
		mbt_ev_step(ev, 10);
	assert(mbt_torrent_hash_failures(lt) >= 1);
	mbt_storage_memory_data(sm)[PIECE_LEN * 2] ^= 0xff;
	for (int i = 0; i < WAIT_STEPS && !mbt_torrent_complete(lt); i++)
		mbt_ev_step(ev, 10);
	assert(mbt_torrent_complete(lt));

	uint64_t down = mbt_torrent_bytes_downloaded(lt);
	uint64_t up = mbt_torrent_bytes_uploaded(st);
	uint64_t fails = mbt_torrent_hash_failures(lt);

	assert(down >= TOTAL_LEN);
	assert(up >= TOTAL_LEN);
	assert(mbt_session_bytes_uploaded(seed) == up);

	/* The seed leaves. The leecher keeps its totals. */
	mbt_session_free(seed);
	for (int i = 0; i < WAIT_STEPS && mbt_torrent_peer_count(lt) > 0; i++)
		mbt_ev_step(ev, 10);
	assert(mbt_torrent_peer_count(lt) == 0);
	assert(mbt_torrent_bytes_downloaded(lt) == down);
	assert(mbt_torrent_hash_failures(lt) == fails);

	/* Removal keeps them in the session total. */
	assert(mbt_session_bytes_downloaded(leech) == down);
	assert(mbt_session_remove_torrent(leech, mbt_torrent_infohash(lt)) == 0);
	assert(mbt_session_bytes_downloaded(leech) == down);

	mbt_session_free(leech);
	mbt_storage_memory_destroy(sm);
	mbt_storage_memory_destroy(lm);
	mbt_ev_destroy(ev);
	free(enc);
	puts("peer_totals: ok");
	return 0;
}
