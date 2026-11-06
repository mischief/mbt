// SPDX-License-Identifier: ISC
/* Peer store calls shared by the UDP and HTTP tracker servers. */
#ifndef MBT_TRACKER_STORE_H
#define MBT_TRACKER_STORE_H

#include <stdint.h>
#include <sys/socket.h>
#include <netinet/in.h>

struct mbt_tracker_store;
struct ts_torrent;

struct ts_torrent *mbt_tracker_store_find(struct mbt_tracker_store *s, const uint8_t *ih);
struct ts_torrent *mbt_tracker_store_get_or_create(struct mbt_tracker_store *s, const uint8_t *ih);
uint64_t mbt_tracker_store_gc_usec(const struct mbt_tracker_store *s);
void mbt_tracker_store_remove_peer(struct mbt_tracker_store *s, struct ts_torrent *t,
                                   const struct sockaddr_storage *addr);
void mbt_tracker_store_add_peer(struct mbt_tracker_store *s, struct ts_torrent *t,
                                const struct sockaddr_storage *addr, int is_seeder);
int mbt_tracker_store_get_peers(struct ts_torrent *t, const struct sockaddr_in *exclude,
                                uint8_t *compact_out, int max_peers);
int mbt_tracker_store_get_peers_v4(struct ts_torrent *t, const struct sockaddr_storage *exclude,
                                   uint8_t *compact_out, int max_peers);
int mbt_tracker_store_get_peers_v6(struct ts_torrent *t, const struct sockaddr_storage *exclude,
                                   uint8_t *compact_out, int max_peers);
uint64_t mbt_tracker_store_make_connection_id(struct mbt_tracker_store *s,
                                              const struct sockaddr_storage *addr);
int mbt_tracker_store_validate_connection_id(struct mbt_tracker_store *s,
                                             const struct sockaddr_storage *addr, uint64_t cid);
int mbt_tracker_store_torrent_seeders(struct ts_torrent *t);
int mbt_tracker_store_torrent_leechers(struct ts_torrent *t);
int mbt_tracker_store_torrent_peer_count(struct ts_torrent *t);

#endif
