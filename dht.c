// SPDX-License-Identifier: ISC
/*
 * dht.c — BitTorrent DHT (BEP 5) implementation
 *
 * Kademlia-based distributed hash table over UDP.
 * Standalone: only depends on event loop, bencode, infohash, logging.
 */
#define _GNU_SOURCE
#include <sys/socket.h>
#include <netinet/in.h>
#include <arpa/inet.h>
#include <stdlib.h>
#include <stdio.h>
#include <string.h>
#include <unistd.h>
#include <errno.h>
#include <time.h>
#include <fcntl.h>

#include "mbt/sha1_compat.h"

#define DHT_PEER_ID_LEN 20
#define DHT_PEER_ID_STRLEN (DHT_PEER_ID_LEN * 2)

#include "mbt/dht.h"
#include "mbt/event.h"
#include "mbt/infohash.h"
#include "mbt/be.h"
#include "mbt/log.h"
#include "mbt/hex.h"
#include "mbt/ratelimit.h"
#include "mbt/resolve.h"
#include "net.h"

/* --- Constants --- */
#define K 8              /* nodes per bucket */
#define ALPHA 3          /* lookup parallelism */
#define MAX_LOOKUPS 8    /* concurrent get_peers lookups */
#define TOKEN_LEN 8
#define TXN_LEN 4
#define NODE_COMPACT_LEN 26   /* 20 id + 4 ip + 2 port */
#define NODE6_COMPACT_LEN 38  /* 20 id + 16 ip + 2 port */
#define MAX_LOOKUP_NODES 64  /* nodes tracked per lookup */
#define LOOKUP_TIMEOUT_US (10 * 1000000ULL) /* 10s per lookup */
#define RPC_TIMEOUT_US   (5 * 1000000ULL)  /* 5s per RPC */
#define REFRESH_US       (15 * 60 * 1000000ULL) /* 15 min bucket refresh */
#define NUM_BUCKETS 160

/* Helper: format sockaddr_storage for logging (returns static buffer) */
static const char *
sa_str(const struct sockaddr_storage *sa, char *buf, size_t len)
{
	return mbt_sockaddr_str(sa, buf, len);
}

static int
sa_equal(const struct sockaddr_storage *a, const struct sockaddr_storage *b)
{
	if (a->ss_family != b->ss_family) return 0;
	if (a->ss_family == AF_INET6) {
		struct sockaddr_in6 *a6 = (struct sockaddr_in6 *)a;
		struct sockaddr_in6 *b6 = (struct sockaddr_in6 *)b;
		return a6->sin6_port == b6->sin6_port &&
		       memcmp(&a6->sin6_addr, &b6->sin6_addr, 16) == 0;
	}
	struct sockaddr_in *a4 = (struct sockaddr_in *)a;
	struct sockaddr_in *b4 = (struct sockaddr_in *)b;
	return a4->sin_port == b4->sin_port &&
	       a4->sin_addr.s_addr == b4->sin_addr.s_addr;
}

/* --- Data structures --- */

struct dht_node {
	uint8_t id[20];
	struct sockaddr_storage addr;
	time_t last_seen;
	int pinged;  /* outstanding ping count */
};

struct dht_bucket {
	struct dht_node nodes[K];
	int count;
	time_t last_changed;
};

/* A node encountered during lookup */
struct lookup_node {
	uint8_t id[20];
	struct sockaddr_storage addr;
	uint8_t token[TOKEN_LEN]; /* token from get_peers response */
	int token_len;
	int queried;
	int responded;
};

struct dht_lookup {
	int active;
	uint8_t target[20];  /* infohash we're looking for */
	struct lookup_node nodes[MAX_LOOKUP_NODES];
	int node_count;
	int pending;  /* outstanding RPCs */
	mbt_dht_peers_cb peers_cb;
	mbt_dht_done_cb done_cb;
	void *arg;
	struct mbt_ev_timer *timer;
};

struct mbt_dht {
	struct mbt_ev *ev;
	struct mbt_listener listener;
	uint8_t id[20];  /* our node ID */
	uint16_t port;

	/* Routing table: 160 buckets */
	struct dht_bucket buckets[NUM_BUCKETS];

	/* Peer storage: announced peers for infohashes we've seen */
#define DHT_MAX_STORED 64
	struct {
		uint8_t infohash[20];
		struct sockaddr_storage peers[8];
		int peer_count;
	} stored[DHT_MAX_STORED];
	int stored_count;

	/* Active lookups */
	struct dht_lookup lookups[MAX_LOOKUPS];

	/* Transaction counter */
	uint32_t txn_counter;

	/* Refresh timer */
	struct mbt_ev_timer *refresh_timer;

	/* Query rate limit (messages/sec as bytes, 1 msg = 1 token) */
	struct mbt_ratelimit *send_limit;
};

/* --- Helpers --- */

static void
random_bytes(uint8_t *buf, size_t len)
{
	for (size_t i = 0; i < len; i++)
		buf[i] = rand() & 0xff;
}

static void
dht_init_node_id(uint8_t *id)
{
	FILE *fp = fopen("/etc/machine-id", "r");
	if (fp) {
		char buf[64] = {0};
		if (fread(buf, 1, 32, fp) == 32) {
			/* SHA1(machine-id) → stable 20-byte node ID */
			SHA1_CTX ctx;
			SHA1Init(&ctx);
			SHA1Update(&ctx, (uint8_t *)buf, 32);
			SHA1Final(id, &ctx);
			fclose(fp);
			return;
		}
		fclose(fp);
	}
	/* Fallback: random */
	random_bytes(id, 20);
}

/* XOR distance — returns bucket index (0 = closest to us) */
static int
bucket_index(const uint8_t *a, const uint8_t *b)
{
	for (int i = 0; i < 20; i++) {
		uint8_t x = a[i] ^ b[i];
		if (x == 0) continue;
		/* Find highest bit set */
		int bit = 7;
		while (bit > 0 && !(x & (1 << bit))) bit--;
		return 159 - (i * 8 + (7 - bit));
	}
	return 0;
}

/* Compare XOR distance to target */
static int
closer_to(const uint8_t *target, const uint8_t *a, const uint8_t *b)
{
	for (int i = 0; i < 20; i++) {
		uint8_t da = a[i] ^ target[i];
		uint8_t db = b[i] ^ target[i];
		if (da < db) return -1;
		if (da > db) return 1;
	}
	return 0;
}

static void
make_txn(struct mbt_dht *d, uint8_t txn[TXN_LEN])
{
	uint32_t t = ++d->txn_counter;
	memcpy(txn, &t, TXN_LEN);
}

/* --- Routing table --- */

static void
routing_add(struct mbt_dht *d, const uint8_t *id, const struct sockaddr_storage *addr)
{
	if (memcmp(id, d->id, DHT_PEER_ID_LEN) == 0) return; /* don't add ourselves */

	int idx = bucket_index(d->id, id);
	struct dht_bucket *b = &d->buckets[idx];

	/* Already in bucket? Update last_seen */
	for (int i = 0; i < b->count; i++) {
		if (memcmp(b->nodes[i].id, id, DHT_PEER_ID_LEN) == 0) {
			b->nodes[i].last_seen = time(NULL);
			b->nodes[i].addr = *addr;
			return;
		}
	}

	/* Room in bucket? Add */
	if (b->count < K) {
		struct dht_node *n = &b->nodes[b->count++];
		memcpy(n->id, id, 20);
		n->addr = *addr;
		n->last_seen = time(NULL);
		n->pinged = 0;
		b->last_changed = time(NULL);
		mbt_log_debug(MBT_LOG_DHT, "routing add bucket=%d count=%d", idx, b->count);
	}
	/* Bucket full — could ping oldest, but skip for now */
}

static int
routing_closest(struct mbt_dht *d, const uint8_t *target, struct dht_node *out, int max)
{
	int count = 0;
	int idx = bucket_index(d->id, target);

	/* Search outward from target bucket */
	for (int dist = 0; dist < 160 && count < max; dist++) {
		int b1 = idx + dist, b2 = idx - dist;
		for (int pass = 0; pass < 2 && count < max; pass++) {
			int bi = (pass == 0) ? b1 : b2;
			if (bi < 0 || bi >= NUM_BUCKETS) continue;
			if (pass == 1 && dist == 0) continue; /* don't double-count */
			struct dht_bucket *b = &d->buckets[bi];
			for (int i = 0; i < b->count && count < max; i++)
				out[count++] = b->nodes[i];
		}
	}
	return count;
}

/* --- Send helpers --- */

static int
dht_send(struct mbt_dht *d, const struct sockaddr_storage *addr,
         const void *data, size_t len)
{
	socklen_t salen = (addr->ss_family == AF_INET6) ?
		sizeof(struct sockaddr_in6) : sizeof(struct sockaddr_in);
	int fd = (addr->ss_family == AF_INET6) ? d->listener.fd6 : d->listener.fd4;
	if (fd < 0) return -EAFNOSUPPORT;
	ssize_t rv = sendto(fd, data, len, 0,
	                    (const struct sockaddr *)addr, salen);
	return rv < 0 ? -errno : 0;
}

static int
dht_send_find_node(struct mbt_dht *d, const struct sockaddr_storage *addr, const uint8_t *target)
{
	if (!mbt_ratelimit_allow(d->send_limit, 1))
		return 0;
	struct mbt_be_value *msg = NULL, *a = NULL;
	uint8_t txn[TXN_LEN];
	make_txn(d, txn);

	mbt_be_dict_new(&msg);
	mbt_be_dict_append_string(msg, "t", txn, TXN_LEN);
	mbt_be_dict_append_string(msg, "y", "q", 1);
	mbt_be_dict_append_string(msg, "q", "find_node", 9);
	mbt_be_dict_new(&a);
	mbt_be_dict_append_string(a, "id", d->id, 20);
	mbt_be_dict_append_string(a, "target", target, 20);
	mbt_be_dict_append(msg, "a", a);

	unsigned char *enc; size_t enc_len;
	mbt_be_value_encode(msg, &enc, &enc_len);
	int rv = dht_send(d, addr, enc, enc_len);
	free(enc);
	mbt_be_value_free(msg);
	return rv;
}

static int
dht_send_get_peers(struct mbt_dht *d, const struct sockaddr_storage *addr, const uint8_t *infohash)
{
	struct mbt_be_value *msg = NULL, *a = NULL;
	uint8_t txn[TXN_LEN];
	make_txn(d, txn);

	mbt_be_dict_new(&msg);
	mbt_be_dict_append_string(msg, "t", txn, TXN_LEN);
	mbt_be_dict_append_string(msg, "y", "q", 1);
	mbt_be_dict_append_string(msg, "q", "get_peers", 9);
	mbt_be_dict_new(&a);
	mbt_be_dict_append_string(a, "id", d->id, 20);
	mbt_be_dict_append_string(a, "info_hash", infohash, 20);
	mbt_be_dict_append(msg, "a", a);

	unsigned char *enc; size_t enc_len;
	mbt_be_value_encode(msg, &enc, &enc_len);
	int rv = dht_send(d, addr, enc, enc_len);
	free(enc);
	mbt_be_value_free(msg);
	return rv;
}

static int
dht_send_announce(struct mbt_dht *d, const struct sockaddr_storage *addr,
                  const uint8_t *infohash, uint16_t port,
                  const uint8_t *token, int token_len)
{
	struct mbt_be_value *msg = NULL, *a = NULL;
	uint8_t txn[TXN_LEN];
	make_txn(d, txn);

	mbt_be_dict_new(&msg);
	mbt_be_dict_append_string(msg, "t", txn, TXN_LEN);
	mbt_be_dict_append_string(msg, "y", "q", 1);
	mbt_be_dict_append_string(msg, "q", "announce_peer", 13);
	mbt_be_dict_new(&a);
	mbt_be_dict_append_string(a, "id", d->id, 20);
	mbt_be_dict_append_string(a, "info_hash", infohash, 20);
	mbt_be_dict_append_number(a, "port", port);
	mbt_be_dict_append_string(a, "token", token, token_len);
	mbt_be_dict_append_number(a, "implied_port", 0);
	mbt_be_dict_append(msg, "a", a);

	unsigned char *enc; size_t enc_len;
	mbt_be_value_encode(msg, &enc, &enc_len);
	int rv = dht_send(d, addr, enc, enc_len);
	free(enc);
	mbt_be_value_free(msg);
	return rv;
}

static int
dht_send_ping(struct mbt_dht *d, const struct sockaddr_storage *addr)
{
	struct mbt_be_value *msg = NULL, *a = NULL;
	uint8_t txn[TXN_LEN];
	make_txn(d, txn);

	mbt_be_dict_new(&msg);
	mbt_be_dict_append_string(msg, "t", txn, TXN_LEN);
	mbt_be_dict_append_string(msg, "y", "q", 1);
	mbt_be_dict_append_string(msg, "q", "ping", 4);
	mbt_be_dict_new(&a);
	mbt_be_dict_append_string(a, "id", d->id, 20);
	mbt_be_dict_append(msg, "a", a);

	unsigned char *enc; size_t enc_len;
	mbt_be_value_encode(msg, &enc, &enc_len);
	int rv = dht_send(d, addr, enc, enc_len);
	free(enc);
	mbt_be_value_free(msg);
	return rv;
}

/* --- Lookup management --- */

static struct dht_lookup *
lookup_find_for_target(struct mbt_dht *d, const uint8_t *target)
{
	for (int i = 0; i < MAX_LOOKUPS; i++)
		if (d->lookups[i].active && memcmp(d->lookups[i].target, target, 20) == 0)
			return &d->lookups[i];
	return NULL;
}

static void
lookup_add_node(struct dht_lookup *lk, const uint8_t *id, const struct sockaddr_storage *addr)
{
	/* Dedup */
	for (int i = 0; i < lk->node_count; i++)
		if (memcmp(lk->nodes[i].id, id, 20) == 0) return;

	if (lk->node_count >= MAX_LOOKUP_NODES) return;
	struct lookup_node *ln = &lk->nodes[lk->node_count++];
	memcpy(ln->id, id, 20);
	ln->addr = *addr;
	ln->queried = 0;
	ln->responded = 0;
	ln->token_len = 0;
}

static void
lookup_advance(struct mbt_dht *d, struct dht_lookup *lk)
{
	/* Send get_peers to the ALPHA closest un-queried nodes */
	int sent = 0;
	for (int i = 0; i < lk->node_count && sent < ALPHA; i++) {
		if (!mbt_ratelimit_allow(d->send_limit, 1))
			break;
		/* Find closest un-queried */
		int best = -1;
		for (int j = 0; j < lk->node_count; j++) {
			if (lk->nodes[j].queried) continue;
			if (best < 0 || closer_to(lk->target, lk->nodes[j].id, lk->nodes[best].id) < 0)
				best = j;
		}
		if (best < 0) break;

		lk->nodes[best].queried = 1;
		lk->pending++;
		dht_send_get_peers(d, &lk->nodes[best].addr, lk->target);
		{ char _b[64]; mbt_log_debug(MBT_LOG_DHT, "get_peers sent to %s",
		              sa_str(&lk->nodes[best].addr, _b, sizeof(_b))); }
		sent++;
	}

	/* If nothing left to query and no pending, lookup is done */
	if (sent == 0 && lk->pending == 0) {
		mbt_log_debug(MBT_LOG_DHT, "lookup complete, %d nodes queried", lk->node_count);
		if (lk->done_cb)
			lk->done_cb(lk->arg);
		if (lk->timer) {
			mbt_ev_timer_free(lk->timer);
			lk->timer = NULL;
		}
		lk->active = 0;
	}
}

static void
lookup_timeout_cb(struct mbt_ev_timer *timer, void *v)
{
	struct dht_lookup *lk = v;
	(void)timer;
	mbt_log_debug(MBT_LOG_DHT, "lookup timed out");
	if (lk->done_cb)
		lk->done_cb(lk->arg);
	mbt_ev_timer_free(lk->timer);
	lk->timer = NULL;
	lk->active = 0;
}

/* --- Receive handler --- */

static void
parse_compact_nodes(struct mbt_dht *d, const uint8_t *data, size_t len, struct dht_lookup *lk)
{
	int count = len / NODE_COMPACT_LEN;
	for (int i = 0; i < count; i++) {
		const uint8_t *p = data + i * NODE_COMPACT_LEN;
		struct sockaddr_storage addr = {0};
		struct sockaddr_in *a4 = (struct sockaddr_in *)&addr;
		a4->sin_family = AF_INET;
		memcpy(&a4->sin_addr, p + 20, 4);
		memcpy(&a4->sin_port, p + 24, 2); /* already network order */

		/* Skip bogus nodes */
		if (a4->sin_addr.s_addr == 0 || a4->sin_port == 0)
			continue;

		routing_add(d, p, &addr);
		if (lk)
			lookup_add_node(lk, p, &addr);
	}
}


static void
parse_compact_nodes6(struct mbt_dht *d, const uint8_t *data, size_t len, struct dht_lookup *lk)
{
	int count = len / NODE6_COMPACT_LEN;
	for (int i = 0; i < count; i++) {
		const uint8_t *p = data + i * NODE6_COMPACT_LEN;
		struct sockaddr_storage addr = {0};
		struct sockaddr_in6 *a6 = (struct sockaddr_in6 *)&addr;
		a6->sin6_family = AF_INET6;
		memcpy(&a6->sin6_addr, p + 20, 16);
		memcpy(&a6->sin6_port, p + 36, 2);

		/* Skip bogus */
		if (a6->sin6_port == 0) continue;
		if (memcmp(&a6->sin6_addr, &in6addr_any, 16) == 0) continue;

		routing_add(d, p, &addr);
		if (lk)
			lookup_add_node(lk, p, &addr);
	}
}

static void
parse_compact_peers(const uint8_t *data, size_t len,
                    mbt_dht_peers_cb cb, void *arg)
{
	if (len == 18) {
		/* Single IPv6 peer: 16 addr + 2 port */
		char addr[INET6_ADDRSTRLEN];
		uint16_t port;
		inet_ntop(AF_INET6, data, addr, sizeof(addr));
		memcpy(&port, data + 16, 2);
		port = ntohs(port);
		if (port > 0) cb(addr, port, arg);
		return;
	}
	int count = len / 6;
	for (int i = 0; i < count; i++) {
		char addr[INET_ADDRSTRLEN];
		uint16_t port;
		inet_ntop(AF_INET, data + i * 6, addr, sizeof(addr));
		memcpy(&port, data + i * 6 + 4, 2);
		port = ntohs(port);
		if (port < 1024) continue;
		cb(addr, port, arg);
	}
}

static void
dht_recv(struct mbt_ev_io *io, int fd, uint32_t events, void *v)
{
	struct mbt_dht *d = v;
	uint8_t buf[1500];
	struct sockaddr_storage from;
	socklen_t fromlen = sizeof(from);
	(void)io; (void)events;

	ssize_t n = recvfrom(fd, buf, sizeof(buf), 0, (struct sockaddr *)&from, &fromlen);
	if (n <= 0) return;

	/* Parse bencode */
	struct mbt_be_value *msg = NULL;
	int rv = mbt_be_value_parse(buf, n, &msg);
	if (rv <= 0) return;

	struct mbt_be_value *y = mbt_be_find(msg, "y");
	if (!y || y->type != BE_STRING) goto out;

	char type = *(char *)y->string.iov_base;

	if (type == 'r') {
		/* Response */
		struct mbt_be_value *r = mbt_be_find(msg, "r");
		if (!r) goto out;

		/* Extract sender's node ID and add to routing table */
		struct mbt_be_value *rid = mbt_be_find(r, "id");
		if (rid && rid->type == BE_STRING && rid->string.iov_len == 20)
			routing_add(d, rid->string.iov_base, &from);

		/* Check for nodes (find_node or get_peers response) */
		struct mbt_be_value *nodes = mbt_be_find(r, "nodes");
		struct mbt_be_value *nodes6 = mbt_be_find(r, "nodes6");
		struct mbt_be_value *values = mbt_be_find(r, "values");
		struct mbt_be_value *token = mbt_be_find(r, "token");

		/* Try to match to a lookup by source address */
		struct dht_lookup *lk = NULL;
		for (int i = 0; i < MAX_LOOKUPS; i++) {
			if (!d->lookups[i].active) continue;
			for (int j = 0; j < d->lookups[i].node_count; j++) {
				if (d->lookups[i].nodes[j].queried && !d->lookups[i].nodes[j].responded &&
				    sa_equal(&d->lookups[i].nodes[j].addr, &from)) {
					d->lookups[i].nodes[j].responded = 1;
					d->lookups[i].pending--;
					/* Update node ID from response */
					if (rid && rid->type == BE_STRING && rid->string.iov_len == 20)
						memcpy(d->lookups[i].nodes[j].id, rid->string.iov_base, 20);
					/* Store token */
					if (token && token->type == BE_STRING && token->string.iov_len <= TOKEN_LEN) {
						memcpy(d->lookups[i].nodes[j].token, token->string.iov_base, token->string.iov_len);
						d->lookups[i].nodes[j].token_len = token->string.iov_len;
					}
					lk = &d->lookups[i];
					break;
				}
			}
			if (lk) break;
		}

		/* Parse nodes */
		if (nodes && nodes->type == BE_STRING)
			parse_compact_nodes(d, nodes->string.iov_base, nodes->string.iov_len, lk);
		if (nodes6 && nodes6->type == BE_STRING)
			parse_compact_nodes6(d, nodes6->string.iov_base, nodes6->string.iov_len, lk);

		/* Parse peers (values is a list of compact peer strings) */
		if (values && values->type == BE_LIST && lk) {
			struct mbt_be_value *val;
			TAILQ_FOREACH(val, &values->listhead, next) {
				if (val->type == BE_STRING && val->string.iov_len >= 6)
					parse_compact_peers(val->string.iov_base, val->string.iov_len,
					                    lk->peers_cb, lk->arg);
			}
		}

		/* Advance lookup */
		if (lk)
			lookup_advance(d, lk);

	} else if (type == 'q') {
		/* Query — respond to pings, ignore others for now */
		struct mbt_be_value *q = mbt_be_find(msg, "q");
		struct mbt_be_value *t = mbt_be_find(msg, "t");
		struct mbt_be_value *a = mbt_be_find(msg, "a");

		if (!q || !t || !a) goto out;

		char _qaddr[64];
		mbt_log_debug(MBT_LOG_DHT, "query %.*s from %s",
		              (int)q->string.iov_len, (char *)q->string.iov_base,
		              sa_str(&from, _qaddr, sizeof(_qaddr)));

		/* Add querying node to routing table */
		struct mbt_be_value *qid = mbt_be_find(a, "id");
		if (qid && qid->type == BE_STRING && qid->string.iov_len == 20)
			routing_add(d, qid->string.iov_base, &from);

		if (q->type == BE_STRING && q->string.iov_len == 4 &&
		    memcmp(q->string.iov_base, "ping", 4) == 0) {
			/* Respond to ping */
			struct mbt_be_value *resp = NULL, *rb = NULL;
			mbt_be_dict_new(&resp);
			mbt_be_dict_append_string(resp, "t", t->string.iov_base, t->string.iov_len);
			mbt_be_dict_append_string(resp, "y", "r", 1);
			mbt_be_dict_new(&rb);
			mbt_be_dict_append_string(rb, "id", d->id, 20);
			mbt_be_dict_append(resp, "r", rb);

			unsigned char *enc; size_t enc_len;
			mbt_be_value_encode(resp, &enc, &enc_len);
			dht_send(d, &from, enc, enc_len);
			free(enc);
			mbt_be_value_free(resp);
		} else if (q->type == BE_STRING && q->string.iov_len == 9 &&
		           memcmp(q->string.iov_base, "find_node", 9) == 0) {
			/* Respond with closest nodes */
			struct mbt_be_value *target_v = mbt_be_find(a, "target");
			if (!target_v || target_v->type != BE_STRING || target_v->string.iov_len != 20)
				goto out;
			struct dht_node closest[K];
			int n = routing_closest(d, target_v->string.iov_base, closest, K);
			uint8_t compact[K * NODE_COMPACT_LEN];
			uint8_t compact6[K * NODE6_COMPACT_LEN];
			int n4 = 0, n6 = 0;
			for (int i = 0; i < n; i++) {
				if (closest[i].addr.ss_family == AF_INET6) {
					memcpy(compact6 + n6*38, closest[i].id, 20);
					memcpy(compact6 + n6*38 + 20, &((struct sockaddr_in6 *)&closest[i].addr)->sin6_addr, 16);
					memcpy(compact6 + n6*38 + 36, &((struct sockaddr_in6 *)&closest[i].addr)->sin6_port, 2);
					n6++;
				} else {
					memcpy(compact + n4*26, closest[i].id, 20);
					memcpy(compact + n4*26 + 20, &((struct sockaddr_in *)&closest[i].addr)->sin_addr, 4);
					memcpy(compact + n4*26 + 24, &((struct sockaddr_in *)&closest[i].addr)->sin_port, 2);
					n4++;
				}
			}
			struct mbt_be_value *resp = NULL, *rb = NULL;
			mbt_be_dict_new(&resp);
			mbt_be_dict_append_string(resp, "t", t->string.iov_base, t->string.iov_len);
			mbt_be_dict_append_string(resp, "y", "r", 1);
			mbt_be_dict_new(&rb);
			mbt_be_dict_append_string(rb, "id", d->id, 20);
			if (n4 > 0)
				mbt_be_dict_append_string(rb, "nodes", compact, n4 * 26);
			if (n6 > 0)
				mbt_be_dict_append_string(rb, "nodes6", compact6, n6 * 38);
			mbt_be_dict_append(resp, "r", rb);
			unsigned char *enc; size_t enc_len;
			mbt_be_value_encode(resp, &enc, &enc_len);
			dht_send(d, &from, enc, enc_len);
			free(enc);
			mbt_be_value_free(resp);

		} else if (q->type == BE_STRING && q->string.iov_len == 9 &&
		           memcmp(q->string.iov_base, "get_peers", 9) == 0) {
			/* Check if we have stored peers for this infohash */
			struct mbt_be_value *ih_v = mbt_be_find(a, "info_hash");
			if (!ih_v || ih_v->type != BE_STRING || ih_v->string.iov_len != 20)
				goto out;
			uint8_t token[4] = {0xDE, 0xAD, 0xBE, 0xEF};
			struct mbt_be_value *resp = NULL, *rb = NULL;
			mbt_be_dict_new(&resp);
			mbt_be_dict_append_string(resp, "t", t->string.iov_base, t->string.iov_len);
			mbt_be_dict_append_string(resp, "y", "r", 1);
			mbt_be_dict_new(&rb);
			mbt_be_dict_append_string(rb, "id", d->id, 20);
			mbt_be_dict_append_string(rb, "token", token, 4);
			/* Check stored peers */
			int found_stored = 0;
			for (int i = 0; i < d->stored_count; i++) {
				if (memcmp(d->stored[i].infohash, ih_v->string.iov_base, 20) == 0) {
					/* Return peers as "values" list */
					struct mbt_be_value *vals;
					mbt_be_list_new(&vals);
					for (int j = 0; j < d->stored[i].peer_count; j++) {
						struct sockaddr_storage *pa = &d->stored[i].peers[j];
						if (pa->ss_family == AF_INET6) {
							uint8_t cp6[18];
							memcpy(cp6, &((struct sockaddr_in6 *)pa)->sin6_addr, 16);
							memcpy(cp6 + 16, &((struct sockaddr_in6 *)pa)->sin6_port, 2);
							struct mbt_be_value *pv;
							mbt_be_string_new(&pv, cp6, 18);
							mbt_be_list_append(vals, pv);
						} else {
							uint8_t cp4[6];
							memcpy(cp4, &((struct sockaddr_in *)pa)->sin_addr, 4);
							memcpy(cp4 + 4, &((struct sockaddr_in *)pa)->sin_port, 2);
							struct mbt_be_value *pv;
							mbt_be_string_new(&pv, cp4, 6);
							mbt_be_list_append(vals, pv);
						}
					}
					mbt_be_dict_append(rb, "values", vals);
					found_stored = 1;
					break;
				}
			}
			if (!found_stored) {
				/* Return closest nodes */
				struct dht_node closest[K];
				int n = routing_closest(d, ih_v->string.iov_base, closest, K);
				uint8_t compact_nodes[K * NODE_COMPACT_LEN];
				uint8_t compact_nodes6[K * NODE6_COMPACT_LEN];
				int cn4 = 0, cn6 = 0;
				for (int i = 0; i < n; i++) {
					if (closest[i].addr.ss_family == AF_INET6) {
						memcpy(compact_nodes6 + cn6*38, closest[i].id, 20);
						memcpy(compact_nodes6 + cn6*38 + 20, &((struct sockaddr_in6 *)&closest[i].addr)->sin6_addr, 16);
						memcpy(compact_nodes6 + cn6*38 + 36, &((struct sockaddr_in6 *)&closest[i].addr)->sin6_port, 2);
						cn6++;
					} else {
						memcpy(compact_nodes + cn4*26, closest[i].id, 20);
						memcpy(compact_nodes + cn4*26 + 20, &((struct sockaddr_in *)&closest[i].addr)->sin_addr, 4);
						memcpy(compact_nodes + cn4*26 + 24, &((struct sockaddr_in *)&closest[i].addr)->sin_port, 2);
						cn4++;
					}
				}
				if (cn4 > 0)
					mbt_be_dict_append_string(rb, "nodes", compact_nodes, cn4 * 26);
				if (cn6 > 0)
					mbt_be_dict_append_string(rb, "nodes6", compact_nodes6, cn6 * 38);
			}
			mbt_be_dict_append(resp, "r", rb);
			unsigned char *enc; size_t enc_len;
			mbt_be_value_encode(resp, &enc, &enc_len);
			dht_send(d, &from, enc, enc_len);
			free(enc);
			mbt_be_value_free(resp);

		} else if (q->type == BE_STRING && q->string.iov_len == 13 &&
		           memcmp(q->string.iov_base, "announce_peer", 13) == 0) {
			/* Store the announcing peer */
			struct mbt_be_value *ih_v = mbt_be_find(a, "info_hash");
			struct mbt_be_value *port_v = mbt_be_find(a, "port");
			if (!ih_v || ih_v->type != BE_STRING || ih_v->string.iov_len != 20)
				goto out;
			uint16_t from_port = (from.ss_family == AF_INET6) ?
				ntohs(((struct sockaddr_in6 *)&from)->sin6_port) :
				ntohs(((struct sockaddr_in *)&from)->sin_port);
			uint16_t aport = port_v ? (uint16_t)port_v->integer : from_port;
			/* Find or create storage slot */
			int slot = -1;
			for (int i = 0; i < d->stored_count; i++) {
				if (memcmp(d->stored[i].infohash, ih_v->string.iov_base, 20) == 0)
					{ slot = i; break; }
			}
			if (slot < 0 && d->stored_count < DHT_MAX_STORED) {
				slot = d->stored_count++;
				memcpy(d->stored[slot].infohash, ih_v->string.iov_base, 20);
				d->stored[slot].peer_count = 0;
			}
			if (slot >= 0 && d->stored[slot].peer_count < 8) {
				struct sockaddr_storage pa = from;
				/* Override port with announced port */
				if (pa.ss_family == AF_INET6)
					((struct sockaddr_in6 *)&pa)->sin6_port = htons(aport);
				else
					((struct sockaddr_in *)&pa)->sin_port = htons(aport);
				d->stored[slot].peers[d->stored[slot].peer_count++] = pa;
			}
			/* Respond */
			struct mbt_be_value *resp = NULL, *rb = NULL;
			mbt_be_dict_new(&resp);
			mbt_be_dict_append_string(resp, "t", t->string.iov_base, t->string.iov_len);
			mbt_be_dict_append_string(resp, "y", "r", 1);
			mbt_be_dict_new(&rb);
			mbt_be_dict_append_string(rb, "id", d->id, 20);
			mbt_be_dict_append(resp, "r", rb);
			unsigned char *enc; size_t enc_len;
			mbt_be_value_encode(resp, &enc, &enc_len);
			dht_send(d, &from, enc, enc_len);
			free(enc);
			mbt_be_value_free(resp);
		}
	}
	/* Ignore errors ('e') */

out:
	mbt_be_value_free(msg);
}

/* --- Refresh timer --- */

static void
dht_refresh_cb(struct mbt_ev_timer *timer, void *v)
{
	struct mbt_dht *d = v;
	int total = mbt_dht_node_count(d);

	/* Evict stale nodes (no activity in 5 min) */
	time_t now = time(NULL);
	for (int i = 0; i < NUM_BUCKETS; i++) {
		int j = 0;
		while (j < d->buckets[i].count) {
			if (now - d->buckets[i].nodes[j].last_seen > 300) {
				d->buckets[i].nodes[j] = d->buckets[i].nodes[--d->buckets[i].count];
			} else {
				j++;
			}
		}
	}
	/* Send find_node to up to 8 known nodes to expand routing table */
	int sent = 0;
	for (int i = 0; i < NUM_BUCKETS && sent < 8; i++) {
		for (int j = 0; j < d->buckets[i].count && sent < 8; j++) {
			dht_send_find_node(d, &d->buckets[i].nodes[j].addr, d->id);
			sent++;
		}
	}

	/* Ping a random node to keep routing table alive */
	for (int i = 0; i < NUM_BUCKETS; i++) {
		if (d->buckets[i].count > 0) {
			int j = rand() % d->buckets[i].count;
			dht_send_ping(d, &d->buckets[i].nodes[j].addr);
			break;
		}
	}

	/* Reschedule: aggressive (10s) while small, normal (15min) when populated */
	uint64_t next = (total < 20) ? 10 * 1000000ULL : REFRESH_US;
	mbt_ev_timer_mod(timer, next);
}

/* --- Public API --- */

int
mbt_dht_new(struct mbt_dht **out, struct mbt_ev *ev, uint16_t port)
{
	struct mbt_dht *d = calloc(1, sizeof(*d));
	if (!d) return -ENOMEM;

	d->ev = ev;
	d->port = port;
	srand(time(NULL) ^ getpid());

	/* Derive stable node ID from machine-id, or fall back to random */
	dht_init_node_id(d->id);

	/* Bind dual-stack UDP sockets */
	int rv = mbt_listen_udp(&d->listener, ev, port, dht_recv, d);
	if (rv < 0) { free(d); return rv; }
	mbt_log_info(MBT_LOG_DHT, "UDP sockets: fd4=%d fd6=%d io4=%p io6=%p",
	             d->listener.fd4, d->listener.fd6,
	             (void*)d->listener.io4, (void*)d->listener.io6);

	/* Refresh timer — first fire at 5s to populate routing table quickly */
	mbt_ev_timer_new(ev, &d->refresh_timer, 5 * 1000000ULL, dht_refresh_cb, d);

	/* Rate limit: 20 queries/sec, burst of 40 */
	mbt_ratelimit_new(&d->send_limit, 20, 40);

	char hex[41];
	mbt_hex_encode(d->id, 20, hex, sizeof(hex));
	mbt_log_info(MBT_LOG_DHT, "DHT started, node_id=%s port=%hu", hex, port);

	*out = d;
	return 0;
}

void
mbt_dht_free(struct mbt_dht *d)
{
	if (!d) return;
	for (int i = 0; i < MAX_LOOKUPS; i++)
		if (d->lookups[i].timer)
			mbt_ev_timer_free(d->lookups[i].timer);
	if (d->refresh_timer) mbt_ev_timer_free(d->refresh_timer);
	mbt_ratelimit_free(d->send_limit);
	mbt_listener_close(&d->listener);
	free(d);
}

struct dht_bootstrap_ctx {
	struct mbt_dht *d;
};

static void
dht_bootstrap_resolved(struct sockaddr_storage *addr, socklen_t len, int err, void *arg)
{
	struct dht_bootstrap_ctx *ctx = arg;
	(void)len;
	if (err < 0 || !addr) { free(ctx); return; }

	uint8_t fake_id[20];
	random_bytes(fake_id, 20);
	routing_add(ctx->d, fake_id, addr);

	char buf[INET6_ADDRSTRLEN];
	if (addr->ss_family == AF_INET)
		inet_ntop(AF_INET, &((struct sockaddr_in *)addr)->sin_addr, buf, sizeof(buf));
	else
		inet_ntop(AF_INET6, &((struct sockaddr_in6 *)addr)->sin6_addr, buf, sizeof(buf));
	mbt_log_info(MBT_LOG_DHT, "bootstrapping from %s", buf);

	dht_send_find_node(ctx->d, addr, ctx->d->id);
	free(ctx);
}

int
mbt_dht_bootstrap(struct mbt_dht *d, const char *addr, uint16_t port)
{
	struct sockaddr_storage sa = {0};
	struct sockaddr_in *s4 = (struct sockaddr_in *)&sa;
	struct sockaddr_in6 *s6 = (struct sockaddr_in6 *)&sa;

	if (inet_pton(AF_INET6, addr, &s6->sin6_addr) > 0) {
		s6->sin6_family = AF_INET6;
		s6->sin6_port = htons(port);
	} else if (inet_pton(AF_INET, addr, &s4->sin_addr) > 0) {
		s4->sin_family = AF_INET;
		s4->sin_port = htons(port);
	} else {
		/* Async DNS resolution */
		struct dht_bootstrap_ctx *ctx = calloc(1, sizeof(*ctx));
		if (!ctx) return -ENOMEM;
		ctx->d = d;
		return mbt_resolve(d->ev, addr, port, dht_bootstrap_resolved, ctx, NULL);
	}

	/* IP literal — immediate */
	uint8_t fake_id[20];
	random_bytes(fake_id, 20);
	routing_add(d, fake_id, &sa);

	mbt_log_info(MBT_LOG_DHT, "bootstrapping from %s:%hu", addr, port);
	return dht_send_find_node(d, &sa, d->id);
}

int
mbt_dht_get_peers(struct mbt_dht *d, struct mbt_infohash *ih,
                  mbt_dht_peers_cb peers_cb, mbt_dht_done_cb done_cb, void *arg)
{
	/* Find a free lookup slot */
	struct dht_lookup *lk = NULL;
	for (int i = 0; i < MAX_LOOKUPS; i++) {
		if (!d->lookups[i].active) { lk = &d->lookups[i]; break; }
	}
	if (!lk) return -EBUSY;

	memset(lk, 0, sizeof(*lk));
	lk->active = 1;
	memcpy(lk->target, ih->hash_sha1, 20);
	lk->peers_cb = peers_cb;
	lk->done_cb = done_cb;
	lk->arg = arg;

	/* Seed lookup with closest nodes from routing table */
	struct dht_node closest[K];
	int n = routing_closest(d, lk->target, closest, K);
	for (int i = 0; i < n; i++)
		lookup_add_node(lk, closest[i].id, &closest[i].addr);

	char hex[41];
	mbt_hex_encode(lk->target, 20, hex, sizeof(hex));
	mbt_log_info(MBT_LOG_DHT, "get_peers lookup for %s (%d seed nodes)", hex, n);

	/* Set timeout */
	mbt_ev_timer_new(d->ev, &lk->timer, LOOKUP_TIMEOUT_US, lookup_timeout_cb, lk);

	/* Start querying */
	lookup_advance(d, lk);
	return 0;
}

int
mbt_dht_announce(struct mbt_dht *d, struct mbt_infohash *ih, uint16_t listen_port)
{
	/* Announce to all nodes that responded with a token in the last lookup */
	struct dht_lookup *lk = lookup_find_for_target(d, ih->hash_sha1);
	if (!lk) {
		/* No recent lookup — announce to closest nodes (no token, will likely fail) */
		mbt_log_warn(MBT_LOG_DHT, "announce without prior get_peers — may fail");
		return -EINVAL;
	}

	int sent = 0;
	for (int i = 0; i < lk->node_count; i++) {
		if (lk->nodes[i].token_len > 0) {
			dht_send_announce(d, &lk->nodes[i].addr, ih->hash_sha1, listen_port,
			                  lk->nodes[i].token, lk->nodes[i].token_len);
			sent++;
		}
	}
	mbt_log_info(MBT_LOG_DHT, "announced to %d nodes", sent);
	return 0;
}

int
mbt_dht_node_count(struct mbt_dht *d)
{
	int total = 0;
	for (int i = 0; i < NUM_BUCKETS; i++)
		total += d->buckets[i].count;
	return total;
}

void
mbt_dht_set_node_id(struct mbt_dht *d, const uint8_t id[20])
{
	memcpy(d->id, id, 20);
}
