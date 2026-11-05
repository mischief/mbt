// SPDX-License-Identifier: ISC
#ifndef MBT_PACK_H
#define MBT_PACK_H

#include <stdint.h>
#include <stddef.h>
#include <errno.h>
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
 * bounds in every build. The first failure sets err to -EMSGSIZE. Later calls
 * do nothing and reads return 0, so a caller can check once, at the end call.
 */

struct mbt_packer {
	unsigned char *start;
	unsigned char *p;
	unsigned char *end;
	int err;    /* first error, negative errno */
};

struct mbt_unpacker {
	const unsigned char *start;
	const unsigned char *p;
	const unsigned char *end;
	int err;    /* first error, negative errno */
};

MBT_API inline void
mbt_pack_init(struct mbt_packer *pk, void *buf, size_t len)
{
	pk->start = pk->p = (unsigned char *)buf;
	pk->end = pk->start + len;
	pk->err = 0;
}

MBT_API inline int
mbt_pack_u8(struct mbt_packer *pk, uint8_t v)
{
	if (pk->err == 0 && pk->end - pk->p < 1)
		pk->err = -EMSGSIZE;
	if (pk->err < 0)
		return pk->err;
	*pk->p++ = v;
	return 0;
}

MBT_API inline int
mbt_pack_u32(struct mbt_packer *pk, uint32_t v)
{
	if (pk->err == 0 && pk->end - pk->p < 4)
		pk->err = -EMSGSIZE;
	if (pk->err < 0)
		return pk->err;
	uint32_t be = MBT_HTOBE32(v);
	memcpy(pk->p, &be, 4);
	pk->p += 4;
	return 0;
}

MBT_API inline int
mbt_pack_mem(struct mbt_packer *pk, const void *src, size_t n)
{
	if (pk->err == 0 && (size_t)(pk->end - pk->p) < n)
		pk->err = -EMSGSIZE;
	if (pk->err < 0)
		return pk->err;
	memcpy(pk->p, src, n);
	pk->p += n;
	return 0;
}

MBT_API inline int __attribute__((warn_unused_result))
mbt_pack_end(struct mbt_packer *pk, size_t *out)
{
	if (pk->err < 0)
		return pk->err;
	*out = (size_t)(pk->p - pk->start);
	return 0;
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
	if (pk->err == 0 && pk->end - pk->p < 1)
		pk->err = -EMSGSIZE;
	if (pk->err < 0)
		return 0;
	return *pk->p++;
}

MBT_API inline uint32_t
mbt_unpack_u32(struct mbt_unpacker *pk)
{
	if (pk->err == 0 && pk->end - pk->p < 4)
		pk->err = -EMSGSIZE;
	if (pk->err < 0)
		return 0;
	uint32_t be;
	memcpy(&be, pk->p, 4);
	pk->p += 4;
	return MBT_BE32TOH(be);
}

MBT_API inline void
mbt_unpack_mem(struct mbt_unpacker *pk, void *dst, size_t n)
{
	if (pk->err == 0 && (size_t)(pk->end - pk->p) < n)
		pk->err = -EMSGSIZE;
	if (pk->err < 0) {
		memset(dst, 0, n);
		return;
	}
	memcpy(dst, pk->p, n);
	pk->p += n;
}

MBT_API inline int __attribute__((warn_unused_result))
mbt_unpack_end(struct mbt_unpacker *pk, size_t *out)
{
	if (pk->err < 0)
		return pk->err;
	*out = (size_t)(pk->p - pk->start);
	return 0;
}

#endif
