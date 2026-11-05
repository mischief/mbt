// SPDX-License-Identifier: ISC
// Fuzz target: info dict parsing in mbt_torrent_set_metadata, reachable over BEP 9
#include <stdint.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>
#include <sys/types.h>

#include <mbt/event.h>
#include <mbt/infohash.h>
#include <mbt/torrent.h>

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
	static const uint8_t peer_id[20];
	struct mbt_infohash *ih = NULL;
	struct mbt_ev *ev = NULL;

	if (mbt_ev_new_default(&ev) < 0)
		return 1;
	if (mbt_infohash_new(&ih, MBT_INFOHASH_SHA1,
	    "ABABABABABABABABABABABABABABABABABABABAB") < 0)
		return 1;

#ifdef __AFL_HAVE_MANUAL_CONTROL
	__AFL_INIT();
#endif
	unsigned char *buf = __AFL_FUZZ_TESTCASE_BUF;

	while (__AFL_LOOP(10000)) {
		int len = __AFL_FUZZ_TESTCASE_LEN;
		if (len < 2) continue;

		/* An exact allocation lets ASan see reads past the input. */
		unsigned char *in = malloc(len);
		if (!in)
			continue;
		memcpy(in, buf, len);

		struct mbt_torrent_config cfg = {.ev = ev, .ih = ih, .peer_id = peer_id};
		struct mbt_torrent *t = NULL;

		if (mbt_torrent_new(&t, &cfg) == 0) {
			mbt_torrent_set_metadata(t, in, len);
			mbt_torrent_free(t);
		}
		free(in);
	}
	mbt_infohash_unref(ih);
	mbt_ev_destroy(ev);
	return 0;
}
