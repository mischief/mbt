#include <assert.h>
#include <string.h>

#include "infohash.h"

static void
test_new_and_string(void)
{
	struct infohash *ih = NULL;
	char buf[INFOHASH_SHA1HEXLEN + 1];
	int rv;

	rv = infohash_new(&ih, INFOHASH_SHA1, "611F70899D4E1D6A9C39CFC925F103DFEF630328");
	assert(rv == 0);
	assert(ih != NULL);

	rv = infohash_string(ih, buf, sizeof(buf));
	assert(rv == 0);
	assert(strcmp(buf, "611F70899D4E1D6A9C39CFC925F103DFEF630328") == 0);

	infohash_unref(ih);
}

static void
test_cmp_equal(void)
{
	struct infohash *a = NULL, *b = NULL;

	infohash_new(&a, INFOHASH_SHA1, "611F70899D4E1D6A9C39CFC925F103DFEF630328");
	infohash_new(&b, INFOHASH_SHA1, "611F70899D4E1D6A9C39CFC925F103DFEF630328");

	assert(infohash_cmp(a, b) == 0);

	infohash_unref(a);
	infohash_unref(b);
}

static void
test_cmp_different(void)
{
	struct infohash *a = NULL, *b = NULL;

	infohash_new(&a, INFOHASH_SHA1, "611F70899D4E1D6A9C39CFC925F103DFEF630328");
	infohash_new(&b, INFOHASH_SHA1, "C8295CE630F2064F08440DB1534E4992CFE4862A");

	assert(infohash_cmp(a, b) != 0);

	infohash_unref(a);
	infohash_unref(b);
}

static void
test_ref_unref(void)
{
	struct infohash *ih = NULL, *ref;

	infohash_new(&ih, INFOHASH_SHA1, "611F70899D4E1D6A9C39CFC925F103DFEF630328");

	ref = infohash_ref(ih);
	assert(ref == ih);

	/* ref=2, unref once → ref=1, still alive */
	infohash_unref(ih);

	/* ref=1, unref → ref=0, freed */
	infohash_unref(ref);
}

static void
test_unrefp(void)
{
	struct infohash *ih = NULL;

	infohash_new(&ih, INFOHASH_SHA1, "611F70899D4E1D6A9C39CFC925F103DFEF630328");
	infohash_unrefp(&ih);

	/* NULL is safe */
	infohash_unrefp(&ih);
}

static void
test_string_buf_too_small(void)
{
	struct infohash *ih = NULL;
	char buf[10];
	int rv;

	infohash_new(&ih, INFOHASH_SHA1, "611F70899D4E1D6A9C39CFC925F103DFEF630328");

	rv = infohash_string(ih, buf, sizeof(buf));
	assert(rv < 0);

	infohash_unref(ih);
}

int
main(void)
{
	test_new_and_string();
	test_cmp_equal();
	test_cmp_different();
	test_ref_unref();
	test_unrefp();
	test_string_buf_too_small();
	return 0;
}
