// SPDX-License-Identifier: ISC
#include <unistd.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <errno.h>
#include <assert.h>

#include "mbt/be.h"

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

int
main(int argc, char *argv[])
{
	struct mbt_be_value *be;
	int rv;

	(void) argc;
	(void) argv;

#ifdef __AFL_HAVE_MANUAL_CONTROL
	__AFL_INIT();
#endif

	// must be after __AFL_INIT
	// and before __AFL_LOOP!
	unsigned char *buf = __AFL_FUZZ_TESTCASE_BUF;

	while (__AFL_LOOP(10000)) {
		// don't use the macro directly in a call!
		int len = __AFL_FUZZ_TESTCASE_LEN;

		// check for a required/useful minimum input length
		if (len < 2) continue;

		/* Setup function call, e.g. struct target *tmp = libtarget_init() */
		/* Call function to be fuzzed, e.g.: */
		be = NULL;
		rv = mbt_be_value_parse(buf, len, &be);
		(void)rv;
		/* Reset state. e.g. libtarget_free(tmp) */
		if(be)
			mbt_be_value_free(be);
	}

	return 0;
}
