#include <stdlib.h>
#include <stddef.h>
#include <stdint.h>
#include <string.h>
#include <errno.h>
#include <assert.h>

#include <tbmap.h>

struct tbmap
{
	int ref;
	size_t bytes;
	uint8_t map[];
};

int
tbmap_new(struct tbmap **out, size_t sz)
{
	struct tbmap *tb;

	tb = calloc(1, sizeof(*tb) + sz);
	if(!tb)
		return -ENOMEM;

	tb->ref = 1;
	tb->bytes = sz;

	*out = tb;

	return 0;
}

int
tbmap_fromarray(struct tbmap **out, uint8_t *buf, size_t len)
{
	int rv;
	size_t i;
	struct tbmap *tb;

	if((rv = tbmap_new(&tb, len)) < 0)
		return rv;

	for(i = 0; i < len; i++){
		tb->map[i] = buf[i];
	}

	*out = tb;

	return 0;
}

void
tbmap_unref(struct tbmap *tb)
{
	if(!tb)
		return;

	assert(tb->ref > 0);

	if(--tb->ref > 0)
		return;

	free(tb);
}

void
tbmap_unrefp(struct tbmap **tb)
{
	if(*tb)
		tbmap_unref(*tb);
}

size_t
tbmap_bits(struct tbmap *tb)
{
	return tb->bytes * 8;
}

size_t
tbmap_bytes(struct tbmap *tb)
{
	return tb->bytes;
}

void
tbmap_set(struct tbmap *tb, size_t bit)
{
	uint8_t *p, setbit = 128 >> (bit & 7);
	size_t byte = bit >> 3;

	assert(byte < tb->bytes);

	p = tb->map + byte;

	*p |= setbit;
}

int
tbmap_isset(struct tbmap *tb, size_t bit)
{
	uint8_t *p, checkbit = 128 >> (bit & 7);
	size_t byte = bit >> 3;

	assert(byte < tb->bytes);

	p = tb->map + byte;

	return (*p & checkbit) != 0;
}

void
tbmap_clear(struct tbmap *tb)
{
	memset(tb->map, 0, tb->bytes);
}

int
tbmap_union(struct tbmap *a, struct tbmap *b, struct tbmap *out)
{
	size_t i, len;
	register uint8_t *outp, *ap, *bp;

	assert(a);
	assert(b);
	assert(out);

	len = tbmap_bytes(a);

	if(len != tbmap_bytes(b))
		return -EINVAL;

	if(len != tbmap_bytes(out))
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
tbmap_intersection(struct tbmap *a, struct tbmap *b, struct tbmap *out)
{
	size_t i, len;
	register uint8_t *outp, *ap, *bp;

	assert(a);
	assert(b);
	assert(out);

	len = tbmap_bytes(a);

	if(len != tbmap_bytes(b))
		return -EINVAL;

	if(len != tbmap_bytes(out))
		return -EINVAL;

	outp = out->map;
	ap = a->map;
	bp = b->map;

	for(i = 0; i < len; i++){
		outp[i] = ap[i] & bp[i];
	}

	return 0;
}

