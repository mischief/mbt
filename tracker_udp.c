// SPDX-License-Identifier: ISC
#include <sys/socket.h>
#include <fcntl.h>
#include <netinet/in.h>
#include <arpa/inet.h>
#include <unistd.h>
#include <stdlib.h>
#include <string.h>
#include <errno.h>
#include "mbt/endian.h"
#include <stdint.h>
#include <sys/types.h>

#include "mbt/event.h"
#include "mbt/infohash.h"
#include "mbt/tracker.h"
#include "mbt/resolve.h"
#include "internal.h"
#include "mbt/log.h"

struct mbt_ev_io;
struct mbt_ev_timer;

/* BEP 15 constants */
#define UDP_TRACKER_MAGIC 0x41727101980ULL
enum {
	ACTION_CONNECT  = 0,
	ACTION_ANNOUNCE = 1,
	EVENT_STARTED   = 2,
};

enum udp_state {
	STATE_CONNECT_SENT,
	STATE_ANNOUNCE_SENT,
	STATE_DONE,
};

struct udp_tracker {
	struct mbt_ev *ev;
	struct mbt_ev_io *io;
	struct mbt_ev_timer *timer;
	int fd;

	enum udp_state state;
	uint32_t transaction_id;
	uint64_t connection_id;

	struct mbt_tracker_request req;
	struct sockaddr_storage addr;
	socklen_t addrlen;
};

static void udp_on_read(struct mbt_ev_io *io, int fd, uint32_t events, void *v);
static void udp_on_timeout(struct mbt_ev_timer *timer, void *v);

static void
udp_cleanup(struct udp_tracker *ut)
{
	if (ut->io) mbt_ev_io_free(ut->io);
	if (ut->timer) mbt_ev_timer_free(ut->timer);
	if (ut->fd >= 0) close(ut->fd);
	free(ut);
}

static int
udp_send_connect(struct udp_tracker *ut)
{
	uint8_t buf[16];
	uint64_t magic = htobe64(UDP_TRACKER_MAGIC);
	uint32_t action = htobe32(ACTION_CONNECT);

	ut->transaction_id = mbt_random32();
	uint32_t tid = htobe32(ut->transaction_id);

	memcpy(buf, &magic, 8);
	memcpy(buf + 8, &action, 4);
	memcpy(buf + 12, &tid, 4);

	ssize_t rv = sendto(ut->fd, buf, 16, 0,
	                    (struct sockaddr *)&ut->addr, ut->addrlen);
	if (rv < 0)
		return -errno;

	ut->state = STATE_CONNECT_SENT;
	return 0;
}

static int
udp_send_announce(struct udp_tracker *ut)
{
	uint8_t buf[98];
	memset(buf, 0, sizeof(buf));

	uint64_t cid = htobe64(ut->connection_id);
	uint32_t action = htobe32(ACTION_ANNOUNCE);
	ut->transaction_id = mbt_random32();
	uint32_t tid = htobe32(ut->transaction_id);

	memcpy(buf, &cid, 8);                         /* connection_id */
	memcpy(buf + 8, &action, 4);                   /* action */
	memcpy(buf + 12, &tid, 4);                     /* transaction_id */
	memcpy(buf + 16, ut->req.ih->hash_sha1, 20);  /* info_hash */
	memcpy(buf + 36, ut->req.peer_id, 20);        /* peer_id */

	uint64_t dl = htobe64(ut->req.downloaded);
	uint64_t left = htobe64(ut->req.left);
	uint64_t ul = htobe64(ut->req.uploaded);
	memcpy(buf + 56, &dl, 8);                     /* downloaded */
	memcpy(buf + 64, &left, 8);                   /* left */
	memcpy(buf + 72, &ul, 8);                     /* uploaded */

	uint32_t event = htobe32(ut->req.event != 0 ? (uint32_t)ut->req.event : (uint32_t)EVENT_STARTED);
	memcpy(buf + 80, &event, 4);                  /* event */
	/* buf[84..87] = IP (0 = default) */
	/* buf[88..91] = key (random) */
	uint32_t key = htobe32(mbt_random32());
	memcpy(buf + 88, &key, 4);

	int32_t numwant = htobe32(200);
	memcpy(buf + 92, &numwant, 4);                /* num_want */

	uint16_t port = htobe16(ut->req.listen_port);
	memcpy(buf + 96, &port, 2);                   /* port */

	ssize_t rv = sendto(ut->fd, buf, 98, 0,
	                    (struct sockaddr *)&ut->addr, ut->addrlen);
	if (rv < 0)
		return -errno;

	ut->state = STATE_ANNOUNCE_SENT;
	return 0;
}

static void
udp_on_read(struct mbt_ev_io *io, int fd, uint32_t events, void *v)
{
	struct udp_tracker *ut = v;
	uint8_t buf[4096];
	ssize_t n;

	(void)io;
	(void)events;

	n = recvfrom(fd, buf, sizeof(buf), 0, NULL, NULL);
	if (n < 0)
		return;

	if (ut->state == STATE_CONNECT_SENT) {
		if (n < 16) return;

		uint32_t action, tid;
		memcpy(&action, buf, 4);
		memcpy(&tid, buf + 4, 4);
		action = be32toh(action);
		tid = be32toh(tid);

		if (action != ACTION_CONNECT || tid != ut->transaction_id)
			return;

		memcpy(&ut->connection_id, buf + 8, 8);
		ut->connection_id = be64toh(ut->connection_id);

		mbt_log_debug(MBT_LOG_SESSION, "tracker: connect OK, sending announce");
		udp_send_announce(ut);

		/* Reset timeout */
		mbt_ev_timer_mod(ut->timer, 15 * 1000000ULL);

	} else if (ut->state == STATE_ANNOUNCE_SENT) {
		if (n < 20) return;

		uint32_t action, tid, interval, leechers, seeders;
		memcpy(&action, buf, 4);
		memcpy(&tid, buf + 4, 4);
		action = be32toh(action);
		tid = be32toh(tid);

		if (action != ACTION_ANNOUNCE || tid != ut->transaction_id) {
			/* Check for error (action=3) */
			if (action == 3 && n > 8) {
				mbt_log_warn(MBT_LOG_SESSION, "tracker error: %.*s",
				             (int)(n - 8), buf + 8);
			}
			if (ut->req.done_cb)
				ut->req.done_cb(ut->req.ih, -EPROTO, 0, ut->req.arg);
			udp_cleanup(ut);
			return;
		}

		memcpy(&interval, buf + 8, 4);
		memcpy(&leechers, buf + 12, 4);
		memcpy(&seeders, buf + 16, 4);
		interval = be32toh(interval);
		leechers = be32toh(leechers);
		seeders = be32toh(seeders);

		/* Parse compact peer list — BEP 7: 18 bytes/peer if IPv6, 6 if IPv4 */
		int peer_size = (ut->addr.ss_family == AF_INET6) ? 18 : 6;
		int peer_count = (n - 20) / peer_size;
		mbt_log_info(MBT_LOG_SESSION, "tracker: %d peers, %u seeders, %u leechers, interval %us",
		             peer_count, seeders, leechers, interval);

		for (int i = 0; i < peer_count; i++) {
			uint8_t *p = buf + 20 + i * peer_size;
			char addr[INET6_ADDRSTRLEN];
			uint16_t port;

			if (peer_size == 18) {
				inet_ntop(AF_INET6, p, addr, sizeof(addr));
				memcpy(&port, p + 16, 2);
			} else {
				inet_ntop(AF_INET, p, addr, sizeof(addr));
				memcpy(&port, p + 4, 2);
			}
			port = be16toh(port);

			if (ut->req.peer_cb)
				ut->req.peer_cb(ut->req.ih, addr, port, ut->req.arg);
		}

		if (ut->req.done_cb)
			ut->req.done_cb(ut->req.ih, 0, (int)interval, ut->req.arg);

		ut->state = STATE_DONE;
		udp_cleanup(ut);
	}
}

static void
udp_on_timeout(struct mbt_ev_timer *timer, void *v)
{
	struct udp_tracker *ut = v;
	(void)timer;

	mbt_log_warn(MBT_LOG_SESSION, "tracker: timeout");
	if (ut->req.done_cb)
		ut->req.done_cb(ut->req.ih, -ETIMEDOUT, 0, ut->req.arg);
	udp_cleanup(ut);
}

/* Parse "udp://host:port/announce" or "udp://[ipv6]:port/announce" */
static int
parse_udp_url(const char *url, char *host, size_t hostlen, uint16_t *port)
{
	if (strncmp(url, "udp://", 6) != 0)
		return -EINVAL;

	const char *h = url + 6;
	const char *colon;
	size_t hlen;

	if (h[0] == '[') {
		/* IPv6 bracketed: [::1]:port */
		const char *end = strchr(h, ']');
		if (!end || end[1] != ':') return -EINVAL;
		hlen = (size_t)(end - h - 1);
		if (hlen >= hostlen) return -ENAMETOOLONG;
		memcpy(host, h + 1, hlen);
		host[hlen] = '\0';
		*port = (uint16_t)atoi(end + 2);
	} else {
		/* IPv4 or hostname: host:port */
		const char *slash = strchr(h, '/');
		colon = strrchr(h, ':');
		if (!colon) return -EINVAL;
		if (slash && colon > slash) return -EINVAL;
		hlen = (size_t)(colon - h);
		if (hlen >= hostlen) return -ENAMETOOLONG;
		memcpy(host, h, hlen);
		host[hlen] = '\0';
		*port = (uint16_t)atoi(colon + 1);
	}
	return 0;
}

/* Context for async DNS → UDP announce */
struct udp_announce_ctx {
	char host[256];
	uint16_t port;
	struct mbt_tracker_request req;
};

static void
udp_announce_resolved(struct sockaddr_storage *addr, socklen_t len, int err, void *arg)
{
	struct udp_announce_ctx *ctx = arg;
	struct udp_tracker *ut;
	int rv;

	if (err < 0) {
		if (ctx->req.done_cb)
			ctx->req.done_cb(ctx->req.ih, err, 0, ctx->req.arg);
		free(ctx);
		return;
	}

	ut = calloc(1, sizeof(*ut));
	if (!ut) {
		if (ctx->req.done_cb)
			ctx->req.done_cb(ctx->req.ih, -ENOMEM, 0, ctx->req.arg);
		free(ctx);
		return;
	}

	ut->ev = ctx->req.ev;
	ut->req = ctx->req;
	ut->fd = -1;

	/* Set up destination address */
	memcpy(&ut->addr, addr, len);
	ut->addrlen = len;
	if (ut->addr.ss_family == AF_INET6)
		((struct sockaddr_in6 *)&ut->addr)->sin6_port = htons(ctx->port);
	else
		((struct sockaddr_in *)&ut->addr)->sin_port = htons(ctx->port);

	/* Create UDP socket matching resolved address family */
	ut->fd = socket(ut->addr.ss_family, SOCK_DGRAM, 0);
	if (ut->fd >= 0)
		fcntl(ut->fd, F_SETFL, fcntl(ut->fd, F_GETFL) | O_NONBLOCK);
	if (ut->fd < 0) {
		free(ut); free(ctx);
		return;
	}

	/* Watch for responses */
	rv = mbt_ev_io_new(ut->ev, &ut->io, ut->fd, MBT_EV_READ, udp_on_read, ut);
	if (rv < 0) {
		close(ut->fd); free(ut); free(ctx);
		return;
	}

	/* Timeout */
	rv = mbt_ev_timer_new(ut->ev, &ut->timer, 15 * 1000000ULL, udp_on_timeout, ut);
	if (rv < 0) {
		mbt_ev_io_free(ut->io); close(ut->fd); free(ut); free(ctx);
		return;
	}

	/* Send connect */
	rv = udp_send_connect(ut);
	if (rv < 0) {
		udp_cleanup(ut); free(ctx);
		return;
	}

	mbt_log_debug(MBT_LOG_SESSION, "tracker: UDP connect sent to %s:%hu", ctx->host, ctx->port);
	free(ctx);
}

int
mbt_tracker_udp_announce(const char *url, struct mbt_tracker_request *req)
{
	char host[256];
	uint16_t port;
	int rv;

	rv = parse_udp_url(url, host, sizeof(host), &port);
	if (rv < 0)
		return rv;

	struct udp_announce_ctx *ctx = calloc(1, sizeof(*ctx));
	if (!ctx) return -ENOMEM;
	memcpy(ctx->host, host, sizeof(ctx->host));
	ctx->port = port;
	ctx->req = *req;

	rv = mbt_resolve(req->ev, host, port, udp_announce_resolved, ctx, NULL);
	if (rv < 0) { free(ctx); return rv; }
	return 0;
}
