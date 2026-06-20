// SPDX-License-Identifier: ISC
#ifndef MBT_TORRENT2_H
#define MBT_TORRENT2_H

#include "export.h"

#include <stdint.h>
#include <stddef.h>

struct mbt_torrent;
struct mbt_ev;
struct mbt_conn;
struct mbt_peer;
struct mbt_infohash;
struct mbt_bitmap;
struct mbt_peerdb;

#include "storage.h"
#include "picker.h"
#include "peer_source.h"
#include "timers.h"

/* Piece states */
enum mbt_piece_state {
	MBT_PIECE_PENDING,     /* not yet requested */
	MBT_PIECE_ACTIVE,      /* blocks being requested/received */
};

/* File entry in a torrent */
struct mbt_file {
	char *path;            /* relative path (single component or multi-component joined with /) */
	uint64_t offset;       /* byte offset within the torrent data */
	uint64_t length;
};

/* Torrent configuration */
struct mbt_torrent_config {
	struct mbt_ev *ev;
	struct mbt_infohash *ih;
	const uint8_t *peer_id;            /* 20 bytes */
	const struct mbt_storage_ops *storage_ops;
	void *storage_ctx;
	mbt_picker_fn picker;
	void *picker_ctx;
	struct mbt_timers timers;
};

/* Torrent events reported to session */
struct mbt_torrent_callbacks {
	void (*on_complete)(struct mbt_torrent *t, void *arg);
	void (*on_piece_complete)(struct mbt_torrent *t, uint32_t piece, void *arg);
	void (*on_error)(struct mbt_torrent *t, int err, void *arg);
};

/* Lifecycle */
MBT_API int mbt_torrent_new(struct mbt_torrent **out, struct mbt_torrent_config *cfg);
MBT_API void mbt_torrent_free(struct mbt_torrent *t);
MBT_API void mbt_torrent_set_callbacks(struct mbt_torrent *t, struct mbt_torrent_callbacks *cb, void *arg);

/* Provide metadata (info dict) — from .torrent file or completed BEP 9 fetch */
MBT_API int mbt_torrent_set_metadata(struct mbt_torrent *t, const void *info, size_t len);

/* Add a connected peer — torrent takes ownership of conn */
MBT_API int mbt_torrent_add_peer(struct mbt_torrent *t, struct mbt_conn *conn, enum mbt_peer_source source);
MBT_API int mbt_torrent_add_peer_incoming(struct mbt_torrent *t, struct mbt_conn *conn,
                                  const uint8_t ext[8], const uint8_t their_id[20]);
MBT_API struct mbt_peerdb *mbt_torrent_peerdb(struct mbt_torrent *t);

/* Query */
MBT_API struct mbt_infohash *mbt_torrent_infohash(struct mbt_torrent *t);
MBT_API const char *mbt_torrent_name(struct mbt_torrent *t);
MBT_API float mbt_torrent_progress(struct mbt_torrent *t);
MBT_API int mbt_torrent_complete(struct mbt_torrent *t);
MBT_API int mbt_torrent_is_private(struct mbt_torrent *t);
MBT_API int mbt_torrent_has_metadata(struct mbt_torrent *t);

enum mbt_torrent_state {
	MBT_TORRENT_INIT,        /* no metadata yet */
	MBT_TORRENT_DOWNLOADING, /* actively receiving pieces */
	MBT_TORRENT_STALLED,     /* incomplete but no progress possible */
	MBT_TORRENT_SEEDING,     /* complete, serving to others */
};

MBT_API enum mbt_torrent_state mbt_torrent_state(struct mbt_torrent *t);

MBT_API uint32_t mbt_torrent_piece_count(struct mbt_torrent *t);
MBT_API uint32_t mbt_torrent_piece_length(struct mbt_torrent *t);
MBT_API uint32_t mbt_torrent_have_count(struct mbt_torrent *t);
MBT_API int mbt_torrent_peer_count(struct mbt_torrent *t);
MBT_API struct mbt_peer *mbt_torrent_peer_at(struct mbt_torrent *t, int idx);
MBT_API int mbt_torrent_has_piece(struct mbt_torrent *t, uint32_t piece);
MBT_API int mbt_torrent_piece_active(struct mbt_torrent *t, uint32_t piece);
MBT_API int mbt_torrent_piece_fully_requested(struct mbt_torrent *t, uint32_t piece);

/* Bulk piece state query: 0=pending, 1=active(requested), 2=have */
MBT_API void mbt_torrent_piece_map(struct mbt_torrent *t, uint8_t *out, uint32_t count);
MBT_API int mbt_torrent_file_count(struct mbt_torrent *t);
MBT_API const struct mbt_file *mbt_torrent_file(struct mbt_torrent *t, int index);

/* Rate limiting — 0 = unlimited, adjustable at runtime */
MBT_API void mbt_torrent_set_upload_rate(struct mbt_torrent *t, uint64_t bytes_per_sec);
MBT_API void mbt_torrent_set_download_rate(struct mbt_torrent *t, uint64_t bytes_per_sec);
MBT_API uint64_t mbt_torrent_upload_rate(struct mbt_torrent *t);
MBT_API uint64_t mbt_torrent_download_rate(struct mbt_torrent *t);

/* Per-peer stats snapshot */
struct mbt_peer_stats {
	const char *addr;
	const char *client_version; /* BEP 10 "v", NULL if unknown (points into peer) */
	uint16_t port;       /* listen port from BEP 10 "p", 0 if unknown */
	uint16_t conn_port;  /* ephemeral TCP source port, always set */
	int choked;       /* we are choked by them */
	int interested;   /* we are interested */
	int snubbed;
	int outstanding;  /* in-flight requests */
	int done_pct;     /* % of pieces this peer has (0-100) */
	int hash_failures;
	int score;            /* peer penalty score (lower = better, 0 = clean) */
	enum mbt_peer_source source;   /* mbt_peer_source */
	uint64_t downloaded;  /* bytes received from peer */
	uint64_t uploaded;    /* bytes sent to peer */
};

MBT_API int mbt_torrent_peer_stats(struct mbt_torrent *t, int idx, struct mbt_peer_stats *out);

#endif
