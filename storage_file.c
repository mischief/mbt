// SPDX-License-Identifier: ISC
#include <sys/stat.h>
#include <fcntl.h>
#include <unistd.h>
#include <stdlib.h>
#include <string.h>
#include <errno.h>
#include <stdio.h>
#include <libgen.h>

#include "mbt/storage.h"
#include "mbt/torrent.h"
#include "mbt/infohash.h"
#include "mbt/log.h"

/*
 * File storage backend.
 * ctx = (const char *) download directory.
 *
 * Single-file: dir/name
 * Multi-file: dir/torrent_name/path/to/file
 *
 * Pieces are written to the correct file(s) based on the file table.
 */

struct file_entry {
	int fd;
	uint64_t offset;  /* global offset in torrent data */
	uint64_t length;
};

struct file_storage {
	struct file_entry *entries;
	int count;
	uint32_t piece_length;
};

static struct file_storage fs = {0};

static int
mkdirs(const char *path)
{
	char *tmp = strdup(path);
	char *dir = dirname(tmp);
	int rv = 0;
	if (strlen(dir) > 1) {
		char *parent = strdup(dir);
		rv = mkdirs(parent);
		free(parent);
		if (rv < 0) { free(tmp); return rv; }
		if (mkdir(dir, 0755) < 0 && errno != EEXIST) {
			rv = -errno;
			free(tmp);
			return rv;
		}
	}
	free(tmp);
	return 0;
}

int
mbt_storage_file_open(void *ctx, struct mbt_torrent *t)
{
	const char *dir = ctx;
	int fc = mbt_torrent_file_count(t);
	const char *name = mbt_torrent_name(t);

	if (fs.entries)
		return 0; /* already open */

	if (fc == 0)
		return -EINVAL;

	fs.entries = calloc(fc, sizeof(struct file_entry));
	if (!fs.entries)
		return -ENOMEM;
	fs.count = fc;
	fs.piece_length = mbt_torrent_piece_length(t);

	for (int i = 0; i < fc; i++) {
		const struct mbt_file *f = mbt_torrent_file(t, i);
		char path[4096];

		if (fc == 1) {
			/* Single-file: dir/filename */
			snprintf(path, sizeof(path), "%s/%s", dir, f->path);
		} else {
			/* Multi-file: dir/torrent_name/file_path */
			snprintf(path, sizeof(path), "%s/%s/%s", dir,
			         name ? name : "download", f->path);
		}

		int rv2 = mkdirs(path);
		if (rv2 < 0) {
			mbt_log_warn(MBT_LOG_STORAGE, "mkdir for %s: %s", path, strerror(-rv2));
			return rv2;
		}

		fs.entries[i].fd = open(path, O_RDWR | O_CREAT, 0644);
		if (fs.entries[i].fd < 0) {
			mbt_log_warn(MBT_LOG_STORAGE, "open %s: %s", path, strerror(errno));
			return -errno;
		}

		fs.entries[i].offset = f->offset;
		fs.entries[i].length = f->length;
	}

	mbt_log_info(MBT_LOG_STORAGE, "opened %d file(s) in %s", fc, dir);
	return 0;
}

int
mbt_storage_file_write_piece(void *ctx, uint32_t index, const void *data, size_t len)
{
	(void)ctx;
	uint64_t piece_offset = (uint64_t)index * fs.piece_length;
	const uint8_t *p = data;
	size_t remaining = len;

	/* Find which file(s) this piece spans */
	for (int i = 0; i < fs.count && remaining > 0; i++) {
		struct file_entry *e = &fs.entries[i];
		uint64_t file_end = e->offset + e->length;

		/* Does this file overlap with the piece? */
		if (piece_offset >= file_end)
			continue;
		if (piece_offset + remaining <= e->offset)
			break;

		/* Compute write offset within this file */
		uint64_t write_start = piece_offset > e->offset ? piece_offset - e->offset : 0;
		uint64_t data_start = piece_offset > e->offset ? 0 : e->offset - piece_offset;
		uint64_t can_write = e->length - write_start;
		size_t to_write = remaining - data_start;
		if (to_write > can_write)
			to_write = can_write;

		ssize_t rv = pwrite(e->fd, p + data_start, to_write, write_start);
		if (rv < 0)
			return -errno;

		remaining -= (data_start + to_write);
		piece_offset += (data_start + to_write);
		p += (data_start + to_write);
	}

	return 0;
}

int
mbt_storage_file_read_piece(void *ctx, uint32_t index, void *buf, size_t len)
{
	(void)ctx;
	uint64_t piece_offset = (uint64_t)index * fs.piece_length;
	uint8_t *p = buf;
	size_t remaining = len;

	for (int i = 0; i < fs.count && remaining > 0; i++) {
		struct file_entry *e = &fs.entries[i];
		uint64_t file_end = e->offset + e->length;

		if (piece_offset >= file_end)
			continue;
		if (piece_offset + remaining <= e->offset)
			break;

		uint64_t read_start = piece_offset > e->offset ? piece_offset - e->offset : 0;
		uint64_t data_start = piece_offset > e->offset ? 0 : e->offset - piece_offset;
		uint64_t can_read = e->length - read_start;
		size_t to_read = remaining - data_start;
		if (to_read > can_read)
			to_read = can_read;

		ssize_t rv = pread(e->fd, p + data_start, to_read, read_start);
		if (rv < 0)
			return -errno;
		if ((size_t)rv < to_read)
			return -EIO; /* short read — file truncated or not yet written */

		remaining -= (data_start + to_read);
		piece_offset += (data_start + to_read);
		p += (data_start + to_read);
	}

	return 0;
}

int
mbt_storage_file_have_piece(void *ctx, uint32_t index)
{
	(void)ctx;
	(void)index;
	return 0; /* no resume support yet */
}

void
mbt_storage_file_close(void *ctx)
{
	(void)ctx;
	if (fs.entries) {
		for (int i = 0; i < fs.count; i++)
			if (fs.entries[i].fd >= 0)
				close(fs.entries[i].fd);
		free(fs.entries);
		fs.entries = NULL;
		fs.count = 0;
	}
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
