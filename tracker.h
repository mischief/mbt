#ifndef MBT_TRACKER_H
#define MBT_TRACKER_H

#include <stdint.h>

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
	mbt_tracker_peer_cb peer_cb;
	mbt_tracker_done_cb done_cb;
	void *arg;
};

/* Announce to a UDP tracker (BEP 15). Non-blocking, uses mbt_ev. */
int mbt_tracker_udp_announce(const char *url, struct mbt_tracker_request *req);

/* Announce to an HTTP tracker (BEP 3). Non-blocking. */
int mbt_tracker_http_announce(const char *url, struct mbt_tracker_request *req);

#endif
