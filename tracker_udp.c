// SPDX-License-Identifier: ISC
#include <sys/queue.h>
#include <sys/socket.h>
#include <fcntl.h>
#include <netinet/in.h>
#include <arpa/inet.h>
#include <unistd.h>
#include <stdlib.h>
#include <string.h>
#include <errno.h>
#include <time.h>
#include "mbt/endian.h"
#include <stdbool.h>
#include <stdint.h>
#include <sys/types.h>

#include "mbt/event.h"
#include "mbt/infohash.h"
#include "mbt/tracker.h"
#include "mbt/resolve.h"
#include "internal.h"
#include "udp_mux.h"
#include "mbt/log.h"
#include "parse.h"
#include "mbt/useful.h"
#include "banned.h"

struct mbt_ev_io;
struct mbt_ev_timer;

/* BEP 15 constants */
#define UDP_TRACKER_MAGIC 0x41727101980ULL
enum {
	ACTION_CONNECT  = 0,
	ACTION_ANNOUNCE = 1,
	ACTION_ERROR    = 3,
	EVENT_STARTED   = 2,
};

/* BEP 15: a client can use a connection ID for one minute. */
#define CONN_ID_USEC (60 * 1000000ULL)
#define UDP_TIMEOUT_USEC (15 * 1000000ULL)

enum udp_state {
	STATE_WAIT_CONNECT,       /* another announce to the same tracker connects */
	STATE_CONNECT_SENT,
	STATE_ANNOUNCE_SENT,
	STATE_DONE,
};

struct udp_tracker {
	TAILQ_ENTRY(udp_tracker) entries;
	struct mbt_tracker_group *group;
	uint8_t peer_id[20];
	struct mbt_ev *ev;
	struct mbt_ev_timer *timer;

	enum udp_state state;
	uint32_t transaction_id;
	uint64_t connection_id;
	bool cached_id;           /* the announce uses a connection ID from the cache */

	struct mbt_tracker_request req;
	uint8_t ip4[4];           /* BEP 15 IP field, zero for none */
	struct sockaddr_storage addr;
	socklen_t addrlen;
};

/* A connection ID that one tracker address gave us, or a connect in flight. */
struct udp_conn_id {
	TAILQ_ENTRY(udp_conn_id) entries;
	struct sockaddr_storage addr;
	uint64_t id;
	uint64_t expires;
	bool pending;
};

struct udp_announce_ctx;

/*
 * Pending announces of one owner. All of them share one UDP socket, the
 * session's when a mux is set, so a tracker sees one source port.
 */
struct mbt_tracker_group {
	TAILQ_HEAD(, udp_tracker) trackers;
	TAILQ_HEAD(, udp_announce_ctx) lookups;
	TAILQ_HEAD(, udp_conn_id) conn_ids;
	struct mbt_udp_mux *mux;
	struct mbt_ev *ev;
	int fd[2];                /* own sockets when no mux: IPv4, IPv6 */
	struct mbt_ev_io *io[2];
	bool temp;                /* made for one request with no group */
};

static void udp_group_dispatch(struct mbt_tracker_group *g, const uint8_t *buf, size_t n,
                               const struct sockaddr *src);
static int udp_send_announce(struct udp_tracker *ut);
static void udp_fail(struct udp_tracker *ut, int err);
static void udp_group_maybe_free(struct mbt_tracker_group *g);

static uint64_t
udp_now_usec(void)
{
	struct timespec ts;

	clock_gettime(CLOCK_MONOTONIC, &ts);
	return (uint64_t)ts.tv_sec * 1000000ULL + (uint64_t)ts.tv_nsec / 1000ULL;
}

/* Compare address and port. An IPv4-mapped IPv6 address equals its IPv4 form. */
static void
udp_addr_key(const struct sockaddr *sa, uint8_t key[18])
{
	memset(key, 0, 18);
	if (sa->sa_family == AF_INET) {
		const struct sockaddr_in *s4 = (const struct sockaddr_in *)sa;

		key[10] = key[11] = 0xff;
		memcpy(key + 12, &s4->sin_addr, 4);
		memcpy(key + 16, &s4->sin_port, 2);
	} else if (sa->sa_family == AF_INET6) {
		const struct sockaddr_in6 *s6 = (const struct sockaddr_in6 *)sa;

		memcpy(key, &s6->sin6_addr, 16);
		memcpy(key + 16, &s6->sin6_port, 2);
	}
}

static bool
udp_same_addr(const struct sockaddr *a, const struct sockaddr *b)
{
	uint8_t ka[18], kb[18];

	udp_addr_key(a, ka);
	udp_addr_key(b, kb);
	return memcmp(ka, kb, sizeof(ka)) == 0;
}

static struct udp_conn_id *
udp_conn_id_find(struct mbt_tracker_group *g, const struct sockaddr *addr)
{
	struct udp_conn_id *c, *next;
	uint64_t now = udp_now_usec();

	for (c = TAILQ_FIRST(&g->conn_ids); c; c = next) {
		next = TAILQ_NEXT(c, entries);
		if (c->expires <= now) {
			TAILQ_REMOVE(&g->conn_ids, c, entries);
			free(c);
			continue;
		}
		if (udp_same_addr((struct sockaddr *)&c->addr, addr))
			return c;
	}
	return NULL;
}

static void
udp_conn_id_put(struct mbt_tracker_group *g, const struct sockaddr_storage *addr, uint64_t id)
{
	struct udp_conn_id *c = udp_conn_id_find(g, (const struct sockaddr *)addr);

	if (!c) {
		c = calloc(1, sizeof(*c));
		if (!c)
			return;
		c->addr = *addr;
		TAILQ_INSERT_TAIL(&g->conn_ids, c, entries);
	}
	c->id = id;
	c->pending = false;
	c->expires = udp_now_usec() + CONN_ID_USEC;
}

/* A connect is in flight to addr: later announces wait for its ID. */
static void
udp_conn_id_pending(struct mbt_tracker_group *g, const struct sockaddr_storage *addr)
{
	struct udp_conn_id *c = calloc(1, sizeof(*c));

	if (!c)
		return;
	c->addr = *addr;
	c->pending = true;
	c->expires = udp_now_usec() + UDP_TIMEOUT_USEC;
	TAILQ_INSERT_TAIL(&g->conn_ids, c, entries);
}

static void
udp_conn_id_drop(struct mbt_tracker_group *g, const struct sockaddr *addr)
{
	struct udp_conn_id *c = udp_conn_id_find(g, addr);

	if (c) {
		TAILQ_REMOVE(&g->conn_ids, c, entries);
		free(c);
	}
}

static void
udp_own_recv(struct mbt_ev_io *io, int fd, uint32_t events, void *v)
{
	struct mbt_tracker_group *g = v;
	uint8_t buf[4096];
	struct sockaddr_storage src;

	(void)io;
	(void)events;
	for (int i = 0; i < 16; i++) {
		socklen_t srclen = sizeof(src);
		ssize_t n = recvfrom(fd, buf, sizeof(buf), MSG_DONTWAIT,
		                     (struct sockaddr *)&src, &srclen);
		bool temp = g->temp;

		if (n <= 0)
			return;
		udp_group_dispatch(g, buf, (size_t)n, (struct sockaddr *)&src);
		/* A reply can end the one request of a temp group, and free g. */
		if (temp)
			return;
	}
}

static int
udp_mux_recv_cb(const void *buf, size_t len, const struct sockaddr *src, socklen_t srclen,
                void *arg)
{
	(void)srclen;
	udp_group_dispatch(arg, buf, len, src);
	return 1;
}

void
mbt_tracker_group_set_mux(struct mbt_tracker_group *g, struct mbt_udp_mux *mux)
{
	if (g->mux)
		mbt_udp_mux_set_tracker(g->mux, NULL, NULL);
	g->mux = mux;
	if (mux)
		mbt_udp_mux_set_tracker(mux, udp_mux_recv_cb, g);
}

static int
udp_group_send(struct mbt_tracker_group *g, struct mbt_ev *ev, const void *buf, size_t len,
               const struct sockaddr_storage *dst, socklen_t dstlen)
{
	int f = dst->ss_family == AF_INET6;
	int rv;

	if (g->mux) {
		rv = mbt_udp_mux_sendto(g->mux, buf, len, (const struct sockaddr *)dst, dstlen);
		if (rv != -EAFNOSUPPORT)
			return rv;
	}
	if (g->fd[f] < 0) {
		int fd = socket(dst->ss_family, SOCK_DGRAM, 0);

		if (fd < 0)
			return -errno;
		fcntl(fd, F_SETFL, fcntl(fd, F_GETFL) | O_NONBLOCK);
		fcntl(fd, F_SETFD, FD_CLOEXEC);
		rv = mbt_ev_io_new(ev, &g->io[f], fd, MBT_EV_READ, udp_own_recv, g);
		if (rv < 0) {
			close(fd);
			return rv;
		}
		g->fd[f] = fd;
	}
	if (sendto(g->fd[f], buf, len, 0, (const struct sockaddr *)dst, dstlen) < 0)
		return -errno;
	return 0;
}

static void
udp_cleanup(struct udp_tracker *ut)
{
	struct mbt_tracker_group *g = ut->group;

	TAILQ_REMOVE(&g->trackers, ut, entries);
	mbt_infohash_unref(ut->req.ih);
	if (ut->timer) mbt_ev_timer_free(ut->timer);
	free(ut);
	udp_group_maybe_free(g);
}

/* The connect to addr ended: announce with id, or fail with err. */
static void
udp_wake_waiters(struct mbt_tracker_group *g, const struct sockaddr_storage *addr, int err,
                 uint64_t id)
{
	TAILQ_HEAD(, udp_tracker) woken = TAILQ_HEAD_INITIALIZER(woken);
	struct udp_tracker *w, *next;

	/* Take them out first: a done callback can start new announces. */
	for (w = TAILQ_FIRST(&g->trackers); w; w = next) {
		next = TAILQ_NEXT(w, entries);
		if (w->state == STATE_WAIT_CONNECT &&
		    udp_same_addr((struct sockaddr *)&w->addr, (const struct sockaddr *)addr)) {
			TAILQ_REMOVE(&g->trackers, w, entries);
			TAILQ_INSERT_TAIL(&woken, w, entries);
		}
	}
	while ((w = TAILQ_FIRST(&woken)) != NULL) {
		int rv = err;

		TAILQ_REMOVE(&woken, w, entries);
		TAILQ_INSERT_TAIL(&g->trackers, w, entries);
		if (rv == 0) {
			w->connection_id = id;
			w->cached_id = true;
			rv = udp_send_announce(w);
			if (rv == 0)
				mbt_ev_timer_mod(w->timer, UDP_TIMEOUT_USEC);
		}
		if (rv < 0) {
			w->cached_id = false;
			w->state = STATE_DONE;
			udp_fail(w, rv);
		}
	}
}

static void
udp_fail(struct udp_tracker *ut, int err)
{
	struct mbt_tracker_group *g = ut->group;

	if (ut->cached_id || ut->state == STATE_CONNECT_SENT)
		udp_conn_id_drop(g, (struct sockaddr *)&ut->addr);
	if (ut->state == STATE_CONNECT_SENT) {
		struct sockaddr_storage addr = ut->addr;

		ut->state = STATE_DONE;
		udp_wake_waiters(g, &addr, err, 0);
	}
	if (ut->req.done_cb)
		ut->req.done_cb(ut->req.ih, err, 0, ut->req.arg);
	udp_cleanup(ut);
}

static int
udp_send_connect(struct udp_tracker *ut)
{
	uint8_t buf[16];
	uint64_t magic = htobe64(UDP_TRACKER_MAGIC);
	uint32_t action = htobe32(ACTION_CONNECT);

	ut->transaction_id = arc4random();
	uint32_t tid = htobe32(ut->transaction_id);

	memcpy(buf, &magic, 8);
	memcpy(buf + 8, &action, 4);
	memcpy(buf + 12, &tid, 4);

	ut->state = STATE_CONNECT_SENT;
	return udp_group_send(ut->group, ut->ev, buf, sizeof(buf), &ut->addr, ut->addrlen);
}

static int
udp_send_announce(struct udp_tracker *ut)
{
	uint8_t buf[98];
	memset(buf, 0, sizeof(buf));

	uint64_t cid = htobe64(ut->connection_id);
	uint32_t action = htobe32(ACTION_ANNOUNCE);
	ut->transaction_id = arc4random();
	uint32_t tid = htobe32(ut->transaction_id);

	memcpy(buf, &cid, 8);                         /* connection_id */
	memcpy(buf + 8, &action, 4);                  /* action */
	memcpy(buf + 12, &tid, 4);                    /* transaction_id */
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
	memcpy(buf + 84, ut->ip4, 4);                 /* IP, 0 = source address */
	uint32_t key = htobe32(arc4random());
	memcpy(buf + 88, &key, 4);                    /* key */

	int32_t numwant = htobe32(200);
	memcpy(buf + 92, &numwant, 4);                /* num_want */

	uint16_t port = htobe16(ut->req.listen_port);
	memcpy(buf + 96, &port, 2);                   /* port */

	ut->state = STATE_ANNOUNCE_SENT;
	return udp_group_send(ut->group, ut->ev, buf, sizeof(buf), &ut->addr, ut->addrlen);
}

static void
udp_on_connect_reply(struct udp_tracker *ut, const uint8_t *buf)
{
	memcpy(&ut->connection_id, buf + 8, 8);
	ut->connection_id = be64toh(ut->connection_id);
	udp_conn_id_put(ut->group, &ut->addr, ut->connection_id);

	mbt_log_debug(MBT_LOG_SESSION, "tracker: connect OK, sending announce");
	int rv = udp_send_announce(ut);
	udp_wake_waiters(ut->group, &ut->addr, 0, ut->connection_id);
	if (rv < 0) {
		udp_fail(ut, rv);
		return;
	}
	mbt_ev_timer_mod(ut->timer, UDP_TIMEOUT_USEC);
}

static void
udp_on_announce_reply(struct udp_tracker *ut, const uint8_t *buf, size_t n)
{
	uint32_t interval, leechers, seeders;

	memcpy(&interval, buf + 8, 4);
	memcpy(&leechers, buf + 12, 4);
	memcpy(&seeders, buf + 16, 4);
	interval = be32toh(interval);
	leechers = be32toh(leechers);
	seeders = be32toh(seeders);

	/* Parse compact peer list — BEP 7: 18 bytes/peer if IPv6, 6 if IPv4 */
	int peer_size = (ut->addr.ss_family == AF_INET6) ? 18 : 6;
	int peer_count = (int)((n - 20) / (size_t)peer_size);
	mbt_log_info(MBT_LOG_SESSION, "tracker: %d peers, %u seeders, %u leechers, interval %us",
	             peer_count, seeders, leechers, interval);

	for (int i = 0; i < peer_count; i++) {
		const uint8_t *p = buf + 20 + i * peer_size;
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

/* Match a reply to its announce by transaction ID and source address. */
static void
udp_group_dispatch(struct mbt_tracker_group *g, const uint8_t *buf, size_t n,
                   const struct sockaddr *src)
{
	struct udp_tracker *ut;
	uint32_t action, tid;

	if (n < 8)
		return;
	memcpy(&action, buf, 4);
	memcpy(&tid, buf + 4, 4);
	action = be32toh(action);
	tid = be32toh(tid);

	TAILQ_FOREACH(ut, &g->trackers, entries) {
		if (ut->transaction_id == tid && ut->state != STATE_DONE &&
		    ut->state != STATE_WAIT_CONNECT &&
		    udp_same_addr((struct sockaddr *)&ut->addr, src))
			break;
	}
	if (!ut)
		return;

	if (action == ACTION_ERROR) {
		mbt_log_warn(MBT_LOG_SESSION, "tracker error: %.*s", (int)(n - 8), buf + 8);
		udp_fail(ut, -EPROTO);
	} else if (ut->state == STATE_CONNECT_SENT && action == ACTION_CONNECT && n >= 16) {
		udp_on_connect_reply(ut, buf);
	} else if (ut->state == STATE_ANNOUNCE_SENT && action == ACTION_ANNOUNCE && n >= 20) {
		udp_on_announce_reply(ut, buf, n);
	} else {
		udp_fail(ut, -EPROTO);
	}
}

static void
udp_on_timeout(struct mbt_ev_timer *timer, void *v)
{
	struct udp_tracker *ut = v;
	(void)timer;

	mbt_log_warn(MBT_LOG_SESSION, "tracker: timeout");
	udp_fail(ut, -ETIMEDOUT);
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
		if (mbt_parse_port(end + 2, '/', port) < 0)
			return -EINVAL;
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
		if (mbt_parse_port(colon + 1, '/', port) < 0)
			return -EINVAL;
	}
	return 0;
}


/* Context for async DNS -> UDP announce */
struct udp_announce_ctx {
	TAILQ_ENTRY(udp_announce_ctx) entries;
	struct mbt_tracker_group *group;
	bool canceled;
	char host[256];
	uint16_t port;
	uint8_t peer_id[20];
	uint8_t ip4[4];
	struct mbt_tracker_request req;
};

static int
udp_group_alloc(struct mbt_tracker_group **out)
{
	struct mbt_tracker_group *g = calloc(1, sizeof(*g));

	if (!g)
		return -ENOMEM;
	TAILQ_INIT(&g->trackers);
	TAILQ_INIT(&g->lookups);
	TAILQ_INIT(&g->conn_ids);
	g->fd[0] = g->fd[1] = -1;
	*out = g;
	return 0;
}

int
mbt_tracker_group_new(struct mbt_tracker_group **out)
{
	ASSERT_RETURN(out, -EINVAL);
	return udp_group_alloc(out);
}

static void
udp_group_destroy(struct mbt_tracker_group *g)
{
	struct udp_conn_id *c;

	mbt_tracker_group_set_mux(g, NULL);
	for (int f = 0; f < 2; f++) {
		if (g->io[f])
			mbt_ev_io_free(g->io[f]);
		if (g->fd[f] >= 0)
			close(g->fd[f]);
	}
	while ((c = TAILQ_FIRST(&g->conn_ids)) != NULL) {
		TAILQ_REMOVE(&g->conn_ids, c, entries);
		free(c);
	}
	free(g);
}

/* A temp group lives as long as its one request. */
static void
udp_group_maybe_free(struct mbt_tracker_group *g)
{
	if (g->temp && TAILQ_EMPTY(&g->trackers) && TAILQ_EMPTY(&g->lookups))
		udp_group_destroy(g);
}

/* A DNS lookup cannot stop: it ends in udp_announce_resolved. */
void
mbt_tracker_group_free(struct mbt_tracker_group *g)
{
	struct udp_tracker *ut;
	struct udp_announce_ctx *ctx;

	if (!g)
		return;
	while ((ut = TAILQ_FIRST(&g->trackers)) != NULL) {
		TAILQ_REMOVE(&g->trackers, ut, entries);
		mbt_infohash_unref(ut->req.ih);
		if (ut->timer)
			mbt_ev_timer_free(ut->timer);
		free(ut);
	}
	while ((ctx = TAILQ_FIRST(&g->lookups)) != NULL) {
		TAILQ_REMOVE(&g->lookups, ctx, entries);
		ctx->group = NULL;
		ctx->canceled = true;
	}
	udp_group_destroy(g);
}

static void
ctx_free(struct udp_announce_ctx *ctx)
{
	struct mbt_tracker_group *g = ctx->group;

	if (g)
		TAILQ_REMOVE(&g->lookups, ctx, entries);
	mbt_infohash_unref(ctx->req.ih);
	free(ctx);
	if (g)
		udp_group_maybe_free(g);
}

static int
udp_start(struct udp_announce_ctx *ctx, struct sockaddr_storage *addr, socklen_t len)
{
	struct udp_tracker *ut = calloc(1, sizeof(*ut));
	struct udp_conn_id *c;
	int rv;

	if (!ut)
		return -ENOMEM;

	ut->ev = ctx->req.ev;
	ut->req = ctx->req;
	memcpy(ut->peer_id, ctx->peer_id, sizeof(ut->peer_id));
	memcpy(ut->ip4, ctx->ip4, sizeof(ut->ip4));
	ut->req.peer_id = ut->peer_id;
	ut->req.ih = mbt_infohash_ref(ctx->req.ih);
	ut->group = ctx->group;
	TAILQ_INSERT_TAIL(&ut->group->trackers, ut, entries);

	memcpy(&ut->addr, addr, len);
	ut->addrlen = len;
	if (ut->addr.ss_family == AF_INET6)
		((struct sockaddr_in6 *)&ut->addr)->sin6_port = htons(ctx->port);
	else
		((struct sockaddr_in *)&ut->addr)->sin_port = htons(ctx->port);

	rv = mbt_ev_timer_new(ut->ev, &ut->timer, UDP_TIMEOUT_USEC, udp_on_timeout, ut);
	if (rv < 0)
		goto fail;

	c = udp_conn_id_find(ut->group, (struct sockaddr *)&ut->addr);
	if (c && c->pending) {
		ut->state = STATE_WAIT_CONNECT;
		return 0;
	}
	if (c) {
		ut->connection_id = c->id;
		ut->cached_id = true;
		rv = udp_send_announce(ut);
	} else {
		rv = udp_send_connect(ut);
		if (rv == 0)
			udp_conn_id_pending(ut->group, &ut->addr);
	}
	if (rv < 0)
		goto fail;
	mbt_log_debug(MBT_LOG_SESSION, "tracker: UDP %s sent to %s:%hu",
	              c ? "announce" : "connect", ctx->host, ctx->port);
	return 0;

fail:
	/* The ctx still holds the group, so it stays. */
	TAILQ_REMOVE(&ut->group->trackers, ut, entries);
	mbt_infohash_unref(ut->req.ih);
	if (ut->timer)
		mbt_ev_timer_free(ut->timer);
	free(ut);
	return rv;
}

static void
udp_announce_resolved(struct sockaddr_storage *addr, socklen_t len, int err, void *arg)
{
	struct udp_announce_ctx *ctx = arg;

	if (ctx->canceled) {
		ctx_free(ctx);
		return;
	}
	if (err == 0)
		err = udp_start(ctx, addr, len);
	if (err < 0 && ctx->req.done_cb)
		ctx->req.done_cb(ctx->req.ih, err, 0, ctx->req.arg);
	ctx_free(ctx);
}

int
mbt_tracker_udp_announce(const char *url, struct mbt_tracker_request *req)
{
	struct mbt_tracker_group *g = req->group;
	char host[256];
	uint16_t port;
	int rv;

	rv = parse_udp_url(url, host, sizeof(host), &port);
	if (rv < 0)
		return rv;

	if (!g) {
		if ((rv = udp_group_alloc(&g)) < 0)
			return rv;
		g->temp = true;
	}
	struct udp_announce_ctx *ctx = calloc(1, sizeof(*ctx));
	if (!ctx) {
		udp_group_maybe_free(g);
		return -ENOMEM;
	}
	memcpy(ctx->host, host, sizeof(ctx->host));
	ctx->port = port;
	ctx->req = *req;
	memcpy(ctx->peer_id, req->peer_id, sizeof(ctx->peer_id));
	ctx->req.peer_id = ctx->peer_id;
	ctx->req.ih = mbt_infohash_ref(req->ih);
	/* The field holds IPv4 only. */
	if (req->ip && inet_pton(AF_INET, req->ip, ctx->ip4) != 1)
		memset(ctx->ip4, 0, sizeof(ctx->ip4));
	ctx->req.ip = NULL;
	ctx->group = g;
	TAILQ_INSERT_TAIL(&g->lookups, ctx, entries);

	rv = mbt_resolve(req->ev, host, port, udp_announce_resolved, ctx, NULL);
	if (rv < 0) {
		ctx_free(ctx);
		return rv;
	}
	return 0;
}
