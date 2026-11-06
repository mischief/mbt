// SPDX-License-Identifier: ISC
/*
 * Fuzz target: the DHT message handler, on a live node. The input is a list of
 * datagrams: 1 source byte, 2 length bytes (big endian), then the message.
 * AFL builds use fixed transaction ids and accept any token, so a reply can
 * match a query and an announce or put can reach the stores.
 */
#include <arpa/inet.h>
#include <netinet/in.h>
#include <stdlib.h>
#include <string.h>
#include <sys/socket.h>
#include <sys/types.h>
#include <unistd.h>

#include <mbt/dht.h>
#include <mbt/event.h>
#include <mbt/infohash.h>
#include "internal.h"
#include "udp_mux.h"

#ifndef __AFL_FUZZ_TESTCASE_LEN
  ssize_t fuzz_len;
  #define __AFL_FUZZ_TESTCASE_LEN fuzz_len
  unsigned char fuzz_buf[1024000];
  #define __AFL_FUZZ_TESTCASE_BUF fuzz_buf
  #define __AFL_FUZZ_INIT() void sync(void);
  #define __AFL_LOOP(x) ((fuzz_len = read(0, fuzz_buf, sizeof(fuzz_buf))) > 0 ? 1 : 0)
  #define __AFL_INIT() sync()
#endif

#define MSG_MAX 1500
#define SEED_PORT 9
#define NSOURCES 4
#define HASH "ABABABABABABABABABABABABABABABABABABABAB"

__AFL_FUZZ_INIT()

static struct sockaddr_storage sources[NSOURCES];

static void
set_source(struct sockaddr_storage *ss, const char *addr, uint16_t port)
{
	struct sockaddr_in *s4 = (struct sockaddr_in *)ss;
	struct sockaddr_in6 *s6 = (struct sockaddr_in6 *)ss;

	if (inet_pton(AF_INET, addr, &s4->sin_addr) > 0) {
		s4->sin_family = AF_INET;
		s4->sin_port = htons(port);
	} else {
		inet_pton(AF_INET6, addr, &s6->sin6_addr);
		s6->sin6_family = AF_INET6;
		s6->sin6_port = htons(port);
	}
}

static int
accept_sig(const uint8_t k[MBT_DHT_PUBKEY_LEN], const uint8_t sig[MBT_DHT_SIG_LEN],
           const void *buf, size_t len, void *arg)
{
	(void)k; (void)sig; (void)buf; (void)len; (void)arg;
	return 1;
}

static void
on_peer(const char *addr, uint16_t port, void *arg)
{
	(void)addr; (void)port; (void)arg;
}

static void
on_item(const uint8_t *v, size_t vlen, int64_t seq, const uint8_t *k, void *arg)
{
	(void)v; (void)vlen; (void)seq; (void)k; (void)arg;
}

static void
on_node(const uint8_t id[20], const char *addr, uint16_t port, void *arg)
{
	(void)id; (void)addr; (void)port; (void)arg;
}

static void
run_input(struct mbt_ev *ev, struct mbt_udp_mux *mux, struct mbt_infohash *ih,
          const unsigned char *data, size_t len)
{
	static const uint8_t node_id[20] = {0x80, 1, 2, 3};
	struct mbt_dht_config cfg = {.ev = ev, .node_id = node_id, .max_stored = 4, .max_items = 4,
	    .verify = accept_sig};
	const uint8_t target[20] = {0};
	struct mbt_dht *d;

	if (mbt_dht_new_mux(&d, &cfg, mux) < 0)
		return;
	mbt_dht_bootstrap(d, "127.0.0.1", SEED_PORT);
	mbt_dht_get_peers(d, ih, on_peer, NULL, NULL);

	while (len >= 3) {
		size_t n = (size_t)data[1] << 8 | data[2];
		const struct sockaddr_storage *from = &sources[data[0] % NSOURCES];

		data += 3;
		len -= 3;
		if (n > len || n > MSG_MAX)
			break;
		/* An exact allocation lets ASan see reads past the message. */
		unsigned char *msg = malloc(n ? n : 1);

		if (!msg)
			break;
		memcpy(msg, data, n);
		mbt_dht_input(d, msg, n, from);
		free(msg);
		data += n;
		len -= n;
	}

	mbt_dht_announce(d, ih, 6881);
	mbt_dht_get_item(d, target, on_item, NULL);
	mbt_dht_foreach_node(d, on_node, NULL);
	mbt_dht_free(d);
}

int
main(void)
{
	struct mbt_udp_mux *mux;
	struct mbt_infohash *ih;
	struct mbt_ev *ev;

	set_source(&sources[0], "127.0.0.1", SEED_PORT);
	set_source(&sources[1], "127.0.0.2", 4000);
	set_source(&sources[2], "::1", SEED_PORT);
	set_source(&sources[3], "127.0.0.1", SEED_PORT + 1);

#ifdef __AFL_HAVE_MANUAL_CONTROL
	__AFL_INIT();
#endif
	/* After the fork: an sd-event loop works only in the process that made it. */
	if (mbt_ev_new_default(&ev) < 0 || mbt_udp_mux_new(&mux, ev, 0) < 0 ||
	    mbt_infohash_new(&ih, MBT_INFOHASH_SHA1, HASH) < 0)
		return 1;
	unsigned char *buf = __AFL_FUZZ_TESTCASE_BUF;

	while (__AFL_LOOP(100000))
		run_input(ev, mux, ih, buf, (size_t)__AFL_FUZZ_TESTCASE_LEN);
	mbt_infohash_unref(ih);
	mbt_udp_mux_free(mux);
	mbt_ev_destroy(ev);
	return 0;
}
