// SPDX-License-Identifier: ISC
#ifndef MBT_TRACKER_SERVER_H
#define MBT_TRACKER_SERVER_H

#include "export.h"

#include <stdint.h>

struct mbt_ev;

/* --- Peer store (shared between UDP and HTTP servers) --- */
struct mbt_tracker_store;

MBT_API int mbt_tracker_store_new(struct mbt_tracker_store **out);
MBT_API void mbt_tracker_store_free(struct mbt_tracker_store *s);
MBT_API int mbt_tracker_store_num_torrents(struct mbt_tracker_store *s);
MBT_API int mbt_tracker_store_num_peers(struct mbt_tracker_store *s);

struct mbt_tracker_stats {
	uint32_t torrents;
	uint32_t peers;
	uint32_t seeders;
	uint32_t leechers;
};
MBT_API void mbt_tracker_store_stats(struct mbt_tracker_store *s, struct mbt_tracker_stats *out);

/* Seconds that a client waits between announces; default 900. Raises the expire
 * age to twice this, if it is lower. */
MBT_API void mbt_tracker_store_set_interval(struct mbt_tracker_store *s, int secs);
/* Swarms held; default 4096. An announce for a new swarm above this gets no reply. */
MBT_API void mbt_tracker_store_set_max_torrents(struct mbt_tracker_store *s, int max);

/* --- UDP tracker server (BEP 15) --- */
struct mbt_tracker_server;

/* store=NULL creates an internal store; non-NULL shares (caller owns lifetime) */
MBT_API int mbt_tracker_server_new(struct mbt_tracker_server **out, struct mbt_ev *ev,
                           uint16_t port, struct mbt_tracker_store *store);
MBT_API void mbt_tracker_server_free(struct mbt_tracker_server *ts);
MBT_API uint16_t mbt_tracker_server_port(struct mbt_tracker_server *ts);
MBT_API int mbt_tracker_server_num_torrents(struct mbt_tracker_server *ts);
MBT_API int mbt_tracker_server_num_peers(struct mbt_tracker_server *ts);

/* --- HTTP tracker server (BEP 3, BEP 48 scrape) --- */
struct mbt_tracker_http;

/* store=NULL creates an internal store; non-NULL shares (caller owns lifetime) */
MBT_API int mbt_tracker_http_new(struct mbt_tracker_http **out, struct mbt_ev *ev,
                         uint16_t port, struct mbt_tracker_store *store);
MBT_API void mbt_tracker_http_free(struct mbt_tracker_http *th);
MBT_API uint16_t mbt_tracker_http_port(struct mbt_tracker_http *th);
MBT_API int mbt_tracker_http_num_torrents(struct mbt_tracker_http *th);
MBT_API int mbt_tracker_http_num_peers(struct mbt_tracker_http *th);

/* Expire stale peers (30 minutes by default) and remove empty torrents. Servers
 * run this on a timer. */
MBT_API void mbt_tracker_store_set_expire(struct mbt_tracker_store *s, int secs);
MBT_API void mbt_tracker_store_gc(struct mbt_tracker_store *s);

#endif
