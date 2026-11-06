// SPDX-License-Identifier: ISC
#ifndef MBT_WEBSEED_H
#define MBT_WEBSEED_H

#include <stddef.h>
#include <stdint.h>

/* BEP 19 web seeds. See mbt_webseed(3). */

enum mbt_webseed_mode {
	MBT_WEBSEED_DEFAULT = 0, /* torrent: the session policy. session: FALLBACK with defaults */
	MBT_WEBSEED_OFF,
	MBT_WEBSEED_FALLBACK,    /* use a web seed when peers do not supply the data */
	MBT_WEBSEED_ALWAYS,
};

/* With a mode other than DEFAULT, each field is used as given. 0 is valid. */
struct mbt_webseed_policy {
	enum mbt_webseed_mode mode;
	uint32_t no_peer_delay_ms;  /* start after this long with no connected peer */
	uint32_t low_rate_delay_ms; /* or after this long below low_rate */
	uint64_t low_rate;          /* bytes per second from peers */
};

/* One byte range of the file at url. total is the file size. */
struct mbt_http_range_request {
	const char *url;
	uint64_t offset;
	uint64_t length;
	uint64_t total;
};

struct mbt_http_range;

/* Delivers bytes in order. A nonzero return stops the transfer. */
typedef int (*mbt_http_data_fn)(const void *data, size_t len, void *arg);

/*
 * status is 206 after all bytes of the range, a negative errno, or another
 * HTTP status. retry_after is a Retry-After value in seconds, or 0.
 * Runs once, unless the caller cancels first. It can start a new request.
 */
typedef void (*mbt_http_range_done_fn)(int status, unsigned retry_after, void *arg);

/*
 * Start a transfer. On success, *out identifies it until done runs. Accept a
 * reply only if it holds the exact range: a 206 with a matching Content-Range,
 * or a 200 if the range is the whole file. Set no limit on total time.
 */
typedef int (*mbt_http_range_fn)(const struct mbt_http_range_request *req,
                                 mbt_http_data_fn data, mbt_http_range_done_fn done,
                                 void *cb_arg, void *hook_arg, struct mbt_http_range **out);

/* Stop a transfer. No callback runs after this. A data callback must not call it. */
typedef void (*mbt_http_range_cancel_fn)(struct mbt_http_range *r, void *hook_arg);

#endif
