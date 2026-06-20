// SPDX-License-Identifier: ISC
// Fuzz target: DHT message parsing (bencode dict with DHT semantics)
#include <stdint.h>
#include <stdlib.h>
#include <unistd.h>

#include <mbt/be.h>

#ifndef __AFL_FUZZ_TESTCASE_LEN
  ssize_t fuzz_len;
  #define __AFL_FUZZ_TESTCASE_LEN fuzz_len
  unsigned char fuzz_buf[1024000];
  #define __AFL_FUZZ_TESTCASE_BUF fuzz_buf
  #define __AFL_FUZZ_INIT() void sync(void);
  #define __AFL_LOOP(x) ((fuzz_len = read(0, fuzz_buf, sizeof(fuzz_buf))) > 0 ? 1 : 0)
  #define __AFL_INIT() sync()
#endif

__AFL_FUZZ_INIT()

int main(void)
{
#ifdef __AFL_HAVE_MANUAL_CONTROL
	__AFL_INIT();
#endif
	unsigned char *buf = __AFL_FUZZ_TESTCASE_BUF;

	while (__AFL_LOOP(10000)) {
		int len = __AFL_FUZZ_TESTCASE_LEN;
		if (len < 2) continue;

		struct mbt_be_value *msg = NULL;
		int rv = mbt_be_value_parse(buf, len, &msg);
		if (rv <= 0) continue;

		/* Exercise DHT-style field lookups */
		struct mbt_be_value *y = mbt_be_find(msg, "y");
		struct mbt_be_value *t = mbt_be_find(msg, "t");
		struct mbt_be_value *r = mbt_be_find(msg, "r");
		struct mbt_be_value *a = mbt_be_find(msg, "a");
		struct mbt_be_value *q = mbt_be_find(msg, "q");
		(void)y; (void)t; (void)r; (void)a; (void)q;

		if (r && r->type == BE_DICTIONARY) {
			struct mbt_be_value *nodes = mbt_be_find(r, "nodes");
			struct mbt_be_value *values = mbt_be_find(r, "values");
			struct mbt_be_value *token = mbt_be_find(r, "token");
			(void)nodes; (void)values; (void)token;
		}

		mbt_be_unref(msg);
	}
	return 0;
}
