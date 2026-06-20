// SPDX-License-Identifier: ISC
/*
 * tracker_server_http.c — BEP 3 HTTP tracker server
 *
 * Minimal HTTP/1.0 server handling GET /announce requests.
 */
#include <sys/socket.h>
#include <netinet/in.h>
#include <arpa/inet.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>
#include <errno.h>
#include <fcntl.h>
#include <stdio.h>

#include "mbt/tracker_server.h"
#include "mbt/event.h"
#include "mbt/be.h"
#include "mbt/log.h"
#include "net.h"

/* Forward declarations from tracker_store.c */
struct ts_torrent;
struct ts_torrent *mbt_tracker_store_find(struct mbt_tracker_store *s, const uint8_t *ih);
struct ts_torrent *mbt_tracker_store_get_or_create(struct mbt_tracker_store *s, const uint8_t *ih);
void mbt_tracker_store_add_peer(struct mbt_tracker_store *s, struct ts_torrent *t,
                                const struct sockaddr_storage *addr, int is_seeder);
int mbt_tracker_store_get_peers_v4(struct ts_torrent *t, const struct sockaddr_storage *exclude,
                                uint8_t *compact_out, int max_peers);

struct mbt_tracker_http {
	struct mbt_ev *ev;
	struct mbt_tracker_store *store;
	int owns_store;
	struct mbt_listener listener;
	uint16_t port;
};

/* URL-decode info_hash (20 raw bytes from percent-encoded query) */
static int
urldecode_infohash(const char *s, uint8_t *out)
{
	int n = 0;
	while (*s && n < 20) {
		if (*s == '%' && s[1] && s[2]) {
			char hex[3] = { s[1], s[2], 0 };
			out[n++] = (uint8_t)strtoul(hex, NULL, 16);
			s += 3;
		} else {
			out[n++] = (uint8_t)*s++;
		}
	}
	return (n == 20) ? 0 : -1;
}

static void
handle_request(struct mbt_tracker_http *th, int cfd, struct sockaddr_storage *from)
{
	char buf[4096];
	ssize_t n = recv(cfd, buf, sizeof(buf) - 1, 0);
	if (n <= 0) { close(cfd); return; }
	buf[n] = '\0';

	/* Parse: GET /announce?... HTTP/1.x */
	if (strncmp(buf, "GET /announce?", 14) != 0) {
		const char *err = "HTTP/1.0 404 Not Found\r\nContent-Length: 0\r\n\r\n";
		send(cfd, err, strlen(err), MSG_NOSIGNAL);
		close(cfd);
		return;
	}

	char *qs = buf + 14;
	char *end = strchr(qs, ' ');
	if (end) *end = '\0';

	/* Parse query params */
	uint8_t info_hash[20] = {0};
	uint16_t peer_port = 0;
	int64_t left = -1;
	int compact = 1;
	int numwant = 50;

	char *param, *saveptr;
	char *qs_copy = strdup(qs);
	for (param = strtok_r(qs_copy, "&", &saveptr); param;
	     param = strtok_r(NULL, "&", &saveptr)) {
		char *eq = strchr(param, '=');
		if (!eq) continue;
		*eq = '\0';
		char *key = param, *val = eq + 1;

		if (strcmp(key, "info_hash") == 0)
			urldecode_infohash(val, info_hash);
		else if (strcmp(key, "port") == 0)
			peer_port = htons((uint16_t)atoi(val));
		else if (strcmp(key, "left") == 0)
			left = atoll(val);
		else if (strcmp(key, "compact") == 0)
			compact = atoi(val);
		else if (strcmp(key, "numwant") == 0)
			numwant = atoi(val);
	}
	free(qs_copy);

	if (numwant <= 0 || numwant > 50) numwant = 50;

	/* Store peer */
	struct sockaddr_storage peer_addr = *from;
	if (peer_port) {
		if (peer_addr.ss_family == AF_INET6)
			((struct sockaddr_in6 *)&peer_addr)->sin6_port = peer_port;
		else
			((struct sockaddr_in *)&peer_addr)->sin_port = peer_port;
	}

	struct ts_torrent *t = mbt_tracker_store_get_or_create(th->store, info_hash);
	if (!t) { close(cfd); return; }

	int is_seeder = (left == 0);
	mbt_tracker_store_add_peer(th->store, t, &peer_addr, is_seeder);

	/* Build bencoded response */
	_cleanup_be_ struct mbt_be_value *resp = NULL;
	if (mbt_be_dict_new(&resp) < 0) { close(cfd); return; }
	mbt_be_dict_append_number(resp, "interval", 120);

	if (compact) {
		uint8_t peers_compact[300]; /* 50 * 6 */
		int count = mbt_tracker_store_get_peers_v4(t, &peer_addr, peers_compact, numwant);
		mbt_be_dict_append_string(resp, "peers", peers_compact, count * 6);
	}

	unsigned char *enc; size_t enc_len;
	if (mbt_be_value_encode(resp, &enc, &enc_len) < 0) { close(cfd); return; }

	/* Send HTTP response */
	char hdr[128];
	int hdr_len = snprintf(hdr, sizeof(hdr),
		"HTTP/1.0 200 OK\r\nContent-Type: text/plain\r\nContent-Length: %zu\r\n\r\n", enc_len);
	send(cfd, hdr, hdr_len, MSG_NOSIGNAL);
	send(cfd, enc, enc_len, MSG_NOSIGNAL);
	free(enc);
	close(cfd);
}

static void
http_accept_cb(struct mbt_ev_io *io, int fd, uint32_t events, void *v)
{
	struct mbt_tracker_http *th = v;
	struct sockaddr_storage from;
	socklen_t fromlen = sizeof(from);
	(void)io; (void)events;

	int cfd = accept4(fd, (struct sockaddr *)&from, &fromlen, SOCK_CLOEXEC);
	if (cfd < 0) return;

	handle_request(th, cfd, &from);
}

/* --- Public API --- */

int
mbt_tracker_http_new(struct mbt_tracker_http **out, struct mbt_ev *ev, uint16_t port, struct mbt_tracker_store *store)
{
	struct mbt_tracker_http *th = calloc(1, sizeof(*th));
	if (!th) return -ENOMEM;

	th->ev = ev;
	if (store) { th->store = store; th->owns_store = 0; } else { mbt_tracker_store_new(&th->store); th->owns_store = 1; }
	th->port = port;

	int rv = mbt_listen_tcp(&th->listener, ev, port, http_accept_cb, th);
	if (rv < 0) { free(th); return rv; }

	mbt_log_info(MBT_LOG_SESSION, "HTTP tracker listening on port %hu", port);
	*out = th;
	return 0;
}

void
mbt_tracker_http_free(struct mbt_tracker_http *th)
{
	if (!th) return;
	mbt_listener_close(&th->listener);
	if (th->store && th->owns_store) mbt_tracker_store_free(th->store);
	free(th);
}

int mbt_tracker_http_num_torrents(struct mbt_tracker_http *th) { return mbt_tracker_store_num_torrents(th->store); }
int mbt_tracker_http_num_peers(struct mbt_tracker_http *th) { return mbt_tracker_store_num_peers(th->store); }
