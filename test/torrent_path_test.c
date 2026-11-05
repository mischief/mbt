// SPDX-License-Identifier: ISC
/* Metadata with file paths that leave the data directory is refused. */
#include <assert.h>
#include <errno.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>

#include "mbt/be.h"
#include "mbt/event.h"
#include "mbt/infohash.h"
#include "mbt/storage.h"
#include "mbt/torrent.h"

#define PIECE_LEN 16384

static struct mbt_ev *ev;
static struct mbt_storage *storage;
static char tmpdir[] = "/tmp/mbt_path_XXXXXX";

/* comps is NULL for a single-file torrent. */
static int
try_metadata(const char *name, const char *const *comps, int ncomps, int *has)
{
	static const uint8_t peer_id[20];
	uint8_t hashes[20] = {0};
	struct mbt_be_value *info = NULL, *files, *file, *path, *c;
	struct mbt_infohash *ih = NULL;
	struct mbt_torrent *t = NULL;
	unsigned char *enc = NULL;
	size_t enc_len = 0;
	int rv;

	assert(mbt_be_dict_new(&info) == 0);
	assert(mbt_be_dict_append_string(info, "name", name, strlen(name)) == 0);
	assert(mbt_be_dict_append_number(info, "piece length", PIECE_LEN) == 0);
	assert(mbt_be_dict_append_string(info, "pieces", hashes, sizeof(hashes)) == 0);
	if (comps == NULL) {
		assert(mbt_be_dict_append_number(info, "length", 100) == 0);
	} else {
		assert(mbt_be_list_new(&files) == 0);
		assert(mbt_be_dict_new(&file) == 0);
		assert(mbt_be_list_new(&path) == 0);
		for (int i = 0; i < ncomps; i++) {
			assert(mbt_be_string_new(&c, comps[i], strlen(comps[i])) == 0);
			mbt_be_list_append(path, c);
			mbt_be_unref(c);
		}
		assert(mbt_be_dict_append_number(file, "length", 100) == 0);
		assert(mbt_be_dict_append(file, "path", path) == 0);
		mbt_be_list_append(files, file);
		assert(mbt_be_dict_append(info, "files", files) == 0);
		mbt_be_unref(path);
		mbt_be_unref(file);
		mbt_be_unref(files);
	}
	assert(mbt_be_value_encode(info, &enc, &enc_len) == 0);
	mbt_be_unref(info);

	assert(mbt_infohash_new(&ih, MBT_INFOHASH_SHA1,
	    "ABABABABABABABABABABABABABABABABABABABAB") == 0);
	struct mbt_torrent_config cfg = {
		.ev = ev, .ih = ih, .peer_id = peer_id, .storage = storage,
	};
	assert(mbt_torrent_new(&t, &cfg) == 0);
	rv = mbt_torrent_set_metadata(t, enc, enc_len);
	*has = mbt_torrent_has_metadata(t);
	assert(*has || mbt_torrent_file_count(t) == 0);
	mbt_torrent_free(t);
	mbt_infohash_unref(ih);
	free(enc);
	return rv;
}

static void
want_refused(const char *name, const char *const *comps, int ncomps)
{
	int has = 1;

	assert(try_metadata(name, comps, ncomps, &has) == -EINVAL);
	assert(!has);
}

int
main(void)
{
	static const char *const dots[] = {"..", "..", "evil"};
	static const char *const embedded[] = {"a/../../evil"};
	static const char *const absolute[] = {"/tmp/evil"};
	static const char *const dot[] = {".", "evil"};
	static const char *const empty[] = {""};
	static const char *const inner[] = {"a", "..", "evil"};
	static const char *const good[] = {"dir", "file..name"};
	char cmd[300], p[256];
	int has = 0;

	assert(mkdtemp(tmpdir));
	assert(mbt_ev_new_default(&ev) == 0);
	storage = mbt_storage_file_new(tmpdir);
	assert(storage);

	want_refused("../evil", NULL, 0);
	want_refused("/tmp/evil", NULL, 0);
	want_refused("a/../../evil", NULL, 0);
	want_refused("..", NULL, 0);
	want_refused("ok", dots, 3);
	want_refused("ok", embedded, 1);
	want_refused("ok", absolute, 1);
	want_refused("ok", dot, 2);
	want_refused("ok", empty, 1);
	want_refused("ok", inner, 3);
	want_refused("../evil", good, 2);

	assert(try_metadata("plain..name", NULL, 0, &has) == 0);
	assert(has);
	assert(try_metadata("multi", good, 2, &has) == 0);
	assert(has);
	snprintf(p, sizeof(p), "%s/dir/file..name", tmpdir);
	assert(access(p, F_OK) == 0);

	mbt_storage_unref(storage);
	mbt_ev_destroy(ev);
	snprintf(cmd, sizeof(cmd), "rm -rf %s", tmpdir);
	assert(system(cmd) == 0);
	fprintf(stderr, "torrent_path_test: PASS\n");
	return 0;
}
