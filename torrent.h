#ifndef MBT_TORRENT2_H
#define MBT_TORRENT2_H

#include <stdint.h>
#include <stddef.h>

struct mbt_torrent;
struct mbt_ev;
struct mbt_conn;
struct mbt_peer;
struct infohash;
struct mbt_bitmap;

#include "storage.h"
#include "picker.h"

/* Piece states */
enum mbt_piece_state {
	MBT_PIECE_PENDING,     /* not yet requested */
	MBT_PIECE_ACTIVE,      /* blocks being requested/received */
};

/* Torrent configuration */
struct mbt_torrent_config {
	struct mbt_ev *ev;
	struct infohash *ih;
	const uint8_t *peer_id;            /* 20 bytes */
	const struct mbt_storage_ops *storage_ops;
	void *storage_ctx;
	mbt_picker_fn picker;
	void *picker_ctx;
	int request_timeout_sec;           /* 0 = default 15s */
};

/* Torrent events reported to session */
struct mbt_torrent_callbacks {
	void (*on_complete)(struct mbt_torrent *t, void *arg);
	void (*on_piece_complete)(struct mbt_torrent *t, uint32_t piece, void *arg);
	void (*on_error)(struct mbt_torrent *t, int err, void *arg);
};

/* Lifecycle */
int mbt_torrent_new(struct mbt_torrent **out, struct mbt_torrent_config *cfg);
void mbt_torrent_free(struct mbt_torrent *t);
void mbt_torrent_set_callbacks(struct mbt_torrent *t, struct mbt_torrent_callbacks *cb, void *arg);

/* Provide metadata (info dict) — from .torrent file or completed BEP 9 fetch */
int mbt_torrent_set_metadata(struct mbt_torrent *t, const void *info, size_t len);

/* Add a connected peer — torrent takes ownership of conn */
int mbt_torrent_add_peer(struct mbt_torrent *t, struct mbt_conn *conn);

/* Query */
struct infohash *mbt_torrent_infohash(struct mbt_torrent *t);
const char *mbt_torrent_name(struct mbt_torrent *t);
float mbt_torrent_progress(struct mbt_torrent *t);
int mbt_torrent_complete(struct mbt_torrent *t);
int mbt_torrent_has_metadata(struct mbt_torrent *t);
int mbt_torrent_has_piece(struct mbt_torrent *t, uint32_t piece);
int mbt_torrent_piece_active(struct mbt_torrent *t, uint32_t piece);
int mbt_torrent_piece_fully_requested(struct mbt_torrent *t, uint32_t piece);

#endif
