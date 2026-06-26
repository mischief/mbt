// SPDX-License-Identifier: ISC
#ifndef MBT_STORAGE_H
#define MBT_STORAGE_H

#include "export.h"

#include <stddef.h>
#include <stdint.h>

struct mbt_torrent;

struct mbt_storage_ops {
	int (*open)(void *ctx, struct mbt_torrent *t);
	int (*write_piece)(void *ctx, uint32_t index, const void *data, size_t len);
	int (*read_piece)(void *ctx, uint32_t index, void *buf, size_t len);
	int (*have_piece)(void *ctx, uint32_t index);  /* 1=have, 0=missing, <0=error */
	void (*close)(void *ctx);
};

/* Bundled ops + ctx — pass this around instead of the two separately */
struct mbt_storage {
	const struct mbt_storage_ops *ops;
	void *ctx;
};

/* --- File storage (single + multi-file torrents) --- */

/* Construct/destroy a file storage instance for a given data directory */
MBT_API struct mbt_storage mbt_storage_file_new(const char *data_dir);
MBT_API void mbt_storage_file_free(struct mbt_storage *s);

MBT_API int mbt_storage_file_open(void *ctx, struct mbt_torrent *t);
MBT_API int mbt_storage_file_write_piece(void *ctx, uint32_t index, const void *data, size_t len);
MBT_API int mbt_storage_file_read_piece(void *ctx, uint32_t index, void *buf, size_t len);
MBT_API int mbt_storage_file_have_piece(void *ctx, uint32_t index);
MBT_API void mbt_storage_file_close(void *ctx);

/* --- Memory storage (testing, small torrents) --- */

struct mbt_storage_memory;
MBT_API struct mbt_storage_memory *mbt_storage_memory_new(void);
MBT_API void mbt_storage_memory_destroy(struct mbt_storage_memory *m);
MBT_API struct mbt_storage mbt_storage_memory_storage(struct mbt_storage_memory *m);

/* File-level access for test setup and verification */
MBT_API int mbt_storage_memory_put_file(struct mbt_storage_memory *m, uint64_t offset,
                                const void *data, size_t len);
MBT_API int mbt_storage_memory_get_file(struct mbt_storage_memory *m, uint64_t offset,
                                void *buf, size_t len);
MBT_API uint8_t *mbt_storage_memory_data(struct mbt_storage_memory *m);
MBT_API uint64_t mbt_storage_memory_length(struct mbt_storage_memory *m);

MBT_API int mbt_storage_memory_open(void *ctx, struct mbt_torrent *t);
MBT_API int mbt_storage_memory_write_piece(void *ctx, uint32_t index, const void *data, size_t len);
MBT_API int mbt_storage_memory_read_piece(void *ctx, uint32_t index, void *buf, size_t len);
MBT_API int mbt_storage_memory_have_piece(void *ctx, uint32_t index);
MBT_API void mbt_storage_memory_close(void *ctx);

#endif
