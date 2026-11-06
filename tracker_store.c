// SPDX-License-Identifier: ISC
/*
 * tracker_store.c — shared peer storage for tracker servers (IPv4 + IPv6)
 */
#include <sys/queue.h>
#include "tracker_store.h"
#include "mbt/compat.h"
#include <errno.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>
#include <sys/socket.h>
#include <netinet/in.h>
#include <sha1.h>
#include <stdint.h>

#include "mbt/log.h"
#include "mbt/tracker_server.h"
#include "banned.h"

#define MAX_PEERS_PER_TORRENT 200
#define PEERS_MIN 4
#define DEFAULT_MAX_TORRENTS 4096
#define DEFAULT_INTERVAL_SECS 900
#define PEER_EXPIRE_SECS (30 * 60) /* 30 minutes */
#define GC_DIVISOR 6
#define GC_MAX_SECS 300
#define GC_SLICES 64               /* one timer tick walks this share of the table */
#define BUCKETS_MIN 256            /* a power of two */
#define FNV_PRIME 0x100000001b3ULL
#define CID_SLOT_SECS 120

struct ts_peer {
	union {
		struct sockaddr sa;
		struct sockaddr_in in;
		struct sockaddr_in6 in6;
	} addr;
	time_t last_seen;
	int is_seeder;
};

struct ts_torrent {
	SLIST_ENTRY(ts_torrent) entry;
	uint8_t infohash[20];
	struct ts_peer *peers;
	int peer_count;
	int peer_cap;
	int seeders;
	int leechers;
	uint32_t downloaded; /* leechers seen to finish */
};

SLIST_HEAD(ts_bucket, ts_torrent);

struct mbt_tracker_store {
	struct ts_bucket *buckets;
	size_t bucket_count;       /* a power of two */
	size_t gc_next;
	uint64_t hash_seed;
	int num_torrents;
	int num_peers;
	uint32_t num_seeders;
	uint32_t num_leechers;
	int max_torrents;
	int interval_secs;
	int expire_secs;
	uint8_t secret[16]; /* for connection_id generation */
};

/* --- Lifecycle --- */

int
mbt_tracker_store_new(struct mbt_tracker_store **out)
{
	struct mbt_tracker_store *s = calloc(1, sizeof(*s));
	if (!s) return -ENOMEM;
	s->bucket_count = BUCKETS_MIN;
	s->buckets = calloc(s->bucket_count, sizeof(*s->buckets));
	if (!s->buckets) {
		free(s);
		return -ENOMEM;
	}
	s->max_torrents = DEFAULT_MAX_TORRENTS;
	s->interval_secs = DEFAULT_INTERVAL_SECS;
	s->expire_secs = PEER_EXPIRE_SECS;
	/* Random secret for connection_id HMAC */
	arc4random_buf(s->secret, sizeof(s->secret));
	arc4random_buf(&s->hash_seed, sizeof(s->hash_seed));
	*out = s;
	return 0;
}

void
mbt_tracker_store_free(struct mbt_tracker_store *s)
{
	if (!s) return;
	for (size_t i = 0; i < s->bucket_count; i++) {
		while (!SLIST_EMPTY(&s->buckets[i])) {
			struct ts_torrent *t = SLIST_FIRST(&s->buckets[i]);
			SLIST_REMOVE_HEAD(&s->buckets[i], entry);
			free(t->peers);
			free(t);
		}
	}
	free(s->buckets);
	free(s);
}

int mbt_tracker_store_num_torrents(struct mbt_tracker_store *s) { return s->num_torrents; }
int mbt_tracker_store_num_peers(struct mbt_tracker_store *s) { return s->num_peers; }
int mbt_tracker_store_interval(const struct mbt_tracker_store *s) { return s->interval_secs; }

void
mbt_tracker_store_set_expire(struct mbt_tracker_store *s, int secs)
{
	if (secs > 0)
		s->expire_secs = secs;
}

void
mbt_tracker_store_set_interval(struct mbt_tracker_store *s, int secs)
{
	if (secs <= 0)
		return;
	s->interval_secs = secs;
	if (s->expire_secs < secs * 2)
		s->expire_secs = secs * 2;
}

void
mbt_tracker_store_set_max_torrents(struct mbt_tracker_store *s, int max)
{
	if (max > 0)
		s->max_torrents = max;
}

/* A server's timer runs one gc step this often. */
uint64_t
mbt_tracker_store_gc_usec(const struct mbt_tracker_store *s)
{
	int secs = s->expire_secs / GC_DIVISOR;

	if (secs < 1)
		secs = 1;
	if (secs > GC_MAX_SECS)
		secs = GC_MAX_SECS;
	return (uint64_t)secs * 1000000ULL / GC_SLICES;
}

void
mbt_tracker_store_stats(struct mbt_tracker_store *s, struct mbt_tracker_stats *out)
{
	out->torrents = s->num_torrents;
	out->peers = s->num_peers;
	out->seeders = s->num_seeders;
	out->leechers = s->num_leechers;
}

/* --- Connection ID (BEP 15 spoofing protection) --- */

static uint64_t
connection_id_at(struct mbt_tracker_store *s, const struct sockaddr_storage *addr,
                 uint32_t time_slot)
{
	SHA1_CTX ctx;
	uint8_t digest[20];

	/* Normalize: hash only IP bytes + port (handles IPv4-mapped IPv6) */
	uint8_t norm[18]; /* max: 16 bytes IPv6 + 2 bytes port */
	int norm_len = 0;

	if (addr->ss_family == AF_INET) {
		struct sockaddr_in *a4 = (struct sockaddr_in *)addr;
		memcpy(norm, &a4->sin_addr, 4);
		memcpy(norm + 4, &a4->sin_port, 2);
		norm_len = 6;
	} else if (addr->ss_family == AF_INET6) {
		struct sockaddr_in6 *a6 = (struct sockaddr_in6 *)addr;
		/* Check for IPv4-mapped (::ffff:x.x.x.x) */
		if (IN6_IS_ADDR_V4MAPPED(&a6->sin6_addr)) {
			memcpy(norm, &a6->sin6_addr.s6_addr[12], 4);
			memcpy(norm + 4, &a6->sin6_port, 2);
			norm_len = 6;
		} else {
			memcpy(norm, &a6->sin6_addr, 16);
			memcpy(norm + 16, &a6->sin6_port, 2);
			norm_len = 18;
		}
	}

	SHA1Init(&ctx);
	SHA1Update(&ctx, s->secret, 16);
	SHA1Update(&ctx, (uint8_t *)&time_slot, 4);
	SHA1Update(&ctx, norm, norm_len);
	SHA1Final(digest, &ctx);

	uint64_t cid;
	memcpy(&cid, digest, 8);
	return cid;
}

uint64_t
mbt_tracker_store_make_connection_id(struct mbt_tracker_store *s,
                                     const struct sockaddr_storage *addr)
{
	return connection_id_at(s, addr, (uint32_t)(time(NULL) / CID_SLOT_SECS));
}

/* A client can use an id for 2 minutes, so also accept the slot before. */
int
mbt_tracker_store_validate_connection_id(struct mbt_tracker_store *s,
                                         const struct sockaddr_storage *addr,
                                         uint64_t cid)
{
	uint32_t slot = (uint32_t)(time(NULL) / CID_SLOT_SECS);

	return cid == connection_id_at(s, addr, slot) ||
	       cid == connection_id_at(s, addr, slot - 1);
}

/* --- Torrent/peer management --- */

/* Keyed, because clients choose the infohash. */
static size_t
bucket_of(const struct mbt_tracker_store *s, const uint8_t *ih)
{
	uint64_t h = s->hash_seed;

	for (int i = 0; i < 20; i++)
		h = (h ^ ih[i]) * FNV_PRIME;
	return (size_t)(h ^ (h >> 32)) & (s->bucket_count - 1);
}

struct ts_torrent *
mbt_tracker_store_find(struct mbt_tracker_store *s, const uint8_t *ih)
{
	struct ts_torrent *t;

	SLIST_FOREACH(t, &s->buckets[bucket_of(s, ih)], entry)
		if (memcmp(t->infohash, ih, 20) == 0) return t;
	return NULL;
}

/* A failed allocation leaves the table as it was, only with longer chains. */
static void
buckets_grow(struct mbt_tracker_store *s)
{
	size_t count = s->bucket_count * 2;
	struct ts_bucket *old = s->buckets;
	size_t old_count = s->bucket_count;
	struct ts_bucket *grown = calloc(count, sizeof(*grown));

	if (!grown)
		return;
	s->buckets = grown;
	s->bucket_count = count;
	for (size_t i = 0; i < old_count; i++) {
		struct ts_torrent *t;

		while ((t = SLIST_FIRST(&old[i])) != NULL) {
			SLIST_REMOVE_HEAD(&old[i], entry);
			SLIST_INSERT_HEAD(&grown[bucket_of(s, t->infohash)], t, entry);
		}
	}
	free(old);
	s->gc_next &= count - 1;
}

struct ts_torrent *
mbt_tracker_store_get_or_create(struct mbt_tracker_store *s, const uint8_t *ih)
{
	struct ts_torrent *t = mbt_tracker_store_find(s, ih);
	if (t) return t;
	if (s->num_torrents >= s->max_torrents) {
		mbt_log_warn(MBT_LOG_TRACKER, "tracker: torrent limit reached (%d)", s->max_torrents);
		return NULL;
	}
	t = calloc(1, sizeof(*t));
	if (!t) return NULL;
	memcpy(t->infohash, ih, 20);
	if ((size_t)s->num_torrents >= s->bucket_count)
		buckets_grow(s);
	SLIST_INSERT_HEAD(&s->buckets[bucket_of(s, ih)], t, entry);
	s->num_torrents++;
	return t;
}

static int
addr_equal(const struct ts_peer *p, const struct sockaddr_storage *b)
{
	if (p->addr.sa.sa_family != b->ss_family) return 0;
	if (b->ss_family == AF_INET) {
		const struct sockaddr_in *b4 = (const struct sockaddr_in *)b;
		return p->addr.in.sin_addr.s_addr == b4->sin_addr.s_addr &&
		       p->addr.in.sin_port == b4->sin_port;
	}
	if (b->ss_family == AF_INET6) {
		const struct sockaddr_in6 *b6 = (const struct sockaddr_in6 *)b;
		return memcmp(&p->addr.in6.sin6_addr, &b6->sin6_addr, 16) == 0 &&
		       p->addr.in6.sin6_port == b6->sin6_port;
	}
	return 0;
}

/* Returns 0, or -1 if t cannot take another peer. */
static int
peers_reserve(struct ts_torrent *t)
{
	struct ts_peer *grown;
	int cap;

	if (t->peer_count < t->peer_cap)
		return 0;
	if (t->peer_cap >= MAX_PEERS_PER_TORRENT)
		return -1;
	cap = t->peer_cap ? t->peer_cap * 2 : PEERS_MIN;
	if (cap > MAX_PEERS_PER_TORRENT)
		cap = MAX_PEERS_PER_TORRENT;
	if ((grown = reallocarray(t->peers, (size_t)cap, sizeof(*grown))) == NULL)
		return -1;
	t->peers = grown;
	t->peer_cap = cap;
	return 0;
}

static void
peer_count_add(struct mbt_tracker_store *s, struct ts_torrent *t, int is_seeder, int delta)
{
	if (is_seeder) {
		t->seeders += delta;
		s->num_seeders += (uint32_t)delta;
	} else {
		t->leechers += delta;
		s->num_leechers += (uint32_t)delta;
	}
	s->num_peers += delta;
}

void
mbt_tracker_store_add_peer(struct mbt_tracker_store *s, struct ts_torrent *t,
                           const struct sockaddr_storage *addr, int is_seeder)
{
	struct ts_peer *p;

	for (int i = 0; i < t->peer_count; i++) {
		p = &t->peers[i];
		if (!addr_equal(p, addr))
			continue;
		p->last_seen = time(NULL);
		if (is_seeder && !p->is_seeder) {
			p->is_seeder = 1;
			peer_count_add(s, t, 0, -1);
			peer_count_add(s, t, 1, 1);
			t->downloaded++;
		}
		return;
	}
	if (peers_reserve(t) < 0)
		return;
	p = &t->peers[t->peer_count++];
	memcpy(&p->addr, addr, sizeof(p->addr));
	p->last_seen = time(NULL);
	p->is_seeder = is_seeder;
	peer_count_add(s, t, is_seeder, 1);
}

/* A peer sent event=stopped. */
void
mbt_tracker_store_remove_peer(struct mbt_tracker_store *s, struct ts_torrent *t,
                              const struct sockaddr_storage *addr)
{
	for (int i = 0; i < t->peer_count; i++) {
		if (!addr_equal(&t->peers[i], addr))
			continue;
		peer_count_add(s, t, t->peers[i].is_seeder, -1);
		t->peers[i] = t->peers[--t->peer_count];
		return;
	}
}

int
mbt_tracker_store_get_peers_v4(struct ts_torrent *t, const struct sockaddr_storage *exclude,
                               uint8_t *compact_out, int max_peers)
{
	int sent = 0;
	for (int i = 0; i < t->peer_count && sent < max_peers; i++) {
		if (t->peers[i].addr.sa.sa_family != AF_INET) continue;
		if (exclude && addr_equal(&t->peers[i], exclude)) continue;
		const struct sockaddr_in *a = &t->peers[i].addr.in;
		memcpy(compact_out + sent * 6, &a->sin_addr, 4);
		memcpy(compact_out + sent * 6 + 4, &a->sin_port, 2);
		sent++;
	}
	return sent;
}

int
mbt_tracker_store_get_peers_v6(struct ts_torrent *t, const struct sockaddr_storage *exclude,
                               uint8_t *compact_out, int max_peers)
{
	int sent = 0;
	for (int i = 0; i < t->peer_count && sent < max_peers; i++) {
		if (t->peers[i].addr.sa.sa_family != AF_INET6) continue;
		if (exclude && addr_equal(&t->peers[i], exclude)) continue;
		const struct sockaddr_in6 *a = &t->peers[i].addr.in6;
		memcpy(compact_out + sent * 18, &a->sin6_addr, 16);
		memcpy(compact_out + sent * 18 + 16, &a->sin6_port, 2);
		sent++;
	}
	return sent;
}

/* Legacy compat wrapper */
int
mbt_tracker_store_get_peers(struct ts_torrent *t, const struct sockaddr_in *exclude,
                            uint8_t *compact_out, int max_peers)
{
	struct sockaddr_storage ss = {0};
	if (exclude) { memcpy(&ss, exclude, sizeof(*exclude)); ss.ss_family = AF_INET; }
	return mbt_tracker_store_get_peers_v4(t, exclude ? &ss : NULL, compact_out, max_peers);
}

int mbt_tracker_store_torrent_seeders(struct ts_torrent *t) { return t->seeders; }
int mbt_tracker_store_torrent_leechers(struct ts_torrent *t) { return t->leechers; }
int mbt_tracker_store_torrent_peer_count(struct ts_torrent *t) { return t->peer_count; }
uint32_t mbt_tracker_store_torrent_downloaded(struct ts_torrent *t) { return t->downloaded; }

static void
gc_bucket(struct mbt_tracker_store *s, struct ts_bucket *b, time_t now)
{
	struct ts_torrent *t, *tmp;

	SLIST_FOREACH_SAFE(t, b, entry, tmp) {
		int j = 0;

		while (j < t->peer_count) {
			if (now - t->peers[j].last_seen > s->expire_secs) {
				peer_count_add(s, t, t->peers[j].is_seeder, -1);
				t->peers[j] = t->peers[--t->peer_count];
			} else {
				j++;
			}
		}
		if (t->peer_count == 0) {
			SLIST_REMOVE(b, t, ts_torrent, entry);
			s->num_torrents--;
			free(t->peers);
			free(t);
		}
	}
}

void
mbt_tracker_store_gc(struct mbt_tracker_store *s)
{
	time_t now = time(NULL);

	for (size_t i = 0; i < s->bucket_count; i++)
		gc_bucket(s, &s->buckets[i], now);
}

/* One slice of the table, so a large store does not hold the event loop. */
void
mbt_tracker_store_gc_step(struct mbt_tracker_store *s)
{
	time_t now = time(NULL);
	size_t count = s->bucket_count / GC_SLICES + 1;

	for (size_t i = 0; i < count; i++) {
		gc_bucket(s, &s->buckets[s->gc_next], now);
		s->gc_next = (s->gc_next + 1) & (s->bucket_count - 1);
	}
}
