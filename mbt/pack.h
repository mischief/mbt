// SPDX-License-Identifier: ISC
#ifndef MBT_PACK_H
#define MBT_PACK_H

#include <stdint.h>
#include <stddef.h>
#include <string.h>
#include "export.h"

/* Avoid platform htobe32/be32toh macros which may expand to static functions,
 * breaking -Wstatic-in-inline on clang (seen on OpenBSD). Use builtins directly. */
#if __BYTE_ORDER__ == __ORDER_LITTLE_ENDIAN__
#  define MBT_HTOBE32(x) __builtin_bswap32(x)
#  define MBT_BE32TOH(x) __builtin_bswap32(x)
#else
#  define MBT_HTOBE32(x) (uint32_t)(x)
#  define MBT_BE32TOH(x) (uint32_t)(x)
#endif

/*
 * Type-safe binary pack/unpack using cursor structs. Every operation checks
 * bounds in every build. A failed operation sets err and writes nothing, and
 * reads return 0. mbt_pack_end and mbt_unpack_end return 0 after an error.
 */

struct mbt_packer {
	unsigned char *start;
	unsigned char *p;
	unsigned char *end;
	int err;
};

struct mbt_unpacker {
	const unsigned char *start;
	const unsigned char *p;
	const unsigned char *end;
	int err;
};

MBT_API inline void
mbt_pack_init(struct mbt_packer *pk, void *buf, size_t len)
{
	pk->start = pk->p = (unsigned char *)buf;
	pk->end = pk->start + len;
	pk->err = 0;
}

MBT_API inline void
mbt_pack_u8(struct mbt_packer *pk, uint8_t v)
{
	if (pk->err || pk->end - pk->p < 1) {
		pk->err = 1;
		return;
	}
	*pk->p++ = v;
}

MBT_API inline void
mbt_pack_u32(struct mbt_packer *pk, uint32_t v)
{
	if (pk->err || pk->end - pk->p < 4) {
		pk->err = 1;
		return;
	}
	uint32_t be = MBT_HTOBE32(v);
	memcpy(pk->p, &be, 4);
	pk->p += 4;
}

MBT_API inline void
mbt_pack_mem(struct mbt_packer *pk, const void *src, size_t n)
{
	if (pk->err || (size_t)(pk->end - pk->p) < n) {
		pk->err = 1;
		return;
	}
	memcpy(pk->p, src, n);
	pk->p += n;
}

MBT_API inline size_t
mbt_pack_end(struct mbt_packer *pk)
{
	return pk->err ? 0 : (size_t)(pk->p - pk->start);
}

MBT_API inline void
mbt_unpack_init(struct mbt_unpacker *pk, const void *buf, size_t len)
{
	pk->start = pk->p = (const unsigned char *)buf;
	pk->end = pk->start + len;
	pk->err = 0;
}

MBT_API inline uint8_t
mbt_unpack_u8(struct mbt_unpacker *pk)
{
	if (pk->err || pk->end - pk->p < 1) {
		pk->err = 1;
		return 0;
	}
	return *pk->p++;
}

MBT_API inline uint32_t
mbt_unpack_u32(struct mbt_unpacker *pk)
{
	if (pk->err || pk->end - pk->p < 4) {
		pk->err = 1;
		return 0;
	}
	uint32_t be;
	memcpy(&be, pk->p, 4);
	pk->p += 4;
	return MBT_BE32TOH(be);
}

MBT_API inline void
mbt_unpack_mem(struct mbt_unpacker *pk, void *dst, size_t n)
{
	if (pk->err || (size_t)(pk->end - pk->p) < n) {
		pk->err = 1;
		memset(dst, 0, n);
		return;
	}
	memcpy(dst, pk->p, n);
	pk->p += n;
}

MBT_API inline size_t
mbt_unpack_end(struct mbt_unpacker *pk)
{
	return pk->err ? 0 : (size_t)(pk->p - pk->start);
}

#endif
