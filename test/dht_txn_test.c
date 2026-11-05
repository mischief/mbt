// SPDX-License-Identifier: ISC
/* A lookup takes a reply only with the transaction id of its query. */
#include <arpa/inet.h>
#include <assert.h>
#include <netinet/in.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/socket.h>
#include <time.h>
#include <unistd.h>

#include "mbt/be.h"
#include "mbt/dht.h"
#include "mbt/event.h"
#include "mbt/infohash.h"
#include "test_util.h"

#define WAIT_SEC 5
#define HASH "ABABABABABABABABABABABABABABABABABABABAB"
#define FORGED_PORT 1111
#define REAL_PORT 2222

static struct mbt_ev *ev;
static int peers, forged_peers;

static void
on_peer(const char *addr, uint16_t port, void *arg)
{
	(void)addr; (void)arg;
	peers++;
	forged_peers += port == FORGED_PORT;
}

static void
on_done(void *arg)
{
	(void)arg;
}

/* Answer as a node: values holds one peer on port, and t is the id given. */
static void
send_reply(int fd, const struct sockaddr_in *to, const void *t, size_t tlen, uint16_t port)
{
	static const uint8_t id[20] = {1, 2, 3};
	uint8_t peer[6] = {127, 0, 0, 1, (uint8_t)(port >> 8), (uint8_t)port};
	struct mbt_be_value *msg, *r, *vals, *v;
	unsigned char *enc;
	size_t len;

	assert(mbt_be_dict_new(&msg) == 0);
	assert(mbt_be_dict_new(&r) == 0);
	assert(mbt_be_list_new(&vals) == 0);
	assert(mbt_be_string_new(&v, peer, sizeof(peer)) == 0);
	mbt_be_list_append(vals, v);
	mbt_be_unref(v);
	assert(mbt_be_dict_append_string(r, "id", id, sizeof(id)) == 0);
	assert(mbt_be_dict_append_string(r, "token", "tok", 3) == 0);
	assert(mbt_be_dict_append(r, "values", vals) == 0);
	mbt_be_unref(vals);
	assert(mbt_be_dict_append_string(msg, "t", t, tlen) == 0);
	assert(mbt_be_dict_append_string(msg, "y", "r", 1) == 0);
	assert(mbt_be_dict_append(msg, "r", r) == 0);
	mbt_be_unref(r);
	assert(mbt_be_value_encode(msg, &enc, &len) == 0);
	mbt_be_unref(msg);
	assert(sendto(fd, enc, len, 0, (const struct sockaddr *)to, sizeof(*to)) == (ssize_t)len);
	free(enc);
}

int
main(void)
{
	struct sockaddr_in me = {.sin_family = AF_INET}, from;
	socklen_t len = sizeof(me);
	struct mbt_infohash *ih;
	struct mbt_dht *d;
	uint8_t buf[512], txn[32];
	size_t txn_len = 0;
	time_t end;
	int fd;

	assert(mbt_ev_new_default(&ev) == 0);
	me.sin_addr.s_addr = htonl(INADDR_LOOPBACK);
	assert((fd = socket(AF_INET, SOCK_DGRAM, 0)) >= 0);
	assert(bind(fd, (struct sockaddr *)&me, sizeof(me)) == 0);
	assert(getsockname(fd, (struct sockaddr *)&me, &len) == 0);

	assert(mbt_dht_new(&d, &(struct mbt_dht_config){.ev = ev, .port = 0}) == 0);
	assert(mbt_infohash_new(&ih, MBT_INFOHASH_SHA1, HASH) == 0);
	assert(mbt_dht_bootstrap(d, "127.0.0.1", ntohs(me.sin_port)) == 0);
	assert(mbt_dht_get_peers(d, ih, on_peer, on_done, NULL) == 0);

	end = time(NULL) + WAIT_SEC;
	while (txn_len == 0) {
		socklen_t flen = sizeof(from);
		ssize_t n = recvfrom(fd, buf, sizeof(buf), MSG_DONTWAIT, (struct sockaddr *)&from, &flen);

		assert(time(NULL) < end);
		if (n > 0) {
			struct mbt_be_value *m = NULL;

			assert(mbt_be_value_parse(buf, (size_t)n, &m) > 0);
			if (mbt_be_streq(mbt_be_find(m, "q"), "get_peers")) {
				struct mbt_be_value *t = mbt_be_find(m, "t");

				assert(t->string.len <= sizeof(txn));
				txn_len = t->string.len;
				memcpy(txn, t->string.buf, txn_len);
			}
			mbt_be_unref(m);
		}
		test_ev_step(ev, 1);
	}

	/* Right source, wrong id: not a reply to the query. */
	uint8_t wrong[32];

	memcpy(wrong, txn, txn_len);
	wrong[0] ^= 0xff;
	send_reply(fd, &from, wrong, txn_len, FORGED_PORT);
	for (int i = 0; i < 20; i++)
		test_ev_step(ev, 5);
	assert(peers == 0);

	send_reply(fd, &from, txn, txn_len, REAL_PORT);
	end = time(NULL) + WAIT_SEC;
	while (peers == 0) {
		assert(time(NULL) < end);
		test_ev_step(ev, 5);
	}
	assert(forged_peers == 0);

	mbt_infohash_unref(ih);
	mbt_dht_free(d);
	mbt_ev_destroy(ev);
	close(fd);
	fprintf(stderr, "dht_txn_test: PASS\n");
	return 0;
}
