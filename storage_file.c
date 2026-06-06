#include "storage.h"

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

int mbt_storage_file_open(void *ctx, struct mbt_torrent *t) { (void)ctx; (void)t; return 0; }
int mbt_storage_file_write_piece(void *ctx, uint32_t index, const void *data, size_t len) { (void)ctx; (void)index; (void)data; (void)len; return 0; }
int mbt_storage_file_read_piece(void *ctx, uint32_t index, void *buf, size_t len) { (void)ctx; (void)index; (void)buf; (void)len; return 0; }
int mbt_storage_file_have_piece(void *ctx, uint32_t index) { (void)ctx; (void)index; return 0; }
void mbt_storage_file_close(void *ctx) { (void)ctx; }
