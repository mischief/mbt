// SPDX-License-Identifier: ISC
/*
 * dht.c — BitTorrent DHT (BEP 5) implementation
 *
 * Kademlia-based distributed hash table over UDP.
 * Standalone: only depends on event loop, bencode, infohash, logging.
 */
#include <sys/socket.h>
#include <netinet/in.h>
#include <arpa/inet.h>
#include <stdlib.h>
#include <stdio.h>
#include <string.h>
#include <unistd.h>
#include <errno.h>
#include <time.h>
#include <sys/queue.h>
#include "mbt/compat.h"
#include <sha1.h>
#include <stdint.h>
#include <sys/types.h>

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
#include "udp_mux.h"
#include "internal.h"

struct mbt_ev_io;
struct mbt_ev_timer;

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
	int pinged;
	SLIST_ENTRY(dht_node) entry;
};

struct dht_bucket {
	SLIST_HEAD(, dht_node) nodes;
	int count;
	int depth;           /* bit prefix depth */
	time_t last_changed;
	struct dht_bucket *next;
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
	struct mbt_udp_mux *mux;   /* UDP socket, shared with uTP or owned */
	int owns_mux;              /* free mux in dht_free if we created it */
	uint8_t id[20];  /* our node ID */
	uint16_t port;

	/* Routing table: 160 buckets */
	struct dht_bucket *buckets; /* linked list of split buckets */
	int bucket_count;

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

	/* Token secrets: rotate every 5 min, accept tokens from either */
#define TOKEN_SECRET_LEN 8
#define TOKEN_ROTATE_US  (5 * 60 * 1000000ULL)
	uint8_t token_secret[TOKEN_SECRET_LEN];
	uint8_t token_secret_prev[TOKEN_SECRET_LEN];
	struct mbt_ev_timer *token_timer;

	/* Query rate limit (messages/sec as bytes, 1 msg = 1 token) */
	struct mbt_ratelimit *send_limit;

	/* BEP 44: item storage */
#define DHT_MAX_ITEMS 64
#define DHT_ITEM_EXPIRE 7200
	struct dht_item {
		uint8_t target[20];
		uint8_t *v;
		size_t vlen;
		uint8_t k[MBT_DHT_PUBKEY_LEN];
		uint8_t sig[MBT_DHT_SIG_LEN];
		int64_t seq;
		char *salt;
		size_t salt_len;
		int mutable;
		time_t stored_at;
	} items[DHT_MAX_ITEMS];
	int item_count;

	mbt_dht_verify_cb verify_cb;
	void *verify_arg;
};

/* --- Helpers --- */

static struct dht_item *dht_item_find(struct mbt_dht *d, const uint8_t *target);
static struct dht_item *dht_item_alloc(struct mbt_dht *d, const uint8_t *target);
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

/* Compute token = first TOKEN_LEN bytes of SHA1(secret || IP bytes) */
static void
make_token_with_secret(const uint8_t *secret, const struct sockaddr_storage *sa,
                       uint8_t out[TOKEN_LEN])
{
	SHA1_CTX ctx;
	SHA1Init(&ctx);
	SHA1Update(&ctx, secret, TOKEN_SECRET_LEN);
	if (sa->ss_family == AF_INET6) {
		SHA1Update(&ctx, (const uint8_t *)&((const struct sockaddr_in6 *)sa)->sin6_addr, 16);
	} else {
		SHA1Update(&ctx, (const uint8_t *)&((const struct sockaddr_in *)sa)->sin_addr, 4);
	}
	uint8_t digest[20];
	SHA1Final(digest, &ctx);
	memcpy(out, digest, TOKEN_LEN);
}

static void
dht_make_token(struct mbt_dht *d, const struct sockaddr_storage *sa, uint8_t out[TOKEN_LEN])
{
	make_token_with_secret(d->token_secret, sa, out);
}

/* Returns 1 if token matches current or previous secret */
static int
dht_verify_token(struct mbt_dht *d, const struct sockaddr_storage *sa,
                 const uint8_t *token, size_t token_len)
{
	if (token_len != TOKEN_LEN) return 0;
	uint8_t expected[TOKEN_LEN];
	make_token_with_secret(d->token_secret, sa, expected);
	if (memcmp(expected, token, TOKEN_LEN) == 0) return 1;
	make_token_with_secret(d->token_secret_prev, sa, expected);
	return memcmp(expected, token, TOKEN_LEN) == 0;
}

static void
dht_token_rotate_cb(struct mbt_ev_timer *timer, void *v)
{
	struct mbt_dht *d = v;
	memcpy(d->token_secret_prev, d->token_secret, TOKEN_SECRET_LEN);
	random_bytes(d->token_secret, TOKEN_SECRET_LEN);
	mbt_ev_timer_mod(timer, TOKEN_ROTATE_US);
}

/* --- Routing table (split-bucket) --- */

/* Find the bucket that covers the given ID */
static struct dht_bucket *
routing_bucket_for(struct mbt_dht *d, const uint8_t *id)
{
	int depth = bucket_index(d->id, id);
	struct dht_bucket *best = d->buckets;
	for (struct dht_bucket *b = d->buckets; b; b = b->next) {
		if (b->depth <= depth)
			best = b;
	}
	return best;
}

/* Check if our own ID falls within this bucket's range */
static int
bucket_contains_self(struct mbt_dht *d, struct dht_bucket *b)
{
	(void)d;
	/* The bucket at depth N covers nodes with prefix length >= N.
	 * Our own ID always has prefix length 159 (distance 0 to itself).
	 * The deepest bucket (highest depth) always contains us. */
	struct dht_bucket *next = b->next;
	if (!next) return 1; /* last bucket always contains us */
	return 0;
}

/* Split a bucket into two: one for depth, one for depth+1 */
static void
routing_split(struct mbt_dht *d, struct dht_bucket *b)
{
	struct dht_bucket *nb = calloc(1, sizeof(*nb));
	if (!nb) return;
	SLIST_INIT(&nb->nodes);
	nb->depth = b->depth + 1;
	nb->last_changed = time(NULL);

	/* Insert new bucket after b in the list */
	nb->next = b->next;
	b->next = nb;
	d->bucket_count++;

	/* Redistribute nodes */
	struct dht_node *n, *tmp; (void)tmp;
	SLIST_HEAD(, dht_node) keep = SLIST_HEAD_INITIALIZER(keep);
	int keep_count = 0, moved_count = 0;

	while ((n = SLIST_FIRST(&b->nodes)) != NULL) {
		SLIST_REMOVE_HEAD(&b->nodes, entry);
		int depth = bucket_index(d->id, n->id);
		if (depth >= nb->depth) {
			SLIST_INSERT_HEAD(&nb->nodes, n, entry);
			moved_count++;
		} else {
			SLIST_INSERT_HEAD(&keep, n, entry);
			keep_count++;
		}
	}
	SLIST_FIRST(&b->nodes) = SLIST_FIRST(&keep);
	b->count = keep_count;
	nb->count = moved_count;
}

static void
routing_add(struct mbt_dht *d, const uint8_t *id, const struct sockaddr_storage *addr)
{
	if (memcmp(id, d->id, DHT_PEER_ID_LEN) == 0) return;

	struct dht_bucket *b = routing_bucket_for(d, id);

	/* Already in bucket? Update */
	struct dht_node *n;
	SLIST_FOREACH(n, &b->nodes, entry) {
		if (memcmp(n->id, id, DHT_PEER_ID_LEN) == 0) {
			n->last_seen = time(NULL);
			n->addr = *addr;
			return;
		}
	}

	/* Room in bucket? Add */
	if (b->count < K) {
		n = calloc(1, sizeof(*n));
		if (!n) return;
		memcpy(n->id, id, 20);
		n->addr = *addr;
		n->last_seen = time(NULL);
		SLIST_INSERT_HEAD(&b->nodes, n, entry);
		b->count++;
		b->last_changed = time(NULL);
		mbt_log_debug(MBT_LOG_DHT, "routing add bucket=%d count=%d", b->depth, b->count);
		return;
	}

	/* Bucket full — split if it contains our own ID */
	if (bucket_contains_self(d, b)) {
		routing_split(d, b);
		/* Retry add into the (now smaller) appropriate bucket */
		routing_add(d, id, addr);
		return;
	}

	/* Bucket full and can't split — evict stale nodes */
	time_t now = time(NULL);
	struct dht_node *stale = NULL;
	SLIST_FOREACH(n, &b->nodes, entry) {
		if (now - n->last_seen > 300) { stale = n; break; }
	}
	if (stale) {
		SLIST_REMOVE(&b->nodes, stale, dht_node, entry);
		b->count--;
		free(stale);
		/* Add the new node */
		n = calloc(1, sizeof(*n));
		if (!n) return;
		memcpy(n->id, id, 20);
		n->addr = *addr;
		n->last_seen = time(NULL);
		SLIST_INSERT_HEAD(&b->nodes, n, entry);
		b->count++;
		b->last_changed = time(NULL);
	}
}

static int
routing_closest(struct mbt_dht *d, const uint8_t *target, struct dht_node *out, int max)
{
	/* Collect all nodes, sort by XOR distance to target */
	struct dht_node *all[256];
	int total = 0;

	for (struct dht_bucket *b = d->buckets; b && total < 256; b = b->next) {
		struct dht_node *n;
		SLIST_FOREACH(n, &b->nodes, entry) {
			if (total < 256) all[total++] = n;
		}
	}

	/* Sort by distance to target */
	for (int i = 0; i < total - 1; i++)
		for (int j = i + 1; j < total; j++)
			if (closer_to(target, all[j]->id, all[i]->id) < 0) {
				struct dht_node *tmp = all[i]; all[i] = all[j]; all[j] = tmp;
			}

	int count = total < max ? total : max;
	for (int i = 0; i < count; i++)
		memcpy(&out[i], all[i], sizeof(out[i]));
	return count;
}

/* --- Send helpers --- */

static int
dht_send(struct mbt_dht *d, const struct sockaddr_storage *addr,
         const void *data, size_t len)
{
	socklen_t salen = (addr->ss_family == AF_INET6) ?
		sizeof(struct sockaddr_in6) : sizeof(struct sockaddr_in);
	return mbt_udp_mux_sendto(d->mux, data, len,
	                          (const struct sockaddr *)addr, salen);
}

static int
dht_send_find_node(struct mbt_dht *d, const struct sockaddr_storage *addr, const uint8_t *target)
{
	if (!mbt_ratelimit_allow(d->send_limit, 1))
		return 0;
	_cleanup_be_ struct mbt_be_value *msg = NULL, *a = NULL;
	uint8_t txn[TXN_LEN];
	make_txn(d, txn);

	if (mbt_be_dict_new(&msg) < 0) return -ENOMEM;
	if (mbt_be_dict_append_string(msg, "t", txn, TXN_LEN) < 0) return -ENOMEM;
	if (mbt_be_dict_append_string(msg, "y", "q", 1) < 0) return -ENOMEM;
	if (mbt_be_dict_append_string(msg, "q", "find_node", 9) < 0) return -ENOMEM;
	if (mbt_be_dict_new(&a) < 0) return -ENOMEM;
	if (mbt_be_dict_append_string(a, "id", d->id, 20) < 0) return -ENOMEM;
	if (mbt_be_dict_append_string(a, "target", target, 20) < 0) return -ENOMEM;
	if (mbt_be_dict_append(msg, "a", a) < 0) return -ENOMEM;

	unsigned char *enc; size_t enc_len;
	if (mbt_be_value_encode(msg, &enc, &enc_len) < 0) return -ENOMEM;
	int rv = dht_send(d, addr, enc, enc_len);
	free(enc);
	return rv;
}

static int
dht_send_get_peers(struct mbt_dht *d, const struct sockaddr_storage *addr, const uint8_t *infohash)
{
	_cleanup_be_ struct mbt_be_value *msg = NULL, *a = NULL;
	uint8_t txn[TXN_LEN];
	make_txn(d, txn);

	if (mbt_be_dict_new(&msg) < 0) return -ENOMEM;
	if (mbt_be_dict_append_string(msg, "t", txn, TXN_LEN) < 0) return -ENOMEM;
	if (mbt_be_dict_append_string(msg, "y", "q", 1) < 0) return -ENOMEM;
	if (mbt_be_dict_append_string(msg, "q", "get_peers", 9) < 0) return -ENOMEM;
	if (mbt_be_dict_new(&a) < 0) return -ENOMEM;
	if (mbt_be_dict_append_string(a, "id", d->id, 20) < 0) return -ENOMEM;
	if (mbt_be_dict_append_string(a, "info_hash", infohash, 20) < 0) return -ENOMEM;
	if (mbt_be_dict_append(msg, "a", a) < 0) return -ENOMEM;

	unsigned char *enc; size_t enc_len;
	if (mbt_be_value_encode(msg, &enc, &enc_len) < 0) return -ENOMEM;
	int rv = dht_send(d, addr, enc, enc_len);
	free(enc);
	return rv;
}

static int
dht_send_announce(struct mbt_dht *d, const struct sockaddr_storage *addr,
                  const uint8_t *infohash, uint16_t port,
                  const uint8_t *token, int token_len)
{
	_cleanup_be_ struct mbt_be_value *msg = NULL, *a = NULL;
	uint8_t txn[TXN_LEN];
	make_txn(d, txn);

	if (mbt_be_dict_new(&msg) < 0) return -ENOMEM;
	if (mbt_be_dict_append_string(msg, "t", txn, TXN_LEN) < 0) return -ENOMEM;
	if (mbt_be_dict_append_string(msg, "y", "q", 1) < 0) return -ENOMEM;
	if (mbt_be_dict_append_string(msg, "q", "announce_peer", 13) < 0) return -ENOMEM;
	if (mbt_be_dict_new(&a) < 0) return -ENOMEM;
	if (mbt_be_dict_append_string(a, "id", d->id, 20) < 0) return -ENOMEM;
	if (mbt_be_dict_append_string(a, "info_hash", infohash, 20) < 0) return -ENOMEM;
	if (mbt_be_dict_append_number(a, "port", port) < 0) return -ENOMEM;
	if (mbt_be_dict_append_string(a, "token", token, token_len) < 0) return -ENOMEM;
	if (mbt_be_dict_append_number(a, "implied_port", 0) < 0) return -ENOMEM;
	if (mbt_be_dict_append(msg, "a", a) < 0) return -ENOMEM;

	unsigned char *enc; size_t enc_len;
	if (mbt_be_value_encode(msg, &enc, &enc_len) < 0) return -ENOMEM;
	int rv = dht_send(d, addr, enc, enc_len);
	free(enc);
	return rv;
}

static int
dht_send_ping(struct mbt_dht *d, const struct sockaddr_storage *addr)
{
	_cleanup_be_ struct mbt_be_value *msg = NULL, *a = NULL;
	uint8_t txn[TXN_LEN];
	make_txn(d, txn);

	if (mbt_be_dict_new(&msg) < 0) return -ENOMEM;
	if (mbt_be_dict_append_string(msg, "t", txn, TXN_LEN) < 0) return -ENOMEM;
	if (mbt_be_dict_append_string(msg, "y", "q", 1) < 0) return -ENOMEM;
	if (mbt_be_dict_append_string(msg, "q", "ping", 4) < 0) return -ENOMEM;
	if (mbt_be_dict_new(&a) < 0) return -ENOMEM;
	if (mbt_be_dict_append_string(a, "id", d->id, 20) < 0) return -ENOMEM;
	if (mbt_be_dict_append(msg, "a", a) < 0) return -ENOMEM;

	unsigned char *enc; size_t enc_len;
	if (mbt_be_value_encode(msg, &enc, &enc_len) < 0) return -ENOMEM;
	int rv = dht_send(d, addr, enc, enc_len);
	free(enc);
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
dht_handle(struct mbt_dht *d, const uint8_t *buf, ssize_t n,
           const struct sockaddr_storage *fromp)
{
	struct sockaddr_storage from = *fromp;

	/* Parse bencode */
	struct mbt_be_value *msg = NULL;
	int rv = mbt_be_value_parse(buf, n, &msg);
	if (rv <= 0) return;

	struct mbt_be_value *y = mbt_be_find(msg, "y");
	if (!y || y->type != BE_STRING) goto out;

	char type = *(char *)y->string.buf;

	if (type == 'r') {
		/* Response */
		struct mbt_be_value *r = mbt_be_find(msg, "r");
		if (!r) goto out;

		/* Extract sender's node ID and add to routing table */
		struct mbt_be_value *rid = mbt_be_find(r, "id");
		if (rid && rid->type == BE_STRING && rid->string.len == 20)
			routing_add(d, rid->string.buf, &from);

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
					if (rid && rid->type == BE_STRING && rid->string.len == 20)
						memcpy(d->lookups[i].nodes[j].id, rid->string.buf, 20);
					/* Store token */
					if (token && token->type == BE_STRING && token->string.len <= TOKEN_LEN) {
						memcpy(d->lookups[i].nodes[j].token, token->string.buf, token->string.len);
						d->lookups[i].nodes[j].token_len = token->string.len;
					}
					lk = &d->lookups[i];
					break;
				}
			}
			if (lk) break;
		}

		/* Parse nodes */
		if (nodes && nodes->type == BE_STRING)
			parse_compact_nodes(d, nodes->string.buf, nodes->string.len, lk);
		if (nodes6 && nodes6->type == BE_STRING)
			parse_compact_nodes6(d, nodes6->string.buf, nodes6->string.len, lk);

		/* Parse peers (values is a list of compact peer strings) */
		if (values && values->type == BE_LIST && lk && lk->peers_cb) {
			struct mbt_be_value *val;
			TAILQ_FOREACH(val, &values->listhead, next) {
				if (val->type == BE_STRING && val->string.len >= 6)
					parse_compact_peers(val->string.buf, val->string.len,
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
		              (int)q->string.len, (char *)q->string.buf,
		              sa_str(&from, _qaddr, sizeof(_qaddr)));

		/* Add querying node to routing table */
		struct mbt_be_value *qid = mbt_be_find(a, "id");
		if (qid && qid->type == BE_STRING && qid->string.len == 20)
			routing_add(d, qid->string.buf, &from);

		if (mbt_be_streq(q, "ping")) {
			/* Respond to ping */
			_cleanup_be_ struct mbt_be_value *resp = NULL, *rb = NULL;
			if (mbt_be_dict_new(&resp) < 0) goto out;
			if (mbt_be_dict_append_string(resp, "t", t->string.buf, t->string.len) < 0) goto out;
			if (mbt_be_dict_append_string(resp, "y", "r", 1) < 0) goto out;
			if (mbt_be_dict_new(&rb) < 0) goto out;
			if (mbt_be_dict_append_string(rb, "id", d->id, 20) < 0) goto out;
			if (mbt_be_dict_append(resp, "r", rb) < 0) goto out;

			unsigned char *enc; size_t enc_len;
			if (mbt_be_value_encode(resp, &enc, &enc_len) < 0) goto out;
			dht_send(d, &from, enc, enc_len);
			free(enc);
		} else if (mbt_be_streq(q, "find_node")) {
			/* Respond with closest nodes */
			struct mbt_be_value *target_v = mbt_be_find(a, "target");
			if (!target_v || target_v->type != BE_STRING || target_v->string.len != 20)
				goto out;
			struct dht_node closest[K];
			int n = routing_closest(d, target_v->string.buf, closest, K);
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
			_cleanup_be_ struct mbt_be_value *resp = NULL, *rb = NULL;
			if (mbt_be_dict_new(&resp) < 0) goto out;
			if (mbt_be_dict_append_string(resp, "t", t->string.buf, t->string.len) < 0) goto out;
			if (mbt_be_dict_append_string(resp, "y", "r", 1) < 0) goto out;
			if (mbt_be_dict_new(&rb) < 0) goto out;
			if (mbt_be_dict_append_string(rb, "id", d->id, 20) < 0) goto out;
			if (n4 > 0)
				if (mbt_be_dict_append_string(rb, "nodes", compact, n4 * 26) < 0) goto out;
			if (n6 > 0)
				if (mbt_be_dict_append_string(rb, "nodes6", compact6, n6 * 38) < 0) goto out;
			if (mbt_be_dict_append(resp, "r", rb) < 0) goto out;
			unsigned char *enc; size_t enc_len;
			if (mbt_be_value_encode(resp, &enc, &enc_len) < 0) goto out;
			dht_send(d, &from, enc, enc_len);
			free(enc);

		} else if (mbt_be_streq(q, "get_peers")) {
			/* Check if we have stored peers for this infohash */
			struct mbt_be_value *ih_v = mbt_be_find(a, "info_hash");
			if (!ih_v || ih_v->type != BE_STRING || ih_v->string.len != 20)
				goto out;
			uint8_t token[TOKEN_LEN];
			dht_make_token(d, &from, token);
			_cleanup_be_ struct mbt_be_value *resp = NULL, *rb = NULL;
			if (mbt_be_dict_new(&resp) < 0) goto out;
			if (mbt_be_dict_append_string(resp, "t", t->string.buf, t->string.len) < 0) goto out;
			if (mbt_be_dict_append_string(resp, "y", "r", 1) < 0) goto out;
			if (mbt_be_dict_new(&rb) < 0) goto out;
			if (mbt_be_dict_append_string(rb, "id", d->id, 20) < 0) goto out;
			if (mbt_be_dict_append_string(rb, "token", token, TOKEN_LEN) < 0) goto out;
			/* Check stored peers */
			int found_stored = 0;
			for (int i = 0; i < d->stored_count; i++) {
				if (memcmp(d->stored[i].infohash, ih_v->string.buf, 20) == 0) {
					/* Return peers as "values" list */
					_cleanup_be_ struct mbt_be_value *vals = NULL;
					if (mbt_be_list_new(&vals) < 0) goto out;
					for (int j = 0; j < d->stored[i].peer_count; j++) {
						struct sockaddr_storage *pa = &d->stored[i].peers[j];
						if (pa->ss_family == AF_INET6) {
							uint8_t cp6[18];
							memcpy(cp6, &((struct sockaddr_in6 *)pa)->sin6_addr, 16);
							memcpy(cp6 + 16, &((struct sockaddr_in6 *)pa)->sin6_port, 2);
							_cleanup_be_ struct mbt_be_value *pv = NULL;
							if (mbt_be_string_new(&pv, cp6, 18) < 0) goto out;
							mbt_be_list_append(vals, pv);
						} else {
							uint8_t cp4[6];
							memcpy(cp4, &((struct sockaddr_in *)pa)->sin_addr, 4);
							memcpy(cp4 + 4, &((struct sockaddr_in *)pa)->sin_port, 2);
							_cleanup_be_ struct mbt_be_value *pv = NULL;
							if (mbt_be_string_new(&pv, cp4, 6) < 0) goto out;
							mbt_be_list_append(vals, pv);
						}
					}
					if (mbt_be_dict_append(rb, "values", vals) < 0) goto out;
					found_stored = 1;
					break;
				}
			}
			if (!found_stored) {
				/* Return closest nodes */
				struct dht_node closest[K];
				int n = routing_closest(d, ih_v->string.buf, closest, K);
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
					if (mbt_be_dict_append_string(rb, "nodes", compact_nodes, cn4 * 26) < 0) goto out;
				if (cn6 > 0)
					if (mbt_be_dict_append_string(rb, "nodes6", compact_nodes6, cn6 * 38) < 0) goto out;
			}
			if (mbt_be_dict_append(resp, "r", rb) < 0) goto out;
			unsigned char *enc; size_t enc_len;
			if (mbt_be_value_encode(resp, &enc, &enc_len) < 0) goto out;
			dht_send(d, &from, enc, enc_len);
			free(enc);

		} else if (mbt_be_streq(q, "announce_peer")) {
			/* Store the announcing peer */
			struct mbt_be_value *ih_v = mbt_be_find(a, "info_hash");
			struct mbt_be_value *port_v = mbt_be_find(a, "port");
			struct mbt_be_value *tok_v = mbt_be_find(a, "token");
			if (!ih_v || ih_v->type != BE_STRING || ih_v->string.len != 20)
				goto out;
			if (!tok_v || tok_v->type != BE_STRING ||
			    !dht_verify_token(d, &from, tok_v->string.buf, tok_v->string.len)) {
				_cleanup_be_ struct mbt_be_value *err = NULL, *el = NULL, *ec = NULL, *em = NULL;
				if (mbt_be_dict_new(&err) < 0) goto out;
				if (mbt_be_dict_append_string(err, "t", t->string.buf, t->string.len) < 0) goto out;
				if (mbt_be_dict_append_string(err, "y", "e", 1) < 0) goto out;
				if (mbt_be_list_new(&el) < 0) goto out;
				if (mbt_be_integer_new(&ec, 203) < 0) goto out;
				if (mbt_be_string_new(&em, (const uint8_t *)"bad token", 9) < 0) goto out;
				mbt_be_list_append(el, ec);
				mbt_be_list_append(el, em);
				if (mbt_be_dict_append(err, "e", el) < 0) goto out;
				unsigned char *enc; size_t enc_len;
				if (mbt_be_value_encode(err, &enc, &enc_len) < 0) goto out;
				dht_send(d, &from, enc, enc_len);
				free(enc);
				goto out;
			}
			uint16_t from_port = (from.ss_family == AF_INET6) ?
				ntohs(((struct sockaddr_in6 *)&from)->sin6_port) :
				ntohs(((struct sockaddr_in *)&from)->sin_port);
			uint16_t aport = port_v ? (uint16_t)port_v->integer : from_port;
			/* Find or create storage slot */
			int slot = -1;
			for (int i = 0; i < d->stored_count; i++) {
				if (memcmp(d->stored[i].infohash, ih_v->string.buf, 20) == 0)
					{ slot = i; break; }
			}
			if (slot < 0 && d->stored_count < DHT_MAX_STORED) {
				slot = d->stored_count++;
				memcpy(d->stored[slot].infohash, ih_v->string.buf, 20);
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
			_cleanup_be_ struct mbt_be_value *resp = NULL, *rb = NULL;
			if (mbt_be_dict_new(&resp) < 0) goto out;
			if (mbt_be_dict_append_string(resp, "t", t->string.buf, t->string.len) < 0) goto out;
			if (mbt_be_dict_append_string(resp, "y", "r", 1) < 0) goto out;
			if (mbt_be_dict_new(&rb) < 0) goto out;
			if (mbt_be_dict_append_string(rb, "id", d->id, 20) < 0) goto out;
			if (mbt_be_dict_append(resp, "r", rb) < 0) goto out;
			unsigned char *enc; size_t enc_len;
			if (mbt_be_value_encode(resp, &enc, &enc_len) < 0) goto out;
			dht_send(d, &from, enc, enc_len);
			free(enc);
		} else if (mbt_be_streq(q, "sample_infohashes")) {
			/* BEP 51: return sample of stored infohashes */
			struct mbt_be_value *tgt = mbt_be_find(a, "target");
			if (!tgt || tgt->type != BE_STRING || tgt->string.len != 20)
				goto out;
			/* Build samples: up to 20 random infohashes */
			int nsamp = d->stored_count < 20 ? d->stored_count : 20;
			uint8_t samples[20 * 20];
			if (nsamp == d->stored_count) {
				for (int i = 0; i < nsamp; i++)
					memcpy(samples + i * 20, d->stored[i].infohash, 20);
			} else {
				/* Fisher-Yates partial shuffle */
				int idx[DHT_MAX_STORED];
				for (int i = 0; i < d->stored_count; i++) idx[i] = i;
				for (int i = 0; i < nsamp; i++) {
					int j = i + rand() % (d->stored_count - i);
					int tmp = idx[i]; idx[i] = idx[j]; idx[j] = tmp;
					memcpy(samples + i * 20, d->stored[idx[i]].infohash, 20);
				}
			}
			/* Closest nodes to target */
			struct dht_node closest[K];
			int nclosest = routing_closest(d, tgt->string.buf, closest, K);
			uint8_t compact[K * 26];
			int clen = 0;
			for (int i = 0; i < nclosest; i++) {
				struct sockaddr_in *sin = (struct sockaddr_in *)&closest[i].addr;
				if (sin->sin_family != AF_INET) continue;
				memcpy(compact + clen, closest[i].id, 20);
				memcpy(compact + clen + 20, &sin->sin_addr, 4);
				memcpy(compact + clen + 24, &sin->sin_port, 2);
				clen += 26;
			}
			_cleanup_be_ struct mbt_be_value *resp = NULL, *rb = NULL;
			if (mbt_be_dict_new(&resp) < 0) goto out;
			if (mbt_be_dict_append_string(resp, "t", t->string.buf, t->string.len) < 0) goto out;
			if (mbt_be_dict_append_string(resp, "y", "r", 1) < 0) goto out;
			if (mbt_be_dict_new(&rb) < 0) goto out;
			if (mbt_be_dict_append_string(rb, "id", d->id, 20) < 0) goto out;
			if (mbt_be_dict_append_number(rb, "interval", 3600) < 0) goto out;
			if (clen > 0)
				if (mbt_be_dict_append_string(rb, "nodes", compact, clen) < 0) goto out;
			if (mbt_be_dict_append_number(rb, "num", d->stored_count) < 0) goto out;
			if (mbt_be_dict_append_string(rb, "samples", samples, nsamp * 20) < 0) goto out;
			if (mbt_be_dict_append(resp, "r", rb) < 0) goto out;
			unsigned char *enc; size_t enc_len;
			if (mbt_be_value_encode(resp, &enc, &enc_len) < 0) goto out;
			dht_send(d, &from, enc, enc_len);
			free(enc);
		} else if (mbt_be_streq(q, "get")) {
			/* BEP 44: get */
			struct mbt_be_value *tgt = mbt_be_find(a, "target");
			if (!tgt || tgt->type != BE_STRING || tgt->string.len != 20)
				goto out;
			struct dht_item *it = dht_item_find(d, tgt->string.buf);
			_cleanup_be_ struct mbt_be_value *resp = NULL, *rb = NULL;
			if (mbt_be_dict_new(&resp) < 0) goto out;
			if (mbt_be_dict_append_string(resp, "t", t->string.buf, t->string.len) < 0) goto out;
			if (mbt_be_dict_append_string(resp, "y", "r", 1) < 0) goto out;
			if (mbt_be_dict_new(&rb) < 0) goto out;
			if (mbt_be_dict_append_string(rb, "id", d->id, 20) < 0) goto out;
			uint8_t tok[4]; make_txn(d, tok);
			if (mbt_be_dict_append_string(rb, "token", tok, 4) < 0) goto out;
			struct dht_node closest[K];
			int nc2 = routing_closest(d, tgt->string.buf, closest, K);
			uint8_t cn[K * NODE_COMPACT_LEN]; int cl = 0;
			for (int j = 0; j < nc2; j++) {
				if (closest[j].addr.ss_family != AF_INET) continue;
				memcpy(cn + cl, closest[j].id, 20);
				struct sockaddr_in *s4 = (struct sockaddr_in *)&closest[j].addr;
				memcpy(cn + cl + 20, &s4->sin_addr, 4);
				memcpy(cn + cl + 24, &s4->sin_port, 2);
				cl += 26;
			}
			if (cl > 0)
				if (mbt_be_dict_append_string(rb, "nodes", cn, cl) < 0) goto out;
			if (it) {
				if (mbt_be_dict_append_string(rb, "v", it->v, it->vlen) < 0) goto out;
				if (it->mutable) {
					if (mbt_be_dict_append_string(rb, "k", it->k, MBT_DHT_PUBKEY_LEN) < 0) goto out;
					if (mbt_be_dict_append_string(rb, "sig", it->sig, MBT_DHT_SIG_LEN) < 0) goto out;
					if (mbt_be_dict_append_number(rb, "seq", it->seq) < 0) goto out;
				}
			}
			if (mbt_be_dict_append(resp, "r", rb) < 0) goto out;
			unsigned char *enc; size_t enc_len;
			if (mbt_be_value_encode(resp, &enc, &enc_len) < 0) goto out;
			dht_send(d, &from, enc, enc_len);
			free(enc);
		} else if (mbt_be_streq(q, "put")) {
			/* BEP 44: put */
			struct mbt_be_value *v_val = mbt_be_find(a, "v");
			if (!v_val) goto out;
			unsigned char *v_enc = NULL; size_t v_enc_len = 0;
			if (mbt_be_value_encode(v_val, &v_enc, &v_enc_len) < 0) goto out;
			if (v_enc_len > MBT_DHT_MAX_VALUE) { free(v_enc); goto out; }
			struct mbt_be_value *k_val = mbt_be_find(a, "k");
			if (k_val && k_val->type == BE_STRING &&
			    k_val->string.len == MBT_DHT_PUBKEY_LEN) {
				struct mbt_be_value *sig_val = mbt_be_find(a, "sig");
				struct mbt_be_value *seq_val = mbt_be_find(a, "seq");
				struct mbt_be_value *salt_val = mbt_be_find(a, "salt");
				if (!sig_val || sig_val->string.len != MBT_DHT_SIG_LEN ||
				    !seq_val || seq_val->type != BE_INTEGER) {
					free(v_enc); goto out;
				}
				const char *salt = NULL; size_t slen = 0;
				if (salt_val && salt_val->type == BE_STRING) {
					salt = (const char *)salt_val->string.buf; slen = salt_val->string.len;
				}
				mbt_dht_put_mutable(d, k_val->string.buf,
				    sig_val->string.buf, seq_val->integer,
				    salt, slen, v_enc, v_enc_len, NULL, NULL);
			} else {
				mbt_dht_put_immutable(d, v_enc, v_enc_len, NULL, NULL);
			}
			free(v_enc);
			/* Respond */
			_cleanup_be_ struct mbt_be_value *resp = NULL, *rb = NULL;
			if (mbt_be_dict_new(&resp) < 0) goto out;
			if (mbt_be_dict_append_string(resp, "t", t->string.buf, t->string.len) < 0) goto out;
			if (mbt_be_dict_append_string(resp, "y", "r", 1) < 0) goto out;
			if (mbt_be_dict_new(&rb) < 0) goto out;
			if (mbt_be_dict_append_string(rb, "id", d->id, 20) < 0) goto out;
			if (mbt_be_dict_append(resp, "r", rb) < 0) goto out;
			unsigned char *enc; size_t enc_len;
			if (mbt_be_value_encode(resp, &enc, &enc_len) < 0) goto out;
			dht_send(d, &from, enc, enc_len);
			free(enc);
		}
	}
	/* Ignore errors ('e') */

out:
	mbt_be_unref(msg);
}

static int
dht_mux_handler(const void *buf, size_t len,
                const struct sockaddr *src, socklen_t srclen, void *arg)
{
	struct mbt_dht *d = arg;
	struct sockaddr_storage from;
	memcpy(&from, src, srclen < sizeof(from) ? srclen : sizeof(from));
	dht_handle(d, buf, (ssize_t)len, &from);
	return 1; /* consumed */
}

/* --- Refresh timer --- */

static void
dht_refresh_cb(struct mbt_ev_timer *timer, void *v)
{
	struct mbt_dht *d = v;
	int total = mbt_dht_node_count(d);

	/* Evict stale nodes (no activity in 5 min) */
	time_t now = time(NULL);
	for (struct dht_bucket *b = d->buckets; b; b = b->next) {
		struct dht_node *n, *tmp;
		SLIST_FOREACH_SAFE(n, &b->nodes, entry, tmp) {
			if (now - n->last_seen > 300) {
				SLIST_REMOVE(&b->nodes, n, dht_node, entry);
				b->count--;
				free(n);
			}
		}
	}

	/* Send find_node to up to 8 known nodes to expand routing table */
	int sent = 0;
	for (struct dht_bucket *b = d->buckets; b && sent < 8; b = b->next) {
		struct dht_node *n;
		SLIST_FOREACH(n, &b->nodes, entry) {
			if (sent >= 8) break;
			dht_send_find_node(d, &n->addr, d->id);
			sent++;
		}
	}

	/* Ping a random node to keep routing table alive */
	if (total > 0) {
		int pick = rand() % total;
		int idx = 0;
		for (struct dht_bucket *b = d->buckets; b; b = b->next) {
			struct dht_node *n;
			SLIST_FOREACH(n, &b->nodes, entry) {
				if (idx++ == pick) {
					dht_send_ping(d, &n->addr);
					goto pinged;
				}
			}
		}
		pinged:;
	}

	/* Reschedule: aggressive (10s) while small, normal (15min) when populated */
	uint64_t next = (total < 20) ? 10 * 1000000ULL : REFRESH_US;
	mbt_ev_timer_mod(timer, next);
}

/* --- Public API --- */

int
mbt_dht_new(struct mbt_dht **out, struct mbt_ev *ev, uint16_t port)
{
	/* Standalone: own a private mux on this port. */
	struct mbt_udp_mux *mux;
	int rv = mbt_udp_mux_new(&mux, ev, port);
	if (rv < 0)
		return rv;
	rv = mbt_dht_new_mux(out, ev, mux, mbt_udp_mux_port(mux));
	if (rv < 0) {
		mbt_udp_mux_free(mux);
		return rv;
	}
	(*out)->owns_mux = 1;
	return 0;
}

int
mbt_dht_new_mux(struct mbt_dht **out, struct mbt_ev *ev,
                struct mbt_udp_mux *mux, uint16_t port)
{
	struct mbt_dht *d = calloc(1, sizeof(*d));
	if (!d) return -ENOMEM;

	d->ev = ev;
	d->port = port;
	srand(time(NULL) ^ getpid());

	/* Initialize routing table with one bucket covering all IDs */
	d->buckets = calloc(1, sizeof(struct dht_bucket));
	if (!d->buckets) { free(d); return -ENOMEM; }
	SLIST_INIT(&d->buckets->nodes);
	d->bucket_count = 1;

	/* Derive stable node ID from machine-id, or fall back to random */
	dht_init_node_id(d->id);

	/* Use the shared mux for UDP (uTP may share this socket). */
	d->mux = mux;
	d->owns_mux = 0;
	mbt_udp_mux_set_dht(mux, dht_mux_handler, d);

	/* Refresh timer — first fire at 5s to populate routing table quickly */
	mbt_ev_timer_new(ev, &d->refresh_timer, 5 * 1000000ULL, dht_refresh_cb, d);

	/* Token secrets */
	random_bytes(d->token_secret, TOKEN_SECRET_LEN);
	random_bytes(d->token_secret_prev, TOKEN_SECRET_LEN);
	mbt_ev_timer_new(ev, &d->token_timer, TOKEN_ROTATE_US, dht_token_rotate_cb, d);

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
	if (d->token_timer) mbt_ev_timer_free(d->token_timer);
	for (int i = 0; i < DHT_MAX_ITEMS; i++)
		free(d->items[i].v);
	/* Free routing table */
	struct dht_bucket *b = d->buckets;
	while (b) {
		struct dht_node *n;
		while ((n = SLIST_FIRST(&b->nodes)) != NULL) {
			SLIST_REMOVE_HEAD(&b->nodes, entry);
			free(n);
		}
		struct dht_bucket *next = b->next;
		free(b);
		b = next;
	}
	mbt_ratelimit_free(d->send_limit);
	if (d->mux) {
		mbt_udp_mux_set_dht(d->mux, NULL, NULL);
		if (d->owns_mux)
			mbt_udp_mux_free(d->mux);
	}
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
	mbt_log_debug(MBT_LOG_DHT, "get_peers lookup for %s (%d seed nodes)", hex, n);

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
	for (struct dht_bucket *b = d->buckets; b; b = b->next)
		total += b->count;
	return total;
}

uint16_t
mbt_dht_port(struct mbt_dht *d)
{
	return mbt_udp_mux_port(d->mux);
}

void
mbt_dht_set_node_id(struct mbt_dht *d, const uint8_t id[20])
{
	memcpy(d->id, id, 20);
}

/* --- BEP 44 --- */

static struct dht_item *
dht_item_find(struct mbt_dht *d, const uint8_t *target)
{
	for (int i = 0; i < d->item_count; i++)
		if (memcmp(d->items[i].target, target, 20) == 0)
			return &d->items[i];
	return NULL;
}

static struct dht_item *
dht_item_alloc(struct mbt_dht *d, const uint8_t *target)
{
	time_t now = time(NULL);
	for (int i = 0; i < d->item_count; ) {
		if (now - d->items[i].stored_at > DHT_ITEM_EXPIRE) {
			free(d->items[i].v);
			free(d->items[i].salt);
			d->items[i] = d->items[--d->item_count];
		} else {
			i++;
		}
	}
	if (d->item_count >= DHT_MAX_ITEMS) return NULL;
	struct dht_item *it = &d->items[d->item_count++];
	memset(it, 0, sizeof(*it));
	memcpy(it->target, target, 20);
	it->stored_at = now;
	return it;
}

void
mbt_dht_set_verify(struct mbt_dht *d, mbt_dht_verify_cb cb, void *arg)
{
	d->verify_cb = cb;
	d->verify_arg = arg;
}

int
mbt_dht_put_immutable(struct mbt_dht *d, const void *v, size_t vlen,
                      mbt_dht_done_cb done_cb, void *arg)
{
	if (vlen > MBT_DHT_MAX_VALUE) return -EINVAL;
	uint8_t target[20];
	SHA1_CTX ctx;
	SHA1Init(&ctx);
	SHA1Update(&ctx, v, vlen);
	SHA1Final(target, &ctx);

	struct dht_item *it = dht_item_find(d, target);
	if (!it) {
		it = dht_item_alloc(d, target);
		if (!it) return -ENOMEM;
		it->v = malloc(vlen);
		if (!it->v) { d->item_count--; return -ENOMEM; }
		memcpy(it->v, v, vlen);
		it->vlen = vlen;
		it->mutable = 0;
		it->seq = -1;
	}
	(void)done_cb; (void)arg;
	return 0;
}

int
mbt_dht_get_immutable(struct mbt_dht *d, const uint8_t target[20],
                      mbt_dht_item_cb cb, void *arg)
{
	struct dht_item *it = dht_item_find(d, target);
	if (it) {
		if (cb) cb(it->v, it->vlen, -1, NULL, arg);
		return 0;
	}
	return -ENOENT;
}

int
mbt_dht_put_mutable(struct mbt_dht *d,
                    const uint8_t k[MBT_DHT_PUBKEY_LEN],
                    const uint8_t sig[MBT_DHT_SIG_LEN],
                    int64_t seq,
                    const char *salt, size_t salt_len,
                    const void *v, size_t vlen,
                    mbt_dht_done_cb done_cb, void *arg)
{
	if (vlen > MBT_DHT_MAX_VALUE) return -EINVAL;
	if (salt_len > 64) return -EINVAL;

	uint8_t target[20];
	SHA1_CTX ctx;
	SHA1Init(&ctx);
	SHA1Update(&ctx, k, MBT_DHT_PUBKEY_LEN);
	if (salt && salt_len > 0)
		SHA1Update(&ctx, (const uint8_t *)salt, salt_len);
	SHA1Final(target, &ctx);

	if (d->verify_cb) {
		uint8_t buf[1200];
		int off = 0;
		if (salt && salt_len > 0)
			off += snprintf((char *)buf + off, sizeof(buf) - off,
			                "4:salt%zu:%.*s", salt_len, (int)salt_len, salt);
		off += snprintf((char *)buf + off, sizeof(buf) - off, "3:seqi%lde1:v",
		                (long)seq);
		if ((size_t)off + vlen > sizeof(buf)) return -EINVAL;
		memcpy(buf + off, v, vlen);
		off += vlen;
		if (!d->verify_cb(k, sig, buf, off, d->verify_arg))
			return -EACCES;
	} else {
		return -ENOSYS;
	}

	struct dht_item *it = dht_item_find(d, target);
	if (it) {
		if (seq <= it->seq) return -EEXIST;
		free(it->v);
	} else {
		it = dht_item_alloc(d, target);
		if (!it) return -ENOMEM;
	}
	it->v = malloc(vlen);
	if (!it->v) { d->item_count--; return -ENOMEM; }
	memcpy(it->v, v, vlen);
	it->vlen = vlen;
	memcpy(it->k, k, MBT_DHT_PUBKEY_LEN);
	memcpy(it->sig, sig, MBT_DHT_SIG_LEN);
	it->seq = seq;
	it->mutable = 1;
	free(it->salt);
	it->salt = (salt && salt_len > 0) ? strndup(salt, salt_len) : NULL;
	it->salt_len = salt_len;
	it->stored_at = time(NULL);
	(void)done_cb; (void)arg;
	return 0;
}

int
mbt_dht_get_mutable(struct mbt_dht *d,
                    const uint8_t k[MBT_DHT_PUBKEY_LEN],
                    const char *salt, size_t salt_len,
                    mbt_dht_item_cb cb, void *arg)
{
	uint8_t target[20];
	SHA1_CTX ctx;
	SHA1Init(&ctx);
	SHA1Update(&ctx, k, MBT_DHT_PUBKEY_LEN);
	if (salt && salt_len > 0)
		SHA1Update(&ctx, (const uint8_t *)salt, salt_len);
	SHA1Final(target, &ctx);

	struct dht_item *it = dht_item_find(d, target);
	if (it) {
		if (cb) cb(it->v, it->vlen, it->seq, it->k, arg);
		return 0;
	}
	return -ENOENT;
}
