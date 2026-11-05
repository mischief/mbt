// SPDX-License-Identifier: ISC
/* Moving a torrent between storages, and holding a torrent with no storage. */
#include <assert.h>
#include <errno.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>
#include <sys/wait.h>
#include <time.h>
#include <unistd.h>

#include "mbt/event.h"
#include "mbt/infohash.h"
#include "mbt/metainfo.h"
#include "mbt/session.h"
#include "mbt/state.h"
#include "mbt/storage.h"
#include "mbt/torrent.h"
#include "test_util.h"

#define PIECE_LEN 16384
#define NUM_PIECES 64
#define FILE_LEN (PIECE_LEN * NUM_PIECES - 100)
#define WAIT_SEC 20

static struct mbt_ev *ev;
static char tmpdir[] = "/tmp/mbt_move_XXXXXX";
static uint8_t data[FILE_LEN];
static unsigned char *enc;
static size_t enc_len;
static int moves;
static int last_err;

static void
path_of(char *p, size_t n, const char *dir, const char *name)
{
	snprintf(p, n, "%s/%s/%s", tmpdir, dir, name);
}

static void
mkdir_in_tmp(const char *name)
{
	char p[256];

	snprintf(p, sizeof(p), "%s/%s", tmpdir, name);
	assert(mkdir(p, 0755) == 0);
}

static int
file_there(const char *dir, const char *name)
{
	char p[256];

	path_of(p, sizeof(p), dir, name);
	return access(p, F_OK) == 0;
}

static void
write_data(const char *dir)
{
	char p[256];
	FILE *fp;

	path_of(p, sizeof(p), dir, "m.bin");
	assert((fp = fopen(p, "wb")) != NULL);
	assert(fwrite(data, 1, FILE_LEN, fp) == FILE_LEN);
	fclose(fp);
}

static void
check_data(const char *dir)
{
	static uint8_t got[FILE_LEN];
	char p[256];
	FILE *fp;

	path_of(p, sizeof(p), dir, "m.bin");
	assert((fp = fopen(p, "rb")) != NULL);
	assert(fread(got, 1, FILE_LEN, fp) == FILE_LEN);
	fclose(fp);
	assert(memcmp(got, data, FILE_LEN) == 0);
}

static struct mbt_storage *
storage_in(const char *dir)
{
	char p[256];
	struct mbt_storage *st;

	snprintf(p, sizeof(p), "%s/%s", tmpdir, dir);
	assert((st = mbt_storage_file_new(p)) != NULL);
	return st;
}

static struct mbt_session *
session_new(void)
{
	struct mbt_session_config cfg = {.ev = ev, .disable_utp = 1};
	struct mbt_session *s;

	assert(mbt_session_new(&s, &cfg) == 0);
	return s;
}

static struct mbt_torrent *
add_on(struct mbt_session *s, const char *dir)
{
	struct mbt_storage *st = storage_in(dir);
	struct mbt_add_torrent_cfg cfg = {.storage = st};
	struct mbt_torrent *t = NULL;

	assert(mbt_session_add_torrent_buf(s, enc, enc_len, &cfg, &t) == 0);
	mbt_storage_unref(st);
	test_wait_verified(ev, t);
	return t;
}

static void
move_cb(struct mbt_torrent *t, int err, void *arg)
{
	(void)t; (void)arg;
	moves++;
	last_err = err;
}

static void
wait_move(void)
{
	time_t end = time(NULL) + WAIT_SEC;

	while (moves == 0) {
		assert(time(NULL) < end);
		test_ev_step(ev, 10);
	}
	assert(moves == 1);
	moves = 0;
}

static void
wait_complete(struct mbt_torrent *t)
{
	time_t end = time(NULL) + WAIT_SEC;

	while (!mbt_torrent_complete(t)) {
		assert(time(NULL) < end);
		test_ev_step(ev, 10);
	}
}

/* A complete torrent moves, and still seeds from the new place. */
static void
test_move_complete(void)
{
	struct mbt_session *s = session_new(), *leech = session_new();
	struct mbt_torrent *t, *lt = NULL;
	struct mbt_storage *dst;

	mkdir_in_tmp("a");
	mkdir_in_tmp("b");
	mkdir_in_tmp("seen");
	write_data("a");
	t = add_on(s, "a");
	assert(mbt_torrent_complete(t));

	dst = storage_in("b");
	assert(mbt_torrent_move_storage(t, dst, "newdir", move_cb, NULL) == 0);
	assert(mbt_torrent_move_storage(t, dst, NULL, move_cb, NULL) == -EBUSY);
	mbt_storage_unref(dst);
	wait_move();
	assert(last_err == 0);
	assert(!file_there("a", "m.bin"));
	check_data("b");
	assert(strcmp(mbt_torrent_data_dir(t), "newdir") == 0);
	assert(mbt_torrent_state(t) == MBT_TORRENT_SEEDING);

	lt = add_on(leech, "seen");
	assert(mbt_session_add_peer(leech, mbt_torrent_infohash(lt), "127.0.0.1",
	    mbt_session_listen_port(s), MBT_SOURCE_MANUAL, 0) == 0);
	wait_complete(lt);
	check_data("seen");
	mbt_session_free(leech);
	mbt_session_free(s);
}

/* Data at the target is not ours to touch, and one storage is no move. */
static void
test_refused(void)
{
	struct mbt_session *s = session_new();
	struct mbt_torrent *t;
	struct mbt_storage *same, *other;

	mkdir_in_tmp("c");
	mkdir_in_tmp("taken");
	write_data("c");
	write_data("taken");
	same = storage_in("c");
	struct mbt_add_torrent_cfg cfg = {.storage = same};
	assert(mbt_session_add_torrent_buf(s, enc, enc_len, &cfg, &t) == 0);
	test_wait_verified(ev, t);
	assert(mbt_torrent_move_storage(t, same, NULL, move_cb, NULL) == -EINVAL);
	other = storage_in("taken");
	assert(mbt_torrent_move_storage(t, other, NULL, move_cb, NULL) == -EEXIST);
	mbt_storage_unref(other);
	mbt_storage_unref(same);
	check_data("taken");
	check_data("c");
	mbt_session_free(s);
}

/* A storage that fails after some writes, or reads back wrong data. */
static int writes_left;
static int corrupt;
static int removed;
static int sync_fail;
static int synced;
static int saves;
static int save_fail;
static char *src_probe;

static int
bad_open(void *ctx, struct mbt_torrent *t, struct mbt_storage_handle **out)
{
	(void)t;
	*out = ctx;
	return 0;
}

static int
bad_write(struct mbt_storage_handle *h, uint32_t p, const void *d, size_t l)
{
	(void)h; (void)p; (void)d; (void)l;
	return writes_left-- > 0 ? 0 : -ENOSPC;
}

static int
bad_read(struct mbt_storage_handle *h, uint32_t p, void *buf, size_t len)
{
	(void)h; (void)p;
	memset(buf, corrupt, len);
	return 0;
}

static int
good_read(struct mbt_storage_handle *h, uint32_t p, void *buf, size_t len)
{
	(void)h;
	memcpy(buf, data + (size_t)p * PIECE_LEN, len);
	return 0;
}

static int
good_write(struct mbt_storage_handle *h, uint32_t p, const void *d, size_t l)
{
	(void)h; (void)p; (void)d; (void)l;
	return 0;
}

static int
do_sync(struct mbt_storage_handle *h)
{
	(void)h;
	synced++;
	return sync_fail;
}

/* A durable save must come after sync and before the old data goes. */
static int
state_save_sync(void *ctx, struct mbt_torrent *t)
{
	(void)ctx; (void)t;
	assert(synced == 1);
	assert(access(src_probe, F_OK) == 0);
	saves++;
	return save_fail;
}

static int
state_save_plain(void *ctx, struct mbt_torrent *t)
{
	(void)ctx; (void)t;
	return 0;
}

static const struct mbt_state_ops order_state = {
	.save = state_save_plain, .save_sync = state_save_sync,
};

static int
bad_remove(struct mbt_storage_handle *h)
{
	(void)h;
	removed++;
	return 0;
}

static void
bad_close(struct mbt_storage_handle *h)
{
	(void)h;
}

static const struct mbt_storage_ops bad_ops = {
	.open = bad_open, .write_piece = bad_write, .read_piece = bad_read,
	.remove = bad_remove, .close = bad_close,
};

static const struct mbt_storage_ops sync_ops = {
	.open = bad_open, .write_piece = good_write, .read_piece = good_read,
	.remove = bad_remove, .close = bad_close, .sync = do_sync,
};

static const struct mbt_storage_ops no_remove_ops = {
	.open = bad_open, .write_piece = bad_write, .read_piece = bad_read,
	.close = bad_close,
};

static void
test_failures(void)
{
	static char handle;
	struct mbt_session *s = session_new();
	struct mbt_torrent *t;
	struct mbt_storage *bad = mbt_storage_new(&bad_ops, &handle);
	struct mbt_storage *lacking = mbt_storage_new(&no_remove_ops, &handle);

	mkdir_in_tmp("d");
	write_data("d");
	t = add_on(s, "d");

	assert(mbt_torrent_move_storage(t, lacking, NULL, move_cb, NULL) == -ENOTSUP);
	removed = 0;

	writes_left = 0;
	assert(mbt_torrent_move_storage(t, bad, NULL, move_cb, NULL) == 0);
	wait_move();
	assert(last_err == -ENOSPC);
	assert(removed == 1);
	check_data("d");

	writes_left = NUM_PIECES;
	corrupt = 0x55;
	assert(mbt_torrent_move_storage(t, bad, NULL, move_cb, NULL) == 0);
	wait_move();
	assert(last_err == -EIO);
	assert(removed == 2);
	check_data("d");
	assert(mbt_torrent_error(t) == 0);
	assert(mbt_torrent_state(t) == MBT_TORRENT_SEEDING);

	/* Removal ends a move at once. */
	writes_left = NUM_PIECES;
	assert(mbt_torrent_move_storage(t, bad, NULL, move_cb, NULL) == 0);
	assert(mbt_session_remove_torrent(s, mbt_torrent_infohash(t)) == 0);
	assert(moves == 1 && last_err == -ECANCELED);
	moves = 0;
	assert(removed == 3);
	check_data("d");

	mbt_storage_unref(bad);
	mbt_storage_unref(lacking);
	mbt_session_free(s);
}

/* The commit order, and a failed sync or save at each step. */
static void
test_commit_order(void)
{
	static char handle;
	struct mbt_session_config cfg = {
		.ev = ev, .disable_utp = 1, .state_ops = &order_state,
	};
	struct mbt_session *s;
	struct mbt_torrent *t;
	struct mbt_storage *dst = mbt_storage_new(&sync_ops, &handle);
	char probe[256];

	assert(mbt_session_new(&s, &cfg) == 0);
	mkdir_in_tmp("order");
	write_data("order");
	path_of(probe, sizeof(probe), "order", "m.bin");
	src_probe = probe;
	t = add_on(s, "order");
	removed = 0;

	sync_fail = -EIO;
	assert(mbt_torrent_move_storage(t, dst, NULL, move_cb, NULL) == 0);
	wait_move();
	assert(last_err == -EIO && saves == 0 && removed == 1);
	check_data("order");

	sync_fail = 0;
	synced = 0;
	save_fail = -ENOSPC;
	assert(mbt_torrent_move_storage(t, dst, "kept", move_cb, NULL) == 0);
	wait_move();
	assert(last_err == -ENOSPC && saves == 1 && removed == 2);
	assert(mbt_torrent_data_dir(t) == NULL ||
	    strcmp(mbt_torrent_data_dir(t), "kept") != 0);
	check_data("order");

	save_fail = 0;
	synced = 0;
	saves = 0;
	assert(mbt_torrent_move_storage(t, dst, NULL, move_cb, NULL) == 0);
	wait_move();
	assert(last_err == 0 && saves == 1);
	assert(!file_there("order", "m.bin"));
	mbt_storage_unref(dst);
	mbt_session_free(s);
}

/* A process that dies in a move leaves the source whole, and a retry works. */
static void
crash_child(void)
{
	struct mbt_session *s;
	struct mbt_torrent *t;
	struct mbt_storage *dst;

	assert(mbt_ev_new_default(&ev) == 0);
	s = session_new();
	t = add_on(s, "crash_a");
	dst = storage_in("crash_b");
	assert(mbt_torrent_move_storage(t, dst, NULL, move_cb, NULL) == 0);
	_exit(0);
}

static void
crash_in_move(void)
{
	pid_t pid;
	int status;

	mkdir_in_tmp("crash_a");
	mkdir_in_tmp("crash_b");
	write_data("crash_a");
	assert((pid = fork()) >= 0);
	if (pid == 0)
		crash_child();
	assert(waitpid(pid, &status, 0) == pid);
	assert(WIFEXITED(status) && WEXITSTATUS(status) == 0);
}

static void
test_retry_after_crash(void)
{
	struct mbt_session *s = session_new();
	struct mbt_torrent *t;
	struct mbt_storage *dst;

	assert(file_there("crash_b", "m.bin"));
	check_data("crash_a");
	t = add_on(s, "crash_a");
	dst = storage_in("crash_b");
	assert(mbt_torrent_move_storage(t, dst, NULL, move_cb, NULL) == 0);
	mbt_storage_unref(dst);
	wait_move();
	assert(last_err == 0);
	check_data("crash_b");
	assert(!file_there("crash_a", "m.bin"));

	char hex[41], name[64];

	mbt_infohash_string(mbt_torrent_infohash(t), hex, sizeof(hex));
	snprintf(name, sizeof(name), ".mbt-move-%s", hex);
	assert(!file_there("crash_b", name));
	mbt_session_free(s);
}

/* A move while pieces arrive. All pieces end up on the target. */
static void
test_move_while_downloading(void)
{
	struct mbt_session *seed = session_new(), *leech = session_new();
	struct mbt_torrent *st, *lt;
	struct mbt_storage *dst;
	time_t end = time(NULL) + WAIT_SEC;

	mkdir_in_tmp("e");
	mkdir_in_tmp("f");
	mkdir_in_tmp("g");
	write_data("e");
	st = add_on(seed, "e");
	assert(mbt_torrent_complete(st));
	lt = add_on(leech, "f");
	assert(mbt_session_add_peer(leech, mbt_torrent_infohash(lt), "127.0.0.1",
	    mbt_session_listen_port(seed), MBT_SOURCE_MANUAL, 0) == 0);
	while (mbt_torrent_have_count(lt) == 0) {
		assert(time(NULL) < end);
		test_ev_step(ev, 1);
	}
	dst = storage_in("g");
	assert(mbt_torrent_move_storage(lt, dst, NULL, move_cb, NULL) == 0);
	mbt_storage_unref(dst);
	wait_move();
	assert(last_err == 0);
	wait_complete(lt);
	check_data("g");
	assert(!file_there("f", "m.bin"));
	mbt_session_free(leech);
	mbt_session_free(seed);
}

/* A magnet with no storage gets metadata, holds, then downloads once attached. */
static void
test_hold(void)
{
	struct mbt_session *seed = session_new(), *s = session_new();
	struct mbt_add_torrent_cfg cfg = {.no_storage = 1};
	struct mbt_torrent *st, *t = NULL;
	struct mbt_storage *late;
	char hex[41], uri[128];
	time_t end = time(NULL) + WAIT_SEC;

	mkdir_in_tmp("h");
	mkdir_in_tmp("i");
	write_data("h");
	st = add_on(seed, "h");
	mbt_infohash_string(mbt_torrent_infohash(st), hex, sizeof(hex));
	snprintf(uri, sizeof(uri), "magnet:?xt=urn:btih:%s&x.pe=127.0.0.1:%hu", hex,
	    mbt_session_listen_port(seed));
	assert(mbt_session_add_magnet(s, uri, &cfg, &t) == 0);
	while (mbt_torrent_state(t) != MBT_TORRENT_NO_STORAGE) {
		assert(time(NULL) < end);
		test_ev_step(ev, 10);
	}
	for (int i = 0; i < 20; i++)
		test_ev_step(ev, 5);
	assert(mbt_torrent_have_count(t) == 0);

	late = storage_in("i");
	assert(mbt_torrent_set_storage(t, late, "laterdir") == 0);
	assert(mbt_torrent_set_storage(t, late, NULL) == -EBUSY);
	mbt_storage_unref(late);
	wait_complete(t);
	check_data("i");
	assert(strcmp(mbt_torrent_data_dir(t), "laterdir") == 0);
	mbt_session_free(s);
	mbt_session_free(seed);
}

static void
make_torrent(void)
{
	struct mbt_metainfo *mi;
	char p[256];

	mkdir_in_tmp("src");
	write_data("src");
	path_of(p, sizeof(p), "src", "m.bin");
	assert(mbt_metainfo_new(&mi) == 0);
	assert(mbt_metainfo_set_content(mi, p, PIECE_LEN) == 0);
	assert(mbt_metainfo_encode(mi, &enc, &enc_len) == 0);
	mbt_metainfo_free(mi);
}

int
main(void)
{
	char cmd[300];

	for (int i = 0; i < FILE_LEN; i++)
		data[i] = (uint8_t)(i * 17 + (i >> 8));
	assert(mkdtemp(tmpdir));
	make_torrent();
	crash_in_move();
	assert(mbt_ev_new_default(&ev) == 0);
	test_move_complete();
	test_commit_order();
	test_retry_after_crash();
	test_refused();
	test_failures();
	test_move_while_downloading();
	test_hold();
	free(enc);
	mbt_ev_destroy(ev);
	snprintf(cmd, sizeof(cmd), "rm -rf %s", tmpdir);
	assert(system(cmd) == 0);
	fprintf(stderr, "storage_move_test: PASS\n");
	return 0;
}
