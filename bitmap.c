#include <stdlib.h>
#include <stddef.h>
#include <stdint.h>
#include <string.h>
#include <errno.h>
#include <assert.h>
#include <sys/types.h>

#include "bitmap.h"

struct mbt_bitmap
{
	int ref;
	size_t bits;
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
	tb->bits = sz * 8;

	*out = tb;

	return 0;
}

int
mbt_bitmap_new_bits(struct mbt_bitmap **out, size_t nbits)
{
	size_t sz = (nbits + 7) / 8;
	int rv = mbt_bitmap_new(out, sz);
	if (rv == 0)
		(*out)->bits = nbits;
	return rv;
}

int
mbt_bitmap_fromarray(struct mbt_bitmap **out, uint8_t *buf, size_t len, size_t nbits)
{
	int rv;
	size_t i;
	struct mbt_bitmap *tb;

	if((rv = mbt_bitmap_new(&tb, len)) < 0)
		return rv;

	tb->bits = nbits ? nbits : len * 8;

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
	return tb->bits;
}

size_t
mbt_bitmap_bytes(struct mbt_bitmap *tb)
{
	return tb->bytes;
}

void
mbt_bitmap_raw(struct mbt_bitmap *tb, void *out, size_t len)
{
	size_t n = len < tb->bytes ? len : tb->bytes;
	memcpy(out, tb->map, n);
}

int
mbt_bitmap_set(struct mbt_bitmap *tb, size_t bit)
{
	uint8_t *p, setbit = 128 >> (bit & 7);
	size_t byte = bit >> 3;

	if (bit >= tb->bits)
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

	if (bit >= tb->bits)
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

	for (i = 0; i < tb->bits; i++) {
		if (mbt_bitmap_isset(tb, i) > 0)
			count++;
	}

	return count;
}

int
mbt_bitmap_full(struct mbt_bitmap *tb)
{
	for (size_t i = 0; i < tb->bits; i++) {
		if (mbt_bitmap_isset(tb, i) <= 0)
			return 0;
	}
	return 1;
}

ssize_t
mbt_bitmap_first_clear(struct mbt_bitmap *tb)
{
	for (size_t i = 0; i < tb->bits; i++) {
		if (mbt_bitmap_isset(tb, i) <= 0)
			return (ssize_t)i;
	}
	return -1;
}

ssize_t
mbt_bitmap_first_set(struct mbt_bitmap *tb)
{
	for (size_t i = 0; i < tb->bits; i++) {
		if (mbt_bitmap_isset(tb, i) > 0)
			return (ssize_t)i;
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

