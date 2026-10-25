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

/* Learned transport capability for a peer -- internal state, NOT a wire value.
 * Distinct from the PEX added.f flags byte (which carries protocol-defined bits
 * like 0x04 = supports uTP). We seed YES from that bit but track tried/failed
 * here so we never overload the protocol flags. */
enum mbt_utp_cap {
	MBT_UTP_UNKNOWN = 0,  /* not yet determined (DHT/tracker/LSD peers) */
	MBT_UTP_YES,          /* believed/known uTP-capable (PEX 0x04 or a success) */
	MBT_UTP_NO,           /* a uTP attempt failed; use TCP */
};

struct mbt_peer_entry {
	char addr[INET6_ADDRSTRLEN];
	uint16_t port;

	enum mbt_peerdb_state state;
	int source;              /* mbt_peer_source — how we first found this peer */
	uint8_t flags;           /* PEX added.f wire flags: 0x04 = supports uTP */
	uint8_t utp_cap;         /* enum mbt_utp_cap — learned transport capability */
	int failures;            /* consecutive failure count */
	uint64_t last_seen;      /* last time reported by tracker/DHT/PEX (usec) */
	uint64_t last_attempt;   /* last time we tried connecting (usec) */

	/* BEP 55: the connected peer that told us about this one via PEX
	 * (addr + listen port), a candidate relay for a holepunch. Empty if
	 * not learned from PEX. */
	char via_addr[INET6_ADDRSTRLEN];
	uint16_t via_port;
	uint64_t last_holepunch; /* last rendezvous sent for this peer (usec) */
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
