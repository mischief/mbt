/*
 * tracker_store.c — shared peer storage for tracker servers
 */
#include <stdlib.h>
#include <string.h>
#include <time.h>
#include <netinet/in.h>

#include "mbt/tracker_server.h"

#define MAX_PEERS_PER_TORRENT 200
#define TORRENT_BUCKETS 256

struct ts_peer {
	struct sockaddr_in addr;
	time_t last_seen;
};

struct ts_torrent {
	uint8_t infohash[20];
	struct ts_peer peers[MAX_PEERS_PER_TORRENT];
	int peer_count;
	int seeders;
	int leechers;
	struct ts_torrent *next;
};

struct mbt_tracker_store {
	struct ts_torrent *torrents[TORRENT_BUCKETS];
	int num_torrents;
	int num_peers;
};

int
mbt_tracker_store_new(struct mbt_tracker_store **out)
{
	struct mbt_tracker_store *s = calloc(1, sizeof(*s));
	if (!s) return -1;
	*out = s;
	return 0;
}

void
mbt_tracker_store_free(struct mbt_tracker_store *s)
{
	if (!s) return;
	for (int i = 0; i < TORRENT_BUCKETS; i++) {
		struct ts_torrent *t = s->torrents[i];
		while (t) { struct ts_torrent *next = t->next; free(t); t = next; }
	}
	free(s);
}

int mbt_tracker_store_num_torrents(struct mbt_tracker_store *s) { return s->num_torrents; }
int mbt_tracker_store_num_peers(struct mbt_tracker_store *s) { return s->num_peers; }

/* --- Internal helpers (used by UDP and HTTP servers) --- */

static unsigned
hash_infohash(const uint8_t *ih)
{
	unsigned h = 0;
	for (int i = 0; i < 20; i++) h = h * 31 + ih[i];
	return h % TORRENT_BUCKETS;
}

struct ts_torrent *
mbt_tracker_store_find(struct mbt_tracker_store *s, const uint8_t *ih)
{
	unsigned bucket = hash_infohash(ih);
	struct ts_torrent *t = s->torrents[bucket];
	while (t) {
		if (memcmp(t->infohash, ih, 20) == 0) return t;
		t = t->next;
	}
	return NULL;
}

struct ts_torrent *
mbt_tracker_store_get_or_create(struct mbt_tracker_store *s, const uint8_t *ih)
{
	struct ts_torrent *t = mbt_tracker_store_find(s, ih);
	if (t) return t;
	t = calloc(1, sizeof(*t));
	if (!t) return NULL;
	memcpy(t->infohash, ih, 20);
	unsigned bucket = hash_infohash(ih);
	t->next = s->torrents[bucket];
	s->torrents[bucket] = t;
	s->num_torrents++;
	return t;
}

void
mbt_tracker_store_add_peer(struct mbt_tracker_store *s, struct ts_torrent *t,
                           const struct sockaddr_in *addr, int is_seeder)
{
	for (int i = 0; i < t->peer_count; i++) {
		if (t->peers[i].addr.sin_addr.s_addr == addr->sin_addr.s_addr &&
		    t->peers[i].addr.sin_port == addr->sin_port) {
			t->peers[i].last_seen = time(NULL);
			return;
		}
	}
	if (t->peer_count >= MAX_PEERS_PER_TORRENT) return;
	t->peers[t->peer_count].addr = *addr;
	t->peers[t->peer_count].last_seen = time(NULL);
	t->peer_count++;
	s->num_peers++;
	if (is_seeder) t->seeders++; else t->leechers++;
}

int
mbt_tracker_store_get_peers(struct ts_torrent *t, const struct sockaddr_in *exclude,
                            uint8_t *compact_out, int max_peers)
{
	int sent = 0;
	for (int i = 0; i < t->peer_count && sent < max_peers; i++) {
		if (exclude && t->peers[i].addr.sin_addr.s_addr == exclude->sin_addr.s_addr &&
		    t->peers[i].addr.sin_port == exclude->sin_port)
			continue;
		memcpy(compact_out + sent * 6, &t->peers[i].addr.sin_addr, 4);
		memcpy(compact_out + sent * 6 + 4, &t->peers[i].addr.sin_port, 2);
		sent++;
	}
	return sent;
}

int mbt_tracker_store_torrent_seeders(struct ts_torrent *t) { return t->seeders; }
int mbt_tracker_store_torrent_leechers(struct ts_torrent *t) { return t->leechers; }
int mbt_tracker_store_torrent_peer_count(struct ts_torrent *t) { return t->peer_count; }
