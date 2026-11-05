// SPDX-License-Identifier: ISC
/* Several torrents on one storage: transfers, removal and path clashes. */
#include <assert.h>
#include <errno.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>
#include <time.h>
#include <unistd.h>

#include "mbt/event.h"
#include "mbt/log.h"
#include "mbt/metainfo.h"
#include "mbt/session.h"
#include "mbt/storage.h"
#include "mbt/torrent.h"
#include "test_util.h"

#define PIECE_LEN 16384
#define FILE_LEN (PIECE_LEN * 3 + 100)
#define WAIT_SEC 20

static struct mbt_ev *ev;
static char tmpdir[] = "/tmp/mbt_shared_XXXXXX";

struct fixture {
	struct mbt_metainfo *mi;
	unsigned char *enc;
	size_t enc_len;
	uint8_t data[FILE_LEN];
};

static char *
path_of(const char *a, const char *b)
{
	static char buf[4][256];
	static int next;
	char *p = buf[next++ % 4];

	snprintf(p, sizeof(buf[0]), "%s/%s/%s", tmpdir, a, b);
	return p;
}

static void
mkdir_in_tmp(const char *name)
{
	char p[256];

	snprintf(p, sizeof(p), "%s/%s", tmpdir, name);
	assert(mkdir(p, 0755) == 0);
}

static void
fixture_make(struct fixture *f, const char *dir, const char *name, int seed)
{
	char *path = path_of(dir, name);
	FILE *fp;

	for (int i = 0; i < FILE_LEN; i++)
		f->data[i] = (uint8_t)(i * 7 + seed);
	fp = fopen(path, "wb");
	assert(fp);
	assert(fwrite(f->data, 1, FILE_LEN, fp) == FILE_LEN);
	fclose(fp);
	assert(mbt_metainfo_new(&f->mi) == 0);
	assert(mbt_metainfo_set_content(f->mi, path, PIECE_LEN) == 0);
	assert(mbt_metainfo_encode(f->mi, &f->enc, &f->enc_len) == 0);
}

static void
fixture_free(struct fixture *f)
{
	free(f->enc);
	mbt_metainfo_free(f->mi);
}

static struct mbt_session *
session_on(struct mbt_storage *st)
{
	struct mbt_session_config cfg = {.ev = ev, .default_storage = st};
	struct mbt_session *s;

	assert(mbt_session_new(&s, &cfg) == 0);
	return s;
}

static struct mbt_torrent *
add(struct mbt_session *s, struct fixture *f)
{
	struct mbt_torrent *t;

	assert(mbt_session_add_torrent_buf(s, f->enc, f->enc_len, NULL) == 0);
	t = mbt_session_find_torrent(s, mbt_metainfo_infohash(f->mi));
	assert(t);
	return t;
}

static void
fetch(struct mbt_session *s, struct mbt_session *from, struct mbt_torrent **ts, int n)
{
	time_t end = time(NULL) + WAIT_SEC;

	for (int i = 0; i < n; i++)
		assert(mbt_session_add_peer(s, mbt_torrent_infohash(ts[i]), "127.0.0.1",
		    mbt_session_listen_port(from), MBT_SOURCE_MANUAL, 0) == 0);
	for (;;) {
		int done = 0;

		for (int i = 0; i < n; i++)
			done += mbt_torrent_complete(ts[i]) != 0;
		if (done == n)
			return;
		assert(time(NULL) < end);
		test_ev_step(ev, 10);
	}
}

static void
check_file(const char *dir, const char *name, const uint8_t *want)
{
	static uint8_t got[FILE_LEN];
	FILE *fp = fopen(path_of(dir, name), "rb");

	assert(fp);
	assert(fread(got, 1, FILE_LEN, fp) == FILE_LEN);
	fclose(fp);
	assert(memcmp(got, want, FILE_LEN) == 0);
}

/* Two torrents share a seed storage and a leech storage. Removing one
 * must leave the other readable. */
static void
test_transfer(void)
{
	static struct fixture a, b;
	char p[256];

	mkdir_in_tmp("seed");
	mkdir_in_tmp("leech");
	mkdir_in_tmp("late");
	fixture_make(&a, "seed", "a.bin", 1);
	fixture_make(&b, "seed", "b.bin", 2);

	snprintf(p, sizeof(p), "%s/seed", tmpdir);
	struct mbt_storage *seed_st = mbt_storage_file_new(p);
	snprintf(p, sizeof(p), "%s/leech", tmpdir);
	struct mbt_storage *leech_st = mbt_storage_file_new(p);
	snprintf(p, sizeof(p), "%s/late", tmpdir);
	struct mbt_storage *late_st = mbt_storage_file_new(p);
	assert(seed_st && leech_st && late_st);

	struct mbt_session *seed = session_on(seed_st);
	struct mbt_session *leech = session_on(leech_st);
	struct mbt_session *late = session_on(late_st);
	mbt_storage_unref(seed_st);
	mbt_storage_unref(leech_st);
	mbt_storage_unref(late_st);

	struct mbt_torrent *sa = add(seed, &a), *sb = add(seed, &b);
	test_wait_verified(ev, sa);
	test_wait_verified(ev, sb);
	assert(mbt_torrent_complete(sa));
	assert(mbt_torrent_complete(sb));

	struct mbt_torrent *ts[2] = {add(leech, &a), add(leech, &b)};
	fetch(leech, seed, ts, 2);
	check_file("leech", "a.bin", a.data);
	check_file("leech", "b.bin", b.data);

	assert(mbt_session_remove_torrent(seed, mbt_metainfo_infohash(a.mi)) == 0);
	ts[0] = add(late, &b);
	fetch(late, seed, ts, 1);
	check_file("late", "b.bin", b.data);

	mbt_session_free(late);
	mbt_session_free(leech);
	mbt_session_free(seed);
	fixture_free(&a);
	fixture_free(&b);
}

/* One file path, two torrents: the second open fails until the first ends. */
static void
test_path_clash(void)
{
	static struct fixture a, b;
	struct mbt_storage_handle *h1 = NULL, *h2 = NULL;
	char p[256];

	mkdir_in_tmp("one");
	mkdir_in_tmp("two");
	mkdir_in_tmp("clash");
	fixture_make(&a, "one", "x.bin", 3);
	fixture_make(&b, "two", "x.bin", 4);

	struct mbt_session *s = session_on(NULL);
	struct mbt_torrent *ta = add(s, &a), *tb = add(s, &b);

	snprintf(p, sizeof(p), "%s/clash", tmpdir);
	struct mbt_storage *st = mbt_storage_file_new(p);
	assert(st);
	assert(st->ops->open(st->ctx, ta, &h1) == 0);
	assert(st->ops->open(st->ctx, tb, &h2) == -EEXIST);
	assert(h2 == NULL);
	st->ops->close(h1);
	assert(st->ops->open(st->ctx, tb, &h2) == 0);
	st->ops->close(h2);
	mbt_storage_unref(st);

	struct mbt_storage_memory *m = mbt_storage_memory_new();
	struct mbt_storage *ms = mbt_storage_memory_storage(m);
	h1 = h2 = NULL;
	assert(ms->ops->open(ms->ctx, ta, &h1) == 0);
	assert(ms->ops->open(ms->ctx, tb, &h2) == -EBUSY);
	ms->ops->close(h1);
	assert(ms->ops->open(ms->ctx, tb, &h2) == 0);
	ms->ops->close(h2);
	mbt_storage_memory_destroy(m);

	mbt_session_free(s);
	fixture_free(&a);
	fixture_free(&b);
}

int
main(void)
{
	char cmd[300];

	assert(mkdtemp(tmpdir));
	assert(mbt_ev_new_default(&ev) == 0);
	test_transfer();
	test_path_clash();
	mbt_ev_destroy(ev);
	snprintf(cmd, sizeof(cmd), "rm -rf %s", tmpdir);
	assert(system(cmd) == 0);
	fprintf(stderr, "storage_shared_test: PASS\n");
	return 0;
}
