// SPDX-License-Identifier: ISC
#include <stdlib.h>
#include <string.h>
#include <time.h>

#include "mbt/peerdb.h"

static uint64_t
peerdb_now(void)
{
	struct timespec ts;
	clock_gettime(CLOCK_MONOTONIC, &ts);
	return (uint64_t)ts.tv_sec * 1000000 + ts.tv_nsec / 1000;
}

static int
peer_cmp(struct mbt_peer_entry *a, struct mbt_peer_entry *b)
{
	int rv = strcmp(a->addr, b->addr);
	if (rv != 0)
		return rv;
	return (int)a->port - (int)b->port;
}

RB_GENERATE(mbt_peerdb, mbt_peer_entry, entry, peer_cmp)

void
mbt_peerdb_init(struct mbt_peerdb *db)
{
	RB_INIT(db);
}

struct mbt_peer_entry *
mbt_peerdb_insert(struct mbt_peerdb *db, const char *addr, uint16_t port, int source)
{
	struct mbt_peer_entry key;
	snprintf(key.addr, sizeof(key.addr), "%s", addr);
	key.port = port;

	struct mbt_peer_entry *existing = RB_FIND(mbt_peerdb, db, &key);
	if (existing) {
		existing->sources |= source;
		existing->last_seen = peerdb_now();
		return existing;
	}

	struct mbt_peer_entry *e = calloc(1, sizeof(*e));
	if (!e)
		return NULL;

	snprintf(e->addr, sizeof(e->addr), "%s", addr);
	e->port = port;
	e->state = MBT_PEERDB_KNOWN;
	e->sources = source;
	e->last_seen = peerdb_now();

	RB_INSERT(mbt_peerdb, db, e);
	return e;
}

struct mbt_peer_entry *
mbt_peerdb_find(struct mbt_peerdb *db, const char *addr, uint16_t port)
{
	struct mbt_peer_entry key;
	snprintf(key.addr, sizeof(key.addr), "%s", addr);
	key.port = port;
	return RB_FIND(mbt_peerdb, db, &key);
}

struct mbt_peer_entry *
mbt_peerdb_select(struct mbt_peerdb *db)
{
	struct mbt_peer_entry *best = NULL;
	struct mbt_peer_entry *e;
	uint64_t now = peerdb_now();

	RB_FOREACH(e, mbt_peerdb, db) {
		if (e->state != MBT_PEERDB_KNOWN && e->state != MBT_PEERDB_FAILED)
			continue;

		/* Don't retry banned peers */
		if (e->state == MBT_PEERDB_BANNED)
			continue;

		/* Don't retry too quickly (backoff: 30s after failure) */
		if (e->state == MBT_PEERDB_FAILED && e->last_attempt > 0 &&
		    now - e->last_attempt < 30 * 1000000ULL)
			continue;

		/* Don't retry too many failures */
		if (e->failures >= 3)
			continue;

		/* Prefer most recently seen */
		if (!best || e->last_seen > best->last_seen)
			best = e;
	}

	if (best)
		best->last_attempt = now;

	return best;
}

int
mbt_peerdb_count(struct mbt_peerdb *db, enum mbt_peerdb_state state)
{
	int n = 0;
	struct mbt_peer_entry *e;
	RB_FOREACH(e, mbt_peerdb, db)
		if (e->state == state)
			n++;
	return n;
}

int
mbt_peerdb_total(struct mbt_peerdb *db)
{
	int n = 0;
	struct mbt_peer_entry *e;
	RB_FOREACH(e, mbt_peerdb, db)
		n++;
	return n;
}

void
mbt_peerdb_free(struct mbt_peerdb *db)
{
	struct mbt_peer_entry *e, *next;
	RB_FOREACH_SAFE(e, mbt_peerdb, db, next) {
		RB_REMOVE(mbt_peerdb, db, e);
		free(e);
	}
}
