// SPDX-License-Identifier: ISC
/*
 * In-memory storage backend for testing. The flat buffer holds one torrent,
 * so the handle is the struct mbt_storage_memory itself.
 */
#include <stdlib.h>
#include <string.h>
#include <errno.h>
#include <stdint.h>

#include "mbt/storage.h"
#include "mbt/torrent.h"

struct mbt_torrent;

struct mbt_storage_memory {
	struct mbt_storage *storage;
	int opened;
	uint8_t *data;
	uint64_t total_length;
	uint32_t piece_length;
	uint32_t piece_count;
	uint8_t *have;  /* bitmap: 1 byte per piece */
};

static const struct mbt_storage_ops memory_ops;

struct mbt_storage_memory *
mbt_storage_memory_new(void)
{
	struct mbt_storage_memory *m = calloc(1, sizeof(*m));

	if (!m)
		return NULL;
	m->storage = mbt_storage_new(&memory_ops, m);
	if (!m->storage) {
		free(m);
		return NULL;
	}
	return m;
}

void
mbt_storage_memory_destroy(struct mbt_storage_memory *m)
{
	if (m)
		mbt_storage_unref(m->storage);
}

static void
memory_destroy(void *ctx)
{
	struct mbt_storage_memory *m = ctx;

	free(m->data);
	free(m->have);
	free(m);
}

/* Write a file's data at the correct offset (for test setup) */
int
mbt_storage_memory_put_file(struct mbt_storage_memory *m, uint64_t offset,
                            const void *data, size_t len)
{
	/* Grow buffer if needed (allows pre-fill before open) */
	if (offset + len > m->total_length) {
		uint64_t new_len = offset + len;
		uint8_t *p = realloc(m->data, new_len);
		if (!p) return -ENOMEM;
		memset(p + m->total_length, 0, new_len - m->total_length);
		m->data = p;
		m->total_length = new_len;
	}
	if (!m->data) {
		m->data = calloc(1, m->total_length);
		if (!m->data) return -ENOMEM;
	}
	memcpy(m->data + offset, data, len);
	return 0;
}

/* Read a file's data from the buffer (for verification) */
int
mbt_storage_memory_get_file(struct mbt_storage_memory *m, uint64_t offset,
                            void *buf, size_t len)
{
	if (!m->data || offset + len > m->total_length)
		return -EINVAL;
	memcpy(buf, m->data + offset, len);
	return 0;
}

/* Get raw pointer to the full data buffer */
uint8_t *
mbt_storage_memory_data(struct mbt_storage_memory *m)
{
	return m->data;
}

uint64_t
mbt_storage_memory_length(struct mbt_storage_memory *m)
{
	return m->total_length;
}

struct mbt_storage *
mbt_storage_memory_storage(struct mbt_storage_memory *m)
{
	return m->storage;
}

/* --- storage_ops implementation --- */

static int
memory_open(void *ctx, struct mbt_torrent *t, struct mbt_storage_handle **out)
{
	struct mbt_storage_memory *m = ctx;

	if (m->opened)
		return -EBUSY;

	uint64_t old_len = m->total_length;
	m->piece_length = mbt_torrent_piece_length(t);
	m->piece_count = mbt_torrent_piece_count(t);
	uint64_t tl = 0;
	for (int i = 0; i < mbt_torrent_file_count(t); i++)
		tl += mbt_torrent_file(t, i)->length;
	m->total_length = tl;

	/* Ensure buffer covers full torrent size */
	uint8_t *p = realloc(m->data, m->total_length);
	if (!p && m->total_length > 0) return -ENOMEM;
	if (p) {
		if (m->total_length > old_len)
			memset(p + old_len, 0, m->total_length - old_len);
		m->data = p;
	}
	m->have = calloc(m->piece_count, 1);
	if (!m->have) return -ENOMEM;

	m->opened = 1;
	*out = (struct mbt_storage_handle *)m;
	return 0;
}

static int
memory_write_piece(struct mbt_storage_handle *h, uint32_t index, const void *data, size_t len)
{
	struct mbt_storage_memory *m = (struct mbt_storage_memory *)h;
	uint64_t offset = (uint64_t)index * m->piece_length;
	if (offset + len > m->total_length)
		return -EINVAL;
	memcpy(m->data + offset, data, len);
	m->have[index] = 1;
	return 0;
}

static int
memory_read_piece(struct mbt_storage_handle *h, uint32_t index, void *buf, size_t len)
{
	struct mbt_storage_memory *m = (struct mbt_storage_memory *)h;
	uint64_t offset = (uint64_t)index * m->piece_length;
	if (offset + len > m->total_length)
		return -EINVAL;
	memcpy(buf, m->data + offset, len);
	return 0;
}

static int
memory_have_piece(struct mbt_storage_handle *h, uint32_t index)
{
	struct mbt_storage_memory *m = (struct mbt_storage_memory *)h;
	if (index >= m->piece_count) return -EINVAL;
	return m->have[index];
}

static void
memory_close(struct mbt_storage_handle *h)
{
	struct mbt_storage_memory *m = (struct mbt_storage_memory *)h;

	free(m->have);
	m->have = NULL;
	m->opened = 0;
}

static const struct mbt_storage_ops memory_ops = {
	.open        = memory_open,
	.write_piece = memory_write_piece,
	.read_piece  = memory_read_piece,
	.have_piece  = memory_have_piece,
	.close       = memory_close,
	.destroy     = memory_destroy,
};
