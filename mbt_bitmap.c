#include <stdlib.h>
#include <stddef.h>
#include <stdint.h>
#include <string.h>
#include <errno.h>
#include <assert.h>
#include <sys/types.h>

#include "mbt_bitmap.h"

struct mbt_bitmap
{
	int ref;
	size_t bytes;
	uint8_t map[];
};

int
mbt_bitmap_new(struct mbt_bitmap **out, size_t sz)
{
	struct mbt_bitmap *tb;

	tb = calloc(1, sizeof(*tb) + sz);
	if(!tb)
		return -ENOMEM;

	tb->ref = 1;
	tb->bytes = sz;

	*out = tb;

	return 0;
}

int
mbt_bitmap_fromarray(struct mbt_bitmap **out, uint8_t *buf, size_t len)
{
	int rv;
	size_t i;
	struct mbt_bitmap *tb;

	if((rv = mbt_bitmap_new(&tb, len)) < 0)
		return rv;

	for(i = 0; i < len; i++){
		tb->map[i] = buf[i];
	}

	*out = tb;

	return 0;
}

void
mbt_bitmap_unref(struct mbt_bitmap *tb)
{
	if(!tb)
		return;

	assert(tb->ref > 0);

	if(--tb->ref > 0)
		return;

	free(tb);
}

void
mbt_bitmap_unrefp(struct mbt_bitmap **tb)
{
	if(*tb)
		mbt_bitmap_unref(*tb);
}

size_t
mbt_bitmap_bits(struct mbt_bitmap *tb)
{
	return tb->bytes * 8;
}

size_t
mbt_bitmap_bytes(struct mbt_bitmap *tb)
{
	return tb->bytes;
}

int
mbt_bitmap_set(struct mbt_bitmap *tb, size_t bit)
{
	uint8_t *p, setbit = 128 >> (bit & 7);
	size_t byte = bit >> 3;

	if (byte >= tb->bytes)
		return -EINVAL;

	p = tb->map + byte;
	*p |= setbit;
	return 0;
}

int
mbt_bitmap_isset(struct mbt_bitmap *tb, size_t bit)
{
	uint8_t *p, checkbit = 128 >> (bit & 7);
	size_t byte = bit >> 3;

	if (byte >= tb->bytes)
		return -EINVAL;

	p = tb->map + byte;
	return (*p & checkbit) != 0;
}

void
mbt_bitmap_clear(struct mbt_bitmap *tb)
{
	memset(tb->map, 0, tb->bytes);
}

size_t
mbt_bitmap_count(struct mbt_bitmap *tb)
{
	size_t i, count = 0;

	for (i = 0; i < tb->bytes; i++) {
		uint8_t v = tb->map[i];
		while (v) {
			count += v & 1;
			v >>= 1;
		}
	}

	return count;
}

int
mbt_bitmap_full(struct mbt_bitmap *tb)
{
	size_t i;

	for (i = 0; i < tb->bytes; i++) {
		if (tb->map[i] != 0xFF)
			return 0;
	}

	return 1;
}

ssize_t
mbt_bitmap_first_clear(struct mbt_bitmap *tb)
{
	size_t i, bit;

	for (i = 0; i < tb->bytes; i++) {
		if (tb->map[i] != 0xFF) {
			for (bit = 0; bit < 8; bit++) {
				if (!(tb->map[i] & (128 >> bit)))
					return (ssize_t)(i * 8 + bit);
			}
		}
	}

	return -1;
}

ssize_t
mbt_bitmap_first_set(struct mbt_bitmap *tb)
{
	size_t i, bit;

	for (i = 0; i < tb->bytes; i++) {
		if (tb->map[i] != 0) {
			for (bit = 0; bit < 8; bit++) {
				if (tb->map[i] & (128 >> bit))
					return (ssize_t)(i * 8 + bit);
			}
		}
	}

	return -1;
}

int
mbt_bitmap_union(struct mbt_bitmap *a, struct mbt_bitmap *b, struct mbt_bitmap *out)
{
	size_t i, len;
	register uint8_t *outp, *ap, *bp;

	assert(a);
	assert(b);
	assert(out);

	len = mbt_bitmap_bytes(a);

	if(len != mbt_bitmap_bytes(b))
		return -EINVAL;

	if(len != mbt_bitmap_bytes(out))
		return -EINVAL;

	outp = out->map;
	ap = a->map;
	bp = b->map;

	for(i = 0; i < len; i++){
		outp[i] = ap[i] | bp[i];
	}

	return 0;
}

int
mbt_bitmap_intersection(struct mbt_bitmap *a, struct mbt_bitmap *b, struct mbt_bitmap *out)
{
	size_t i, len;
	register uint8_t *outp, *ap, *bp;

	assert(a);
	assert(b);
	assert(out);

	len = mbt_bitmap_bytes(a);

	if(len != mbt_bitmap_bytes(b))
		return -EINVAL;

	if(len != mbt_bitmap_bytes(out))
		return -EINVAL;

	outp = out->map;
	ap = a->map;
	bp = b->map;

	for(i = 0; i < len; i++){
		outp[i] = ap[i] & bp[i];
	}

	return 0;
}

