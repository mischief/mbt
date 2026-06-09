// SPDX-License-Identifier: ISC
#include "export.h"
#include <sys/types.h>
#include <stdint.h>
#include <stddef.h>

struct mbt_bitmap;

MBT_API int mbt_bitmap_new(struct mbt_bitmap **out, size_t sz);
MBT_API int mbt_bitmap_new_bits(struct mbt_bitmap **out, size_t nbits);
MBT_API int mbt_bitmap_fromarray(struct mbt_bitmap **out, uint8_t *buf, size_t len, size_t nbits);
MBT_API void mbt_bitmap_unref(struct mbt_bitmap *tb);
MBT_API void mbt_bitmap_unrefp(struct mbt_bitmap **tb);

MBT_API size_t mbt_bitmap_bits(struct mbt_bitmap *tb);
MBT_API size_t mbt_bitmap_bytes(struct mbt_bitmap *tb);
MBT_API void mbt_bitmap_raw(struct mbt_bitmap *tb, void *out, size_t len);

MBT_API int mbt_bitmap_set(struct mbt_bitmap *tb, size_t bit);    /* 0=ok, <0=out of bounds */
MBT_API int mbt_bitmap_isset(struct mbt_bitmap *tb, size_t bit);  /* 1=set, 0=clear, <0=out of bounds */

MBT_API void mbt_bitmap_clear(struct mbt_bitmap *tb);

MBT_API size_t mbt_bitmap_count(struct mbt_bitmap *tb);        /* number of set bits */
MBT_API int mbt_bitmap_full(struct mbt_bitmap *tb);            /* all bits set? */
MBT_API ssize_t mbt_bitmap_first_clear(struct mbt_bitmap *tb); /* first unset bit, or -1 */
MBT_API ssize_t mbt_bitmap_first_set(struct mbt_bitmap *tb);   /* first set bit, or -1 */

MBT_API int mbt_bitmap_union(struct mbt_bitmap *a, struct mbt_bitmap *b, struct mbt_bitmap *out);
MBT_API int mbt_bitmap_intersection(struct mbt_bitmap *a, struct mbt_bitmap *b, struct mbt_bitmap *out);

