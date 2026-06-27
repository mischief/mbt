// SPDX-License-Identifier: ISC
#include <stdlib.h>
#include <sys/socket.h>
#include <stdio.h>
#include <string.h>
#include <errno.h>
#include <arpa/inet.h>
#include <netinet/in.h>
#include <stdint.h>

#include "mbt/infohash.h"
#include "mbt/tracker.h"
#include "mbt/be.h"
#include "mbt/log.h"

/* URL-encode binary data (infohash, peer_id) */
static size_t
url_encode_binary(const uint8_t *in, size_t inlen, char *out, size_t outlen)
{
	size_t w = 0;
	for (size_t i = 0; i < inlen && w + 3 < outlen; i++) {
		w += snprintf(out + w, outlen - w, "%%%02X", in[i]);
	}
	return w;
}

/* Context passed to HTTP done callback */
struct http_announce_ctx {
	struct mbt_tracker_request req; /* copy of original request */
};

static void
http_announce_done(struct mbt_http_response *resp, void *arg)
{
	struct http_announce_ctx *ctx = arg;
	struct mbt_tracker_request *req = &ctx->req;

	if (!resp || !resp->data || resp->len == 0) {
		if (req->done_cb)
			req->done_cb(req->ih, -EIO, 0, req->arg);
		mbt_infohash_unref(req->ih);
		free(ctx);
		return;
	}

	/* Parse bencoded response */
	struct mbt_be_value *be = NULL;
	int rv = mbt_be_value_parse(resp->data, resp->len, &be);
	if (rv < 0) {
		free(resp->data);
		if (req->done_cb)
			req->done_cb(req->ih, -EPROTO, 0, req->arg);
		mbt_infohash_unref(req->ih);
		free(ctx);
		return;
	}

	/* Check for failure reason */
	struct mbt_be_value *fail = mbt_be_find(be, "failure reason");
	if (fail && fail->type == BE_STRING) {
		mbt_log_warn(MBT_LOG_SESSION, "tracker: %.*s",
		             (int)fail->string.len, (char *)fail->string.buf);
		mbt_be_unref(be);
		free(resp->data);
		if (req->done_cb)
			req->done_cb(req->ih, -EPROTO, 0, req->arg);
		mbt_infohash_unref(req->ih);
		free(ctx);
		return;
	}

	/* Extract interval */
	int interval = 1800;
	struct mbt_be_value *intv = mbt_be_find(be, "interval");
	if (intv && intv->type == BE_INTEGER)
		interval = (int)intv->integer;

	/* Parse compact peer list */
	struct mbt_be_value *peers = mbt_be_find(be, "peers");
	if (peers && peers->type == BE_STRING) {
		int count = (int)(peers->string.len / 6);
		uint8_t *p = peers->string.buf;

		mbt_log_info(MBT_LOG_SESSION, "tracker: %d peers, interval %ds", count, interval);

		for (int i = 0; i < count; i++) {
			char addr[INET_ADDRSTRLEN];
			uint16_t port;

			inet_ntop(AF_INET, p + i * 6, addr, sizeof(addr));
			memcpy(&port, p + i * 6 + 4, 2);
			port = ntohs(port);

			if (req->peer_cb)
				req->peer_cb(req->ih, addr, port, req->arg);
		}
	}

	/* BEP 7: peers6 (18 bytes per peer) */
	struct mbt_be_value *peers6 = mbt_be_find(be, "peers6");
	if (peers6 && peers6->type == BE_STRING) {
		int count = (int)(peers6->string.len / 18);
		uint8_t *p = peers6->string.buf;

		for (int i = 0; i < count; i++) {
			char addr[INET6_ADDRSTRLEN];
			uint16_t port;

			inet_ntop(AF_INET6, p + i * 18, addr, sizeof(addr));
			memcpy(&port, p + i * 18 + 16, 2);
			port = ntohs(port);

			if (req->peer_cb)
				req->peer_cb(req->ih, addr, port, req->arg);
		}
	}

	if (req->done_cb)
		req->done_cb(req->ih, 0, interval, req->arg);

	mbt_be_unref(be);
	free(resp->data);
	mbt_infohash_unref(req->ih);
	free(ctx);
}

int
mbt_tracker_http_announce(const char *url, struct mbt_tracker_request *req,
                          mbt_http_fn http, void *http_arg)
{
	if (!http)
		return -ENOTSUP;

	struct http_announce_ctx *ctx = calloc(1, sizeof(*ctx));
	if (!ctx) return -ENOMEM;
	ctx->req = *req;
	ctx->req.ih = mbt_infohash_ref(req->ih); /* prevent use-after-free */

	char announce_url[2048];
	char ih_enc[61], pid_enc[61];

	url_encode_binary(req->ih->hash_sha1, 20, ih_enc, sizeof(ih_enc));
	url_encode_binary(req->peer_id, 20, pid_enc, sizeof(pid_enc));

	const char *event_str = "";
	if (req->event == MBT_TRACKER_EVENT_STARTED)   event_str = "&event=started";
	else if (req->event == MBT_TRACKER_EVENT_STOPPED)   event_str = "&event=stopped";
	else if (req->event == MBT_TRACKER_EVENT_COMPLETED) event_str = "&event=completed";

	snprintf(announce_url, sizeof(announce_url),
	         "%s%cinfo_hash=%s&peer_id=%s&port=%hu"
	         "&uploaded=%llu&downloaded=%llu&left=%llu"
	         "&compact=1%s&numwant=200",
	         url,
	         strchr(url, '?') ? '&' : '?',
	         ih_enc, pid_enc, req->listen_port,
	         (unsigned long long)req->uploaded,
	         (unsigned long long)req->downloaded,
	         (unsigned long long)req->left,
	         event_str);

	struct mbt_http_request hreq = {
		.url = announce_url,
		.method = "GET",
	};

	int rv = http(&hreq, http_announce_done, ctx, http_arg);
	if (rv < 0) {
		mbt_infohash_unref(ctx->req.ih);
		free(ctx);
		return rv;
	}

	return 0;
}

/* --- Dispatch by URL scheme --- */

int
mbt_tracker_announce(const char *url, struct mbt_tracker_request *req,
                     mbt_http_fn http, void *http_arg)
{
	if (strncmp(url, "udp://", 6) == 0)
		return mbt_tracker_udp_announce(url, req);
	else if (strncmp(url, "http://", 7) == 0 || strncmp(url, "https://", 8) == 0)
		return mbt_tracker_http_announce(url, req, http, http_arg);
	else
		return -ENOTSUP;
}
