#ifndef MBT_PEER_H
#define MBT_PEER_H

#include <sys/queue.h>
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

	/* Peer disconnected or errored */
	void (*on_error)(struct mbt_peer *p, int err, void *arg);
};

/* Create peer from a connected mbt_conn (takes ownership of conn) */
int mbt_peer_new(struct mbt_peer **out, struct mbt_ev *ev, struct mbt_conn *conn,
                 struct mbt_infohash *ih, const uint8_t peer_id[20],
                 uint32_t piece_count,
                 struct mbt_peer_callbacks *cb, void *arg);

/* Free peer (closes connection) */
void mbt_peer_free(struct mbt_peer *p);

/* Query state */
enum mbt_peer_state mbt_peer_state(struct mbt_peer *p);
int mbt_peer_choked(struct mbt_peer *p);      /* are we choked by them? */
int mbt_peer_interested(struct mbt_peer *p);   /* are we interested? */
struct mbt_bitmap *mbt_peer_bitfield(struct mbt_peer *p);

/* Commands (call down from torrent) */
int mbt_peer_send_interested(struct mbt_peer *p, int interested);
int mbt_peer_send_choke(struct mbt_peer *p, int choke);
int mbt_peer_send_request(struct mbt_peer *p, uint32_t piece, uint32_t offset, uint32_t length);
int mbt_peer_send_cancel(struct mbt_peer *p, uint32_t piece, uint32_t offset, uint32_t length);
int mbt_peer_send_have(struct mbt_peer *p, uint32_t piece);
int mbt_peer_send_piece(struct mbt_peer *p, uint32_t piece, uint32_t offset,
                        const void *data, size_t len);

/* BEP 9: request metadata piece from peer */
int mbt_peer_send_metadata_request(struct mbt_peer *p, int piece);

/* Extension info (available after on_active) */
int mbt_peer_has_extension(struct mbt_peer *p);       /* BEP 10 supported? */
uint32_t mbt_peer_flags(struct mbt_peer *p);          /* mbt_peer_flags bitmask */
size_t mbt_peer_metadata_size(struct mbt_peer *p);    /* 0 if unknown */
int mbt_peer_reqq(struct mbt_peer *p);                /* max outstanding, 0=unknown */
const char *mbt_peer_addr(struct mbt_peer *p);
uint16_t mbt_peer_port(struct mbt_peer *p);

#endif
