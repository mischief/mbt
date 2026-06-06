#include <stdlib.h>
#include <stddef.h>
#include <stdint.h>
#include <string.h>
#include <errno.h>
#include <assert.h>
#include <sys/types.h>

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

size_t
tbmap_count(struct tbmap *tb)
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
tbmap_full(struct tbmap *tb)
{
	size_t i;

	for (i = 0; i < tb->bytes; i++) {
		if (tb->map[i] != 0xFF)
			return 0;
	}

	return 1;
}

ssize_t
tbmap_first_clear(struct tbmap *tb)
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
tbmap_first_set(struct tbmap *tb)
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

