// SPDX-License-Identifier: ISC
// Test: BEP 51 sample_infohashes
// Stores infohashes via announce_peer, then queries sample_infohashes.
#include <assert.h>
#include <string.h>
#include <stdio.h>
#include <stdlib.h>
#include <unistd.h>
#include <sys/socket.h>
#include <netinet/in.h>
#include <arpa/inet.h>
#include <stdint.h>
#include <sys/types.h>
#include <sys/uio.h>

#include "mbt/dht.h"
#include "mbt/be.h"
#include "mbt/event.h"

struct mbt_ev_timer;

static struct mbt_ev *ev;
static int ticks;

static void tick_cb(struct mbt_ev_timer *timer, void *v)
{
	(void)timer; (void)v;
	if (++ticks >= 3) mbt_ev_stop(ev);
}

/* Send a bencoded message to the DHT port */
static void
send_msg(int sock, uint16_t port, struct mbt_be_value *msg)
{
	unsigned char *enc;
	size_t len;
	mbt_be_value_encode(msg, &enc, &len);
	struct sockaddr_in dst = {
		.sin_family = AF_INET,
		.sin_port = htons(port),
		.sin_addr.s_addr = htonl(INADDR_LOOPBACK),
	};
	sendto(sock, enc, len, 0, (struct sockaddr *)&dst, sizeof(dst));
	free(enc);
}

/* Send get_peers, run the event loop once to let DHT respond, return token (caller frees) */
static uint8_t *
get_token(int sock, uint16_t port, const uint8_t *ih, size_t *token_len_out)
{
	uint8_t fakeid[20];
	memset(fakeid, 0xBB, 20);
	uint8_t txn[2] = { 'g', '0' };

	struct mbt_be_value *msg = NULL, *a = NULL;
	mbt_be_dict_new(&msg);
	mbt_be_dict_append_string(msg, "t", txn, 2);
	mbt_be_dict_append_string(msg, "y", "q", 1);
	mbt_be_dict_append_string(msg, "q", "get_peers", 9);
	mbt_be_dict_new(&a);
	mbt_be_dict_append_string(a, "id", fakeid, 20);
	mbt_be_dict_append_string(a, "info_hash", ih, 20);
	mbt_be_dict_append(msg, "a", a);
	mbt_be_unref(a);
	send_msg(sock, port, msg);
	mbt_be_unref(msg);

	/* Run event loop so DHT processes the query and sends the response */
	struct mbt_ev_timer *t;
	mbt_ev_timer_new(ev, &t, 100000, tick_cb, NULL);
	mbt_ev_run(ev);
	mbt_ev_timer_free(t);
	ticks = 0;

	uint8_t buf[4096];
	ssize_t n = recvfrom(sock, buf, sizeof(buf), MSG_DONTWAIT, NULL, NULL);
	if (n <= 0) return NULL;
	struct mbt_be_value *resp = NULL;
	if (mbt_be_value_parse(buf, n, &resp) <= 0) return NULL;
	struct mbt_be_value *r = mbt_be_find(resp, "r");
	struct mbt_be_value *tok = r ? mbt_be_find(r, "token") : NULL;
	uint8_t *token = NULL;
	if (tok && tok->type == BE_STRING) {
		token = malloc(tok->string.iov_len);
		if (token) {
			memcpy(token, tok->string.iov_base, tok->string.iov_len);
			*token_len_out = tok->string.iov_len;
		}
	}
	mbt_be_unref(resp);
	return token;
}

/* Send announce_peer for a given infohash */
static void
send_announce(int sock, uint16_t port, const uint8_t *ih, const uint8_t *txn,
              const uint8_t *token, size_t token_len)
{
	uint8_t fakeid[20];
	memset(fakeid, 0xBB, 20);

	_cleanup_be_ struct mbt_be_value *msg = NULL, *a = NULL;
	if (mbt_be_dict_new(&msg) < 0) return;
	mbt_be_dict_append_string(msg, "t", txn, 2);
	mbt_be_dict_append_string(msg, "y", "q", 1);
	mbt_be_dict_append_string(msg, "q", "announce_peer", 13);
	if (mbt_be_dict_new(&a) < 0) return;
	mbt_be_dict_append_string(a, "id", fakeid, 20);
	mbt_be_dict_append_string(a, "info_hash", ih, 20);
	mbt_be_dict_append_number(a, "port", 6881);
	mbt_be_dict_append_string(a, "token", token, token_len);
	if (mbt_be_dict_append(msg, "a", a) < 0) return;
	send_msg(sock, port, msg);
}

/* Send sample_infohashes query */
static void
send_sample(int sock, uint16_t port, const uint8_t *txn)
{
	uint8_t fakeid[20], target[20];
	memset(fakeid, 0xCC, 20);
	memset(target, 0, 20);

	_cleanup_be_ struct mbt_be_value *msg = NULL, *a = NULL;
	if (mbt_be_dict_new(&msg) < 0) return;
	mbt_be_dict_append_string(msg, "t", txn, 2);
	mbt_be_dict_append_string(msg, "y", "q", 1);
	mbt_be_dict_append_string(msg, "q", "sample_infohashes", 17);
	if (mbt_be_dict_new(&a) < 0) return;
	mbt_be_dict_append_string(a, "id", fakeid, 20);
	mbt_be_dict_append_string(a, "target", target, 20);
	if (mbt_be_dict_append(msg, "a", a) < 0) return;
	send_msg(sock, port, msg);
}

int main(void)
{
	int rv = mbt_ev_new_pollable(&ev);
	assert(rv == 0);

	struct mbt_dht *d;
	rv = mbt_dht_new(&d, ev, 0);
	assert(rv == 0);
	uint16_t port = mbt_dht_port(d);

	int sock = socket(AF_INET, SOCK_DGRAM, 0);
	assert(sock >= 0);
	/* Bind so we get responses */
	struct sockaddr_in bind_addr = { .sin_family = AF_INET, .sin_addr.s_addr = htonl(INADDR_LOOPBACK) };
	bind(sock, (struct sockaddr *)&bind_addr, sizeof(bind_addr));

	/* Get a token via get_peers (token is IP-based, one suffices for all announces) */
	uint8_t dummy_ih[20] = {0};
	size_t token_len = 0;
	uint8_t *token = get_token(sock, port, dummy_ih, &token_len);
	assert(token && token_len > 0);

	/* Store 5 infohashes via announce_peer */
	uint8_t hashes[5][20];
	for (int i = 0; i < 5; i++) {
		memset(hashes[i], i + 1, 20);
		uint8_t txn[2] = { 'a', (uint8_t)i };
		send_announce(sock, port, hashes[i], txn, token, token_len);
	}
	free(token);

	/* Run event loop to process announces */
	struct mbt_ev_timer *t;
	mbt_ev_timer_new(ev, &t, 100000, tick_cb, NULL);
	mbt_ev_run(ev);
	mbt_ev_timer_free(t);
	ticks = 0;

	/* Drain announce responses */
	uint8_t drain[4096];
	while (recvfrom(sock, drain, sizeof(drain), MSG_DONTWAIT, NULL, NULL) > 0)
		;

	/* Send sample_infohashes query */
	uint8_t txn[2] = { 's', '1' };
	send_sample(sock, port, txn);

	/* Run event loop to process sample query */
	mbt_ev_timer_new(ev, &t, 100000, tick_cb, NULL);
	mbt_ev_run(ev);
	mbt_ev_timer_free(t);

	/* Read responses — skip announce_peer responses, find sample response */
	uint8_t buf[4096];
	int found = 0;
	for (int attempt = 0; attempt < 10; attempt++) {
		ssize_t n = recvfrom(sock, buf, sizeof(buf), MSG_DONTWAIT, NULL, NULL);
		if (n <= 0) break;
		_cleanup_be_ struct mbt_be_value *resp = NULL;
		int pv = mbt_be_value_parse(buf, n, &resp);
		if (pv <= 0 || !resp) continue;
		struct mbt_be_value *r = mbt_be_find(resp, "r");
		if (r) {
			struct mbt_be_value *samples = mbt_be_find(r, "samples");
			struct mbt_be_value *num = mbt_be_find(r, "num");
			if (samples && samples->type == BE_STRING) {
				int nsamp = samples->string.iov_len / 20;
				printf("bep51_test: got %d samples, num=%lld\n",
				       nsamp, num ? (long long)num->integer : -1LL);
				assert(nsamp > 0);
				assert(num && num->integer == 5);
				found = 1;
				break;
			}
		}
	}
	close(sock);
	assert(found && "did not receive sample_infohashes response");

	printf("bep51_test: PASS\n");
	mbt_dht_free(d);
	mbt_ev_destroy(ev);
	return 0;
}
