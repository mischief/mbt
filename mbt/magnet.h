// SPDX-License-Identifier: ISC
#ifndef MBT_MAGNET_H
#define MBT_MAGNET_H

#include "export.h"

#include <netinet/in.h>
#include <stdint.h>

#include "infohash.h"

#define MBT_MAGNET_MAX_TRACKERS 16
#define MBT_MAGNET_MAX_PEERS    64

struct mbt_magnet_peer {
	char addr[INET6_ADDRSTRLEN];
	uint16_t port;
};

struct mbt_magnet {
	char infohash_hex[MBT_MBT_INFOHASH_SHA1HEXLEN + 1]; /* hex string (supports future sha256 via longer buf) */
	int infohash_type;                           /* MBT_INFOHASH_SHA1 etc */
	char *display_name;                          /* dn= (malloc'd, may be NULL) */
	char *trackers[MBT_MAGNET_MAX_TRACKERS];     /* tr= (malloc'd) */
	int tracker_count;
	struct mbt_magnet_peer peers[MBT_MAGNET_MAX_PEERS]; /* x.pe= */
	int peer_count;
};

/* Parse a magnet URI. Returns 0 on success, <0 on error. */
MBT_API int mbt_magnet_parse(const char *uri, struct mbt_magnet *out);

/* Free internals (display_name, trackers). Does not free the struct itself. */
MBT_API void mbt_magnet_free(struct mbt_magnet *m);

#endif
