// SPDX-License-Identifier: ISC
#ifndef MBT_PACK_H
#define MBT_PACK_H

#include <stdint.h>
#include <stddef.h>
#include <string.h>
#include <stdio.h>
#include <stdlib.h>
#include <assert.h>
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
 * Type-safe binary pack/unpack using cursor structs.
 * mbt_pack_end / mbt_unpack_end assert bounds and return bytes used.
 * Per-operation checks use assert() (debug only); _end uses abort() always.
 */

#define MBT_ASSERT(cond) do { \
	if (!(cond)) { \
		fprintf(stderr, "%s:%d: assertion failed: %s\n", \
		        __FILE__, __LINE__, #cond); \
		abort(); \
	} \
} while (0)

struct mbt_packer {
	unsigned char *start;
	unsigned char *p;
	unsigned char *end;
};

struct mbt_unpacker {
	const unsigned char *start;
	const unsigned char *p;
	const unsigned char *end;
};

inline void
mbt_pack_init(struct mbt_packer *pk, void *buf, size_t len)
{
	pk->start = pk->p = (unsigned char *)buf;
	pk->end = pk->start + len;
}

inline void
mbt_pack_u8(struct mbt_packer *pk, uint8_t v)
{
	assert(pk->p + 1 <= pk->end);
	*pk->p++ = v;
}

inline void
mbt_pack_u32(struct mbt_packer *pk, uint32_t v)
{
	assert(pk->p + 4 <= pk->end);
	uint32_t be = MBT_HTOBE32(v);
	memcpy(pk->p, &be, 4);
	pk->p += 4;
}

inline void
mbt_pack_mem(struct mbt_packer *pk, const void *src, size_t n)
{
	assert(pk->p + n <= pk->end);
	memcpy(pk->p, src, n);
	pk->p += n;
}

inline size_t
mbt_pack_end(struct mbt_packer *pk)
{
	MBT_ASSERT(pk->p <= pk->end);
	return (size_t)(pk->p - pk->start);
}

inline void
mbt_unpack_init(struct mbt_unpacker *pk, const void *buf, size_t len)
{
	pk->start = pk->p = (const unsigned char *)buf;
	pk->end = pk->start + len;
}

inline uint8_t
mbt_unpack_u8(struct mbt_unpacker *pk)
{
	assert(pk->p + 1 <= pk->end);
	return *pk->p++;
}

inline uint32_t
mbt_unpack_u32(struct mbt_unpacker *pk)
{
	assert(pk->p + 4 <= pk->end);
	uint32_t be;
	memcpy(&be, pk->p, 4);
	pk->p += 4;
	return MBT_BE32TOH(be);
}

inline void
mbt_unpack_mem(struct mbt_unpacker *pk, void *dst, size_t n)
{
	assert(pk->p + n <= pk->end);
	memcpy(dst, pk->p, n);
	pk->p += n;
}

inline size_t
mbt_unpack_end(struct mbt_unpacker *pk)
{
	MBT_ASSERT(pk->p <= pk->end);
	return (size_t)(pk->p - pk->start);
}

#endif
