// SPDX-License-Identifier: ISC
/*
 * storage_memory.c — in-memory storage backend for testing
 *
 * Stores torrent data in a flat buffer. Supports piece-level and
 * file-level access for constructing test scenarios.
 */
#include <stdlib.h>
#include <string.h>
#include <errno.h>

#include "mbt/storage.h"
#include "mbt/torrent.h"

struct mbt_storage_memory {
	uint8_t *data;
	uint64_t total_length;
	uint32_t piece_length;
	uint32_t piece_count;
	uint8_t *have;  /* bitmap: 1 byte per piece */
};

struct mbt_storage_memory *
mbt_storage_memory_new(void)
{
	return calloc(1, sizeof(struct mbt_storage_memory));
}

void
mbt_storage_memory_destroy(struct mbt_storage_memory *m)
{
	if (!m) return;
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

/* --- storage_ops implementation --- */

int
mbt_storage_memory_open(void *ctx, struct mbt_torrent *t)
{
	struct mbt_storage_memory *m = ctx;
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
	if (!m->have) {
		m->have = calloc(m->piece_count, 1);
		if (!m->have) return -ENOMEM;
	}
	return 0;
}

int
mbt_storage_memory_write_piece(void *ctx, uint32_t index, const void *data, size_t len)
{
	struct mbt_storage_memory *m = ctx;
	uint64_t offset = (uint64_t)index * m->piece_length;
	if (offset + len > m->total_length)
		return -EINVAL;
	memcpy(m->data + offset, data, len);
	m->have[index] = 1;
	return 0;
}

int
mbt_storage_memory_read_piece(void *ctx, uint32_t index, void *buf, size_t len)
{
	struct mbt_storage_memory *m = ctx;
	uint64_t offset = (uint64_t)index * m->piece_length;
	if (offset + len > m->total_length)
		return -EINVAL;
	memcpy(buf, m->data + offset, len);
	return 0;
}

int
mbt_storage_memory_have_piece(void *ctx, uint32_t index)
{
	struct mbt_storage_memory *m = ctx;
	if (index >= m->piece_count) return -EINVAL;
	return m->have[index];
}

void
mbt_storage_memory_close(void *ctx)
{
	(void)ctx;
}

static const struct mbt_storage_ops memory_ops = {
	.open        = mbt_storage_memory_open,
	.write_piece = mbt_storage_memory_write_piece,
	.read_piece  = mbt_storage_memory_read_piece,
	.have_piece  = mbt_storage_memory_have_piece,
	.close       = mbt_storage_memory_close,
};

const struct mbt_storage_ops *
mbt_storage_memory_ops(void)
{
	return &memory_ops;
}
