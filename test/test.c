#include <sys/param.h>
#include <stdio.h>
#include <string.h>
#include <errno.h>
#include <assert.h>
#include <err.h>

#include <be.h>

static void
be_pp_errno(struct be_value *be, void *arg)
{
	int *err = arg;

	*err = be->integer;
}

static void
assert_errno(int v, int err)
{
	assert(v == err);
}

int
main(int argc, char *argv[])
{
	int rv;
	struct be_value *be;

	(void)argc;
	(void)argv;

	const char *buf = "4:eggs";
	size_t len = strlen(buf);
	/*

	rv = be_value_parse(buf, len, &be);
	assert(rv > 0);
	assert(be->type == BE_STRING);
	assert(strcmp(be->string, "eggs") == 0);

	be_value_free(be);

	buf = "i42e";
	len = strlen(buf);

	rv = be_value_parse(buf, len, &be);
	assert(rv > 0);
	assert(be->type == BE_INTEGER);
	assert(be->integer == 42);

	be_value_free(be);

	buf = "l4:spam4:eggse";
	len = strlen(buf);

	rv = be_value_parse(buf, len, &be);
	assert(rv > 0);
	assert(be->type == BE_LIST);
	//assert(be->integer == 42);

	be_value_free(be);
	*/

	const char transmission[] = "d1:ei1e1:md11:ut_metadatai3e6:ut_pexi1ee13:metadata_sizei53699e1:pi52434e4:reqqi512e11:upload_onlyi1e1:v18:Transmission 4.0.66:yourip4:\300\250\000\fe";
	len = sizeof(transmission);

	rv = be_value_parse((const unsigned char *)transmission, len, &be);
	if(rv < 0)
		warnx("be_value_parse: %s", strerror(-rv));
	assert(rv > 0);
	assert(be->type == BE_DICTIONARY);

	be_walk(be, be_pp, NULL);

	be_value_free(be);

	buf = "d3:cow3:moo4:spam4:eggs1:tld3:fooi42eei69eee";
	len = strlen(buf);

	rv = be_value_parse((const unsigned char *)buf, len, &be);
	assert(rv > 0);
	assert(be->type == BE_DICTIONARY);

	struct be_value *cow;

	cow = be_find(be, "cow");
	assert(cow);

	assert(cow->type == BE_STRING);
	assert(strcmp(cow->string.iov_base, "moo") == 0);

	be_walk(be, be_pp, NULL);

	be_value_free(be);

	return 0;
}
