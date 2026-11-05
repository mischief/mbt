// SPDX-License-Identifier: ISC
#ifndef MBT_DHT_H
#define MBT_DHT_H

#include "export.h"

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
MBT_API int mbt_dht_new(struct mbt_dht **out, struct mbt_ev *ev, uint16_t port);
MBT_API void mbt_dht_free(struct mbt_dht *d);

/* Bootstrap: send find_node to a known node to populate routing table */
MBT_API int mbt_dht_bootstrap(struct mbt_dht *d, const char *addr, uint16_t port);

/* Look up peers for an infohash. Calls peers_cb for each discovered peer,
 * done_cb when the lookup is finished (may be NULL). */
MBT_API int mbt_dht_get_peers(struct mbt_dht *d, struct mbt_infohash *ih,
                      mbt_dht_peers_cb peers_cb, mbt_dht_done_cb done_cb, void *arg);

/* Announce ourselves as a peer for an infohash (call after get_peers).
 * Returns the number of nodes told, or -EINVAL with no prior get_peers. */
MBT_API int mbt_dht_announce(struct mbt_dht *d, struct mbt_infohash *ih, uint16_t listen_port);

/* Visit each node in the routing table. cb must not call into d. */
typedef void (*mbt_dht_node_cb)(const uint8_t id[20], const char *addr, uint16_t port, void *arg);
MBT_API void mbt_dht_foreach_node(struct mbt_dht *d, mbt_dht_node_cb cb, void *arg);

/* Our node ID, 20 bytes. */
MBT_API const uint8_t *mbt_dht_node_id(struct mbt_dht *d);

/*
 * Raw KRPC, for tests and tools. reply is the whole message, "r" or "e", and
 * valid only during cb. err is 0, -ETIMEDOUT, or -ECANCELED from mbt_dht_free.
 * addr is an IP literal. args is the "a" dict, sent as it is.
 */
struct mbt_be_value;
typedef void (*mbt_dht_query_cb)(struct mbt_be_value *reply, int err, void *arg);
MBT_API int mbt_dht_query(struct mbt_dht *d, const char *addr, uint16_t port, const char *q,
                          struct mbt_be_value *args, mbt_dht_query_cb cb, void *arg);
MBT_API int mbt_dht_send_raw(struct mbt_dht *d, const char *addr, uint16_t port,
                             const void *data, size_t len);

/* Number of nodes in routing table */
MBT_API int mbt_dht_node_count(struct mbt_dht *d);

/* Port the DHT is listening on (useful with port=0) */
MBT_API uint16_t mbt_dht_port(struct mbt_dht *d);

/* Override the node ID (call before bootstrap) */
MBT_API void mbt_dht_set_node_id(struct mbt_dht *d, const uint8_t id[20]);

/* --- BEP 44: Arbitrary data storage --- */

#define MBT_DHT_PUBKEY_LEN 32
#define MBT_DHT_SIG_LEN    64
#define MBT_DHT_MAX_VALUE   1000

/* Callback: verify mutable put signature. Return 0 = reject, nonzero = accept.
 * buf/buflen is the signed payload (e.g. "3:seqi1e1:v12:Hello World!"). */
typedef int (*mbt_dht_verify_cb)(const uint8_t k[MBT_DHT_PUBKEY_LEN],
                                 const uint8_t sig[MBT_DHT_SIG_LEN],
                                 const void *buf, size_t buflen, void *arg);

/* Set the signature verification callback for mutable items.
 * Without this, mutable puts are rejected. */
MBT_API void mbt_dht_set_verify(struct mbt_dht *d, mbt_dht_verify_cb cb, void *arg);

/* Callback: called when a BEP 44 get completes */
typedef void (*mbt_dht_item_cb)(const uint8_t *value, size_t vlen,
                                int64_t seq, /* -1 for immutable */
                                const uint8_t *k, /* NULL for immutable */
                                void *arg);

/* Item storage keys. An immutable value is its bencoded form. */
MBT_API void mbt_dht_immutable_target(const void *v, size_t vlen, uint8_t target[20]);
MBT_API void mbt_dht_mutable_target(const uint8_t k[MBT_DHT_PUBKEY_LEN], const char *salt,
                                    size_t salt_len, uint8_t target[20]);

/* Read any stored item. cb runs before this returns. -ENOENT if absent. */
MBT_API int mbt_dht_get_item(struct mbt_dht *d, const uint8_t target[20],
                             mbt_dht_item_cb cb, void *arg);

/* The get and put below act on the local store only, which serves other nodes. */

/* Get an immutable item by its SHA-1 hash */
MBT_API int mbt_dht_get_immutable(struct mbt_dht *d, const uint8_t target[20],
                                  mbt_dht_item_cb cb, void *arg);

/* Put an immutable item (value will be stored under SHA-1(v)) */
MBT_API int mbt_dht_put_immutable(struct mbt_dht *d, const void *v, size_t vlen,
                                  mbt_dht_done_cb done_cb, void *arg);

/* Get a mutable item by public key (+ optional salt) */
MBT_API int mbt_dht_get_mutable(struct mbt_dht *d,
                                const uint8_t k[MBT_DHT_PUBKEY_LEN],
                                const char *salt, size_t salt_len,
                                mbt_dht_item_cb cb, void *arg);

/* Put a mutable item (caller provides key, sig, seq, value) */
MBT_API int mbt_dht_put_mutable(struct mbt_dht *d,
                                const uint8_t k[MBT_DHT_PUBKEY_LEN],
                                const uint8_t sig[MBT_DHT_SIG_LEN],
                                int64_t seq,
                                const char *salt, size_t salt_len,
                                const void *v, size_t vlen,
                                mbt_dht_done_cb done_cb, void *arg);

#endif
