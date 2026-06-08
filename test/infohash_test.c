// SPDX-License-Identifier: ISC
#include <assert.h>
#include <string.h>

#include "mbt/infohash.h"

static void
test_new_and_string(void)
{
	struct mbt_infohash *ih = NULL;
	char buf[MBT_MBT_INFOHASH_SHA1HEXLEN + 1];
	int rv;

	rv = mbt_infohash_new(&ih, MBT_INFOHASH_SHA1, "611F70899D4E1D6A9C39CFC925F103DFEF630328");
	assert(rv == 0);
	assert(ih != NULL);

	rv = mbt_infohash_string(ih, buf, sizeof(buf));
	assert(rv == 0);
	assert(strcmp(buf, "611F70899D4E1D6A9C39CFC925F103DFEF630328") == 0);

	mbt_infohash_unref(ih);
}

static void
test_cmp_equal(void)
{
	struct mbt_infohash *a = NULL, *b = NULL;

	mbt_infohash_new(&a, MBT_INFOHASH_SHA1, "611F70899D4E1D6A9C39CFC925F103DFEF630328");
	mbt_infohash_new(&b, MBT_INFOHASH_SHA1, "611F70899D4E1D6A9C39CFC925F103DFEF630328");

	assert(mbt_infohash_cmp(a, b) == 0);

	mbt_infohash_unref(a);
	mbt_infohash_unref(b);
}

static void
test_cmp_different(void)
{
	struct mbt_infohash *a = NULL, *b = NULL;

	mbt_infohash_new(&a, MBT_INFOHASH_SHA1, "611F70899D4E1D6A9C39CFC925F103DFEF630328");
	mbt_infohash_new(&b, MBT_INFOHASH_SHA1, "C8295CE630F2064F08440DB1534E4992CFE4862A");

	assert(mbt_infohash_cmp(a, b) != 0);

	mbt_infohash_unref(a);
	mbt_infohash_unref(b);
}

static void
test_ref_unref(void)
{
	struct mbt_infohash *ih = NULL, *ref;

	mbt_infohash_new(&ih, MBT_INFOHASH_SHA1, "611F70899D4E1D6A9C39CFC925F103DFEF630328");

	ref = mbt_infohash_ref(ih);
	assert(ref == ih);

	/* ref=2, unref once → ref=1, still alive */
	mbt_infohash_unref(ih);

	/* ref=1, unref → ref=0, freed */
	mbt_infohash_unref(ref);
}

static void
test_unrefp(void)
{
	struct mbt_infohash *ih = NULL;

	mbt_infohash_new(&ih, MBT_INFOHASH_SHA1, "611F70899D4E1D6A9C39CFC925F103DFEF630328");
	mbt_mbt_infohash_unrefp(&ih);

	/* ih is now dangling (unrefp doesn't NULL it), test NULL path */
	ih = NULL;
	mbt_mbt_infohash_unrefp(&ih);
}

static void
test_string_buf_too_small(void)
{
	struct mbt_infohash *ih = NULL;
	char buf[10];
	int rv;

	mbt_infohash_new(&ih, MBT_INFOHASH_SHA1, "611F70899D4E1D6A9C39CFC925F103DFEF630328");

	rv = mbt_infohash_string(ih, buf, sizeof(buf));
	assert(rv < 0);

	mbt_infohash_unref(ih);
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
