// SPDX-License-Identifier: ISC
// Fuzz target: metainfo (.torrent) parsing
#include <stdint.h>
#include <stdlib.h>
#include <unistd.h>

#include <mbt/metainfo.h>

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

		struct mbt_metainfo *mi = NULL;
		mbt_metainfo_parse(&mi, buf, len);
		if (mi)
			mbt_metainfo_free(mi);
	}
	return 0;
}
