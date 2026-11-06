// SPDX-License-Identifier: ISC
/* A session with more torrents than lookup slots announces all of them. */
#include <assert.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>

#include "mbt/be.h"
#include "mbt/dht.h"
#include "mbt/event.h"
#include "mbt/metainfo.h"
#include "mbt/peerdb.h"
#include "mbt/session.h"
#include "mbt/storage.h"
#include "mbt/torrent.h"

#define NUM_TORRENTS 40          /* above MAX_LOOKUPS */
#define WAIT_SEC 15              /* the session maintain timer runs each 5 s */
#define PACE_RATE 20             /* queries per second, slow enough to fill the queue */
#define PRIORITY_WAIT_SEC 5

static char tmpdir[] = "/tmp/mbt_dhtmany_XXXXXX";
static struct mbt_ev *ev;
static long long stored = -1;

static void
sample_reply(struct mbt_be_value *reply, int err, void *arg)
{
	struct mbt_be_value *r = err == 0 ? mbt_be_find(reply, "r") : NULL;
	struct mbt_be_value *n = r ? mbt_be_find(r, "num") : NULL;

	(void)arg;
	if (n)
		stored = n->integer;
}

static void
ask_stored(struct mbt_dht *probe, uint16_t port)
{
	static const char id[21] = "aaaaaaaaaaaaaaaaaaaa";
	struct mbt_be_value *a;

	assert(mbt_be_dict_new(&a) == 0);
	assert(mbt_be_dict_append_string(a, "id", id, 20) == 0);
	assert(mbt_be_dict_append_string(a, "target", id, 20) == 0);
	assert(mbt_dht_query(probe, "127.0.0.1", port, "sample_infohashes", a,
	                     sample_reply, NULL) == 0);
	mbt_be_unref(a);
}

static void
add_torrent(struct mbt_session *s, int i)
{
	struct mbt_metainfo *mi;
	unsigned char *enc;
	char path[300];
	size_t len;
	FILE *fp;

	snprintf(path, sizeof(path), "%s/f%d", tmpdir, i);
	fp = fopen(path, "wb");
	assert(fp);
	fprintf(fp, "file %d", i);
	fclose(fp);
	assert(mbt_metainfo_new(&mi) == 0);
	assert(mbt_metainfo_set_content(mi, path, 16384) == 0);
	assert(mbt_metainfo_encode(mi, &enc, &len) == 0);
	mbt_metainfo_free(mi);
	assert(mbt_session_add_torrent_buf(s, enc, len, NULL, NULL) == 0);
	free(enc);
}

static void
probe_announce(struct mbt_dht *probe, uint16_t port, struct mbt_infohash *ih)
{
	assert(mbt_dht_bootstrap(probe, "127.0.0.1", port) == 0);
	for (int i = 0; i < 50; i++)
		mbt_ev_step(ev, 10);
	assert(mbt_dht_get_peers(probe, ih, NULL, NULL, NULL) == 0);
	for (int i = 0; i < 50; i++)
		mbt_ev_step(ev, 10);
	assert(mbt_dht_announce(probe, ih, 7777) > 0);
	for (int i = 0; i < 50; i++)
		mbt_ev_step(ev, 10);
}

/* Seeds at a low query rate must not hold back a download. */
static void
test_download_first(struct mbt_dht *probe, uint16_t port, struct mbt_storage *st)
{
	struct mbt_session_config cfg = {.ev = ev, .disable_utp = 1, .dht_no_bootstrap = 1,
	    .default_storage = st, .dht = {.send_rate = PACE_RATE}};
	struct mbt_storage_memory *m = mbt_storage_memory_new();
	struct mbt_add_torrent_cfg ac = {.storage = mbt_storage_memory_storage(m)};
	struct mbt_session *s;
	struct mbt_metainfo *mi;
	struct mbt_torrent *t;
	unsigned char *enc;
	char path[300];
	size_t len;
	FILE *fp;
	time_t end;
	int found = 0;

	snprintf(path, sizeof(path), "%s/wanted", tmpdir);
	fp = fopen(path, "wb");
	assert(fp);
	fputs("wanted", fp);
	fclose(fp);
	assert(mbt_metainfo_new(&mi) == 0);
	assert(mbt_metainfo_set_content(mi, path, 16384) == 0);
	assert(mbt_metainfo_encode(mi, &enc, &len) == 0);
	probe_announce(probe, port, mbt_metainfo_infohash(mi));
	mbt_metainfo_free(mi);

	assert(mbt_session_new(&s, &cfg) == 0);
	mbt_session_disable_lsd(s);
	assert(mbt_session_enable_dht_local(s) == 0);
	assert(mbt_session_dht_bootstrap(s, "127.0.0.1", port) == 0);
	for (int i = 0; i < 50; i++)
		mbt_ev_step(ev, 10);
	for (int i = 0; i < NUM_TORRENTS; i++)
		add_torrent(s, i);
	for (int i = 0; i < 20; i++)
		mbt_ev_step(ev, 10);
	assert(mbt_session_add_torrent_buf(s, enc, len, &ac, &t) == 0);
	free(enc);

	end = time(NULL) + PRIORITY_WAIT_SEC;
	while (!found && time(NULL) < end) {
		struct mbt_peerdb *db = mbt_torrent_peerdb(t);

		mbt_ev_step(ev, 10);
		found = mbt_peerdb_count(db, MBT_PEERDB_KNOWN) +
		        mbt_peerdb_count(db, MBT_PEERDB_CONNECTING) +
		        mbt_peerdb_count(db, MBT_PEERDB_FAILED) > 0;
	}
	assert(found);
	mbt_session_free(s);
	mbt_storage_memory_destroy(m);
}

int
main(void)
{
	struct mbt_dht *node, *probe;
	struct mbt_session *s;
	struct mbt_storage *st;
	char cmd[300];
	time_t end;

	assert(mkdtemp(tmpdir));
	assert(mbt_ev_new_default(&ev) == 0);
	struct mbt_dht_config ncfg = {.ev = ev, .send_rate = 1000};

	assert(mbt_dht_new(&node, &ncfg) == 0);
	assert(mbt_dht_new(&probe, &ncfg) == 0);
	st = mbt_storage_file_new(tmpdir);
	assert(st);

	struct mbt_session_config cfg = {.ev = ev, .disable_utp = 1, .dht_no_bootstrap = 1,
	    .default_storage = st, .dht = {.send_rate = 1000}};

	assert(mbt_session_new(&s, &cfg) == 0);
	mbt_session_disable_lsd(s);
	assert(mbt_session_enable_dht_local(s) == 0);
	assert(mbt_session_dht_bootstrap(s, "127.0.0.1", mbt_dht_port(node)) == 0);
	for (int i = 0; i < 50; i++)
		mbt_ev_step(ev, 10);
	for (int i = 0; i < NUM_TORRENTS; i++)
		add_torrent(s, i);

	end = time(NULL) + WAIT_SEC;
	while (stored < NUM_TORRENTS && time(NULL) < end) {
		ask_stored(probe, mbt_dht_port(node));
		for (int i = 0; i < 20; i++)
			mbt_ev_step(ev, 10);
	}
	assert(stored == NUM_TORRENTS);

	mbt_session_free(s);
	test_download_first(probe, mbt_dht_port(node), st);
	mbt_storage_unref(st);
	mbt_dht_free(probe);
	mbt_dht_free(node);
	mbt_ev_destroy(ev);
	snprintf(cmd, sizeof(cmd), "rm -rf %s", tmpdir);
	assert(system(cmd) == 0);
	puts("dht_many: ok");
	return 0;
}
