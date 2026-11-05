// SPDX-License-Identifier: ISC
/* Public calls refuse bad arguments with -EINVAL. */
#include <assert.h>
#include <errno.h>
#include <stdio.h>
#include <stdlib.h>

#include "mbt/be.h"
#include "mbt/dht.h"
#include "mbt/event.h"
#include "mbt/infohash.h"
#include "mbt/magnet.h"
#include "mbt/metainfo.h"
#include "mbt/session.h"
#include "mbt/storage.h"
#include "mbt/torrent.h"

static void
ignore_query(struct mbt_be_value *reply, int err, void *arg)
{
	(void)reply; (void)err; (void)arg;
}

static void
test_session(struct mbt_ev *ev)
{
	struct mbt_session_config cfg = {.ev = ev};
	struct mbt_session_config no_ev = {0};
	struct mbt_infohash *ih = NULL;
	struct mbt_session *s = NULL;
	struct mbt_torrent *t = NULL;

	assert(mbt_session_new(NULL, &cfg) == -EINVAL);
	assert(mbt_session_new(&s, NULL) == -EINVAL);
	assert(mbt_session_new(&s, &no_ev) == -EINVAL);
	assert(mbt_session_new(&s, &cfg) == 0);
	assert(mbt_infohash_new(&ih, MBT_INFOHASH_SHA1,
	    "ABABABABABABABABABABABABABABABABABABABAB") == 0);

	assert(mbt_session_add_infohash(NULL, ih, NULL, NULL) == -EINVAL);
	assert(mbt_session_add_infohash(s, NULL, NULL, NULL) == -EINVAL);
	assert(mbt_session_add_torrent_buf(s, NULL, 1, NULL, NULL) == -EINVAL);
	assert(mbt_session_add_torrent_file(s, NULL, NULL, NULL) == -EINVAL);
	assert(mbt_session_add_magnet(s, NULL, NULL, NULL) == -EINVAL);
	assert(mbt_session_remove_torrent(s, NULL) == -EINVAL);
	assert(mbt_session_add_peer(s, NULL, "127.0.0.1", 1, MBT_SOURCE_MANUAL, 0) == -EINVAL);
	assert(mbt_session_add_peer(s, ih, NULL, 1, MBT_SOURCE_MANUAL, 0) == -EINVAL);
	assert(mbt_session_add_tracker(s, NULL, "udp://x:1") == -EINVAL);

	assert(mbt_session_add_infohash(s, ih, NULL, &t) == 0);
	assert(mbt_session_add_tracker(s, t, NULL) == -EINVAL);
	assert(mbt_torrent_set_metadata(t, NULL, 1) == -EINVAL);
	assert(mbt_torrent_set_storage(t, NULL, NULL) == -EINVAL);
	assert(mbt_torrent_read_piece(t, 0, NULL, 1) == -EINVAL);
	mbt_session_free(s);
	mbt_infohash_unref(ih);
}

static void
test_dht(struct mbt_ev *ev)
{
	struct mbt_dht_config cfg = {.ev = ev};
	struct mbt_dht_config no_ev = {0};
	struct mbt_be_value *args = NULL;
	struct mbt_dht *d = NULL;
	uint8_t target[20] = {0};

	assert(mbt_dht_new(NULL, &cfg) == -EINVAL);
	assert(mbt_dht_new(&d, NULL) == -EINVAL);
	assert(mbt_dht_new(&d, &no_ev) == -EINVAL);
	assert(mbt_dht_new(&d, &cfg) == 0);
	assert(mbt_be_dict_new(&args) == 0);

	assert(mbt_dht_bootstrap(d, NULL, 1) == -EINVAL);
	assert(mbt_dht_get_peers(d, NULL, NULL, NULL, NULL) == -EINVAL);
	assert(mbt_dht_announce(d, NULL, 1) == -EINVAL);
	assert(mbt_dht_query(d, NULL, 1, "ping", args, ignore_query, NULL) == -EINVAL);
	assert(mbt_dht_query(d, "127.0.0.1", 1, NULL, args, ignore_query, NULL) == -EINVAL);
	assert(mbt_dht_query(d, "127.0.0.1", 1, "ping", NULL, ignore_query, NULL) == -EINVAL);
	assert(mbt_dht_query(d, "127.0.0.1", 1, "ping", args, NULL, NULL) == -EINVAL);
	assert(mbt_dht_send_raw(d, NULL, 1, "x", 1) == -EINVAL);
	assert(mbt_dht_get_item(d, target, NULL, NULL) == -EINVAL);
	assert(mbt_dht_put_immutable(d, NULL, 1, NULL, NULL) == -EINVAL);
	mbt_be_unref(args);
	mbt_dht_free(d);
}

static void
test_parsers(void)
{
	struct mbt_be_value *be = NULL;
	struct mbt_metainfo *mi = NULL;
	struct mbt_magnet m;
	unsigned char *out;
	size_t len;

	assert(mbt_be_value_parse(NULL, 1, &be) == -EINVAL);
	assert(mbt_be_value_parse((const unsigned char *)"i1e", 3, NULL) == -EINVAL);
	assert(mbt_be_value_encode(NULL, &out, &len) == -EINVAL);
	assert(mbt_metainfo_parse(NULL, "x", 1) == -EINVAL);
	assert(mbt_metainfo_parse(&mi, NULL, 1) == -EINVAL);
	assert(mbt_magnet_parse(NULL, &m) == -EINVAL);
	assert(mbt_magnet_parse("magnet:", NULL) == -EINVAL);
	assert(mbt_infohash_new(NULL, MBT_INFOHASH_SHA1, "AB") == -EINVAL);
}

int
main(void)
{
	struct mbt_ev *ev;

	assert(mbt_ev_new_default(&ev) == 0);
	test_session(ev);
	test_dht(ev);
	test_parsers();
	mbt_ev_destroy(ev);
	fprintf(stderr, "api_args_test: PASS\n");
	return 0;
}
