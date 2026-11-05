// SPDX-License-Identifier: ISC
#include <sys/queue.h>
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
 * ctx is a struct file_storage. Each open torrent has a struct file_torrent.
 * Single-file: data_dir/name
 * Multi-file: data_dir/torrent_name/path/to/file
 */

struct file_entry {
	int fd;                /* -1 for padding: reads as zeros, writes are dropped */
	char *path;
	uint64_t offset;
	uint64_t length;
};

struct file_torrent {
	SLIST_ENTRY(file_torrent) next;
	struct file_storage *fs;
	struct file_entry *entries;
	int count;
	uint32_t piece_length;
};

struct file_storage {
	char *data_dir;
	SLIST_HEAD(, file_torrent) handles;
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

struct mbt_storage *
mbt_storage_file_new(const char *data_dir)
{
	struct file_storage *fs = calloc(1, sizeof(*fs));
	struct mbt_storage *s;

	if (!fs)
		return NULL;
	SLIST_INIT(&fs->handles);
	fs->data_dir = strdup(data_dir);
	s = fs->data_dir ? mbt_storage_new(&file_ops, fs) : NULL;
	if (!s) {
		free(fs->data_dir);
		free(fs);
	}
	return s;
}

static void
file_destroy(void *ctx)
{
	struct file_storage *fs = ctx;

	free(fs->data_dir);
	free(fs);
}

static void
handle_free(struct file_torrent *h)
{
	for (int i = 0; i < h->count; i++) {
		if (h->entries[i].fd >= 0)
			close(h->entries[i].fd);
		free(h->entries[i].path);
	}
	free(h->entries);
	free(h);
}

static int
path_in_use(struct file_storage *fs, const char *path)
{
	struct file_torrent *h;

	SLIST_FOREACH(h, &fs->handles, next)
		for (int i = 0; i < h->count; i++)
			if (h->entries[i].path && strcmp(h->entries[i].path, path) == 0)
				return 1;
	return 0;
}

static char *
entry_path(const char *dir, const char *name, const char *file, int multi)
{
	size_t n = strlen(dir) + strlen(name) + strlen(file) + 3;
	char *path = malloc(n);

	if (!path)
		return NULL;
	if (multi)
		snprintf(path, n, "%s/%s/%s", dir, name, file);
	else
		snprintf(path, n, "%s/%s", dir, file);
	return path;
}

static int
file_exists(void *ctx, struct mbt_torrent *t)
{
	struct file_storage *fs = ctx;
	int fc = mbt_torrent_file_count(t);
	const char *name = mbt_torrent_name(t);

	for (int i = 0; i < fc; i++) {
		const struct mbt_file *f = mbt_torrent_file(t, i);
		char *path;
		int found;

		if (f->pad || !f->path)
			continue;
		path = entry_path(fs->data_dir, name ? name : "download", f->path, fc > 1);
		if (!path)
			return -ENOMEM;
		found = access(path, F_OK) == 0;
		free(path);
		if (found)
			return 1;
	}
	return 0;
}

/* Delete the empty directories above path, up to the data directory. */
static void
rmdirs(const char *data_dir, char *path)
{
	size_t root = strlen(data_dir);
	char *slash;

	while ((slash = strrchr(path, '/')) != NULL && (size_t)(slash - path) > root) {
		*slash = '\0';
		if (rmdir(path) < 0)
			return;
	}
}

static int
file_remove(struct mbt_storage_handle *handle)
{
	struct file_torrent *h = (struct file_torrent *)handle;
	int rv = 0;

	for (int i = 0; i < h->count; i++) {
		char *path = h->entries[i].path;

		if (!path)
			continue;
		if (unlink(path) < 0 && errno != ENOENT && rv == 0)
			rv = -errno;
		rmdirs(h->fs->data_dir, path);
		h->entries[i].path = NULL;
		free(path);
	}
	return rv;
}

static int
file_open(void *ctx, struct mbt_torrent *t, struct mbt_storage_handle **out)
{
	struct file_storage *fs = ctx;
	const char *dir = fs->data_dir;
	int fc = mbt_torrent_file_count(t);
	const char *name = mbt_torrent_name(t);
	struct file_torrent *h;
	int rv;

	if (fc == 0)
		return -EINVAL;

	h = calloc(1, sizeof(*h));
	if (!h)
		return -ENOMEM;
	h->fs = fs;
	h->piece_length = mbt_torrent_piece_length(t);
	h->entries = calloc(fc, sizeof(struct file_entry));
	if (!h->entries) {
		free(h);
		return -ENOMEM;
	}

	for (int i = 0; i < fc; i++) {
		const struct mbt_file *f = mbt_torrent_file(t, i);
		struct file_entry *e = &h->entries[i];

		e->fd = -1;
		e->offset = f->offset;
		e->length = f->length;
		h->count = i + 1;
		if (f->pad)
			continue;
		if (!f->path) {
			rv = -EINVAL;
			goto fail;
		}
		e->path = entry_path(dir, name ? name : "download", f->path, fc > 1);
		if (!e->path) {
			rv = -ENOMEM;
			goto fail;
		}
		if (path_in_use(fs, e->path)) {
			mbt_log_warn(MBT_LOG_STORAGE, "%s: in use by another torrent", e->path);
			rv = -EEXIST;
			goto fail;
		}

		rv = mkdirs(e->path);
		if (rv < 0) {
			mbt_log_warn(MBT_LOG_STORAGE, "mkdir for %s: %s", e->path, strerror(-rv));
			goto fail;
		}

		e->fd = open(e->path, O_RDWR | O_CREAT, 0644);
		if (e->fd < 0) {
			rv = -errno;
			mbt_log_warn(MBT_LOG_STORAGE, "open %s: %s", e->path, strerror(-rv));
			goto fail;
		}
	}

	SLIST_INSERT_HEAD(&fs->handles, h, next);
	*out = (struct mbt_storage_handle *)h;
	mbt_log_info(MBT_LOG_STORAGE, "opened %d file(s) in %s", fc, dir);
	return 0;

fail:
	handle_free(h);
	return rv;
}

static int
file_write_piece(struct mbt_storage_handle *handle, uint32_t index,
                 const void *data, size_t len)
{
	struct file_torrent *h = (struct file_torrent *)handle;
	uint64_t piece_offset = (uint64_t)index * h->piece_length;
	const uint8_t *p = data;
	size_t remaining = len;

	for (int i = 0; i < h->count && remaining > 0; i++) {
		struct file_entry *e = &h->entries[i];
		uint64_t file_end = e->offset + e->length;

		if (piece_offset >= file_end) continue;
		if (piece_offset + remaining <= e->offset) break;

		uint64_t write_start = piece_offset > e->offset ? piece_offset - e->offset : 0;
		uint64_t data_start  = piece_offset > e->offset ? 0 : e->offset - piece_offset;
		uint64_t can_write   = e->length - write_start;
		size_t to_write = remaining < can_write ? remaining : (size_t)can_write;

		if (e->fd >= 0) {
			ssize_t rv = pwrite(e->fd, p + data_start, to_write, write_start);
			if (rv < 0) return -errno;
		}

		remaining     -= to_write;
		piece_offset  += to_write;
		p             += to_write;
	}
	return 0;
}

static int
file_read_piece(struct mbt_storage_handle *handle, uint32_t index, void *buf, size_t len)
{
	struct file_torrent *h = (struct file_torrent *)handle;
	uint64_t piece_offset = (uint64_t)index * h->piece_length;
	uint8_t *p = buf;
	size_t remaining = len;

	for (int i = 0; i < h->count && remaining > 0; i++) {
		struct file_entry *e = &h->entries[i];
		uint64_t file_end = e->offset + e->length;

		if (piece_offset >= file_end) continue;
		if (piece_offset + remaining <= e->offset) break;

		uint64_t read_start = piece_offset > e->offset ? piece_offset - e->offset : 0;
		uint64_t data_start = piece_offset > e->offset ? 0 : e->offset - piece_offset;
		uint64_t can_read   = e->length - read_start;
		size_t to_read = remaining < can_read ? remaining : (size_t)can_read;

		if (e->fd >= 0) {
			ssize_t rv = pread(e->fd, p + data_start, to_read, read_start);
			if (rv < 0) return -errno;
			if ((size_t)rv < to_read) return -EIO;
		} else {
			memset(p + data_start, 0, to_read);
		}

		remaining    -= to_read;
		piece_offset += to_read;
		p            += to_read;
	}
	return 0;
}

static int
file_have_piece(struct mbt_storage_handle *handle, uint32_t index)
{
	(void)handle;
	(void)index;
	return 0;
}

static void
file_close(struct mbt_storage_handle *handle)
{
	struct file_torrent *h = (struct file_torrent *)handle;

	SLIST_REMOVE(&h->fs->handles, h, file_torrent, next);
	handle_free(h);
}

static const struct mbt_storage_ops file_ops = {
	.open        = file_open,
	.write_piece = file_write_piece,
	.read_piece  = file_read_piece,
	.have_piece  = file_have_piece,
	.close       = file_close,
	.destroy     = file_destroy,
	.remove      = file_remove,
	.exists      = file_exists,
};
