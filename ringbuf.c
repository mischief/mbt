// SPDX-License-Identifier: ISC
#include <stdlib.h>
#include <string.h>
#include <stdint.h>
#include <errno.h>

#include "mbt/ringbuf.h"

#define RINGBUF_MIN	1460	/* 1 MSS */

struct mbt_ringbuf {
	uint8_t    *buf;
	uint32_t    cap;	/* allocated capacity (power of 2) */
	uint32_t    mask;	/* cap - 1 */
	uint32_t    max;	/* hard limit (0 = unbounded) */
	uint32_t    rd;		/* monotonic read offset */
	uint32_t    wr;		/* monotonic write offset */
	uint32_t    peak;	/* high-water mark of live data */
};

static uint32_t
pow2_ceil(uint32_t v)
{
	uint32_t p = 1;
	while (p < v)
		p <<= 1;
	return p;
}

int
mbt_ringbuf_new(struct mbt_ringbuf **out, uint32_t max_cap)
{
	struct mbt_ringbuf *r = calloc(1, sizeof(*r));
	if (!r)
		return -ENOMEM;

	r->max = max_cap;
	*out = r;
	return 0;
}

void
mbt_ringbuf_free(struct mbt_ringbuf *r)
{
	if (!r)
		return;
	free(r->buf);
	free(r);
}

uint32_t
mbt_ringbuf_available(const struct mbt_ringbuf *r)
{
	return r->wr - r->rd;
}

uint32_t
mbt_ringbuf_space(const struct mbt_ringbuf *r)
{
	return r->cap - mbt_ringbuf_available(r);
}

uint32_t
mbt_ringbuf_cap(const struct mbt_ringbuf *r)
{
	return r->cap;
}

uint32_t
mbt_ringbuf_rd(const struct mbt_ringbuf *r)
{
	return r->rd;
}

int
mbt_ringbuf_grow(struct mbt_ringbuf *r, uint32_t want, uint32_t *out_delta)
{
	uint32_t avail = mbt_ringbuf_available(r);
	uint32_t need = avail + want;

	/* Minimum allocation, clamped to max_cap so small-capacity buffers work */
	uint32_t min_init = RINGBUF_MIN;
	if (r->max && r->max < min_init)
		min_init = r->max;
	if (need < min_init)
		need = min_init;

	uint32_t new_cap = r->cap ? r->cap * 2 : pow2_ceil(min_init);
	/* On first alloc, ensure we can satisfy the requested size */
	if (!r->cap && new_cap < need)
		new_cap = pow2_ceil(need);
	if (r->max && new_cap > r->max)
		new_cap = r->max;
	if (new_cap < need)
		return -ENOBUFS;	/* at max capacity */

	if (new_cap <= r->cap)
		return -ENOBUFS;	/* cannot grow further */

	uint32_t old_rd = r->rd;
	uint8_t *old = r->buf;

	uint8_t *tmp = malloc(new_cap);
	if (!tmp)
		return -ENOMEM;

	if (avail > 0) {
		uint32_t start = old_rd & r->mask;
		uint32_t first = r->cap - start;
		if (first > avail)
			first = avail;
		if (avail <= first) {
			memcpy(tmp, old + start, avail);
		} else {
			memcpy(tmp, old + start, first);
			memcpy(tmp + first, old, avail - first);
		}
	}
	free(old);

	r->buf = tmp;
	r->cap = new_cap;
	r->mask = new_cap - 1;
	r->rd = 0;
	r->wr = avail;

	if (out_delta)
		*out_delta = old_rd;
	return 0;
}

int
mbt_ringbuf_shrink(struct mbt_ringbuf *r, uint32_t min_cap, uint32_t *out_delta)
{
	if (!r->buf || !r->cap) {
		if (out_delta)
			*out_delta = 0;
		return 0;
	}

	uint32_t avail = mbt_ringbuf_available(r);

	if (avail == 0) {
		uint32_t old_rd = r->rd;
		free(r->buf);
		r->buf = NULL;
		r->cap = 0;
		r->mask = 0;
		r->rd = 0;
		r->wr = 0;
		if (out_delta)
			*out_delta = old_rd;
		return 0;
	}

	/* Target must hold at least `avail + min_cap`.
	 * Shrink has no RINGBUF_MIN floor -- caller controls the target. */
	uint32_t target = pow2_ceil(avail + min_cap);
	if (target >= r->cap) {
		if (out_delta)
			*out_delta = 0;
		return 0;
	}

	uint32_t old_rd = r->rd;
	uint8_t *old = r->buf;
	uint8_t *tmp = malloc(target);
	if (!tmp) {
		if (out_delta)
			*out_delta = 0;
		return 0; /* non-fatal */
	}

	uint32_t start = old_rd & r->mask;
	uint32_t first = r->cap - start;
	if (first > avail)
		first = avail;
	if (avail <= first) {
		memcpy(tmp, old + start, avail);
	} else {
		memcpy(tmp, old + start, first);
		memcpy(tmp + first, old, avail - first);
	}
	free(old);

	r->buf = tmp;
	r->cap = target;
	r->mask = target - 1;
	r->rd = 0;
	r->wr = avail;

	if (out_delta)
		*out_delta = old_rd;
	return 0;
}

void
mbt_ringbuf_update_peak(struct mbt_ringbuf *r)
{
	uint32_t avail = mbt_ringbuf_available(r);
	if (avail > r->peak)
		r->peak = avail;
}

uint32_t
mbt_ringbuf_peak(const struct mbt_ringbuf *r)
{
	return r->peak;
}

int
mbt_ringbuf_produce(struct mbt_ringbuf *r, const void *src, uint32_t len)
{
	const uint8_t *p = src;

	while (len) {
		if (!r->buf || r->wr + len > r->cap) {
			int err = mbt_ringbuf_grow(r, len, NULL);
			if (err)
				return err;
			if (r->wr + len > r->cap)
				return -ENOBUFS;
		}

		uint32_t idx = r->wr & r->mask;
		uint32_t chunk = r->cap - idx;
		if (chunk > len)
			chunk = len;
		memcpy(r->buf + idx, p, chunk);
		r->wr += chunk;
		p += chunk;
		len -= chunk;
	}
	return 0;
}

ssize_t
mbt_ringbuf_consume(struct mbt_ringbuf *r, void *dst, uint32_t len)
{
	uint32_t avail = mbt_ringbuf_available(r);
	if (len > avail)
		len = avail;
	if (!len)
		return 0;

	uint8_t *d = dst;
	while (len) {
		uint32_t idx = r->rd & r->mask;
		uint32_t chunk = r->cap - idx;
		if (chunk > len)
			chunk = len;
		memcpy(d, r->buf + idx, chunk);
		r->rd += chunk;
		d += chunk;
		len -= chunk;
	}
	return d - (uint8_t *)dst;
}

void
mbt_ringbuf_discard(struct mbt_ringbuf *r, uint32_t len)
{
	uint32_t avail = mbt_ringbuf_available(r);
	if (len > avail)
		len = avail;
	r->rd += len;
}

uint8_t *
mbt_ringbuf_write_ptr(struct mbt_ringbuf *r, uint32_t *out_len)
{
	if (!r->buf)
		return NULL;

	uint32_t space = mbt_ringbuf_space(r);
	if (!space) {
		if (out_len)
			*out_len = 0;
		return NULL;
	}

	uint32_t idx = r->wr & r->mask;
	uint32_t cont = r->cap - idx;
	if (cont > space)
		cont = space;

	if (out_len)
		*out_len = cont;
	return r->buf + idx;
}

void
mbt_ringbuf_write_done(struct mbt_ringbuf *r, uint32_t len)
{
	r->wr += len;
}

const uint8_t *
mbt_ringbuf_peek_at(const struct mbt_ringbuf *r, uint32_t offset, uint32_t *out_len)
{
	if (!r->buf) {
		if (out_len) *out_len = 0;
		return NULL;
	}
	uint32_t avail = mbt_ringbuf_available(r);
	if (offset >= avail) {
		if (out_len) *out_len = 0;
		return NULL;
	}
	uint32_t idx = (r->rd + offset) & r->mask;
	uint32_t cont = r->cap - idx;
	uint32_t remaining = avail - offset;
	if (cont > remaining)
		cont = remaining;
	if (out_len)
		*out_len = cont;
	return r->buf + idx;
}
