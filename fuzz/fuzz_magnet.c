// SPDX-License-Identifier: ISC
// Fuzz target: magnet URI parsing
#include <stdint.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>

#include <mbt/magnet.h>

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
		if (len < 8 || len > 4095) continue;

		/* Null-terminate for string parsing */
		char uri[4096];
		memcpy(uri, buf, len);
		uri[len] = '\0';

		struct mbt_magnet m = {0};
		int rv = mbt_magnet_parse(uri, &m);
		if (rv == 0)
			mbt_magnet_free(&m);
	}
	return 0;
}
