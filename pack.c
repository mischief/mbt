// SPDX-License-Identifier: ISC
#include "mbt/pack.h"
#include "banned.h"

extern inline void   mbt_pack_init(struct mbt_packer *pk, void *buf, size_t len);
extern inline int    mbt_pack_u8(struct mbt_packer *pk, uint8_t v);
extern inline int    mbt_pack_u32(struct mbt_packer *pk, uint32_t v);
extern inline int    mbt_pack_mem(struct mbt_packer *pk, const void *src, size_t n);
extern inline int    mbt_pack_end(struct mbt_packer *pk, size_t *out);

extern inline void     mbt_unpack_init(struct mbt_unpacker *pk, const void *buf, size_t len);
extern inline uint8_t  mbt_unpack_u8(struct mbt_unpacker *pk);
extern inline uint32_t mbt_unpack_u32(struct mbt_unpacker *pk);
extern inline void     mbt_unpack_mem(struct mbt_unpacker *pk, void *dst, size_t n);
extern inline int      mbt_unpack_end(struct mbt_unpacker *pk, size_t *out);
