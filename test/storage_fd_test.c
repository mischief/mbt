// SPDX-License-Identifier: ISC
/* Descriptor storage: rules at open, and a transfer between two descriptors. */
#include <assert.h>
#include <errno.h>
#include <fcntl.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/socket.h>
#include <sys/stat.h>
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
#define TOTAL_LEN (PIECE_LEN * NUM_PIECES - 100)
#define WAIT_STEPS 2000          /* 20 seconds at 10 ms */

static char tmpdir[] = "/tmp/mbt_fd_XXXXXX";
static uint8_t data[TOTAL_LEN];
static unsigned char *enc;
static size_t enc_len;
static unsigned char *multi_enc;
static size_t multi_len;
static struct mbt_ev *ev;

static int
temp_fd(const void *content, size_t len, int mode)
{
	char path[256];
	int fd;

	snprintf(path, sizeof(path), "%s/fd.XXXXXX", tmpdir);
	fd = mkstemp(path);
	assert(fd >= 0);
	if (len)
		assert(pwrite(fd, content, len, 0) == (ssize_t)len);
	if (mode != O_RDWR) {
		int rd = open(path, mode);

		assert(rd >= 0);
		close(fd);
		fd = rd;
	}
	assert(unlink(path) == 0);
	return fd;
}

static void
make_torrents(void)
{
	struct mbt_metainfo *mi;
	char path[300], sub[280];
	FILE *fp;

	for (size_t i = 0; i < sizeof(data); i++)
		data[i] = (uint8_t)(i * 31 + i / 251);
	snprintf(path, sizeof(path), "%s/data.bin", tmpdir);
	fp = fopen(path, "wb");
	assert(fp);
	assert(fwrite(data, 1, sizeof(data), fp) == sizeof(data));
	fclose(fp);
	assert(mbt_metainfo_new(&mi) == 0);
	assert(mbt_metainfo_set_content(mi, path, PIECE_LEN) == 0);
	assert(mbt_metainfo_encode(mi, &enc, &enc_len) == 0);
	mbt_metainfo_free(mi);

	snprintf(sub, sizeof(sub), "%s/multi", tmpdir);
	assert(mkdir(sub, 0700) == 0);
	for (int i = 0; i < 2; i++) {
		snprintf(path, sizeof(path), "%s/%d", sub, i);
		fp = fopen(path, "wb");
		assert(fp);
		assert(fwrite(data, 1, 100, fp) == 100);
		fclose(fp);
	}
	assert(mbt_metainfo_new(&mi) == 0);
	assert(mbt_metainfo_set_content(mi, sub, PIECE_LEN) == 0);
	assert(mbt_metainfo_encode(mi, &multi_enc, &multi_len) == 0);
	mbt_metainfo_free(mi);
}

static struct mbt_session *
new_session(void)
{
	struct mbt_session_config cfg = {.ev = ev, .disable_utp = 1};
	struct mbt_session *s;

	assert(mbt_session_new(&s, &cfg) == 0);
	return s;
}

/* Returns what add returned, with the storage dropped. t may be NULL. */
static int
add(struct mbt_session *s, const void *buf, size_t len, struct mbt_storage *st,
    struct mbt_torrent **t)
{
	struct mbt_add_torrent_cfg cfg = {.storage = st};
	int rv = mbt_session_add_torrent_buf(s, buf, len, &cfg, t);

	mbt_storage_unref(st);
	return rv;
}

static void
wait_verified(struct mbt_torrent *t)
{
	test_wait_verified(ev, t);
	for (int i = 0; i < 5; i++)
		mbt_ev_step(ev, 1);
}

static void
test_refused(void)
{
	struct mbt_session *s = new_session();
	struct mbt_storage *st;
	int fd;

	assert(mbt_storage_fd_new(-1, 0) == NULL);
	fd = temp_fd(NULL, 0, O_RDWR);
	assert(mbt_storage_fd_new(fd, 0x80) == NULL);
	/* NULL leaves fd open for the caller. */
	assert(fcntl(fd, F_GETFD) >= 0);

	assert((st = mbt_storage_fd_new(fd, 0)) != NULL);
	assert(add(s, multi_enc, multi_len, st, NULL) == -EINVAL);

	/* Read-only descriptor with no read-only flag. */
	assert((st = mbt_storage_fd_new(temp_fd(data, sizeof(data), O_RDONLY), 0)) != NULL);
	assert(add(s, enc, enc_len, st, NULL) == -EBADF);

	/* A read-only seed must have the exact size. */
	assert((st = mbt_storage_fd_new(temp_fd(data, 10, O_RDONLY),
	                                MBT_STORAGE_FD_READONLY)) != NULL);
	assert(add(s, enc, enc_len, st, NULL) == -EINVAL);

	/* A file above the torrent size. */
	static uint8_t big[TOTAL_LEN + 1];

	assert((st = mbt_storage_fd_new(temp_fd(big, sizeof(big), O_RDWR), 0)) != NULL);
	assert(add(s, enc, enc_len, st, NULL) == -EINVAL);

	fd = temp_fd(NULL, 0, O_RDWR);
	assert(fcntl(fd, F_SETFL, O_APPEND) == 0);
	assert((st = mbt_storage_fd_new(fd, 0)) != NULL);
	assert(add(s, enc, enc_len, st, NULL) == -EINVAL);

	int sv[2];

	assert(socketpair(AF_UNIX, SOCK_STREAM, 0, sv) == 0);
	assert((st = mbt_storage_fd_new(sv[0], 0)) != NULL);
	assert(add(s, enc, enc_len, st, NULL) == -EINVAL);
	close(sv[1]);
	mbt_session_free(s);
}

static void
test_second_open_and_close(void)
{
	struct mbt_session *s = new_session(), *s2 = new_session();
	int fd = temp_fd(NULL, 0, O_RDWR);
	int probe = dup(fd);
	struct mbt_storage *st = mbt_storage_fd_new(fd, 0);
	struct mbt_add_torrent_cfg cfg = {.storage = st};
	char c;

	assert(st && probe >= 0);
	assert(mbt_session_add_torrent_buf(s, enc, enc_len, &cfg, NULL) == 0);
	assert(mbt_session_add_torrent_buf(s2, enc, enc_len, &cfg, NULL) == -EBUSY);
	mbt_session_free(s2);
	/* A session reference holds the storage, so fd stays open. */
	mbt_storage_unref(st);
	assert(fcntl(fd, F_GETFD) >= 0);
	mbt_session_free(s);
	assert(fcntl(fd, F_GETFD) < 0 && errno == EBADF);
	assert(read(probe, &c, 1) == 0);
	close(probe);
}

/* Short file: verify skips what it cannot read, and the swarm supplies it. */
static void
test_transfer(void)
{
	struct mbt_session *a = new_session(), *b = new_session();
	struct mbt_torrent *ta, *tb;
	int lfd = temp_fd(data, PIECE_LEN, O_RDWR);
	int check = dup(lfd);
	static uint8_t got[TOTAL_LEN];

	assert(add(a, enc, enc_len, mbt_storage_fd_new(temp_fd(data, sizeof(data), O_RDONLY),
	                                               MBT_STORAGE_FD_READONLY), &ta) == 0);
	assert(add(b, enc, enc_len, mbt_storage_fd_new(lfd, 0), &tb) == 0);
	wait_verified(ta);
	wait_verified(tb);
	assert(mbt_torrent_have_count(ta) == NUM_PIECES);
	assert(mbt_torrent_have_count(tb) == 1);

	assert(mbt_session_add_peer(b, mbt_torrent_infohash(tb), "127.0.0.1",
	                            mbt_session_listen_port(a), MBT_SOURCE_MANUAL, 0) == 0);
	for (int i = 0; i < WAIT_STEPS && mbt_torrent_have_count(tb) < NUM_PIECES; i++)
		mbt_ev_step(ev, 10);
	assert(mbt_torrent_have_count(tb) == NUM_PIECES);
	assert(pread(check, got, sizeof(got), 0) == (ssize_t)sizeof(got));
	assert(memcmp(got, data, sizeof(got)) == 0);
	close(check);
	mbt_session_free(b);
	mbt_session_free(a);
}

/* A read-only seed with a bad piece keeps seeding and asks for nothing. */
static void
test_readonly_bad_piece(void)
{
	static uint8_t bad[TOTAL_LEN];
	struct mbt_session *a = new_session(), *b = new_session();
	struct mbt_torrent *ta, *tb;
	int rd;

	memcpy(bad, data, sizeof(bad));
	bad[PIECE_LEN + 5] ^= 0xff;
	rd = temp_fd(bad, sizeof(bad), O_RDONLY);
	assert(add(a, enc, enc_len, mbt_storage_fd_new(rd, MBT_STORAGE_FD_READONLY), &ta) == 0);
	assert(add(b, enc, enc_len, mbt_storage_fd_new(temp_fd(data, sizeof(data), O_RDONLY),
	                                               MBT_STORAGE_FD_READONLY), &tb) == 0);
	wait_verified(ta);
	wait_verified(tb);
	assert(mbt_torrent_have_count(ta) == NUM_PIECES - 1);
	assert(mbt_session_add_peer(a, mbt_torrent_infohash(ta), "127.0.0.1",
	                            mbt_session_listen_port(b), MBT_SOURCE_MANUAL, 0) == 0);
	for (int i = 0; i < 200; i++)
		mbt_ev_step(ev, 10);
	assert(mbt_torrent_peer_count(ta) >= 1);
	assert(mbt_torrent_have_count(ta) == NUM_PIECES - 1);
	assert(mbt_torrent_error(ta) == 0);
	mbt_session_free(b);
	mbt_session_free(a);
}

int
main(void)
{
	char cmd[300];

	assert(mkdtemp(tmpdir));
	assert(mbt_ev_new_default(&ev) == 0);
	make_torrents();
	test_refused();
	test_second_open_and_close();
	test_transfer();
	test_readonly_bad_piece();
	free(enc);
	free(multi_enc);
	mbt_ev_destroy(ev);
	snprintf(cmd, sizeof(cmd), "rm -rf %s", tmpdir);
	assert(system(cmd) == 0);
	puts("storage_fd: ok");
	return 0;
}
