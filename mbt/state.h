// SPDX-License-Identifier: ISC
#ifndef MBT_STATE_H
#define MBT_STATE_H

#include "export.h"

#include <stddef.h>
#include <stdint.h>

struct mbt_torrent;

/*
 * One saved torrent entry, provided to the enumerate callback.
 * All pointers are valid only for the duration of the callback.
 */
struct mbt_state_entry {
	const void *metadata;      /* raw info dict bytes; NULL if magnet not yet resolved */
	size_t      metadata_len;
	const char *data_dir;      /* directory where piece data lives */
	int64_t     added;         /* unix timestamp when torrent was added */
};

/*
 * Persistence ops — pluggable backend, same pattern as mbt_storage_ops.
 * Session holds ops + ctx; calls through these without knowing the impl.
 */
struct mbt_state_ops {
	/* Iterate all saved entries, calling cb for each. Return <0 to abort. */
	int  (*enumerate)(void *ctx,
	                  int (*cb)(const struct mbt_state_entry *e, void *arg),
	                  void *arg);
	/* Persist torrent state (metadata + data_dir). Called on piece complete and metadata received. */
	int  (*save)(void *ctx, struct mbt_torrent *t);
	/* Delete saved state for this torrent. */
	int  (*remove)(void *ctx, struct mbt_torrent *t);
	/* Clean up ctx. */
	void (*close)(void *ctx);
};

/* --- File-based implementation --- */

struct mbt_state;

/* Open (or create) a state directory. Returns 0 on success. */
MBT_API int  mbt_state_open(struct mbt_state **out, const char *dir);
MBT_API void mbt_state_close(struct mbt_state *st);

/* Returns the ops struct backed by struct mbt_state. */
MBT_API const struct mbt_state_ops *mbt_state_file_ops(void);

#endif
