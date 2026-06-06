#include <stdint.h>
#include <stddef.h>
#include <assert.h>

#include <tbmap.h>

static void
basic(void)
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
	assert(!tbmap_isset(tb, 2));
	assert(!tbmap_isset(tb, 3));
	assert(!tbmap_isset(tb, 4));
	assert(!tbmap_isset(tb, 5));
	assert(!tbmap_isset(tb, 6));
}

int
main(int argc, char *argv[])
{
	(void)argc;
	(void)argv;

	basic();

	return 0;
}
