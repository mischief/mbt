#include <stdlib.h>
#include <stdio.h>
#include <string.h>
#include <errno.h>
#include <arpa/inet.h>

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

int
mbt_tracker_http_announce(const char *url, struct mbt_tracker_request *req,
                          mbt_http_fn http, void *http_arg)
{
	char announce_url[2048];
	char ih_enc[61], pid_enc[61];
	int rv;

	if (!http)
		return -ENOTSUP;

	url_encode_binary(req->ih->hash_sha1, 20, ih_enc, sizeof(ih_enc));
	url_encode_binary(req->peer_id, 20, pid_enc, sizeof(pid_enc));

	snprintf(announce_url, sizeof(announce_url),
	         "%s%cinfo_hash=%s&peer_id=%s&port=%hu"
	         "&uploaded=%llu&downloaded=%llu&left=%llu"
	         "&compact=1&event=started&numwant=200",
	         url,
	         strchr(url, '?') ? '&' : '?',
	         ih_enc, pid_enc, req->listen_port,
	         (unsigned long long)req->uploaded,
	         (unsigned long long)req->downloaded,
	         (unsigned long long)req->left);

	struct mbt_http_request hreq = {
		.url = announce_url,
		.method = "GET",
	};
	struct mbt_http_response hresp = {0};

	rv = http(&hreq, &hresp, http_arg);
	if (rv < 0) {
		if (req->done_cb)
			req->done_cb(req->ih, rv, 0, req->arg);
		return rv;
	}

	/* Parse bencoded response */
	struct mbt_be_value *be = NULL;
	rv = mbt_be_value_parse(hresp.data, hresp.len, &be);
	if (rv < 0) {
		free(hresp.data);
		if (req->done_cb)
			req->done_cb(req->ih, -EPROTO, 0, req->arg);
		return rv;
	}

	/* Check for failure reason */
	struct mbt_be_value *fail = mbt_be_find(be, "failure reason");
	if (fail && fail->type == BE_STRING) {
		mbt_log_warn(MBT_LOG_SESSION, "tracker: %.*s",
		             (int)fail->string.iov_len, (char *)fail->string.iov_base);
		mbt_be_value_free(be);
		free(hresp.data);
		if (req->done_cb)
			req->done_cb(req->ih, -EPROTO, 0, req->arg);
		return -EPROTO;
	}

	/* Extract interval */
	int interval = 1800;
	struct mbt_be_value *intv = mbt_be_find(be, "interval");
	if (intv && intv->type == BE_INTEGER)
		interval = (int)intv->integer;

	/* Parse compact peer list */
	struct mbt_be_value *peers = mbt_be_find(be, "peers");
	if (peers && peers->type == BE_STRING) {
		/* Compact: 6 bytes per peer (4 IP + 2 port) */
		int count = (int)(peers->string.iov_len / 6);
		uint8_t *p = peers->string.iov_base;

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

	if (req->done_cb)
		req->done_cb(req->ih, 0, interval, req->arg);

	mbt_be_value_free(be);
	free(hresp.data);
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
