// SPDX-License-Identifier: ISC
#ifndef MBT_SESSION_H
#define MBT_SESSION_H

#include "export.h"

#include <stdint.h>
#include <stddef.h>

#include "peer_source.h"
#include "storage.h"
#include "picker.h"
#include "tracker.h"

struct mbt_session;
struct mbt_ev;
struct mbt_torrent;
struct mbt_infohash;
struct mbt_state_ops;

#include "timers.h"

/* Session configuration — caller's policy choices */
struct mbt_add_torrent_cfg {
	struct mbt_storage storage; /* zero = use session default_storage */
	const char *data_dir;       /* passed to mbt_torrent_data_dir(); NULL = unknown */
};

struct mbt_session_config {
	struct mbt_ev *ev;

	/* default storage inherited by all torrents; override per-torrent via mbt_add_torrent_cfg */
	struct mbt_storage default_storage;
	const char *default_data_dir; /* passed to mbt_torrent_data_dir() for all torrents */

	/* optional persistence layer */
	const struct mbt_state_ops *state_ops;
	void *state_ctx;

	mbt_picker_fn picker;
	void *picker_ctx;

	uint16_t listen_port;  /* 0 = don't listen for incoming */
	int max_peers;         /* max peers per torrent (0 = default 30) */
	int max_pieces_per_peer; /* active pieces per peer (0 = default 4) */
	int min_peers_before_download; /* hold off requesting until this many peers connected (0 = off) */
	int reqq;              /* max outstanding requests we accept (0 = don't advertise) */
	int max_pipeline;      /* max in-flight requests per peer (0 = default 512) */
	int mbt_lsd_announce;      /* if nonzero, periodically multicast our infohashes via LSD */
	int enable_pcp;        /* if nonzero, map listen_port via PCP (RFC 6887) */
	const char *pcp_gateway; /* NULL = auto-detect */
	mbt_http_fn http;      /* HTTP callback for HTTP/HTTPS trackers (NULL = disabled) */
	void *http_arg;

	struct mbt_timers timers;  /* 0-initialized = all defaults */

	uint64_t upload_rate;    /* bytes/sec, 0 = unlimited */
	uint64_t download_rate;  /* bytes/sec, 0 = unlimited */
};

/* Session lifecycle */
MBT_API int mbt_session_new(struct mbt_session **out, struct mbt_session_config *cfg);
MBT_API void mbt_session_free(struct mbt_session *s);

/*
 * Sensible defaults: platform event backend, file storage at `path`,
 * random piece picker, listen on port 6881.
 */
MBT_API int mbt_session_default(struct mbt_session **out, const char *path);

/* Event loop — only needed if caller doesn't run their own */
MBT_API int mbt_session_run(struct mbt_session *s);
MBT_API void mbt_session_stop(struct mbt_session *s);

/* Add torrents — cfg may be NULL for defaults */
MBT_API int mbt_session_add_torrent_file(struct mbt_session *s, const char *path,
                                          struct mbt_add_torrent_cfg *cfg);
MBT_API int mbt_session_add_torrent_buf(struct mbt_session *s, const void *data, size_t len,
                                         struct mbt_add_torrent_cfg *cfg);
MBT_API int mbt_session_add_magnet(struct mbt_session *s, const char *uri,
                                    struct mbt_add_torrent_cfg *cfg);
MBT_API int mbt_session_add_infohash(struct mbt_session *s, struct mbt_infohash *ih,
                                      struct mbt_add_torrent_cfg *cfg);

/* Remove/query */
MBT_API int mbt_session_remove_torrent(struct mbt_session *s, struct mbt_infohash *ih);
MBT_API struct mbt_torrent *mbt_session_find_torrent(struct mbt_session *s, struct mbt_infohash *ih);
MBT_API int mbt_session_torrent_count(struct mbt_session *s);
MBT_API struct mbt_torrent *mbt_session_torrent_first(struct mbt_session *s);
MBT_API struct mbt_torrent *mbt_session_torrent_next(struct mbt_session *s, struct mbt_torrent *after);
MBT_API int mbt_session_torrent_tracker_count(struct mbt_session *s, struct mbt_torrent *t);
MBT_API const char *mbt_session_torrent_tracker(struct mbt_session *s, struct mbt_torrent *t, int idx);

/* Peer discovery — enable/disable sources */
MBT_API int mbt_session_enable_lsd(struct mbt_session *s);
MBT_API void mbt_session_disable_lsd(struct mbt_session *s);
MBT_API int mbt_session_enable_dht(struct mbt_session *s);
MBT_API int mbt_session_enable_dht_local(struct mbt_session *s);
MBT_API void mbt_session_disable_dht(struct mbt_session *s);
MBT_API int mbt_session_dht_bootstrap(struct mbt_session *s, const char *addr, uint16_t port);
MBT_API void mbt_session_dht_get_peers(struct mbt_session *s, struct mbt_infohash *ih);
MBT_API uint16_t mbt_session_dht_port(struct mbt_session *s);
MBT_API uint16_t mbt_session_listen_port(struct mbt_session *s);
MBT_API int mbt_session_add_peer(struct mbt_session *s, struct mbt_infohash *ih,
                         const char *addr, uint16_t port, enum mbt_peer_source source);

/* Manually trigger re-announce to all trackers (or specific torrent if ih != NULL) */
MBT_API int mbt_session_reannounce(struct mbt_session *s, struct mbt_infohash *ih);

/* Callbacks for session-level events (optional) */
struct mbt_session_callbacks {
	void (*on_torrent_complete)(struct mbt_session *s, struct mbt_torrent *t, void *arg);
	void (*on_torrent_error)(struct mbt_session *s, struct mbt_torrent *t, int err, void *arg);
	/* LSD: called for each discovered infohash. Return 1 to add torrent + connect, 0 to ignore. */
	int (*on_lsd_announce)(struct mbt_session *s, struct mbt_infohash *ih,
	                       const char *addr, uint16_t port, void *arg);
	void *arg;
};

MBT_API int mbt_session_set_callbacks(struct mbt_session *s, struct mbt_session_callbacks *cb);

/* Rate limiting — 0 = unlimited, adjustable at runtime */
MBT_API void mbt_session_set_upload_rate(struct mbt_session *s, uint64_t bytes_per_sec);
MBT_API void mbt_session_set_download_rate(struct mbt_session *s, uint64_t bytes_per_sec);
MBT_API uint64_t mbt_session_upload_rate(struct mbt_session *s);
MBT_API uint64_t mbt_session_download_rate(struct mbt_session *s);

#endif
