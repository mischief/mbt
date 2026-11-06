// SPDX-License-Identifier: ISC
/* Internal definitions shared between library source files. Not installed. */
#ifndef MBT_INTERNAL_H
#define MBT_INTERNAL_H

#include <stddef.h>
#include <stdint.h>
#include "mbt/webseed.h"
#include "mbt/utp.h"   /* mbt_utp_accept_cb, struct mbt_utp_ctx */
#include "mbt/dht.h"   /* struct mbt_dht */

/* Internal mux-sharing constructors: let uTP and DHT share one UDP socket
 * (the session creates one mbt_udp_mux and passes it to both). The public
 * mbt_utp_ctx_new and mbt_dht_new make their own private mux. */
struct mbt_ev;
struct mbt_udp_mux;
int mbt_utp_ctx_new_mux(struct mbt_utp_ctx **out, struct mbt_ev *ev,
                        struct mbt_udp_mux *mux,
                        mbt_utp_accept_cb accept_cb, void *accept_arg);
struct mbt_dht_config;
int mbt_dht_new_mux(struct mbt_dht **out, const struct mbt_dht_config *cfg,
                    struct mbt_udp_mux *mux);
/* Handle one received datagram, without a socket. */
struct sockaddr_storage;
void mbt_dht_input(struct mbt_dht *d, const void *buf, size_t len,
                   const struct sockaddr_storage *from);


/* BT message IDs (BEP 3) */
#define BT_CHOKE           0
#define BT_UNCHOKE         1
#define BT_INTERESTED      2
#define BT_NOT_INTERESTED  3
#define BT_HAVE            4
#define BT_BITFIELD        5
#define BT_REQUEST         6
#define BT_PIECE           7
#define BT_CANCEL          8
#define BT_PORT            9
#define BT_EXTENSION       20

/* BEP 6 Fast Extension */
#define BT_HAVE_ALL        14
#define BT_HAVE_NONE       15
#define BT_REJECT          16
#define BT_ALLOWED_FAST    17

/* Handshake reserved byte flags */
#define RESERVED_DHT       0x01  /* ext[7] bit 0 */
#define RESERVED_FAST      0x04  /* ext[7] bit 2 */
#define RESERVED_EXTENSION 0x10  /* ext[5] bit 4 */

/* Internal interfaces shared between session.c and torrent.c */
struct mbt_ratelimit;
struct mbt_session;
struct mbt_torrent;
struct mbt_peer;
struct mbt_ratelimit *mbt_session_get_upload_limit(struct mbt_session *s);
struct mbt_ratelimit *mbt_session_get_download_limit(struct mbt_session *s);
void mbt_torrent_set_session(struct mbt_torrent *t, struct mbt_session *s);
int mbt_session_save_torrent(struct mbt_session *s, struct mbt_torrent *t);
int mbt_session_max_pieces_per_peer(struct mbt_session *s);
int mbt_session_min_peers_before_download(struct mbt_session *s);
int mbt_session_reqq(struct mbt_session *s);

/* Web seeds: webseed.c, with session.c and torrent.c */
struct mbt_webseed_set;
struct mbt_webseed_policy;
struct mbt_ev;

enum {
	TORRENT_COMMIT_OK = 0,   /* stored, or the torrent had the piece */
	TORRENT_COMMIT_BAD_HASH, /* nothing changed */
	TORRENT_COMMIT_DONE,     /* the torrent is complete; t may be gone */
};

int webseed_set_new(struct mbt_webseed_set **out, struct mbt_torrent *t, struct mbt_ev *ev,
                    const struct mbt_webseed_policy *policy);
int webseed_set_add(struct mbt_webseed_set *set, const char *url);
void webseed_set_stop(struct mbt_webseed_set *set);
void webseed_set_free(struct mbt_webseed_set *set);
void webseed_set_sources_empty(struct mbt_webseed_set *set);
void webseed_set_kick(struct mbt_webseed_set *set);
uint64_t webseed_set_bytes(const struct mbt_webseed_set *set);
int webseed_set_busy(const struct mbt_webseed_set *set);
int webseed_set_count(const struct mbt_webseed_set *set);
const char *webseed_set_url(const struct mbt_webseed_set *set, int idx);

struct mbt_session *torrent_session(struct mbt_torrent *t);
int torrent_set_source(struct mbt_torrent *t, const void *data, size_t len);
const void *torrent_source(struct mbt_torrent *t, size_t *len);
void torrent_source_clear(struct mbt_torrent *t);
const char *session_saved_tracker(struct mbt_session *s, struct mbt_torrent *t, int idx);
uint64_t torrent_total_length(struct mbt_torrent *t);
int torrent_webseed_wanted(struct mbt_torrent *t);
int torrent_webseed_throttled(struct mbt_torrent *t);
void torrent_webseed_consume(struct mbt_torrent *t, size_t len);
int torrent_webseed_limited(struct mbt_torrent *t);
int torrent_webseed_pick(struct mbt_torrent *t);
int torrent_webseed_claim(struct mbt_torrent *t, uint32_t piece, size_t *len);
uint32_t torrent_webseed_run_end(struct mbt_torrent *t, uint32_t first, uint32_t max,
                                 uint64_t *bytes);
void torrent_webseed_release(struct mbt_torrent *t, uint32_t piece);
void torrent_webseed_drive(struct mbt_torrent *t);
int torrent_webseed_commit(struct mbt_torrent *t, uint32_t piece, const void *buf, size_t len);
void torrent_webseed_sources_empty(struct mbt_torrent *t);

int session_webseed_hook(struct mbt_session *s, mbt_http_range_fn *fn,
                         mbt_http_range_cancel_fn *cancel, void **arg);
int session_webseed_acquire(struct mbt_session *s);
void session_webseed_release(struct mbt_session *s);
int session_torrent_no_sources(struct mbt_session *s, struct mbt_torrent *t);
int mbt_session_max_pipeline(struct mbt_session *s);
int mbt_picker_rarest_bucket(struct mbt_torrent *t, void *peer, void *ctx);
/* BEP 55 (session.c): uTP-only connect to the endpoint a relay gave us. */
int mbt_session_holepunch_connect(struct mbt_session *s, struct mbt_torrent *t,
                                  const char *addr, uint16_t port);
const uint8_t *mbt_session_peer_id(struct mbt_session *s);
const char *mbt_session_external_addr(struct mbt_session *s);
/* Self-dial avoidance (session.c): remember one of our own addresses, and
 * test whether addr:port is us (only our own listen port counts). */
void mbt_session_note_self_addr(struct mbt_session *s, const char *addr);
int mbt_session_is_self(struct mbt_session *s, const char *addr, uint16_t port);
/* BEP 55 (torrent.c): a direct connect to addr:port failed; ask the peer that
 * told us about it to introduce us. Returns 0 if a rendezvous was sent. */
int mbt_torrent_resolve_duplicate(struct mbt_torrent *t, struct mbt_peer *self,
                                  const uint8_t their_id[20], int new_outgoing);
int mbt_torrent_holepunch_rendezvous(struct mbt_torrent *t,
                                     const char *addr, uint16_t port);

/* peer.h */
struct mbt_peer;
void *mbt_peer_user(struct mbt_peer *p);
void mbt_peer_set_user(struct mbt_peer *p, void *user);

#endif
