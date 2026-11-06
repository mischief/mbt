// SPDX-License-Identifier: ISC
/* Replies fill the routing table only when they answer a query of ours. */
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
#include "test_util.h"

#define WAIT_SEC 5
#define NODE_LEN 26
#define BATCHES 8
#define PER_BATCH 8
#define TXN_MAX 32
#define K_NODES 8

struct txn {
	uint8_t buf[TXN_MAX];
	size_t len;
};

static struct mbt_ev *ev;
static struct sockaddr_in dht_addr;
static const uint8_t node_id[20] = {0xAA, 0xAA, 0xAA};
static const uint8_t stranger_id[20] = {0xCC, 0xCC, 0xCC};

static int
udp_socket(uint16_t *port)
{
	struct sockaddr_in me = {.sin_family = AF_INET};
	socklen_t len = sizeof(me);
	int fd;

	me.sin_addr.s_addr = htonl(INADDR_LOOPBACK);
	assert((fd = socket(AF_INET, SOCK_DGRAM, 0)) >= 0);
	assert(bind(fd, (struct sockaddr *)&me, sizeof(me)) == 0);
	assert(getsockname(fd, (struct sockaddr *)&me, &len) == 0);
	*port = ntohs(me.sin_port);
	return fd;
}

static void
send_msg(int fd, struct mbt_be_value *msg)
{
	unsigned char *enc;
	size_t len;

	assert(mbt_be_value_encode(msg, &enc, &len) == 0);
	mbt_be_unref(msg);
	assert(sendto(fd, enc, len, 0, (struct sockaddr *)&dht_addr, sizeof(dht_addr)) == (ssize_t)len);
	free(enc);
}

/* Wait for a message of kind y (and query name q, if given). */
static struct mbt_be_value *
wait_msg(int fd, const char *y, const char *q)
{
	time_t end = time(NULL) + WAIT_SEC;
	uint8_t buf[2048];

	for (;;) {
		ssize_t n = recv(fd, buf, sizeof(buf), MSG_DONTWAIT);
		struct mbt_be_value *m = NULL;

		assert(time(NULL) < end);
		if (n <= 0) {
			test_ev_step(ev, 5);
			continue;
		}
		assert(mbt_be_value_parse(buf, (size_t)n, &m) > 0);
		if (mbt_be_streq(mbt_be_find(m, "y"), y) &&
		    (!q || mbt_be_streq(mbt_be_find(m, "q"), q)))
			return m;
		mbt_be_unref(m);
	}
}

static void
get_txn(struct mbt_be_value *m, struct txn *t)
{
	struct mbt_be_value *v = mbt_be_find(m, "t");

	assert(v && v->string.len <= TXN_MAX);
	t->len = v->string.len;
	memcpy(t->buf, v->string.buf, t->len);
	mbt_be_unref(m);
}

/* The reply of a node with this id, with n nodes that start at base. */
static void
send_reply(int fd, const uint8_t *id, const struct txn *t, int base, int n)
{
	uint8_t nodes[PER_BATCH * NODE_LEN] = {0};
	struct mbt_be_value *msg, *r;

	for (int i = 0; i < n; i++) {
		uint8_t *p = nodes + i * NODE_LEN;

		p[0] = (uint8_t)(base + i);
		p[1] = (uint8_t)((base + i) ^ 0x55);
		p[20] = 10;
		p[22] = (uint8_t)(base >> 8);
		p[23] = (uint8_t)(base + i);
		p[24] = 0x1A;
		p[25] = 0xE1;
	}
	assert(mbt_be_dict_new(&msg) == 0);
	assert(mbt_be_dict_new(&r) == 0);
	assert(mbt_be_dict_append_string(r, "id", id, 20) == 0);
	if (n > 0)
		assert(mbt_be_dict_append_string(r, "nodes", nodes, (size_t)n * NODE_LEN) == 0);
	assert(mbt_be_dict_append_string(msg, "t", t->buf, t->len) == 0);
	assert(mbt_be_dict_append_string(msg, "y", "r", 1) == 0);
	assert(mbt_be_dict_append(msg, "r", r) == 0);
	mbt_be_unref(r);
	send_msg(fd, msg);
}

static void
send_query(int fd, const uint8_t *id, const char *q, const uint8_t *target)
{
	struct mbt_be_value *msg, *a;

	assert(mbt_be_dict_new(&msg) == 0);
	assert(mbt_be_dict_new(&a) == 0);
	assert(mbt_be_dict_append_string(a, "id", id, 20) == 0);
	if (target)
		assert(mbt_be_dict_append_string(a, "target", target, 20) == 0);
	assert(mbt_be_dict_append_string(msg, "t", "aa", 2) == 0);
	assert(mbt_be_dict_append_string(msg, "y", "q", 1) == 0);
	assert(mbt_be_dict_append_string(msg, "q", q, strlen(q)) == 0);
	assert(mbt_be_dict_append(msg, "a", a) == 0);
	mbt_be_unref(a);
	send_msg(fd, msg);
}

static void
settle(void)
{
	for (int i = 0; i < 10; i++)
		test_ev_step(ev, 5);
}

/* Ask our node, as fd, whether it tells others about id. */
static int
tells_about(int fd, const uint8_t *id)
{
	struct mbt_be_value *r, *m, *nodes;
	int found = 0;

	send_query(fd, stranger_id, "find_node", id);
	m = wait_msg(fd, "r", NULL);
	r = mbt_be_find(m, "r");
	nodes = mbt_be_find(r, "nodes");
	for (size_t i = 0; nodes && i < nodes->string.len / NODE_LEN; i++)
		found |= memcmp((uint8_t *)nodes->string.buf + i * NODE_LEN, id, 20) == 0;
	mbt_be_unref(m);
	return found;
}

struct seek {
	const uint8_t *id;
	uint16_t port;
};

static void
find_port(const uint8_t id[20], const char *addr, uint16_t port, void *arg)
{
	struct seek *s = arg;

	(void)addr;
	if (memcmp(id, s->id, 20) == 0)
		s->port = port;
}

static uint16_t
node_port(struct mbt_dht *d, const uint8_t *id)
{
	struct seek s = {.id = id};

	mbt_dht_foreach_node(d, find_port, &s);
	return s.port;
}

int
main(void)
{
	struct mbt_dht *d;
	struct txn t;
	uint16_t node_port_nr, other_port;
	int node, other, count;

	assert(mbt_ev_new_pollable(&ev) == 0);
	assert(mbt_dht_new(&d, &(struct mbt_dht_config){.ev = ev, .port = 0}) == 0);
	dht_addr.sin_family = AF_INET;
	dht_addr.sin_port = htons(mbt_dht_port(d));
	dht_addr.sin_addr.s_addr = htonl(INADDR_LOOPBACK);
	node = udp_socket(&node_port_nr);
	other = udp_socket(&other_port);

	/* Each answer to a query adds its nodes, enough to split buckets. */
	for (int b = 0; b < BATCHES; b++) {
		assert(mbt_dht_bootstrap(d, "127.0.0.1", node_port_nr) == 0);
		get_txn(wait_msg(node, "q", "find_node"), &t);
		send_reply(node, node_id, &t, b * PER_BATCH, PER_BATCH);
		settle();
	}
	count = mbt_dht_node_count(d);
	assert(count > K_NODES);

	/* The same reply again answers nothing. */
	send_reply(node, node_id, &t, 100, PER_BATCH);
	settle();
	assert(mbt_dht_node_count(d) == count);

	/* A reply to no query proves nothing, and teaches nothing. */
	t.buf[0] ^= 0xff;
	send_reply(other, stranger_id, &t, 120, PER_BATCH);
	settle();
	assert(mbt_dht_node_count(d) == count);
	assert(node_port(d, stranger_id) == 0);

	/* The right id from the wrong source is forged too. */
	assert(mbt_dht_bootstrap(d, "127.0.0.1", node_port_nr) == 0);
	get_txn(wait_msg(node, "q", "find_node"), &t);
	send_reply(other, stranger_id, &t, 140, PER_BATCH);
	settle();
	assert(node_port(d, stranger_id) == 0);
	send_reply(node, node_id, &t, 0, 0);
	settle();

	/* A forged query cannot move a proved node. */
	assert(node_port(d, node_id) == node_port_nr);
	send_query(other, node_id, "ping", NULL);
	settle();
	assert(node_port(d, node_id) == node_port_nr);
	mbt_be_unref(wait_msg(other, "r", NULL));

	/* A query adds a node, but others hear of it only once it answers. */
	send_query(other, stranger_id, "ping", NULL);
	mbt_be_unref(wait_msg(other, "r", NULL));
	assert(node_port(d, stranger_id) == other_port);
	get_txn(wait_msg(other, "q", "ping"), &t);
	assert(!tells_about(other, stranger_id));
	send_reply(other, stranger_id, &t, 0, 0);
	settle();
	assert(tells_about(other, stranger_id));

	mbt_dht_free(d);
	mbt_ev_destroy(ev);
	close(node);
	close(other);
	fprintf(stderr, "dht_routing_test: PASS (%d nodes)\n", count);
	return 0;
}
