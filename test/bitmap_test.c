// SPDX-License-Identifier: ISC
#include <stdint.h>
#include <stddef.h>
#include <string.h>
#include <assert.h>
#include <sys/types.h>

#include "mbt/bitmap.h"

static void
test_basic(void)
{
	int rv;
	__attribute__((cleanup(mbt_bitmap_unrefp))) struct mbt_bitmap *tb = NULL;

	rv = mbt_bitmap_new(&tb, 1);
	assert(rv == 0);
	assert(mbt_bitmap_bytes(tb) == 1);
	assert(mbt_bitmap_bits(tb) == 8);

	mbt_bitmap_set(tb, 0);
	mbt_bitmap_set(tb, 7);
	assert(mbt_bitmap_isset(tb, 0));
	assert(mbt_bitmap_isset(tb, 7));
	assert(!mbt_bitmap_isset(tb, 1));
	assert(!mbt_bitmap_isset(tb, 6));
}

static void
test_clear(void)
{
	struct mbt_bitmap *tb = NULL;
	mbt_bitmap_new(&tb, 2);

	mbt_bitmap_set(tb, 0);
	mbt_bitmap_set(tb, 8);
	mbt_bitmap_set(tb, 15);
	assert(mbt_bitmap_isset(tb, 0));
	assert(mbt_bitmap_isset(tb, 8));
	assert(mbt_bitmap_isset(tb, 15));

	mbt_bitmap_clear(tb);
	assert(!mbt_bitmap_isset(tb, 0));
	assert(!mbt_bitmap_isset(tb, 8));
	assert(!mbt_bitmap_isset(tb, 15));

	mbt_bitmap_unref(tb);
}

static void
test_fromarray(void)
{
	struct mbt_bitmap *tb = NULL;
	uint8_t data[] = { 0x80, 0x01 }; /* bit 0 and bit 15 set */

	mbt_bitmap_fromarray(&tb, data, 2, 16);
	assert(mbt_bitmap_bytes(tb) == 2);
	assert(mbt_bitmap_bits(tb) == 16);
	assert(mbt_bitmap_isset(tb, 0));
	assert(!mbt_bitmap_isset(tb, 1));
	assert(mbt_bitmap_isset(tb, 15));
	assert(!mbt_bitmap_isset(tb, 14));

	mbt_bitmap_unref(tb);
}

static void
test_union(void)
{
	struct mbt_bitmap *a = NULL, *b = NULL, *out = NULL;

	mbt_bitmap_new(&a, 1);
	mbt_bitmap_new(&b, 1);
	mbt_bitmap_new(&out, 1);

	mbt_bitmap_set(a, 0);
	mbt_bitmap_set(a, 2);
	mbt_bitmap_set(b, 1);
	mbt_bitmap_set(b, 2);

	int rv = mbt_bitmap_union(a, b, out);
	assert(rv == 0);
	assert(mbt_bitmap_isset(out, 0));
	assert(mbt_bitmap_isset(out, 1));
	assert(mbt_bitmap_isset(out, 2));
	assert(!mbt_bitmap_isset(out, 3));

	mbt_bitmap_unref(a);
	mbt_bitmap_unref(b);
	mbt_bitmap_unref(out);
}

static void
test_intersection(void)
{
	struct mbt_bitmap *a = NULL, *b = NULL, *out = NULL;

	mbt_bitmap_new(&a, 1);
	mbt_bitmap_new(&b, 1);
	mbt_bitmap_new(&out, 1);

	mbt_bitmap_set(a, 0);
	mbt_bitmap_set(a, 2);
	mbt_bitmap_set(b, 1);
	mbt_bitmap_set(b, 2);

	int rv = mbt_bitmap_intersection(a, b, out);
	assert(rv == 0);
	assert(!mbt_bitmap_isset(out, 0));
	assert(!mbt_bitmap_isset(out, 1));
	assert(mbt_bitmap_isset(out, 2));

	mbt_bitmap_unref(a);
	mbt_bitmap_unref(b);
	mbt_bitmap_unref(out);
}

static void
test_size_mismatch(void)
{
	struct mbt_bitmap *a = NULL, *b = NULL, *out = NULL;

	mbt_bitmap_new(&a, 1);
	mbt_bitmap_new(&b, 2);
	mbt_bitmap_new(&out, 1);

	assert(mbt_bitmap_union(a, b, out) < 0);
	assert(mbt_bitmap_intersection(a, b, out) < 0);

	mbt_bitmap_unref(a);
	mbt_bitmap_unref(b);
	mbt_bitmap_unref(out);
}

static void
test_count(void)
{
	struct mbt_bitmap *tb = NULL;
	mbt_bitmap_new(&tb, 2);

	assert(mbt_bitmap_count(tb) == 0);

	mbt_bitmap_set(tb, 0);
	assert(mbt_bitmap_count(tb) == 1);

	mbt_bitmap_set(tb, 7);
	mbt_bitmap_set(tb, 8);
	mbt_bitmap_set(tb, 15);
	assert(mbt_bitmap_count(tb) == 4);

	mbt_bitmap_unref(tb);
}

static void
test_full(void)
{
	struct mbt_bitmap *tb = NULL;
	mbt_bitmap_new(&tb, 1);

	assert(!mbt_bitmap_full(tb));

	for (int i = 0; i < 8; i++)
		mbt_bitmap_set(tb, i);

	assert(mbt_bitmap_full(tb));

	mbt_bitmap_unref(tb);
}

static void
test_first_clear(void)
{
	struct mbt_bitmap *tb = NULL;
	mbt_bitmap_new(&tb, 2);

	/* empty — first clear is bit 0 */
	assert(mbt_bitmap_first_clear(tb) == 0);

	mbt_bitmap_set(tb, 0);
	assert(mbt_bitmap_first_clear(tb) == 1);

	/* fill first byte */
	for (int i = 0; i < 8; i++)
		mbt_bitmap_set(tb, i);
	assert(mbt_bitmap_first_clear(tb) == 8);

	/* fill all */
	for (int i = 8; i < 16; i++)
		mbt_bitmap_set(tb, i);
	assert(mbt_bitmap_first_clear(tb) == -1);

	mbt_bitmap_unref(tb);
}

static void
test_first_set(void)
{
	struct mbt_bitmap *tb = NULL;
	mbt_bitmap_new(&tb, 2);

	/* empty */
	assert(mbt_bitmap_first_set(tb) == -1);

	mbt_bitmap_set(tb, 5);
	assert(mbt_bitmap_first_set(tb) == 5);

	mbt_bitmap_set(tb, 1);
	assert(mbt_bitmap_first_set(tb) == 1);

	mbt_bitmap_unref(tb);
}

static void
test_multi_byte(void)
{
	struct mbt_bitmap *tb = NULL;
	mbt_bitmap_new(&tb, 4);
	assert(mbt_bitmap_bits(tb) == 32);

	mbt_bitmap_set(tb, 0);
	mbt_bitmap_set(tb, 7);
	mbt_bitmap_set(tb, 8);
	mbt_bitmap_set(tb, 16);
	mbt_bitmap_set(tb, 31);

	assert(mbt_bitmap_isset(tb, 0));
	assert(mbt_bitmap_isset(tb, 7));
	assert(mbt_bitmap_isset(tb, 8));
	assert(mbt_bitmap_isset(tb, 16));
	assert(mbt_bitmap_isset(tb, 31));
	assert(!mbt_bitmap_isset(tb, 1));
	assert(!mbt_bitmap_isset(tb, 30));

	/* out of bounds returns error */
	assert(mbt_bitmap_set(tb, 32) < 0);
	assert(mbt_bitmap_set(tb, 100) < 0);
	assert(mbt_bitmap_isset(tb, 32) < 0);
	assert(mbt_bitmap_isset(tb, 100) < 0);

	mbt_bitmap_unref(tb);
}

static void
test_non_round_bits(void)
{
	struct mbt_bitmap *tb = NULL;

	/* 10 bits = 2 bytes, but only 10 valid bits */
	mbt_bitmap_new_bits(&tb, 10);
	assert(mbt_bitmap_bits(tb) == 10);
	assert(mbt_bitmap_bytes(tb) == 2);

	/* Set all 10 bits */
	for (int i = 0; i < 10; i++)
		mbt_bitmap_set(tb, i);

	assert(mbt_bitmap_full(tb));
	assert(mbt_bitmap_count(tb) == 10);
	assert(mbt_bitmap_first_clear(tb) == -1);

	/* Spare bits (10-15) should not be addressable */
	assert(mbt_bitmap_set(tb, 10) < 0);
	assert(mbt_bitmap_isset(tb, 10) < 0);

	/* Clear one bit — no longer full */
	mbt_bitmap_clear(tb);
	assert(!mbt_bitmap_full(tb));
	assert(mbt_bitmap_count(tb) == 0);
	assert(mbt_bitmap_first_clear(tb) == 0);

	mbt_bitmap_unref(tb);
}

int
main(void)
{
	test_basic();
	test_clear();
	test_fromarray();
	test_union();
	test_intersection();
	test_size_mismatch();
	test_count();
	test_full();
	test_first_clear();
	test_first_set();
	test_multi_byte();
	test_non_round_bits();
	return 0;
}
