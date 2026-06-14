// SPDX-License-Identifier: ISC
#ifndef MBT_PEER_H
#define MBT_PEER_H

#include "export.h"
#include "peer_source.h"

#include <sys/queue.h>
#include <string.h>

#define MBT_PEER_ID_LEN 20
#define MBT_PEER_ID_STRLEN (MBT_PEER_ID_LEN * 2)

static inline int mbt_peer_id_eq(const uint8_t *a, const uint8_t *b)
{
	return memcmp(a, b, MBT_PEER_ID_LEN) == 0;
}
#include <netinet/in.h>
#include <stdint.h>

struct mbt_peer;
struct mbt_conn;
struct mbt_ev;
struct mbt_infohash;
struct mbt_bitmap;

/* Peer states */
enum mbt_peer_state {
	MBT_PEER_CONNECTING,
	MBT_PEER_HANDSHAKE,
	MBT_PEER_EXTENSION,
	MBT_PEER_ACTIVE,
	MBT_PEER_CLOSED,
};

/* Reserved byte flags from handshake (BEP 3/4) */
enum mbt_peer_flags {
	MBT_PEER_FLAG_DHT       = 1 << 0, /* ext[7] & 0x01 — BEP 5 */
	MBT_PEER_FLAG_FAST      = 1 << 1, /* ext[7] & 0x04 — BEP 6 */
	MBT_PEER_FLAG_EXTENSION = 1 << 2, /* ext[5] & 0x10 — BEP 10 */
};

/* Events reported up to the torrent/owner */
struct mbt_peer_callbacks {
	/* Handshake complete — peer is now usable */
	void (*on_active)(struct mbt_peer *p, void *arg);

	/* Choke state changed */
	void (*on_choke)(struct mbt_peer *p, int choked, void *arg);

	/* Peer expressed interest (wants data from us) */
	void (*on_interest)(struct mbt_peer *p, int interested, void *arg);

	/* Peer has a piece */
	void (*on_have)(struct mbt_peer *p, uint32_t piece, void *arg);

	/* Full bitfield received */
	void (*on_bitfield)(struct mbt_peer *p, struct mbt_bitmap *bits, void *arg);

	/* Piece block received */
	void (*on_piece)(struct mbt_peer *p, uint32_t piece, uint32_t offset,
	                 const void *data, size_t len, void *arg);

	/* Peer requests a block from us */
	void (*on_request)(struct mbt_peer *p, uint32_t piece, uint32_t offset,
	                   uint32_t length, void *arg);

	/* Metadata block received (BEP 9) */
	void (*on_metadata)(struct mbt_peer *p, int piece, const void *data,
	                    size_t len, void *arg);
	/* BEP 9: peer requests metadata piece from us */
	void (*on_metadata_request)(struct mbt_peer *p, int piece, void *arg);

	/* Peer disconnected or errored */
	void (*on_error)(struct mbt_peer *p, int err, void *arg);

	/* PEX: peer exchange — new peer discovered */
	void (*on_pex)(struct mbt_peer *p, const char *addr, uint16_t port, void *arg);

	/* BEP 5: peer's DHT port */
	void (*on_port)(struct mbt_peer *p, uint16_t dht_port, void *arg);

	/* BEP 6 Fast: peer rejected our request */
	void (*on_reject)(struct mbt_peer *p, uint32_t piece, uint32_t offset,
	                  uint32_t length, void *arg);
};

/* Create peer from a connected mbt_conn (takes ownership of conn) */
MBT_API int mbt_peer_new(struct mbt_peer **out, struct mbt_ev *ev, struct mbt_conn *conn,
                 struct mbt_infohash *ih, const uint8_t peer_id[20],
                 uint32_t piece_count, enum mbt_peer_source source,
                 struct mbt_peer_callbacks *cb, void *arg);

/* Create peer from an already-accepted incoming connection (handshake already read) */
MBT_API int mbt_peer_new_incoming(struct mbt_peer **out, struct mbt_ev *ev, struct mbt_conn *conn,
                          struct mbt_infohash *ih, const uint8_t peer_id[20],
                          uint32_t piece_count, enum mbt_peer_source source, const uint8_t ext[8], const uint8_t their_id[20],
                          size_t metadata_size,
                          struct mbt_peer_callbacks *cb, void *arg);

MBT_API void mbt_peer_set_our_metadata_size(struct mbt_peer *p, size_t sz);
MBT_API void mbt_peer_set_our_reqq(struct mbt_peer *p, int reqq);
MBT_API void mbt_peer_set_piece_count(struct mbt_peer *p, uint32_t count);
MBT_API uint8_t mbt_peer_ut_pex_id(struct mbt_peer *p);

/* Free peer (closes connection) */
MBT_API void mbt_peer_free(struct mbt_peer *p);

/* Disconnect peer (triggers on_error callback asynchronously) */
MBT_API void mbt_peer_disconnect(struct mbt_peer *p);

/* Query state */
MBT_API enum mbt_peer_state mbt_peer_state(struct mbt_peer *p);
MBT_API int mbt_peer_choked(struct mbt_peer *p);      /* are we choked by them? */
MBT_API int mbt_peer_interested(struct mbt_peer *p);   /* are we interested? */
MBT_API struct mbt_bitmap *mbt_peer_bitfield(struct mbt_peer *p);

/* Commands (call down from torrent) */
MBT_API int mbt_peer_send_interested(struct mbt_peer *p, int interested);
MBT_API int mbt_peer_send_choke(struct mbt_peer *p, int choke);
MBT_API int mbt_peer_send_request(struct mbt_peer *p, uint32_t piece, uint32_t offset, uint32_t length);
MBT_API int mbt_peer_send_cancel(struct mbt_peer *p, uint32_t piece, uint32_t offset, uint32_t length);
MBT_API int mbt_peer_send_have(struct mbt_peer *p, uint32_t piece);
MBT_API int mbt_peer_send_port(struct mbt_peer *p, uint16_t port);
MBT_API int mbt_peer_send_bitfield(struct mbt_peer *p, struct mbt_bitmap *bm);
MBT_API int mbt_peer_send_msg(struct mbt_peer *p, const void *buf, size_t len);
MBT_API int mbt_peer_send_piece(struct mbt_peer *p, uint32_t piece, uint32_t offset,
                        const void *data, size_t len);

/* BEP 9: request metadata piece from peer */
MBT_API int mbt_peer_send_metadata_request(struct mbt_peer *p, int piece);
MBT_API int mbt_peer_send_metadata_data(struct mbt_peer *p, int piece, size_t total_size,
                                const void *data, size_t len);

/* Extension info (available after on_active) */
MBT_API int mbt_peer_has_extension(struct mbt_peer *p);       /* BEP 10 supported? */
MBT_API uint32_t mbt_peer_flags(struct mbt_peer *p);          /* mbt_peer_flags bitmask */
MBT_API size_t mbt_peer_metadata_size(struct mbt_peer *p);    /* 0 if unknown */
MBT_API int mbt_peer_reqq(struct mbt_peer *p);                /* max outstanding, 0=unknown */
MBT_API const char *mbt_peer_addr(struct mbt_peer *p);
MBT_API const uint8_t *mbt_peer_id(struct mbt_peer *p);
MBT_API uint16_t mbt_peer_port(struct mbt_peer *p);
MBT_API enum mbt_peer_source mbt_peer_source(struct mbt_peer *p);
MBT_API const char *mbt_peer_source_str(enum mbt_peer_source source);

#endif
