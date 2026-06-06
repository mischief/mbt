#include <sys/param.h>
#include <stdio.h>
#include <string.h>
#include <errno.h>
#include <assert.h>
#include <stdlib.h>

#include <be.h>

static void
test_parse_string(void)
{
	struct be_value *be = NULL;
	int rv;

	rv = be_value_parse((const unsigned char *)"4:eggs", 6, &be);
	assert(rv == 6);
	assert(be->type == BE_STRING);
	assert(be->string.iov_len == 4);
	assert(memcmp(be->string.iov_base, "eggs", 4) == 0);
	be_value_free(be);
}

static void
test_parse_integer(void)
{
	struct be_value *be = NULL;
	int rv;

	rv = be_value_parse((const unsigned char *)"i42e", 4, &be);
	assert(rv == 4);
	assert(be->type == BE_INTEGER);
	assert(be->integer == 42);
	be_value_free(be);

	rv = be_value_parse((const unsigned char *)"i-1e", 4, &be);
	assert(rv == 4);
	assert(be->type == BE_INTEGER);
	assert(be->integer == -1);
	be_value_free(be);

	rv = be_value_parse((const unsigned char *)"i0e", 3, &be);
	assert(rv == 3);
	assert(be->integer == 0);
	be_value_free(be);
}

static void
test_parse_list(void)
{
	struct be_value *be = NULL;
	int rv;

	rv = be_value_parse((const unsigned char *)"l4:spam4:eggse", 14, &be);
	assert(rv == 14);
	assert(be->type == BE_LIST);

	struct be_value *first = STAILQ_FIRST(&be->listhead);
	assert(first);
	assert(first->type == BE_STRING);
	assert(memcmp(first->string.iov_base, "spam", 4) == 0);

	struct be_value *second = STAILQ_NEXT(first, next);
	assert(second);
	assert(second->type == BE_STRING);
	assert(memcmp(second->string.iov_base, "eggs", 4) == 0);

	be_value_free(be);
}

static void
test_parse_dict(void)
{
	struct be_value *be = NULL;
	int rv;

	rv = be_value_parse((const unsigned char *)"d3:cow3:moo4:spam4:eggse", 24, &be);
	assert(rv == 24);
	assert(be->type == BE_DICTIONARY);

	struct be_value *cow = be_find(be, "cow");
	assert(cow);
	assert(cow->type == BE_STRING);
	assert(memcmp(cow->string.iov_base, "moo", 3) == 0);

	struct be_value *spam = be_find(be, "spam");
	assert(spam);
	assert(spam->type == BE_STRING);
	assert(memcmp(spam->string.iov_base, "eggs", 4) == 0);

	assert(be_find(be, "missing") == NULL);

	be_value_free(be);
}

static void
test_parse_nested(void)
{
	struct be_value *be = NULL;
	int rv;

	/* dict containing a list */
	const char *buf = "d1:ali1ei2ei3eee";
	rv = be_value_parse((const unsigned char *)buf, strlen(buf), &be);
	assert(rv == (int)strlen(buf));
	assert(be->type == BE_DICTIONARY);

	struct be_value *a = be_find(be, "a");
	assert(a);
	assert(a->type == BE_LIST);

	be_value_free(be);
}

static void
test_parse_errors(void)
{
	struct be_value *be = NULL;

	/* truncated string: says length 10, only 2 bytes of data */
	assert(be_value_parse((const unsigned char *)"10:eg", 5, &be) < 0);

	/* truncated integer */
	assert(be_value_parse((const unsigned char *)"i42", 3, &be) < 0);

	/* empty input */
	assert(be_value_parse((const unsigned char *)"", 0, &be) < 0);
}

static void
test_encode_string(void)
{
	struct be_value *be = NULL;
	unsigned char *out = NULL;
	size_t len;
	int rv;

	rv = be_value_parse((const unsigned char *)"4:test", 6, &be);
	assert(rv == 6);

	rv = be_value_encode(be, &out, &len);
	assert(rv == 0);
	assert(len == 6);
	assert(memcmp(out, "4:test", 6) == 0);

	free(out);
	be_value_free(be);
}

static void
test_encode_integer(void)
{
	struct be_value *be = NULL;
	unsigned char *out = NULL;
	size_t len;
	int rv;

	rv = be_value_parse((const unsigned char *)"i999e", 5, &be);
	assert(rv == 5);

	rv = be_value_encode(be, &out, &len);
	assert(rv == 0);
	assert(len == 5);
	assert(memcmp(out, "i999e", 5) == 0);

	free(out);
	be_value_free(be);
}

static void
test_encode_dict(void)
{
	struct be_value *be = NULL;
	unsigned char *out = NULL;
	size_t len;
	int rv;

	rv = be_dict_new(&be);
	assert(rv == 0);

	rv = be_dict_append_number(be, "port", 6881);
	assert(rv == 0);

	rv = be_value_encode(be, &out, &len);
	assert(rv == 0);
	/* d4:porti6881ee */
	assert(memcmp(out, "d4:porti6881ee", len) == 0);

	free(out);
	be_value_free(be);
}

static void
test_dict_build(void)
{
	struct be_value *be = NULL, *inner = NULL;
	int rv;

	rv = be_dict_new(&be);
	assert(rv == 0);

	rv = be_dict_new(&inner);
	assert(rv == 0);

	rv = be_dict_append_number(inner, "ut_metadata", 3);
	assert(rv == 0);

	rv = be_dict_append(be, "m", inner);
	assert(rv == 0);

	struct be_value *m = be_find(be, "m");
	assert(m);
	assert(m->type == BE_DICTIONARY);

	struct be_value *utm = be_find(m, "ut_metadata");
	assert(utm);
	assert(utm->integer == 3);

	be_value_free(be);
}

static void
test_walk(void)
{
	struct be_value *be = NULL;
	int rv;

	rv = be_value_parse((const unsigned char *)"d1:ai1e1:bi2ee", 14, &be);
	assert(rv == 14);

	be_walk(be, be_pp, NULL);

	be_value_free(be);
}

int
main(void)
{
	test_parse_string();
	test_parse_integer();
	test_parse_list();
	test_parse_dict();
	test_parse_nested();
	test_parse_errors();
	test_encode_string();
	test_encode_integer();
	test_encode_dict();
	test_dict_build();
	test_walk();
	return 0;
}
