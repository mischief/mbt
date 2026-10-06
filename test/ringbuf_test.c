// SPDX-License-Identifier: ISC
#include <assert.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <errno.h>
#include <stdint.h>

#include "mbt/ringbuf.h"

#define TEST(name)	static void name(void)
#define RUN(name)	do { printf("  %s...", #name); name(); printf(" ok\n"); } while(0)

static void
assert_buf(struct mbt_ringbuf *r, const uint8_t *expected, uint32_t elen)
{
	uint8_t tmp[4096];
	ssize_t n = mbt_ringbuf_consume(r, tmp, sizeof(tmp));
	assert(n > 0 && (uint32_t)n == elen);
	assert(memcmp(tmp, expected, elen) == 0);
	mbt_ringbuf_discard(r, 0); /* noop, rd already advanced */
	/* put data back: produce again and reset rd */
	assert(mbt_ringbuf_produce(r, expected, elen) == 0);
}

/* Basic produce/consume */
TEST(test_basic)
{
	struct mbt_ringbuf *r;
	assert(mbt_ringbuf_new(&r, 4096) == 0);
	assert(mbt_ringbuf_available(r) == 0);
	assert(mbt_ringbuf_cap(r) == 0);

	const char *hello = "hello";
	assert(mbt_ringbuf_produce(r, hello, 5) == 0);
	assert(mbt_ringbuf_available(r) == 5);
	assert(mbt_ringbuf_cap(r) >= 5);

	uint8_t buf[8];
	ssize_t n = mbt_ringbuf_consume(r, buf, 3);
	assert(n == 3);
	assert(memcmp(buf, "hel", 3) == 0);
	assert(mbt_ringbuf_available(r) == 2);

	n = mbt_ringbuf_consume(r, buf, 100);
	assert(n == 2);
	assert(memcmp(buf, "lo", 2) == 0);
	assert(mbt_ringbuf_available(r) == 0);

	mbt_ringbuf_free(r);
}

/* Discard */
TEST(test_discard)
{
	struct mbt_ringbuf *r;
	mbt_ringbuf_new(&r, 1024);

	uint8_t data[8] = { 1, 2, 3, 4, 5, 6, 7, 8 };
	mbt_ringbuf_produce(r, data, 8);
	mbt_ringbuf_discard(r, 3);
	assert(mbt_ringbuf_available(r) == 5);

	uint8_t buf[8];
	ssize_t n = mbt_ringbuf_consume(r, buf, 8);
	assert(n == 5);
	assert(buf[0] == 4 && buf[4] == 8);

	mbt_ringbuf_free(r);
}

/* Wrap-around: write past buffer midpoint, consume some, write again */
TEST(test_wrap)
{
	struct mbt_ringbuf *r;
	mbt_ringbuf_new(&r, 64);

	/* Force a small initial allocation, then fill and partially consume */
	uint8_t a[32];
	memset(a, 0xAA, 32);
	mbt_ringbuf_produce(r, a, 32);

	uint8_t tmp[16];
	mbt_ringbuf_consume(r, tmp, 16);
	assert(mbt_ringbuf_available(r) == 16);

	/* Write 20 more -- should wrap around the buffer */
	uint8_t b[20];
	memset(b, 0xBB, 20);
	mbt_ringbuf_produce(r, b, 20);
	assert(mbt_ringbuf_available(r) == 36);

	/* Read back 36 bytes */
	uint8_t out[40];
	ssize_t n = mbt_ringbuf_consume(r, out, 40);
	assert(n == 36);
	assert(out[0] == 0xAA && out[15] == 0xAA);
	assert(out[16] == 0xBB && out[35] == 0xBB);

	mbt_ringbuf_free(r);
}

/* Grow: start small, keep producing past initial cap */
TEST(test_grow)
{
	struct mbt_ringbuf *r;
	mbt_ringbuf_new(&r, 4096);

	uint8_t data[2048];
	for (int i = 0; i < 2048; i++)
		data[i] = i & 0xFF;

	mbt_ringbuf_produce(r, data, 2048);
	assert(mbt_ringbuf_available(r) == 2048);

	uint8_t out[2048];
	ssize_t n = mbt_ringbuf_consume(r, out, 2048);
	assert(n == 2048);
	for (int i = 0; i < 2048; i++)
		assert(out[i] == (i & 0xFF));

	mbt_ringbuf_free(r);
}

/* Max capacity: produce more than max returns -ENOBUFS */
TEST(test_max_cap)
{
	struct mbt_ringbuf *r;
	mbt_ringbuf_new(&r, 64); /* max 64 bytes */

	uint8_t data[64];
	memset(data, 0xFF, 64);
	assert(mbt_ringbuf_produce(r, data, 64) == 0);
	assert(mbt_ringbuf_available(r) == 64);

	uint8_t more[1];
	more[0] = 0;
	int err = mbt_ringbuf_produce(r, more, 1);
	assert(err == -ENOBUFS);
	assert(mbt_ringbuf_available(r) == 64); /* original data intact */

	mbt_ringbuf_free(r);
}

/* Grow with delta: verify external offset adjustment */
TEST(test_grow_delta)
{
	struct mbt_ringbuf *r;
	mbt_ringbuf_new(&r, 4096);

	uint8_t data[64];
	memset(data, 0xCC, 64);
	mbt_ringbuf_produce(r, data, 64);
	mbt_ringbuf_discard(r, 40); /* rd = 40, avail = 24 */

	uint32_t delta = 999;
	uint8_t more[32];
	memset(more, 0xDD, 32);
	int err = mbt_ringbuf_grow(r, 32, &delta);
	assert(err == 0);
	assert(delta == 40); /* rd was 40 */
	assert(mbt_ringbuf_available(r) == 24);

	mbt_ringbuf_produce(r, more, 32);
	assert(mbt_ringbuf_available(r) == 56);

	/* Verify data integrity */
	uint8_t out[64];
	ssize_t n = mbt_ringbuf_consume(r, out, 64);
	assert(n == 56);
	assert(out[0] == 0xCC && out[23] == 0xCC);
	assert(out[24] == 0xDD && out[55] == 0xDD);

	mbt_ringbuf_free(r);
}

/* Shrink: produce a lot, consume most, shrink back */
TEST(test_shrink)
{
	struct mbt_ringbuf *r;
	mbt_ringbuf_new(&r, 4096);

	uint8_t big[2048];
	memset(big, 0xEE, 2048);
	mbt_ringbuf_produce(r, big, 2048);
	uint32_t cap_after_prod = mbt_ringbuf_cap(r);

	/* Consume 2000 bytes, leaving 48 */
	uint8_t tmp[2048];
	mbt_ringbuf_consume(r, tmp, 2000);
	assert(mbt_ringbuf_available(r) == 48);

	uint32_t delta = 0;
	mbt_ringbuf_shrink(r, 64, &delta);
	uint32_t cap_after_shrink = mbt_ringbuf_cap(r);
	assert(cap_after_shrink < cap_after_prod);
	assert(cap_after_shrink >= 64);
	assert(mbt_ringbuf_available(r) == 48);

	/* Data still valid */
	uint8_t out[64];
	ssize_t n = mbt_ringbuf_consume(r, out, 64);
	assert(n == 48);
	for (int i = 0; i < 48; i++)
		assert(out[i] == 0xEE);

	mbt_ringbuf_free(r);
}

/* Shrink empty buffer */
TEST(test_shrink_empty)
{
	struct mbt_ringbuf *r;
	mbt_ringbuf_new(&r, 4096);

	uint8_t data[128];
	memset(data, 0xFF, 128);
	mbt_ringbuf_produce(r, data, 128);
	mbt_ringbuf_discard(r, 128);
	assert(mbt_ringbuf_available(r) == 0);

	uint32_t delta = 0;
	mbt_ringbuf_shrink(r, 64, &delta);
	assert(mbt_ringbuf_cap(r) == 0);
	assert(mbt_ringbuf_available(r) == 0);

	/* Can still produce after shrink-to-empty */
	uint8_t small[8] = { 1, 2, 3, 4, 5, 6, 7, 8 };
	assert(mbt_ringbuf_produce(r, small, 8) == 0);
	assert(mbt_ringbuf_available(r) == 8);

	mbt_ringbuf_free(r);
}

/* Peak tracking */
TEST(test_peak)
{
	struct mbt_ringbuf *r;
	mbt_ringbuf_new(&r, 4096);
	assert(mbt_ringbuf_peak(r) == 0);

	uint8_t data[256];
	memset(data, 0xAB, 256);
	mbt_ringbuf_produce(r, data, 256);
	mbt_ringbuf_update_peak(r);
	assert(mbt_ringbuf_peak(r) == 256);

	/* Consume half, peak stays */
	uint8_t tmp[128];
	mbt_ringbuf_consume(r, tmp, 128);
	assert(mbt_ringbuf_available(r) == 128);
	mbt_ringbuf_update_peak(r);
	assert(mbt_ringbuf_peak(r) == 256);

	/* Produce more to beat peak */
	mbt_ringbuf_produce(r, data, 128);
	assert(mbt_ringbuf_available(r) == 256);
	mbt_ringbuf_update_peak(r);
	assert(mbt_ringbuf_peak(r) == 256); /* same peak */

	mbt_ringbuf_free(r);
}

/* Zero-copy write pointer */
TEST(test_write_ptr)
{
	struct mbt_ringbuf *r;
	mbt_ringbuf_new(&r, 256);

	uint32_t wlen = 0;
	uint8_t *wp = mbt_ringbuf_write_ptr(r, &wlen);
	assert(wp == NULL); /* no buffer yet */

	/* Allocate by producing 1 byte */
	uint8_t init = 0;
	mbt_ringbuf_produce(r, &init, 1);

	wp = mbt_ringbuf_write_ptr(r, &wlen);
	assert(wp != NULL);
	assert(wlen > 0);

	uint8_t data[64];
	memset(data, 0x12, 64);
	uint32_t towrite = wlen < 64 ? wlen : 64;
	memcpy(wp, data, towrite);
	mbt_ringbuf_write_done(r, towrite);
	assert(mbt_ringbuf_available(r) == 1 + towrite);

	/* First byte is init (0), rest is 0x12 */
	uint8_t out[72];
	ssize_t n = mbt_ringbuf_consume(r, out, 72);
	assert(n == 1 + towrite);
	assert(out[0] == 0);
	for (uint32_t i = 1; i < towrite + 1; i++)
		assert(out[i] == 0x12);

	mbt_ringbuf_free(r);
}

/* Large produce: write more than initial cap in one call */
TEST(test_large_produce)
{
	struct mbt_ringbuf *r;
	mbt_ringbuf_new(&r, 8192);

	uint8_t data[4096];
	for (int i = 0; i < 4096; i++)
		data[i] = i & 0xFF;

	assert(mbt_ringbuf_produce(r, data, 4096) == 0);
	assert(mbt_ringbuf_available(r) == 4096);

	uint8_t out[4096];
	ssize_t n = mbt_ringbuf_consume(r, out, 4096);
	assert(n == 4096);
	for (int i = 0; i < 4096; i++)
		assert(out[i] == (i & 0xFF));

	mbt_ringbuf_free(r);
}

/* Unbounded ringbuf (max_cap = 0) */
TEST(test_unbounded)
{
	struct mbt_ringbuf *r;
	mbt_ringbuf_new(&r, 0); /* no limit */

	for (int i = 0; i < 10; i++) {
		uint8_t data[512];
		memset(data, i & 0xFF, 512);
		assert(mbt_ringbuf_produce(r, data, 512) == 0);
	}
	assert(mbt_ringbuf_available(r) == 5120);

	mbt_ringbuf_free(r);
}

/* Stress: produce and consume in small chunks, verify wrap */
TEST(test_stress_wrap)
{
	struct mbt_ringbuf *r;
	mbt_ringbuf_new(&r, 128);

	for (int round = 0; round < 100; round++) {
		uint8_t in[4] = { (uint8_t)round, 0, 0, 0 };
		assert(mbt_ringbuf_produce(r, in, 1) == 0);
		if (mbt_ringbuf_available(r) >= 3) {
			uint8_t out[4];
			ssize_t n = mbt_ringbuf_consume(r, out, 3);
			assert(n == 3);
			/* just verify we can read, not specific values */
			(void)out;
		}
	}

	/* Drain remaining */
	uint8_t tmp[128];
	ssize_t n = mbt_ringbuf_consume(r, tmp, 128);
	assert(n >= 0);
	assert(mbt_ringbuf_available(r) == 0);

	mbt_ringbuf_free(r);
}

int
main(void)
{
	printf("ringbuf:\n");
	RUN(test_basic);
	RUN(test_discard);
	RUN(test_wrap);
	RUN(test_grow);
	RUN(test_max_cap);
	RUN(test_grow_delta);
	RUN(test_shrink);
	RUN(test_shrink_empty);
	RUN(test_peak);
	RUN(test_write_ptr);
	RUN(test_large_produce);
	RUN(test_unbounded);
	RUN(test_stress_wrap);
	printf("all ringbuf tests passed\n");
	return 0;
}
