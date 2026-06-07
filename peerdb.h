#ifndef MBT_PEERDB_H
#define MBT_PEERDB_H

#include <sys/tree.h>
#include <netinet/in.h>
#include <stdint.h>
#include <time.h>

enum mbt_peer_state {
	MBT_PEERDB_KNOWN,       /* discovered but not yet tried */
	MBT_PEERDB_CONNECTING,  /* TCP connect in progress */
	MBT_PEERDB_ACTIVE,      /* connected and handshook */
	MBT_PEERDB_FAILED,      /* connect failed or disconnected */
	MBT_PEERDB_BANNED,      /* hash failures, won't retry */
};

enum mbt_peer_source {
	MBT_SOURCE_TRACKER = 1 << 0,
	MBT_SOURCE_DHT     = 1 << 1,
	MBT_SOURCE_PEX     = 1 << 2,
	MBT_SOURCE_LSD     = 1 << 3,
	MBT_SOURCE_INCOMING = 1 << 4,
	MBT_SOURCE_MANUAL  = 1 << 5,
};

struct mbt_peer_entry {
	char addr[INET6_ADDRSTRLEN];
	uint16_t port;

	enum mbt_peer_state state;
	int sources;             /* bitmask of mbt_peer_source */
	int failures;            /* consecutive failure count */
	uint64_t last_seen;      /* last time reported by tracker/DHT/PEX (usec) */
	uint64_t last_attempt;   /* last time we tried connecting (usec) */

	RB_ENTRY(mbt_peer_entry) entry;
};

RB_HEAD(mbt_peerdb, mbt_peer_entry);

/* Initialize an empty peer database */
void mbt_peerdb_init(struct mbt_peerdb *db);

/* Insert or update a peer. Returns the entry (existing or new). */
struct mbt_peer_entry *mbt_peerdb_insert(struct mbt_peerdb *db,
                                         const char *addr, uint16_t port,
                                         int source);

/* Find a peer by addr:port. Returns NULL if not found. */
struct mbt_peer_entry *mbt_peerdb_find(struct mbt_peerdb *db,
                                       const char *addr, uint16_t port);

/* Select the best peer to connect to. Returns NULL if none available.
 * Picks KNOWN peers, preferring recent last_seen, low failures. */
struct mbt_peer_entry *mbt_peerdb_select(struct mbt_peerdb *db);

/* Count peers in a given state */
int mbt_peerdb_count(struct mbt_peerdb *db, enum mbt_peer_state state);

/* Count total peers */
int mbt_peerdb_total(struct mbt_peerdb *db);

/* Free all entries */
void mbt_peerdb_free(struct mbt_peerdb *db);

#endif
