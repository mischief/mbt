#ifndef MBT_TRACKER_H
#define MBT_TRACKER_H

#include <stdint.h>
#include <stddef.h>

struct mbt_ev;
struct mbt_infohash;

/* Callback fired for each peer returned by the tracker */
typedef void (*mbt_tracker_peer_cb)(struct mbt_infohash *ih,
                                    const char *addr, uint16_t port, void *arg);

/* Callback fired on tracker error or completion */
typedef void (*mbt_tracker_done_cb)(struct mbt_infohash *ih, int err,
                                    int interval, void *arg);

struct mbt_tracker_request {
	struct mbt_ev *ev;
	struct mbt_infohash *ih;
	const uint8_t *peer_id;   /* 20 bytes */
	uint16_t listen_port;
	uint64_t downloaded;
	uint64_t uploaded;
	uint64_t left;
	int event;                /* 0=none, 1=completed, 2=started, 3=stopped */
	mbt_tracker_peer_cb peer_cb;
	mbt_tracker_done_cb done_cb;
	void *arg;
};

#define MBT_TRACKER_EVENT_NONE      0
#define MBT_TRACKER_EVENT_COMPLETED 1
#define MBT_TRACKER_EVENT_STARTED   2
#define MBT_TRACKER_EVENT_STOPPED   3

/* --- HTTP hook (caller-provided) --- */

struct mbt_http_request {
	const char *url;
	const char *method;       /* "GET" or "POST" */
	const void *body;         /* NULL for GET */
	size_t body_len;
};

struct mbt_http_response {
	void *data;               /* malloc'd by callback, library frees */
	size_t len;
};

typedef int (*mbt_http_fn)(struct mbt_http_request *req,
                           struct mbt_http_response *resp, void *arg);

/* --- Announce functions --- */

/* UDP tracker (BEP 15). Async, uses mbt_ev internally. */
int mbt_tracker_udp_announce(const char *url, struct mbt_tracker_request *req);

/* HTTP tracker (BEP 3). Uses the provided HTTP callback (blocking). */
int mbt_tracker_http_announce(const char *url, struct mbt_tracker_request *req,
                              mbt_http_fn http, void *http_arg);

/* Dispatch: picks UDP or HTTP based on URL scheme. */
int mbt_tracker_announce(const char *url, struct mbt_tracker_request *req,
                         mbt_http_fn http, void *http_arg);

#endif
