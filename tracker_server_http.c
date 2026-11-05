// SPDX-License-Identifier: ISC
/*
 * tracker_server_http.c — BEP 3 HTTP tracker server
 *
 * Minimal HTTP/1.0 server handling GET /announce requests.
 */
#include <sys/queue.h>
#include <sys/socket.h>
#include <netinet/in.h>
#include <arpa/inet.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>
#include <errno.h>
#include <stdio.h>
#include <stdint.h>
#include <sys/types.h>

#include "mbt/tracker_server.h"
#include "mbt/be.h"
#include "mbt/event.h"
#include "mbt/log.h"
#include "net.h"
#include "parse.h"
#include "banned.h"

#define HTTP_MAX_CONNS 256
#define HTTP_CONN_TIMEOUT_USEC (10 * 1000000ULL)

#include "tracker_store.h"

struct mbt_tracker_http {
	struct mbt_ev *ev;
	struct mbt_tracker_store *store;
	int owns_store;
	struct mbt_listener listener;
	uint16_t port;
	TAILQ_HEAD(, http_conn) conns;
	int nconns;
};

/* A connection that has not sent its request line. */
struct http_conn {
	TAILQ_ENTRY(http_conn) entries;
	struct mbt_tracker_http *th;
	int fd;
	struct mbt_ev_io *io;
	struct mbt_ev_timer *timer;
	struct sockaddr_storage from;
	size_t len;
	char buf[4096];
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

/* buf holds a NUL-terminated request line. The caller closes cfd. */
static void
handle_request(struct mbt_tracker_http *th, int cfd, struct sockaddr_storage *from, char *buf)
{
	/* Parse: GET /announce?... HTTP/1.x */
	if (strncmp(buf, "GET /announce?", 14) != 0) {
		const char *err = "HTTP/1.0 404 Not Found\r\nContent-Length: 0\r\n\r\n";
		send(cfd, err, strlen(err), MSG_NOSIGNAL);
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
	long long num;
	uint16_t port_v;
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
			peer_port = mbt_parse_port(val, '\0', &port_v) == 0 ? htons(port_v) : 0;
		else if (strcmp(key, "left") == 0)
			left = mbt_parse_int(val, 0, INT64_MAX, '\0', &num) == 0 ? num : -1;
		else if (strcmp(key, "compact") == 0)
			compact = mbt_parse_int(val, 0, 1, '\0', &num) == 0 ? (int)num : 1;
		else if (strcmp(key, "numwant") == 0)
			numwant = mbt_parse_int(val, 0, 50, '\0', &num) == 0 ? (int)num : 50;
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
	if (!t) return;

	int is_seeder = (left == 0);
	mbt_tracker_store_add_peer(th->store, t, &peer_addr, is_seeder);

	/* Build bencoded response */
	_cleanup_be_ struct mbt_be_value *resp = NULL;
	if (mbt_be_dict_new(&resp) < 0) return;
	mbt_be_dict_append_number(resp, "interval", 120);

	if (compact) {
		uint8_t peers_compact[300]; /* 50 * 6 */
		int count = mbt_tracker_store_get_peers_v4(t, &peer_addr, peers_compact, numwant);
		mbt_be_dict_append_string(resp, "peers", peers_compact, count * 6);
	}

	unsigned char *enc; size_t enc_len;
	if (mbt_be_value_encode(resp, &enc, &enc_len) < 0) return;

	/* Send HTTP response */
	char hdr[128];
	int hdr_len = snprintf(hdr, sizeof(hdr),
		"HTTP/1.0 200 OK\r\nContent-Type: text/plain\r\nContent-Length: %zu\r\n\r\n", enc_len);
	send(cfd, hdr, hdr_len, MSG_NOSIGNAL);
	send(cfd, enc, enc_len, MSG_NOSIGNAL);
	free(enc);
}

static void
conn_free(struct http_conn *c)
{
	TAILQ_REMOVE(&c->th->conns, c, entries);
	c->th->nconns--;
	mbt_ev_timer_free(c->timer);
	mbt_ev_io_free(c->io);
	close(c->fd);
	free(c);
}

static void
conn_timeout(struct mbt_ev_timer *timer, void *v)
{
	(void)timer;
	conn_free(v);
}

/* Read until the request line ends. A client must not stall the server. */
static void
conn_read(struct mbt_ev_io *io, int fd, uint32_t events, void *v)
{
	struct http_conn *c = v;
	ssize_t n;

	(void)io; (void)events;
	n = recv(fd, c->buf + c->len, sizeof(c->buf) - 1 - c->len, 0);
	if (n < 0 && (errno == EAGAIN || errno == EWOULDBLOCK || errno == EINTR))
		return;
	if (n <= 0) {
		conn_free(c);
		return;
	}
	c->len += (size_t)n;
	c->buf[c->len] = '\0';
	if (memchr(c->buf, '\n', c->len) == NULL && c->len < sizeof(c->buf) - 1)
		return;
	handle_request(c->th, c->fd, &c->from, c->buf);
	conn_free(c);
}

static void
http_accept_cb(struct mbt_ev_io *io, int fd, uint32_t events, void *v)
{
	struct mbt_tracker_http *th = v;
	struct sockaddr_storage from;
	socklen_t fromlen = sizeof(from);
	(void)io; (void)events;

	int cfd = accept4(fd, (struct sockaddr *)&from, &fromlen, SOCK_CLOEXEC | SOCK_NONBLOCK);
	if (cfd < 0) return;

	struct http_conn *c = th->nconns < HTTP_MAX_CONNS ? calloc(1, sizeof(*c)) : NULL;
	if (!c) {
		close(cfd);
		return;
	}
	c->th = th;
	c->fd = cfd;
	c->from = from;
	if (mbt_ev_io_new(th->ev, &c->io, cfd, MBT_EV_READ, conn_read, c) < 0) {
		close(cfd);
		free(c);
		return;
	}
	if (mbt_ev_timer_new(th->ev, &c->timer, HTTP_CONN_TIMEOUT_USEC, conn_timeout, c) < 0) {
		mbt_ev_io_free(c->io);
		close(cfd);
		free(c);
		return;
	}
	TAILQ_INSERT_TAIL(&th->conns, c, entries);
	th->nconns++;
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
	TAILQ_INIT(&th->conns);

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
	while (!TAILQ_EMPTY(&th->conns))
		conn_free(TAILQ_FIRST(&th->conns));
	if (th->store && th->owns_store) mbt_tracker_store_free(th->store);
	free(th);
}

int mbt_tracker_http_num_torrents(struct mbt_tracker_http *th) { return mbt_tracker_store_num_torrents(th->store); }
int mbt_tracker_http_num_peers(struct mbt_tracker_http *th) { return mbt_tracker_store_num_peers(th->store); }
