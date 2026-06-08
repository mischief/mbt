// SPDX-License-Identifier: ISC
#ifndef MBT_PACK_H
#define MBT_PACK_H

/*
 * Wire format pack/unpack helpers.
 *
 * Format characters:
 *   'b' — 1 byte (pack: int arg, unpack: unsigned char* out)
 *   'l' — 4 bytes big-endian uint32 (pack: uint32_t, unpack: uint32_t*)
 *   '*' — raw bytes (pack: size_t len, void* data; unpack: int len, unsigned char* out)
 *
 * Returns number of bytes consumed/produced.
 */

int mbt_pack(unsigned char *out, char *v, ...);
int mbt_unpack(unsigned char *in, char *v, ...);

#endif
