// SPDX-License-Identifier: ISC
/*
 * Storage on one open descriptor, for a process with no file system access.
 * It holds one single-file torrent, so the handle is the struct mbt_storage_fd.
 */
#include <errno.h>
#include <fcntl.h>
#include <stdint.h>
#include <stdlib.h>
#include <sys/stat.h>
#include <unistd.h>

#include "mbt/storage.h"
#include "mbt/torrent.h"
#include "banned.h"

struct mbt_storage_fd {
	int fd;
	int flags;
	int opened;
	uint32_t piece_length;
	uint64_t total_length;
};

static const struct mbt_storage_ops fd_ops;

struct mbt_storage *
mbt_storage_fd_new(int fd, int flags)
{
	struct mbt_storage_fd *f;
	struct mbt_storage *s;

	if (fd < 0 || (flags & ~MBT_STORAGE_FD_READONLY))
		return NULL;
	if ((f = calloc(1, sizeof(*f))) == NULL)
		return NULL;
	f->fd = fd;
	f->flags = flags;
	if ((s = mbt_storage_new(&fd_ops, f)) == NULL)
		free(f);
	return s;
}

static int
fd_check_mode(const struct mbt_storage_fd *f)
{
	int fl = fcntl(f->fd, F_GETFL);

	if (fl < 0)
		return -errno;
	/* pwrite with O_APPEND ignores the offset on Linux. */
	if (fl & O_APPEND)
		return -EINVAL;
	switch (fl & O_ACCMODE) {
	case O_RDWR:
		return 0;
	case O_RDONLY:
		return (f->flags & MBT_STORAGE_FD_READONLY) ? 0 : -EBADF;
	default:
		return -EBADF;
	}
}

static int
fd_open(void *ctx, struct mbt_torrent *t, struct mbt_storage_handle **out)
{
	struct mbt_storage_fd *f = ctx;
	struct stat st;
	uint64_t total;
	int rv;

	if (f->opened)
		return -EBUSY;
	if (mbt_torrent_file_count(t) != 1)
		return -EINVAL;
	if ((rv = fd_check_mode(f)) < 0)
		return rv;
	if (fstat(f->fd, &st) < 0)
		return -errno;
	if (!S_ISREG(st.st_mode))
		return -EINVAL;
	total = mbt_torrent_file(t, 0)->length;
	if ((uint64_t)st.st_size > total ||
	    ((f->flags & MBT_STORAGE_FD_READONLY) && (uint64_t)st.st_size != total))
		return -EINVAL;
	f->piece_length = mbt_torrent_piece_length(t);
	f->total_length = total;
	f->opened = 1;
	*out = (struct mbt_storage_handle *)f;
	return 0;
}

static int
fd_range(const struct mbt_storage_fd *f, uint32_t index, size_t len, off_t *off)
{
	uint64_t start = (uint64_t)index * f->piece_length;

	if (start >= f->total_length || len > f->total_length - start)
		return -EINVAL;
	*off = (off_t)start;
	return 0;
}

static int
fd_write_piece(struct mbt_storage_handle *h, uint32_t index, const void *data, size_t len)
{
	struct mbt_storage_fd *f = (struct mbt_storage_fd *)h;
	const uint8_t *p = data;
	off_t off;
	int rv;

	if (f->flags & MBT_STORAGE_FD_READONLY)
		return -EROFS;
	if ((rv = fd_range(f, index, len, &off)) < 0)
		return rv;
	while (len > 0) {
		ssize_t n = pwrite(f->fd, p, len, off);

		if (n < 0) {
			if (errno == EINTR)
				continue;
			return -errno;
		}
		if (n == 0)
			return -EIO;
		p += n;
		off += n;
		len -= (size_t)n;
	}
	return 0;
}

static int
fd_read_piece(struct mbt_storage_handle *h, uint32_t index, void *buf, size_t len)
{
	struct mbt_storage_fd *f = (struct mbt_storage_fd *)h;
	uint8_t *p = buf;
	off_t off;
	int rv;

	if ((rv = fd_range(f, index, len, &off)) < 0)
		return rv;
	while (len > 0) {
		ssize_t n = pread(f->fd, p, len, off);

		if (n < 0) {
			if (errno == EINTR)
				continue;
			return -errno;
		}
		if (n == 0)
			return -EIO;
		p += n;
		off += n;
		len -= (size_t)n;
	}
	return 0;
}

static int
fd_read_only(struct mbt_storage_handle *h)
{
	return (((struct mbt_storage_fd *)h)->flags & MBT_STORAGE_FD_READONLY) != 0;
}

static void
fd_close(struct mbt_storage_handle *h)
{
	((struct mbt_storage_fd *)h)->opened = 0;
}

static void
fd_destroy(void *ctx)
{
	struct mbt_storage_fd *f = ctx;

	close(f->fd);
	free(f);
}

static const struct mbt_storage_ops fd_ops = {
	.open        = fd_open,
	.write_piece = fd_write_piece,
	.read_piece  = fd_read_piece,
	.read_only   = fd_read_only,
	.close       = fd_close,
	.destroy     = fd_destroy,
};
