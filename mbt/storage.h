// SPDX-License-Identifier: ISC
#ifndef MBT_STORAGE_H
#define MBT_STORAGE_H

#include "export.h"

#include <stddef.h>
#include <stdint.h>

struct mbt_torrent;

/* One torrent open on a storage. Each backend defines its own. */
struct mbt_storage_handle;

/*
 * open sets *out to a handle that is not NULL. The torrent passes this
 * handle to the other calls, and calls close before it drops its reference
 * to the storage. destroy is optional. It runs when the last reference goes.
 *
 * remove and exists are optional, and mbt_torrent_move_storage needs remove.
 * remove deletes the data of an open torrent, and h stays valid for close.
 * exists returns 1 if data for t is already present, else 0.
 */
struct mbt_storage_ops {
	int (*open)(void *ctx, struct mbt_torrent *t, struct mbt_storage_handle **out);
	int (*write_piece)(struct mbt_storage_handle *h, uint32_t index,
	                   const void *data, size_t len);
	int (*read_piece)(struct mbt_storage_handle *h, uint32_t index,
	                  void *buf, size_t len);
	int (*have_piece)(struct mbt_storage_handle *h, uint32_t index);  /* 1=have, 0=missing, <0=error */
	void (*close)(struct mbt_storage_handle *h);
	void (*destroy)(void *ctx);
	int (*remove)(struct mbt_storage_handle *h);
	int (*exists)(void *ctx, struct mbt_torrent *t);
};

/* A storage holds any number of torrents, if the backend allows it. */
struct mbt_storage {
	int ref;
	const struct mbt_storage_ops *ops;
	void *ctx;
};

/* Returns a storage with one reference, or NULL. */
MBT_API struct mbt_storage *mbt_storage_new(const struct mbt_storage_ops *ops, void *ctx);
MBT_API struct mbt_storage *mbt_storage_ref(struct mbt_storage *s);
MBT_API void mbt_storage_unref(struct mbt_storage *s);

/* --- File storage (single + multi-file torrents) --- */

/*
 * Holds any number of torrents under data_dir. open fails with -EEXIST if a
 * file path is in use by a torrent that is open on this storage.
 */
MBT_API struct mbt_storage *mbt_storage_file_new(const char *data_dir);

/* --- Memory storage (testing, small torrents) --- */

/* Holds one torrent at a time: a second open fails with -EBUSY. */
struct mbt_storage_memory;
MBT_API struct mbt_storage_memory *mbt_storage_memory_new(void);

/* Drops the reference from mbt_storage_memory_new. */
MBT_API void mbt_storage_memory_destroy(struct mbt_storage_memory *m);

/* Returns the storage without a new reference. */
MBT_API struct mbt_storage *mbt_storage_memory_storage(struct mbt_storage_memory *m);

/* File-level access for test setup and verification */
MBT_API int mbt_storage_memory_put_file(struct mbt_storage_memory *m, uint64_t offset,
                                const void *data, size_t len);
MBT_API int mbt_storage_memory_get_file(struct mbt_storage_memory *m, uint64_t offset,
                                void *buf, size_t len);
MBT_API uint8_t *mbt_storage_memory_data(struct mbt_storage_memory *m);
MBT_API uint64_t mbt_storage_memory_length(struct mbt_storage_memory *m);

#endif
