// SPDX-License-Identifier: ISC
#ifndef MBT_PEERDB_H
#define MBT_PEERDB_H

#include "export.h"

#include <netinet/in.h>
#include <stdint.h>
#include <time.h>

enum mbt_peerdb_state {
	MBT_PEERDB_KNOWN,       /* discovered but not yet tried */
	MBT_PEERDB_CONNECTING,  /* TCP connect in progress */
	MBT_PEERDB_ACTIVE,      /* connected and handshook */
	MBT_PEERDB_FAILED,      /* connect failed or disconnected */
	MBT_PEERDB_BANNED,      /* hash failures, won't retry */
};

#include "peer_source.h"

struct mbt_peer_entry {
	char addr[INET6_ADDRSTRLEN];
	uint16_t port;

	enum mbt_peerdb_state state;
	int source;              /* mbt_peer_source — how we first found this peer */
	uint8_t flags;           /* PEX flags: 0x04 = supports uTP */
	int failures;            /* consecutive failure count */
	uint64_t last_seen;      /* last time reported by tracker/DHT/PEX (usec) */
	uint64_t last_attempt;   /* last time we tried connecting (usec) */
};

struct mbt_peerdb {
	struct mbt_peer_entry **slots;
	size_t cap;   /* power of two */
	size_t count;
};

/* Initialize an empty peer database */
MBT_API void mbt_peerdb_init(struct mbt_peerdb *db);

/* Insert or update a peer. Returns the entry (existing or new). */
MBT_API struct mbt_peer_entry *mbt_peerdb_insert(struct mbt_peerdb *db,
                                         const char *addr, uint16_t port,
                                         int source);

/* Find a peer by addr:port. Returns NULL if not found. */
MBT_API struct mbt_peer_entry *mbt_peerdb_find(struct mbt_peerdb *db,
                                       const char *addr, uint16_t port);

/* Select the best peer to connect to. Returns NULL if none available.
 * Picks KNOWN peers, preferring recent last_seen, low failures. */
MBT_API struct mbt_peer_entry *mbt_peerdb_select(struct mbt_peerdb *db);

/* Count peers in a given state */
MBT_API int mbt_peerdb_count(struct mbt_peerdb *db, enum mbt_peerdb_state state);

/* Count total peers */
MBT_API int mbt_peerdb_total(struct mbt_peerdb *db);

/* Free all entries */
MBT_API void mbt_peerdb_free(struct mbt_peerdb *db);

#endif
