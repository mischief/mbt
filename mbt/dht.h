#ifndef MBT_DHT_H
#define MBT_DHT_H

#include <stdint.h>
#include <stddef.h>

struct mbt_ev;
struct mbt_dht;
struct mbt_infohash;

/* Callback: called for each peer discovered during get_peers */
typedef void (*mbt_dht_peers_cb)(const char *addr, uint16_t port, void *arg);

/* Callback: called when a get_peers lookup completes (all rounds done) */
typedef void (*mbt_dht_done_cb)(void *arg);

/* Create DHT instance bound to a UDP port */
int mbt_dht_new(struct mbt_dht **out, struct mbt_ev *ev, uint16_t port);
void mbt_dht_free(struct mbt_dht *d);

/* Bootstrap: send find_node to a known node to populate routing table */
int mbt_dht_bootstrap(struct mbt_dht *d, const char *addr, uint16_t port);

/* Look up peers for an infohash. Calls peers_cb for each discovered peer,
 * done_cb when the lookup is finished (may be NULL). */
int mbt_dht_get_peers(struct mbt_dht *d, struct mbt_infohash *ih,
                      mbt_dht_peers_cb peers_cb, mbt_dht_done_cb done_cb, void *arg);

/* Announce ourselves as a peer for an infohash (call after get_peers) */
int mbt_dht_announce(struct mbt_dht *d, struct mbt_infohash *ih, uint16_t listen_port);

/* Number of nodes in routing table */
int mbt_dht_node_count(struct mbt_dht *d);

#endif
