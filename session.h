#ifndef MBT_SESSION_H
#define MBT_SESSION_H

#include <stdint.h>
#include <stddef.h>

struct mbt_session;
struct mbt_ev;
struct mbt_torrent;
struct infohash;

/* Storage backend */
struct mbt_storage_ops {
	int (*open)(void *ctx, struct mbt_torrent *t);
	int (*write_piece)(void *ctx, uint32_t index, const void *data, size_t len);
	int (*read_piece)(void *ctx, uint32_t index, void *buf, size_t len);
	int (*have_piece)(void *ctx, uint32_t index);
	void (*close)(void *ctx);
};

/* Piece picker: returns piece index to request, or -1 */
typedef int (*mbt_picker_fn)(struct mbt_torrent *t, void *peer, void *ctx);

/* Session configuration — caller's policy choices */
struct mbt_session_config {
	struct mbt_ev *ev;

	/* defaults inherited by all torrents (can override per-torrent) */
	struct mbt_storage_ops *storage_ops;
	void *storage_ctx;
	mbt_picker_fn picker;
	void *picker_ctx;

	uint16_t listen_port;  /* 0 = don't listen for incoming */
};

/* Session lifecycle */
int mbt_session_new(struct mbt_session **out, struct mbt_session_config *cfg);
void mbt_session_free(struct mbt_session *s);

/* Event loop — only needed if caller doesn't run their own */
int mbt_session_run(struct mbt_session *s);
void mbt_session_stop(struct mbt_session *s);

/* Add torrents */
int mbt_session_add_torrent_file(struct mbt_session *s, const char *path);
int mbt_session_add_magnet(struct mbt_session *s, const char *uri);
int mbt_session_add_infohash(struct mbt_session *s, struct infohash *ih);

/* Remove/query */
int mbt_session_remove_torrent(struct mbt_session *s, struct infohash *ih);
struct mbt_torrent *mbt_session_find_torrent(struct mbt_session *s, struct infohash *ih);

/* Peer discovery — enable/disable sources */
int mbt_session_enable_lsd(struct mbt_session *s);
int mbt_session_add_peer(struct mbt_session *s, struct infohash *ih,
                         const char *addr, uint16_t port);

/* Callbacks for session-level events (optional) */
struct mbt_session_callbacks {
	void (*on_torrent_complete)(struct mbt_session *s, struct mbt_torrent *t, void *arg);
	void (*on_torrent_error)(struct mbt_session *s, struct mbt_torrent *t, int err, void *arg);
	void *arg;
};

int mbt_session_set_callbacks(struct mbt_session *s, struct mbt_session_callbacks *cb);

#endif
