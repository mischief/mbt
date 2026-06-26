// SPDX-License-Identifier: ISC
#include <stdlib.h>
#include <stdio.h>
#include <string.h>
#include <time.h>
#include <stdint.h>

#include "mbt/peerdb.h"

#define PEERDB_INIT_CAP 64
#define PEERDB_LOAD_NUM 3
#define PEERDB_LOAD_DEN 4  /* grow at 75% load */

static uint64_t
peerdb_now(void)
{
	struct timespec ts;
	clock_gettime(CLOCK_MONOTONIC, &ts);
	return (uint64_t)ts.tv_sec * 1000000 + ts.tv_nsec / 1000;
}

static uint32_t
peer_hash(const char *addr, uint16_t port)
{
	/* FNV-1a */
	uint32_t h = 2166136261u;
	for (const unsigned char *p = (const unsigned char *)addr; *p; p++) {
		h ^= *p;
		h *= 16777619u;
	}
	h ^= port & 0xff;
	h *= 16777619u;
	h ^= (port >> 8) & 0xff;
	h *= 16777619u;
	return h;
}

static struct mbt_peer_entry **
peer_slot(struct mbt_peer_entry **slots, size_t cap,
          const char *addr, uint16_t port)
{
	size_t i = peer_hash(addr, port) & (cap - 1);
	while (slots[i] && (strcmp(slots[i]->addr, addr) != 0 || slots[i]->port != port))
		i = (i + 1) & (cap - 1);
	return &slots[i];
}

static int
peerdb_grow(struct mbt_peerdb *db)
{
	size_t newcap = db->cap ? db->cap * 2 : PEERDB_INIT_CAP;
	struct mbt_peer_entry **newslots = calloc(newcap, sizeof(*newslots));
	if (!newslots)
		return -1;

	for (size_t i = 0; i < db->cap; i++) {
		if (!db->slots[i]) continue;
		*peer_slot(newslots, newcap, db->slots[i]->addr, db->slots[i]->port) = db->slots[i];
	}

	free(db->slots);
	db->slots = newslots;
	db->cap = newcap;
	return 0;
}

void
mbt_peerdb_init(struct mbt_peerdb *db)
{
	db->slots = NULL;
	db->cap = 0;
	db->count = 0;
}

struct mbt_peer_entry *
mbt_peerdb_insert(struct mbt_peerdb *db, const char *addr, uint16_t port, int source)
{
	if (db->count * PEERDB_LOAD_DEN >= db->cap * PEERDB_LOAD_NUM)
		if (peerdb_grow(db) < 0)
			return NULL;

	struct mbt_peer_entry **slot = peer_slot(db->slots, db->cap, addr, port);
	if (*slot) {
		(*slot)->source = source;
		(*slot)->last_seen = peerdb_now();
		return *slot;
	}

	struct mbt_peer_entry *e = calloc(1, sizeof(*e));
	if (!e)
		return NULL;

	snprintf(e->addr, sizeof(e->addr), "%s", addr);
	e->port = port;
	e->state = MBT_PEERDB_KNOWN;
	e->source = source;
	e->last_seen = peerdb_now();

	*slot = e;
	db->count++;
	return e;
}

struct mbt_peer_entry *
mbt_peerdb_find(struct mbt_peerdb *db, const char *addr, uint16_t port)
{
	if (!db->cap)
		return NULL;
	return *peer_slot(db->slots, db->cap, addr, port);
}

struct mbt_peer_entry *
mbt_peerdb_select(struct mbt_peerdb *db)
{
	struct mbt_peer_entry *best = NULL;
	uint64_t now = peerdb_now();

	for (size_t i = 0; i < db->cap; i++) {
		struct mbt_peer_entry *e = db->slots[i];
		if (!e) continue;

		if (e->state != MBT_PEERDB_KNOWN && e->state != MBT_PEERDB_FAILED)
			continue;
		if (e->state == MBT_PEERDB_BANNED)
			continue;

		if (e->state == MBT_PEERDB_FAILED && e->last_attempt > 0) {
			static const uint64_t backoff[] = {
				30, 120, 600, 1800, 3600,
			};
			int idx = e->failures < 5 ? e->failures : 4;
			if (now - e->last_attempt < backoff[idx] * 1000000ULL)
				continue;
		}

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
	for (size_t i = 0; i < db->cap; i++)
		if (db->slots[i] && db->slots[i]->state == state)
			n++;
	return n;
}

int
mbt_peerdb_total(struct mbt_peerdb *db)
{
	return (int)db->count;
}

void
mbt_peerdb_free(struct mbt_peerdb *db)
{
	for (size_t i = 0; i < db->cap; i++)
		free(db->slots[i]);
	free(db->slots);
	db->slots = NULL;
	db->cap = 0;
	db->count = 0;
}
