// SPDX-License-Identifier: ISC
#include <sys/stat.h>
#include <fcntl.h>
#include <unistd.h>
#include <stdlib.h>
#include <string.h>
#include <errno.h>
#include <stdio.h>
#include <stdint.h>
#include <sys/types.h>

#include "mbt/storage.h"
#include "mbt/torrent.h"
#include "mbt/log.h"

struct mbt_torrent;

/*
 * File storage backend.
 * ctx = struct file_storage *, created by mbt_storage_file_new(data_dir).
 *
 * Single-file: data_dir/name
 * Multi-file: data_dir/torrent_name/path/to/file
 */

struct file_entry {
	int fd;
	uint64_t offset;
	uint64_t length;
};

struct file_storage {
	char *data_dir;
	struct file_entry *entries;
	int count;
	uint32_t piece_length;
};

static const struct mbt_storage_ops file_ops;

static int
mkdirs(const char *path)
{
	char *tmp = strdup(path);
	if (!tmp) return -ENOMEM;
	char *p = tmp;

	if (*p == '/') p++;

	while (*p) {
		char *slash = strchr(p, '/');
		if (!slash) break;
		*slash = '\0';
		if (mkdir(tmp, 0755) < 0 && errno != EEXIST) {
			int e = errno;
			free(tmp);
			return -e;
		}
		*slash = '/';
		p = slash + 1;
	}
	free(tmp);
	return 0;
}

struct mbt_storage
mbt_storage_file_new(const char *data_dir)
{
	struct file_storage *fs = calloc(1, sizeof(*fs));
	if (fs) {
		fs->data_dir = strdup(data_dir);
		if (!fs->data_dir) { free(fs); fs = NULL; }
	}
	return (struct mbt_storage){ .ops = fs ? &file_ops : NULL, .ctx = fs };
}

void
mbt_storage_file_free(struct mbt_storage *s)
{
	struct file_storage *fs = s->ctx;
	if (!fs) return;
	free(fs->data_dir);
	free(fs);
	s->ops = NULL;
	s->ctx = NULL;
}

int
mbt_storage_file_open(void *ctx, struct mbt_torrent *t)
{
	struct file_storage *fs = ctx;
	const char *dir = fs->data_dir;
	int fc = mbt_torrent_file_count(t);
	const char *name = mbt_torrent_name(t);

	if (fs->entries)
		return 0;

	if (fc == 0)
		return -EINVAL;

	fs->entries = calloc(fc, sizeof(struct file_entry));
	if (!fs->entries)
		return -ENOMEM;
	fs->count = fc;
	fs->piece_length = mbt_torrent_piece_length(t);

	for (int i = 0; i < fc; i++) {
		const struct mbt_file *f = mbt_torrent_file(t, i);
		char path[256];

		if (fc == 1)
			snprintf(path, sizeof(path), "%s/%s", dir, f->path);
		else
			snprintf(path, sizeof(path), "%s/%s/%s", dir,
			         name ? name : "download", f->path);

		int rv = mkdirs(path);
		if (rv < 0) {
			mbt_log_warn(MBT_LOG_STORAGE, "mkdir for %s: %s", path, strerror(-rv));
			return rv;
		}

		fs->entries[i].fd = open(path, O_RDWR | O_CREAT, 0644);
		if (fs->entries[i].fd < 0) {
			mbt_log_warn(MBT_LOG_STORAGE, "open %s: %s", path, strerror(errno));
			return -errno;
		}

		fs->entries[i].offset = f->offset;
		fs->entries[i].length = f->length;
	}

	mbt_log_info(MBT_LOG_STORAGE, "opened %d file(s) in %s", fc, dir);
	return 0;
}

int
mbt_storage_file_write_piece(void *ctx, uint32_t index, const void *data, size_t len)
{
	struct file_storage *fs = ctx;
	uint64_t piece_offset = (uint64_t)index * fs->piece_length;
	const uint8_t *p = data;
	size_t remaining = len;

	for (int i = 0; i < fs->count && remaining > 0; i++) {
		struct file_entry *e = &fs->entries[i];
		uint64_t file_end = e->offset + e->length;

		if (piece_offset >= file_end) continue;
		if (piece_offset + remaining <= e->offset) break;

		uint64_t write_start = piece_offset > e->offset ? piece_offset - e->offset : 0;
		uint64_t data_start  = piece_offset > e->offset ? 0 : e->offset - piece_offset;
		uint64_t can_write   = e->length - write_start;
		size_t to_write = remaining < can_write ? remaining : (size_t)can_write;

		ssize_t rv = pwrite(e->fd, p + data_start, to_write, write_start);
		if (rv < 0) return -errno;

		remaining     -= to_write;
		piece_offset  += to_write;
		p             += to_write;
	}
	return 0;
}

int
mbt_storage_file_read_piece(void *ctx, uint32_t index, void *buf, size_t len)
{
	struct file_storage *fs = ctx;
	uint64_t piece_offset = (uint64_t)index * fs->piece_length;
	uint8_t *p = buf;
	size_t remaining = len;

	for (int i = 0; i < fs->count && remaining > 0; i++) {
		struct file_entry *e = &fs->entries[i];
		uint64_t file_end = e->offset + e->length;

		if (piece_offset >= file_end) continue;
		if (piece_offset + remaining <= e->offset) break;

		uint64_t read_start = piece_offset > e->offset ? piece_offset - e->offset : 0;
		uint64_t data_start = piece_offset > e->offset ? 0 : e->offset - piece_offset;
		uint64_t can_read   = e->length - read_start;
		size_t to_read = remaining < can_read ? remaining : (size_t)can_read;

		ssize_t rv = pread(e->fd, p + data_start, to_read, read_start);
		if (rv < 0) return -errno;
		if ((size_t)rv < to_read) return -EIO;

		remaining    -= to_read;
		piece_offset += to_read;
		p            += to_read;
	}
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
	struct file_storage *fs = ctx;
	if (!fs || !fs->entries) return;
	for (int i = 0; i < fs->count; i++)
		if (fs->entries[i].fd >= 0)
			close(fs->entries[i].fd);
	free(fs->entries);
	fs->entries = NULL;
	fs->count = 0;
}

static const struct mbt_storage_ops file_ops = {
	.open        = mbt_storage_file_open,
	.write_piece = mbt_storage_file_write_piece,
	.read_piece  = mbt_storage_file_read_piece,
	.have_piece  = mbt_storage_file_have_piece,
	.close       = mbt_storage_file_close,
};
