#include <stdint.h>
#include <stddef.h>
#include <string.h>
#include <assert.h>

#include <tbmap.h>

static void
test_basic(void)
{
	int rv;
	__attribute__((cleanup(tbmap_unrefp))) struct tbmap *tb = NULL;

	rv = tbmap_new(&tb, 1);
	assert(rv == 0);
	assert(tbmap_bytes(tb) == 1);
	assert(tbmap_bits(tb) == 8);

	tbmap_set(tb, 0);
	tbmap_set(tb, 7);
	assert(tbmap_isset(tb, 0));
	assert(tbmap_isset(tb, 7));
	assert(!tbmap_isset(tb, 1));
	assert(!tbmap_isset(tb, 6));
}

static void
test_clear(void)
{
	struct tbmap *tb = NULL;
	tbmap_new(&tb, 2);

	tbmap_set(tb, 0);
	tbmap_set(tb, 8);
	tbmap_set(tb, 15);
	assert(tbmap_isset(tb, 0));
	assert(tbmap_isset(tb, 8));
	assert(tbmap_isset(tb, 15));

	tbmap_clear(tb);
	assert(!tbmap_isset(tb, 0));
	assert(!tbmap_isset(tb, 8));
	assert(!tbmap_isset(tb, 15));

	tbmap_unref(tb);
}

static void
test_fromarray(void)
{
	struct tbmap *tb = NULL;
	uint8_t data[] = { 0x80, 0x01 }; /* bit 0 and bit 15 set */

	tbmap_fromarray(&tb, data, 2);
	assert(tbmap_bytes(tb) == 2);
	assert(tbmap_bits(tb) == 16);
	assert(tbmap_isset(tb, 0));
	assert(!tbmap_isset(tb, 1));
	assert(tbmap_isset(tb, 15));
	assert(!tbmap_isset(tb, 14));

	tbmap_unref(tb);
}

static void
test_union(void)
{
	struct tbmap *a = NULL, *b = NULL, *out = NULL;

	tbmap_new(&a, 1);
	tbmap_new(&b, 1);
	tbmap_new(&out, 1);

	tbmap_set(a, 0);
	tbmap_set(a, 2);
	tbmap_set(b, 1);
	tbmap_set(b, 2);

	int rv = tbmap_union(a, b, out);
	assert(rv == 0);
	assert(tbmap_isset(out, 0));
	assert(tbmap_isset(out, 1));
	assert(tbmap_isset(out, 2));
	assert(!tbmap_isset(out, 3));

	tbmap_unref(a);
	tbmap_unref(b);
	tbmap_unref(out);
}

static void
test_intersection(void)
{
	struct tbmap *a = NULL, *b = NULL, *out = NULL;

	tbmap_new(&a, 1);
	tbmap_new(&b, 1);
	tbmap_new(&out, 1);

	tbmap_set(a, 0);
	tbmap_set(a, 2);
	tbmap_set(b, 1);
	tbmap_set(b, 2);

	int rv = tbmap_intersection(a, b, out);
	assert(rv == 0);
	assert(!tbmap_isset(out, 0));
	assert(!tbmap_isset(out, 1));
	assert(tbmap_isset(out, 2));

	tbmap_unref(a);
	tbmap_unref(b);
	tbmap_unref(out);
}

static void
test_size_mismatch(void)
{
	struct tbmap *a = NULL, *b = NULL, *out = NULL;

	tbmap_new(&a, 1);
	tbmap_new(&b, 2);
	tbmap_new(&out, 1);

	assert(tbmap_union(a, b, out) < 0);
	assert(tbmap_intersection(a, b, out) < 0);

	tbmap_unref(a);
	tbmap_unref(b);
	tbmap_unref(out);
}

static void
test_multi_byte(void)
{
	struct tbmap *tb = NULL;
	tbmap_new(&tb, 4);
	assert(tbmap_bits(tb) == 32);

	tbmap_set(tb, 0);
	tbmap_set(tb, 7);
	tbmap_set(tb, 8);
	tbmap_set(tb, 16);
	tbmap_set(tb, 31);

	assert(tbmap_isset(tb, 0));
	assert(tbmap_isset(tb, 7));
	assert(tbmap_isset(tb, 8));
	assert(tbmap_isset(tb, 16));
	assert(tbmap_isset(tb, 31));
	assert(!tbmap_isset(tb, 1));
	assert(!tbmap_isset(tb, 30));

	tbmap_unref(tb);
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
	test_multi_byte();
	return 0;
}
