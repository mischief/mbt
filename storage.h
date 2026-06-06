#ifndef MBT_STORAGE_H
#define MBT_STORAGE_H

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

/* --- File storage (single + multi-file torrents) --- */

const struct mbt_storage_ops *mbt_storage_file_ops(void);

int mbt_storage_file_open(void *ctx, struct mbt_torrent *t);
int mbt_storage_file_write_piece(void *ctx, uint32_t index, const void *data, size_t len);
int mbt_storage_file_read_piece(void *ctx, uint32_t index, void *buf, size_t len);
int mbt_storage_file_have_piece(void *ctx, uint32_t index);
void mbt_storage_file_close(void *ctx);

/* ctx for file storage: path to output directory (const char *) */

/* --- Memory storage (testing, small torrents) --- */

const struct mbt_storage_ops *mbt_storage_memory_ops(void);

int mbt_storage_memory_open(void *ctx, struct mbt_torrent *t);
int mbt_storage_memory_write_piece(void *ctx, uint32_t index, const void *data, size_t len);
int mbt_storage_memory_read_piece(void *ctx, uint32_t index, void *buf, size_t len);
int mbt_storage_memory_have_piece(void *ctx, uint32_t index);
void mbt_storage_memory_close(void *ctx);

/* ctx for memory storage: NULL (allocates internally) */

#endif
