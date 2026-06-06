#include <sys/stat.h>
#include <fcntl.h>
#include <unistd.h>
#include <stdlib.h>
#include <string.h>
#include <errno.h>
#include <stdio.h>

#include "storage.h"
#include "torrent.h"
#include "infohash.h"
#include "log.h"

/*
 * File storage backend.
 * ctx = (const char *) download directory.
 *
 * On open: creates dir/name (single-file) or dir/name/ (multi-file).
 * Per-torrent fd tracked by infohash in a simple list.
 */

#define MAX_TORRENTS 64

struct file_entry {
	struct mbt_infohash *ih;
	int fd;
	uint32_t piece_length;
};

static struct file_entry entries[MAX_TORRENTS];
static int entry_count;

static struct file_entry *
find_entry(struct mbt_torrent *t)
{
	struct mbt_infohash *ih = mbt_torrent_infohash(t);
	for (int i = 0; i < entry_count; i++) {
		if (mbt_infohash_cmp(entries[i].ih, ih) == 0)
			return &entries[i];
	}
	return NULL;
}

int
mbt_storage_file_open(void *ctx, struct mbt_torrent *t)
{
	const char *dir = ctx;
	const char *name = mbt_torrent_name(t);
	char path[4096];
	int fd, rv;

	if (find_entry(t))
		return 0;

	if (entry_count >= MAX_TORRENTS)
		return -ENOMEM;

	if (!name)
		name = "download";

	rv = snprintf(path, sizeof(path), "%s/%s", dir, name);
	if (rv < 0 || (size_t)rv >= sizeof(path))
		return -ENAMETOOLONG;

	fd = open(path, O_RDWR | O_CREAT, 0644);
	if (fd < 0)
		return -errno;

	entries[entry_count].ih = mbt_infohash_ref(mbt_torrent_infohash(t));
	entries[entry_count].fd = fd;
	entries[entry_count].piece_length = 0;
	entry_count++;

	mbt_log_info(MBT_LOG_STORAGE, "opened %s", path);
	return 0;
}

int
mbt_storage_file_write_piece(void *ctx, uint32_t index, const void *data, size_t len)
{
	(void)ctx;
	/* Caller passes ctx but we need the torrent to find fd.
	 * Workaround: for now use last opened entry (single-torrent case).
	 * TODO: proper per-torrent dispatch once interface supports it. */
	struct file_entry *e = &entries[entry_count - 1];
	off_t offset;
	ssize_t rv;

	if (e->piece_length == 0)
		e->piece_length = len;

	offset = (off_t)index * e->piece_length;

	rv = pwrite(e->fd, data, len, offset);
	if (rv < 0)
		return -errno;
	if ((size_t)rv != len)
		return -EIO;

	return 0;
}

int
mbt_storage_file_read_piece(void *ctx, uint32_t index, void *buf, size_t len)
{
	(void)ctx;
	struct file_entry *e = &entries[entry_count - 1];
	off_t offset;
	ssize_t rv;

	if (e->piece_length == 0)
		return -EINVAL;

	offset = (off_t)index * e->piece_length;

	rv = pread(e->fd, buf, len, offset);
	if (rv < 0)
		return -errno;
	if ((size_t)rv != len)
		return -EIO;

	return 0;
}

int
mbt_storage_file_have_piece(void *ctx, uint32_t index)
{
	(void)ctx;
	(void)index;
	return 0;
}

void
mbt_storage_file_close(void *ctx)
{
	(void)ctx;
	for (int i = 0; i < entry_count; i++) {
		close(entries[i].fd);
		mbt_infohash_unref(entries[i].ih);
	}
	entry_count = 0;
}

static const struct mbt_storage_ops file_ops = {
	.open        = mbt_storage_file_open,
	.write_piece = mbt_storage_file_write_piece,
	.read_piece  = mbt_storage_file_read_piece,
	.have_piece  = mbt_storage_file_have_piece,
	.close       = mbt_storage_file_close,
};

const struct mbt_storage_ops *
mbt_storage_file_ops(void)
{
	return &file_ops;
}
