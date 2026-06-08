#define _GNU_SOURCE
#include <sys/queue.h>
#include <sys/socket.h>
#include <netinet/in.h>
#include <arpa/inet.h>
#include <stdlib.h>
#include <stdio.h>
#include <string.h>
#include <unistd.h>
#include <errno.h>
#include <fcntl.h>

#include <sha1.h>

#include "mbt/event.h"
#include "mbt/conn.h"
#include "mbt/infohash.h"
#include "mbt/hex.h"
#include "mbt/be.h"
#include "mbt/torrent.h"
#include "mbt/session.h"
#include "mbt/log.h"
#include "mbt/lsd.h"
#include "mbt/dht.h"
#include "mbt/metainfo.h"
#include "mbt/magnet.h"
#include "mbt/tracker.h"

/* --- Torrent list entry --- */

#include "mbt/peerdb.h"

struct session_torrent {
	struct mbt_session *session;
	struct mbt_torrent *torrent;
	struct mbt_infohash *ih;
	char *trackers[16];
	int tracker_count;
	struct mbt_ev_timer *reannounce_timer;
	int announce_interval;
	time_t last_dht_lookup;
	int completed_announced; /* sent event=completed to tracker */
	STAILQ_ENTRY(session_torrent) next;
};

STAILQ_HEAD(session_torrent_list, session_torrent);

/* --- Pending connect --- */

struct pending_connect {
	struct mbt_session *session;
	struct mbt_torrent *torrent;
	struct mbt_ev_io *io;
	struct mbt_ev_timer *timeout;
	int fd;
	char addr[INET6_ADDRSTRLEN];
	uint16_t port;
	STAILQ_ENTRY(pending_connect) next;
};

STAILQ_HEAD(pending_connect_list, pending_connect);

/* --- Session --- */

struct mbt_session {
	struct mbt_ev *ev;

	const struct mbt_storage_ops *storage_ops;
	void *storage_ctx;
	mbt_picker_fn picker;
	void *picker_ctx;

	uint16_t listen_port;
	int request_timeout_sec;
	int max_peers;
	mbt_http_fn http;
	void *http_arg;
	uint8_t peer_id[20];

	struct mbt_session_callbacks cb;

	/* LSD */
	struct mbt_lsd *lsd;
	struct mbt_ev_io *lsd_io;

	/* DHT */
	struct mbt_dht *dht;

	/* Listen socket */
	int listen_fd;
	struct mbt_ev_io *listen_io;

	/* Maintenance timer (refill peers from pool) */
	struct mbt_ev_timer *maintain_timer;

	/* LSD announcer timer (optional) */
	struct mbt_ev_timer *lsd_announce_timer;

	/* Torrents */
	struct session_torrent_list torrents;

	/* Pending outgoing connections */
	struct pending_connect_list pending;
};

/* --- Forward declarations --- */

static void
session_lsd_announce_cb(struct mbt_ev_timer *timer, void *v)
{
	struct mbt_session *s = v;
	struct session_torrent *st;

	(void)timer;
	if (!s->lsd || s->listen_port == 0)
		return;

	STAILQ_FOREACH(st, &s->torrents, next) {
		char hex[MBT_MBT_INFOHASH_SHA1HEXLEN + 1];
		mbt_infohash_string(st->ih, hex, sizeof(hex));
		mbt_lsd_announce(s->lsd, hex, s->listen_port);
		mbt_log_debug(MBT_LOG_LSD, "LSD announce: %s port %hu", hex, s->listen_port);
	}
}

static void
session_maintain_cb(struct mbt_ev_timer *timer, void *v)
{
	struct mbt_session *s = v;
	struct session_torrent *st;

	STAILQ_FOREACH(st, &s->torrents, next) {
		/* Refill peers from peerdb if below cap */
		while (mbt_torrent_peer_count(st->torrent) +
		       mbt_peerdb_count(mbt_torrent_peerdb(st->torrent), MBT_PEERDB_CONNECTING) < s->max_peers) {
			struct mbt_peer_entry *pe = mbt_peerdb_select(mbt_torrent_peerdb(st->torrent));
			if (!pe)
				break;
			pe->state = MBT_PEERDB_CONNECTING;
			mbt_session_add_peer(s, st->ih, pe->addr, pe->port);
		}

		/* Announce event=completed once when torrent finishes */
		if (mbt_torrent_complete(st->torrent) && !st->completed_announced &&
		    st->tracker_count > 0) {
			st->completed_announced = 1;
			struct mbt_tracker_request treq = {
				.ev = s->ev, .ih = st->ih, .peer_id = s->peer_id,
				.listen_port = s->listen_port, .left = 0,
				.event = MBT_TRACKER_EVENT_COMPLETED,
			};
			for (int i = 0; i < st->tracker_count; i++)
				mbt_tracker_announce(st->trackers[i], &treq, s->http, s->http_arg);
		}

		/* DHT re-lookup every 5 min if incomplete and few peers */
		if (s->dht && !mbt_torrent_complete(st->torrent)) {
			time_t now = time(NULL);
			if (now - st->last_dht_lookup >= 300) {
				st->last_dht_lookup = now;
				mbt_session_dht_get_peers(s, st->ih);
			}
		}
	}

	mbt_ev_timer_mod(timer, 5 * 1000000ULL);
}

static void session_generate_peer_id(uint8_t *id);

/* --- Pending connect handler --- */

static void
pending_connect_timeout(struct mbt_ev_timer *timer, void *v)
{
	struct pending_connect *pc = v;
	struct mbt_session *s = pc->session;
	(void)timer;

	mbt_log_debug(MBT_LOG_SESSION, "connect to %s:%hu timed out", pc->addr, pc->port);
	mbt_ev_io_free(pc->io);
	pc->io = NULL;
	mbt_ev_timer_free(pc->timeout);
	pc->timeout = NULL;
	close(pc->fd);
	STAILQ_REMOVE(&s->pending, pc, pending_connect, next);
	free(pc);
}

static void
pending_connect_cb(struct mbt_ev_io *io, int fd, uint32_t events, void *v)
{
	struct pending_connect *pc = v;
	struct mbt_session *s = pc->session;
	socklen_t slen;
	int error, rv;
	struct mbt_conn *conn;

	(void)io;
	(void)fd;
	(void)events;

	slen = sizeof(error);
	rv = getsockopt(pc->fd, SOL_SOCKET, SO_ERROR, &error, &slen);
	if (rv < 0 || error != 0) {
		mbt_log_warn(MBT_LOG_SESSION, "connect to %s:%hu failed: %s",
		             pc->addr, pc->port, strerror(error ? error : errno));
		goto fail;
	}

	mbt_log_debug(MBT_LOG_SESSION, "connected to %s:%hu", pc->addr, pc->port);

	/* Free connect watcher before creating conn (conn registers its own) */
	mbt_ev_io_free(pc->io);
	pc->io = NULL;

	/* Connection succeeded — create mbt_conn */
	rv = mbt_conn_new(s->ev, &conn, pc->fd, NULL, NULL, NULL);
	if (rv < 0) {
		mbt_log_warn(MBT_LOG_SESSION, "mbt_conn_new failed: %s", strerror(-rv));
		goto fail;
	}

	/* Hand to torrent — torrent takes ownership of conn */
	rv = mbt_torrent_add_peer(pc->torrent, conn);
	if (rv < 0) {
		mbt_conn_free(conn);
		goto fail;
	}

	/* Clean up pending */
	if (pc->timeout)
		mbt_ev_timer_free(pc->timeout);
	STAILQ_REMOVE(&s->pending, pc, pending_connect, next);
	free(pc);
	return;

fail:
	close(pc->fd);
	if (pc->io)
		mbt_ev_io_free(pc->io);
	if (pc->timeout)
		mbt_ev_timer_free(pc->timeout);
	STAILQ_REMOVE(&s->pending, pc, pending_connect, next);
	free(pc);
}

/* --- Helpers --- */

static void
session_generate_peer_id(uint8_t *id)
{
	/* -MB0001- followed by 12 random bytes */
	memcpy(id, "-MB0001-", 8);
	arc4random_buf(id + 8, 12);
}

static struct session_torrent *
session_find(struct mbt_session *s, struct mbt_infohash *ih)
{
	struct session_torrent *st;
	STAILQ_FOREACH(st, &s->torrents, next) {
		if (mbt_infohash_cmp(st->ih, ih) == 0)
			return st;
	}
	return NULL;
}


/* --- Listen socket (async handshake) --- */

#define HANDSHAKE_SIZE 68

struct pending_accept {
	struct mbt_session *session;
	int fd;
	struct mbt_ev_io *io;
	struct sockaddr_in addr;
	unsigned char hs[HANDSHAKE_SIZE];
	size_t hs_got;
};

static void
pending_accept_cb(struct mbt_ev_io *io, int fd, uint32_t events, void *v)
{
	struct pending_accept *pa = v;
	struct mbt_session *s = pa->session;
	(void)io; (void)fd; (void)events;

	ssize_t n = recv(pa->fd, pa->hs + pa->hs_got, HANDSHAKE_SIZE - pa->hs_got, 0);
	if (n <= 0) {
		close(pa->fd);
		mbt_ev_io_free(pa->io);
		free(pa);
		return;
	}
	pa->hs_got += n;
	if (pa->hs_got < HANDSHAKE_SIZE)
		return;

	if (pa->hs[0] != 19 || memcmp(pa->hs + 1, "BitTorrent protocol", 19) != 0) {
		close(pa->fd);
		mbt_ev_io_free(pa->io);
		free(pa);
		return;
	}

	uint8_t hash[20], ext[8], their_id[20];
	memcpy(ext, pa->hs + 20, 8);
	memcpy(hash, pa->hs + 28, 20);
	memcpy(their_id, pa->hs + 48, 20);

	struct session_torrent *st;
	STAILQ_FOREACH(st, &s->torrents, next) {
		if (memcmp(st->ih->hash_sha1, hash, 20) == 0)
			break;
	}
	if (!st) {
		close(pa->fd);
		mbt_ev_io_free(pa->io);
		free(pa);
		return;
	}

	/* Enforce peer limit */
	if (mbt_torrent_peer_count(st->torrent) >= s->max_peers) {
		close(pa->fd);
		mbt_ev_io_free(pa->io);
		free(pa);
		return;
	}

	char paddr[INET_ADDRSTRLEN];
	inet_ntop(AF_INET, &pa->addr.sin_addr, paddr, sizeof(paddr));
	mbt_log_info(MBT_LOG_SESSION, "incoming peer %s:%hu", paddr, ntohs(pa->addr.sin_port));

	mbt_ev_io_free(pa->io);

	struct mbt_conn *conn;
	int rv = mbt_conn_new(s->ev, &conn, pa->fd, NULL, NULL, NULL);
	if (rv < 0) { close(pa->fd); free(pa); return; }

	rv = mbt_torrent_add_peer_incoming(st->torrent, conn, ext, their_id);
	if (rv < 0)
		mbt_conn_free(conn);
	free(pa);
}

static void
listen_accept_cb(struct mbt_ev_io *io, int fd, uint32_t events, void *v)
{
	struct mbt_session *s = v;
	struct sockaddr_in addr;
	socklen_t alen = sizeof(addr);
	int cfd;
	(void)io; (void)events;

	cfd = accept4(fd, (struct sockaddr *)&addr, &alen, SOCK_NONBLOCK | SOCK_CLOEXEC);
	if (cfd < 0)
		return;

	struct pending_accept *pa = calloc(1, sizeof(*pa));
	if (!pa) { close(cfd); return; }
	pa->session = s;
	pa->fd = cfd;
	pa->addr = addr;

	int rv = mbt_ev_io_new(s->ev, &pa->io, cfd, MBT_EV_READ, pending_accept_cb, pa);
	if (rv < 0) { close(cfd); free(pa); return; }
}
static int
session_listen(struct mbt_session *s)
{
	int fd, rv, yes = 1;
	struct sockaddr_in sa = {
		.sin_family = AF_INET,
		.sin_port = htons(s->listen_port),
		.sin_addr.s_addr = INADDR_ANY,
	};

	fd = socket(AF_INET, SOCK_STREAM | SOCK_NONBLOCK | SOCK_CLOEXEC, 0);
	if (fd < 0)
		return -errno;

	setsockopt(fd, SOL_SOCKET, SO_REUSEADDR, &yes, sizeof(yes));

	if (bind(fd, (struct sockaddr *)&sa, sizeof(sa)) < 0) {
		rv = -errno;
		close(fd);
		return rv;
	}

	if (listen(fd, 16) < 0) {
		rv = -errno;
		close(fd);
		return rv;
	}

	s->listen_fd = fd;
	rv = mbt_ev_io_new(s->ev, &s->listen_io, fd, MBT_EV_READ, listen_accept_cb, s);
	if (rv < 0) { close(fd); return rv; }

	mbt_log_info(MBT_LOG_SESSION, "listening on port %hu", s->listen_port);
	return 0;
}

/* --- Public API --- */

int
mbt_session_new(struct mbt_session **out, struct mbt_session_config *cfg)
{
	struct mbt_session *s;

	s = calloc(1, sizeof(*s));
	if (!s)
		return -ENOMEM;

	s->ev = cfg->ev;
	s->storage_ops = cfg->storage_ops;
	s->storage_ctx = cfg->storage_ctx;
	s->picker = cfg->picker;
	s->picker_ctx = cfg->picker_ctx;
	s->listen_port = cfg->listen_port;
	s->request_timeout_sec = cfg->request_timeout;
	s->max_peers = cfg->max_peers ? cfg->max_peers : 30;
	s->http = cfg->http;
	s->http_arg = cfg->http_arg;

	STAILQ_INIT(&s->torrents);
	STAILQ_INIT(&s->pending);

	session_generate_peer_id(s->peer_id);

	/* Maintenance timer: refill peers from pool every 5s */
	mbt_ev_timer_new(s->ev, &s->maintain_timer, 5 * 1000000ULL, session_maintain_cb, s);

	/* Listen socket for incoming peers */
	if (s->listen_port > 0)
		session_listen(s);

	/* LSD announcer: multicast our infohashes every 5 min */
	if (cfg->mbt_lsd_announce && s->lsd && s->listen_port > 0)
		mbt_ev_timer_new(s->ev, &s->lsd_announce_timer, 300 * 1000000ULL,
		                 session_lsd_announce_cb, s);

	*out = s;
	return 0;
}

void
mbt_session_free(struct mbt_session *s)
{
	if (!s)
		return;

	if (s->listen_io)
		mbt_ev_io_free(s->listen_io);
	if (s->listen_fd > 0)
		close(s->listen_fd);
	if (s->lsd_announce_timer)
		mbt_ev_timer_free(s->lsd_announce_timer);
	if (s->lsd_io)
		mbt_ev_io_free(s->lsd_io);
	if (s->lsd)
		mbt_lsd_freep(&s->lsd);
	if (s->dht)
		mbt_dht_free(s->dht);

	/* Free pending connects */
	struct pending_connect *pc;
	while ((pc = STAILQ_FIRST(&s->pending))) {
		STAILQ_REMOVE_HEAD(&s->pending, next);
		mbt_ev_io_free(pc->io);
		close(pc->fd);
		free(pc);
	}

	/* Announce event=stopped to all trackers */
	struct session_torrent *sst;
	STAILQ_FOREACH(sst, &s->torrents, next) {
		if (sst->tracker_count > 0) {
			struct mbt_tracker_request treq = {
				.ev = s->ev, .ih = sst->ih, .peer_id = s->peer_id,
				.listen_port = s->listen_port, .left = 0,
				.event = MBT_TRACKER_EVENT_STOPPED,
				.arg = s,
			};
			for (int i = 0; i < sst->tracker_count; i++)
				mbt_tracker_announce(sst->trackers[i], &treq, s->http, s->http_arg);
		}
	}

	/* Free torrents */
	struct session_torrent *st;
	while ((st = STAILQ_FIRST(&s->torrents))) {
		STAILQ_REMOVE_HEAD(&s->torrents, next);
		mbt_torrent_free(st->torrent);
		mbt_infohash_unref(st->ih);
		if (st->reannounce_timer)
			mbt_ev_timer_free(st->reannounce_timer);
		for (int i = 0; i < st->tracker_count; i++)
			free(st->trackers[i]);
		free(st);
	}

	if (s->lsd_io)
		mbt_ev_io_free(s->lsd_io);
	if (s->maintain_timer)
		mbt_ev_timer_free(s->maintain_timer);

	free(s);
}

int
mbt_session_default(struct mbt_session **out, const char *path)
{
	struct mbt_ev *ev;
	int rv;

#ifdef HAVE_SD_EVENT
	rv = mbt_ev_sd_new(&ev);
#elif defined(HAVE_LIBEVENT)
	rv = mbt_ev_libevent_new(&ev);
#else
#error "No event backend configured"
#endif
	if (rv < 0)
		return rv;

	struct mbt_session_config cfg = {
		.ev = ev,
		.storage_ops = mbt_storage_file_ops(),
		.storage_ctx = (void *)path,
		.picker = mbt_picker_sequential,
		.listen_port = 6881,
	};

	return mbt_session_new(out, &cfg);
}

int
mbt_session_run(struct mbt_session *s)
{
	return mbt_ev_run(s->ev);
}

void
mbt_session_stop(struct mbt_session *s)
{
	mbt_ev_stop(s->ev);
}

int
mbt_session_set_callbacks(struct mbt_session *s, struct mbt_session_callbacks *cb)
{
	s->cb = *cb;
	return 0;
}

/* --- LSD --- */

static void
session_lsd_io_cb(struct mbt_ev_io *io, int fd, uint32_t events, void *v)
{
	struct mbt_session *s = v;
	int rv;
	__attribute__((cleanup(mbt_lsd_frame_freep))) struct mbt_lsd_frame *frame = NULL;
	__attribute__((cleanup(mbt_lsd_frame_info_freep))) struct mbt_lsd_frame_info *info = NULL;
	struct mbt_infohash *ih;

	(void)io;
	(void)fd;
	(void)events;

	rv = mbt_lsd_recv(s->lsd, &frame);
	if (rv < 0)
		return;

	rv = mbt_lsd_frame_parse(frame, &info);
	if (rv < 0)
		return;

	STAILQ_FOREACH(ih, &info->infohashes, next) {
		struct session_torrent *st = session_find(s, ih);

		if (s->cb.on_lsd_announce) {
			if (s->cb.on_lsd_announce(s, ih, info->remote, info->port, s->cb.arg)) {
				if (!st) {
					mbt_session_add_infohash(s, ih);
					st = session_find(s, ih);
				}
				if (st)
					mbt_peerdb_insert(mbt_torrent_peerdb(st->torrent), info->remote, info->port, MBT_SOURCE_LSD);
				mbt_session_add_peer(s, ih, info->remote, info->port);
			}
		} else {
			if (st) {
				mbt_peerdb_insert(mbt_torrent_peerdb(st->torrent), info->remote, info->port, MBT_SOURCE_LSD);
				mbt_session_add_peer(s, ih, info->remote, info->port);
			}
		}
	}
}

int
mbt_session_enable_lsd(struct mbt_session *s)
{
	int rv;

	if (s->lsd)
		return 0;

	rv = mbt_lsd_init(&s->lsd, O_NONBLOCK);
	if (rv < 0)
		return rv;

	rv = mbt_ev_io_new(s->ev, &s->lsd_io, s->lsd->fd, MBT_EV_READ,
	                   session_lsd_io_cb, s);
	if (rv < 0) {
		mbt_lsd_freep(&s->lsd);
		return rv;
	}

	return 0;
}

void
mbt_session_disable_lsd(struct mbt_session *s)
{
	if (s->lsd_io) { mbt_ev_io_free(s->lsd_io); s->lsd_io = NULL; }
	if (s->lsd) { mbt_lsd_freep(&s->lsd); s->lsd = NULL; }
	if (s->lsd_announce_timer) { mbt_ev_timer_free(s->lsd_announce_timer); s->lsd_announce_timer = NULL; }
}

/* --- DHT --- */

struct dht_lookup_ctx {
	struct mbt_session *session;
	struct mbt_infohash *ih;
};

static void
dht_peers_cb(const char *addr, uint16_t port, void *arg)
{
	struct dht_lookup_ctx *ctx = arg;
	struct session_torrent *st = session_find(ctx->session, ctx->ih);
	if (st) {
		mbt_peerdb_insert(mbt_torrent_peerdb(st->torrent), addr, port, MBT_SOURCE_DHT);
		mbt_log_debug(MBT_LOG_DHT, "DHT peer: %s:%hu", addr, port);
	}
}

static void
dht_done_cb(void *arg)
{
	struct dht_lookup_ctx *ctx = arg;
	mbt_infohash_unref(ctx->ih);
	free(ctx);
}

int
mbt_session_enable_dht(struct mbt_session *s)
{
	if (s->dht)
		return 0;

	int rv = mbt_dht_new(&s->dht, s->ev, s->listen_port ? s->listen_port : 6881);
	if (rv < 0)
		return rv;

	/* Default bootstrap nodes */
	mbt_session_dht_bootstrap(s, "router.bittorrent.com", 6881);
	mbt_session_dht_bootstrap(s, "dht.transmissionbt.com", 6881);

	return 0;
}
void
mbt_session_disable_dht(struct mbt_session *s)
{
	if (s->dht) { mbt_dht_free(s->dht); s->dht = NULL; }
}

int
mbt_session_dht_bootstrap(struct mbt_session *s, const char *addr, uint16_t port)
{
	if (!s->dht)
		return -EINVAL;

	int rv = mbt_dht_bootstrap(s->dht, addr, port);
	if (rv < 0)
		return rv;

	/* Trigger lookups for all existing torrents (new node may help) */
	struct session_torrent *st;
	STAILQ_FOREACH(st, &s->torrents, next)
		mbt_session_dht_get_peers(s, st->ih);

	return 0;
}

void
mbt_session_dht_get_peers(struct mbt_session *s, struct mbt_infohash *ih)
{
	if (!s->dht) return;

	struct dht_lookup_ctx *ctx = malloc(sizeof(*ctx));
	if (!ctx) return;
	ctx->session = s;
	ctx->ih = mbt_infohash_ref(ih);

	mbt_dht_get_peers(s->dht, ih, dht_peers_cb, dht_done_cb, ctx);
}

/* --- Tracker callbacks --- */

static void
session_tracker_peer_cb(struct mbt_infohash *ih, const char *addr, uint16_t port, void *arg)
{
	struct mbt_session *s = arg;
	struct session_torrent *st = session_find(s, ih);
	if (!st) return;

	/* Insert into peerdb (deduplicates automatically) */
	mbt_peerdb_insert(mbt_torrent_peerdb(st->torrent), addr, port, MBT_SOURCE_TRACKER);

	/* If below cap, connect immediately */
	if (mbt_torrent_peer_count(st->torrent) < s->max_peers)
		mbt_session_add_peer(s, ih, addr, port);

	mbt_session_add_peer(s, ih, addr, port);
}

static void session_reannounce_timer_cb(struct mbt_ev_timer *timer, void *v);

static void
session_tracker_done_cb(struct mbt_infohash *ih, int err, int interval, void *arg)
{
	struct mbt_session *s = arg;

	if (err < 0) {
		mbt_log_warn(MBT_LOG_SESSION, "tracker announce failed: %s", strerror(-err));
		return;
	}

	/* Schedule re-announce */
	struct session_torrent *st = session_find(s, ih);
	if (st && interval > 0) {
		/* Clamp interval: min 30s, max 15min */
		if (interval < 30) interval = 30;
		if (interval > 900) interval = 900;
		st->announce_interval = interval;
		if (!st->reannounce_timer) {
			mbt_ev_timer_new(s->ev, &st->reannounce_timer,
			                 (uint64_t)interval * 1000000ULL,
			                 session_reannounce_timer_cb, st);
		} else {
			mbt_ev_timer_mod(st->reannounce_timer,
			                 (uint64_t)interval * 1000000ULL);
		}
		mbt_log_debug(MBT_LOG_SESSION, "next announce in %ds", interval);
	}
}

static void
session_do_announce(struct mbt_session *s, struct session_torrent *st)
{
	if (st->tracker_count == 0)
		return;

	struct mbt_torrent *t = st->torrent;
	uint64_t left = 0;
	int event = MBT_TRACKER_EVENT_NONE;

	if (t && mbt_torrent_piece_count(t) > 0) {
		uint64_t total = (uint64_t)mbt_torrent_piece_length(t) * mbt_torrent_piece_count(t);
		uint64_t have = (uint64_t)mbt_torrent_piece_length(t) * mbt_torrent_have_count(t);
		left = (total > have) ? total - have : 0;
	}

	struct mbt_tracker_request treq = {
		.ev = s->ev,
		.ih = st->ih,
		.peer_id = s->peer_id,
		.listen_port = s->listen_port,
		.left = left,
		.event = event,
		.peer_cb = session_tracker_peer_cb,
		.done_cb = session_tracker_done_cb,
		.arg = s,
	};

	for (int i = 0; i < st->tracker_count; i++)
		mbt_tracker_announce(st->trackers[i], &treq, s->http, s->http_arg);
}

static void
session_reannounce_timer_cb(struct mbt_ev_timer *timer, void *v)
{
	struct session_torrent *st = v;
	(void)timer;

	mbt_log_info(MBT_LOG_SESSION, "re-announcing to tracker");
	session_do_announce(st->session, st);
}

/* --- Torrent management --- */

int
mbt_session_add_infohash(struct mbt_session *s, struct mbt_infohash *ih)
{
	/* Already have it? */
	if (session_find(s, ih))
		return 0;

	struct session_torrent *st = calloc(1, sizeof(*st));
	if (!st)
		return -ENOMEM;

	struct mbt_torrent_config cfg = {
		.ev = s->ev,
		.ih = ih,
		.peer_id = s->peer_id,
		.storage_ops = s->storage_ops,
		.storage_ctx = s->storage_ctx,
		.picker = s->picker,
		.picker_ctx = s->picker_ctx,
		.request_timeout_sec = s->request_timeout_sec,
	};

	int rv = mbt_torrent_new(&st->torrent, &cfg);
	if (rv < 0) {
		free(st);
		return rv;
	}


	st->session = s;
	st->ih = mbt_infohash_ref(ih);
	STAILQ_INSERT_TAIL(&s->torrents, st, next);

	/* Start DHT lookup if enabled */
	if (s->dht)
		mbt_session_dht_get_peers(s, ih);

	return 0;
}

int
mbt_session_add_torrent_buf(struct mbt_session *s, const void *data, size_t len)
{
	int rv;
	struct mbt_metainfo *mi = NULL;
	struct mbt_infohash *ih;
	struct mbt_torrent *t;
	size_t raw_len;

	rv = mbt_metainfo_parse(&mi, data, len);
	if (rv < 0) return rv;

	ih = mbt_metainfo_infohash(mi);
	if (!ih) { mbt_metainfo_free(mi); return -EINVAL; }

	rv = mbt_session_add_infohash(s, ih);
	if (rv < 0) { mbt_metainfo_free(mi); return rv; }

	t = mbt_session_find_torrent(s, ih);
	if (t) {
		const void *raw = mbt_metainfo_raw_info(mi, &raw_len);
		mbt_torrent_set_metadata(t, raw, raw_len);
	}

	/* Extract tracker URLs */
	struct session_torrent *sta = session_find(s, ih);
	if (sta) {
		for (int i = 0; i < mbt_metainfo_tracker_count(mi) && sta->tracker_count < 16; i++) {
			char *u = strdup(mbt_metainfo_tracker(mi, i));
			if (u) sta->trackers[sta->tracker_count++] = u;
		}
		if (sta->tracker_count > 0)
			session_do_announce(s, sta);
	}

	mbt_metainfo_free(mi);
	return 0;
}

int
mbt_session_add_torrent_file(struct mbt_session *s, const char *path)
{
	FILE *fp = fopen(path, "rb");
	if (!fp) return -errno;

	unsigned char *buf = malloc(4 * 1024 * 1024);
	if (!buf) { fclose(fp); return -ENOMEM; }

	size_t len = fread(buf, 1, 4 * 1024 * 1024, fp);
	fclose(fp);

	if (len == 0) { free(buf); return -EIO; }

	int rv = mbt_session_add_torrent_buf(s, buf, len);
	free(buf);
	return rv;
}

int
mbt_session_add_magnet(struct mbt_session *s, const char *uri)
{
	struct mbt_magnet m;
	struct mbt_infohash *ih = NULL;
	int rv;

	rv = mbt_magnet_parse(uri, &m);
	if (rv < 0)
		return rv;

	rv = mbt_infohash_new(&ih, m.infohash_type, m.infohash_hex);
	if (rv < 0) {
		mbt_magnet_free(&m);
		return rv;
	}

	rv = mbt_session_add_infohash(s, ih);
	if (rv < 0) {
		mbt_infohash_unref(ih);
		mbt_magnet_free(&m);
		return rv;
	}

	/* Connect peer hints */
	for (int i = 0; i < m.peer_count; i++)
		mbt_session_add_peer(s, ih, m.peers[i].addr, m.peers[i].port);

	/* Announce to trackers from magnet */
	struct session_torrent *st = session_find(s, ih);
	if (st) {
		for (int i = 0; i < m.tracker_count && st->tracker_count < 16; i++) {
			char *u = strdup(m.trackers[i]);
			if (u)
				st->trackers[st->tracker_count++] = u;
		}
		if (st->tracker_count > 0)
			session_do_announce(s, st);
	}

	mbt_infohash_unref(ih);
	mbt_magnet_free(&m);
	return 0;
}

int
mbt_session_remove_torrent(struct mbt_session *s, struct mbt_infohash *ih)
{
	struct session_torrent *st = session_find(s, ih);
	if (!st)
		return -ENOENT;

	STAILQ_REMOVE(&s->torrents, st, session_torrent, next);
	mbt_torrent_free(st->torrent);
	mbt_infohash_unref(st->ih);
	free(st);
	return 0;
}

struct mbt_torrent *
mbt_session_find_torrent(struct mbt_session *s, struct mbt_infohash *ih)
{
	struct session_torrent *st = session_find(s, ih);
	return st ? st->torrent : NULL;
}

int
mbt_session_torrent_count(struct mbt_session *s)
{
	int n = 0;
	struct session_torrent *st;
	STAILQ_FOREACH(st, &s->torrents, next)
		n++;
	return n;
}

struct mbt_torrent *
mbt_session_torrent_first(struct mbt_session *s)
{
	struct session_torrent *st = STAILQ_FIRST(&s->torrents);
	return st ? st->torrent : NULL;
}

struct mbt_torrent *
mbt_session_torrent_next(struct mbt_session *s, struct mbt_torrent *after)
{
	struct session_torrent *st;
	STAILQ_FOREACH(st, &s->torrents, next) {
		if (st->torrent == after) {
			st = STAILQ_NEXT(st, next);
			return st ? st->torrent : NULL;
		}
	}
	return NULL;
}

/* --- Peer connection --- */

int
mbt_session_reannounce(struct mbt_session *s, struct mbt_infohash *ih)
{
	if (ih) {
		struct session_torrent *st = session_find(s, ih);
		if (!st)
			return -ENOENT;
		session_do_announce(s, st);
	} else {
		struct session_torrent *st;
		STAILQ_FOREACH(st, &s->torrents, next)
			session_do_announce(s, st);
	}
	return 0;
}

int
mbt_session_add_peer(struct mbt_session *s, struct mbt_infohash *ih,
                     const char *addr, uint16_t port)
{
	int rv, fd;
	struct sockaddr_in sa;
	struct session_torrent *st;
	struct pending_connect *pc;

	/* Find or create torrent */
	st = session_find(s, ih);
	if (!st) {
		rv = mbt_session_add_infohash(s, ih);
		if (rv < 0)
			return rv;
		st = session_find(s, ih);
		if (!st)
			return -ENOMEM;
	}

	/* Check peer limit (active + pending connects) */
	int pending = 0;
	struct pending_connect *pcc;
	STAILQ_FOREACH(pcc, &s->pending, next) {
		if (pcc->torrent == st->torrent)
			pending++;
	}
	if (mbt_torrent_peer_count(st->torrent) + pending >= s->max_peers)
		return 0;

	/* Set up non-blocking connect */
	sa.sin_family = AF_INET;
	rv = inet_pton(AF_INET, addr, &sa.sin_addr);
	if (rv <= 0)
		return -EINVAL;
	sa.sin_port = htons(port);

	fd = socket(AF_INET, SOCK_STREAM | SOCK_NONBLOCK | SOCK_CLOEXEC, 0);
	if (fd < 0)
		return -errno;

	/* Enable TCP keepalive (detect dead connections faster) */
	int yes = 1;
	setsockopt(fd, SOL_SOCKET, SO_KEEPALIVE, &yes, sizeof(yes));

	rv = connect(fd, (struct sockaddr *)&sa, sizeof(sa));
	if (rv < 0 && errno != EINPROGRESS) {
		mbt_log_warn(MBT_LOG_SESSION, "connect to %s:%hu failed: %s", addr, port, strerror(errno));
		close(fd);
		return -errno;
	}

	mbt_log_debug(MBT_LOG_SESSION, "connecting to %s:%hu", addr, port);

	if (rv == 0) {
		/* Connected immediately (localhost) */
		struct mbt_conn *conn;
		rv = mbt_conn_new(s->ev, &conn, fd, NULL, NULL, NULL);
		if (rv < 0) {
			close(fd);
			return rv;
		}
		return mbt_torrent_add_peer(st->torrent, conn);
	}

	/* EINPROGRESS — wait for writable */
	pc = calloc(1, sizeof(*pc));
	if (!pc) {
		close(fd);
		return -ENOMEM;
	}

	pc->session = s;
	pc->torrent = st->torrent;
	pc->fd = fd;
	snprintf(pc->addr, sizeof(pc->addr), "%s", addr);
	pc->port = port;

	rv = mbt_ev_io_new(s->ev, &pc->io, fd, MBT_EV_WRITE, pending_connect_cb, pc);
	if (rv < 0) {
		mbt_log_warn(MBT_LOG_SESSION, "mbt_ev_io_new failed: %s", strerror(-rv));
		close(fd);
		free(pc);
		return rv;
	}

	mbt_log_debug(MBT_LOG_SESSION, "pending connect registered for %s:%hu", addr, port);

	/* 10s connect timeout */
	mbt_ev_timer_new(s->ev, &pc->timeout, 20 * 1000000ULL, pending_connect_timeout, pc);

	STAILQ_INSERT_TAIL(&s->pending, pc, next);
	return 0;
}
