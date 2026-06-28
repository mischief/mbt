// SPDX-License-Identifier: ISC
#include <sys/queue.h>
#include <sys/socket.h>
#include <netinet/in.h>
#include <stdlib.h>
#include <stdio.h>
#include <string.h>
#include <unistd.h>
#include <errno.h>
#include <fcntl.h>
#include <stdint.h>
#include <sys/types.h>
#include <time.h>

#include "mbt/event.h"
#include "mbt/conn.h"
#include "mbt/utp.h"
#include "mbt/infohash.h"
#include "mbt/hex.h"
#include "mbt/torrent.h"
#include "mbt/peer.h"
#include "mbt/session.h"
#include "mbt/ratelimit.h"
#include "mbt/pcp.h"
#include "net.h"
#include "mbt/log.h"
#include "mbt/lsd.h"
#include "mbt/dht.h"
#include "mbt/metainfo.h"
#include "internal.h"
#include "mbt/magnet.h"
#include "mbt/tracker.h"
#include "mbt/state.h"
#include "mbt/peer_source.h"
#include "mbt/picker.h"
#include "mbt/storage.h"
#include "mbt/timers.h"

static int session_state_restore_cb(const struct mbt_state_entry *e, void *arg);

/* --- Torrent list entry --- */

#include "mbt/peerdb.h"

struct mbt_ev_io;
struct mbt_ev_timer;
struct mbt_torrent;

struct session_torrent {
	struct mbt_session *session;
	struct mbt_torrent *torrent;
	struct mbt_infohash *ih;
	char *trackers[16];
	int tracker_count;
	struct mbt_ev_timer *reannounce_timer;
	int announce_interval;
	time_t last_dht_lookup;
	time_t last_dht_announce;
	int started_announced;   /* sent event=started on first announce */
	int completed_announced; /* sent event=completed to tracker */
	int emergency_reannounce; /* fired one early reannounce due to dry peerdb */
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
	enum mbt_peer_source source;
	STAILQ_ENTRY(pending_connect) next;
};

STAILQ_HEAD(pending_connect_list, pending_connect);

/* --- Session --- */

struct mbt_session {
	struct mbt_ev *ev;

	struct mbt_storage default_storage;
	const char *default_data_dir;
	const struct mbt_state_ops *state_ops;
	void *state_ctx;
	mbt_picker_fn picker;
	void *picker_ctx;

	uint16_t listen_port;
	struct mbt_timers timers;
	int max_peers;
	int max_pieces_per_peer;
	int min_peers_before_download;
	int reqq;
	int max_pipeline;
	mbt_http_fn http;
	void *http_arg;
	uint8_t peer_id[20];

	struct mbt_session_callbacks cb;

	/* LSD */
	struct mbt_lsd *lsd;
	struct mbt_ev_io *lsd_io;

	/* DHT */
	struct mbt_dht *dht;

	/* PCP */
	struct mbt_pcp *pcp;
	struct mbt_pcp *pcp_udp;
	struct mbt_ev_timer *pcp_retry_timer;
	const char *pcp_gateway;
	int enable_pcp;
	char external_addr[INET6_ADDRSTRLEN]; /* our external IP from PCP, empty if unknown */
	/* Listen socket (TCP) */
	struct mbt_listener listener;

	/* uTP context (UDP, same port as TCP listener) */
	struct mbt_utp_ctx *utp_ctx;

	/* Maintenance timer (refill peers from pool) */
	struct mbt_ev_timer *maintain_timer;

	/* LSD announcer timer (optional) */
	struct mbt_ev_timer *lsd_announce_timer;

	/* Torrents */
	struct session_torrent_list torrents;

	/* Pending outgoing connections */
	struct pending_connect_list pending;

	/* Rate limiting */
	struct mbt_ratelimit *upload_limit;
	struct mbt_ratelimit *download_limit;
};

/* --- Forward declarations --- */

static void session_pcp_start(struct mbt_session *s);
static void
session_pcp_retry_cb(struct mbt_ev_timer *timer, void *v)
{
	struct mbt_session *s = v;
	(void)timer;
	session_pcp_start(s);
}

static void
session_pcp_on_map(const char *addr, uint16_t port, uint32_t lifetime, void *arg)
{
	struct mbt_session *s = arg;
	(void)lifetime;
	mbt_log_info(MBT_LOG_SESSION, "PCP: external %s:%hu", addr, port);
	snprintf(s->external_addr, sizeof(s->external_addr), "%s", addr);
	/* Success — cancel retry timer */
	if (s->pcp_retry_timer) {
		mbt_ev_timer_free(s->pcp_retry_timer);
		s->pcp_retry_timer = NULL;
	}
}

static void
session_pcp_start(struct mbt_session *s)
{
	if (!s->pcp) {
		int rv = mbt_pcp_new(&s->pcp, s->ev, s->pcp_gateway);
		if (rv < 0) {
			mbt_log_debug(MBT_LOG_SESSION, "PCP: no gateway (%s), retry in 30s",
			              strerror(-rv));
			if (!s->pcp_retry_timer)
				mbt_ev_timer_new(s->ev, &s->pcp_retry_timer,
				                 30 * 1000000ULL, session_pcp_retry_cb, s);
			else
				mbt_ev_timer_mod(s->pcp_retry_timer, 30 * 1000000ULL);
			return;
		}
	}
	mbt_pcp_map(s->pcp, 6 /* TCP */, s->listen_port, 300,
	            session_pcp_on_map, s);
}

static void
session_lsd_announce_cb(struct mbt_ev_timer *timer, void *v)
{
	struct mbt_session *s = v;
	struct session_torrent *st;

	if (!s->lsd || s->listen_port == 0)
		goto reschedule;

	/* Batch all infohashes into one announce */
	const char *hashes[25];
	int n = 0;
	STAILQ_FOREACH(st, &s->torrents, next) {
		if (mbt_torrent_is_private(st->torrent)) continue;
		if (n >= 25) {
			mbt_log_warn(MBT_LOG_LSD, "LSD announce truncated at 25 torrents");
			break;
		}
		/* Use static buffer per entry — safe since we send before returning */
		char hexbuf[25][MBT_MBT_INFOHASH_SHA1HEXLEN + 1];
		mbt_infohash_string(st->ih, hexbuf[n], sizeof(hexbuf[n]));
		hashes[n] = hexbuf[n];
		n++;
	}
	if (n > 0) {
		mbt_lsd_announce_multi(s->lsd, hashes, n, s->listen_port);
		mbt_log_debug(MBT_LOG_LSD, "LSD announce: %d torrents on port %hu", n, s->listen_port);
	}

reschedule:
	mbt_ev_timer_mod(timer, (uint64_t)s->timers.lsd_interval * 1000000ULL);
}

static void
session_maintain_cb(struct mbt_ev_timer *timer, void *v)
{
	struct mbt_session *s = v;
	struct session_torrent *st;

	STAILQ_FOREACH(st, &s->torrents, next) {
		/* Refill peers from peerdb if below cap */
		int need_peers = 0;
		while (mbt_torrent_peer_count(st->torrent) +
		       mbt_peerdb_count(mbt_torrent_peerdb(st->torrent), MBT_PEERDB_CONNECTING) < s->max_peers) {
			struct mbt_peer_entry *pe = mbt_peerdb_select(mbt_torrent_peerdb(st->torrent));
			if (!pe) {
				need_peers = 1;
				break;
			}
			pe->state = MBT_PEERDB_CONNECTING;
			mbt_session_add_peer(s, st->ih, pe->addr, pe->port, pe->source);
		}

		/* Peerdb exhausted and no active peers — fire one early re-announce */
		if (need_peers && mbt_torrent_peer_count(st->torrent) == 0 &&
		    !mbt_torrent_complete(st->torrent) && st->reannounce_timer &&
		    !st->emergency_reannounce) {
			st->emergency_reannounce = 1;
			mbt_ev_timer_mod(st->reannounce_timer, 5 * 1000000ULL);
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
		if (s->dht && !mbt_torrent_complete(st->torrent) && !mbt_torrent_is_private(st->torrent)) {
			time_t now = time(NULL);
			/* Re-lookup aggressively (10s) when few peers, otherwise dht_refresh */
			int interval = mbt_torrent_peer_count(st->torrent) < 5 ? 10 : s->timers.dht_refresh;
			if (now - st->last_dht_lookup >= interval) {
				st->last_dht_lookup = now;
				mbt_session_dht_get_peers(s, st->ih);
			}
		}

		/* DHT announce: tell the network we have this torrent (every 15 min) */
		if (s->dht && mbt_torrent_have_count(st->torrent) > 0 &&
		    !mbt_torrent_is_private(st->torrent)) {
			time_t now = time(NULL);
			if (now - st->last_dht_announce >= s->timers.dht_refresh) {
				st->last_dht_announce = now;
				mbt_dht_announce(s->dht, st->ih, s->listen_port);
			}
		}
	}

	mbt_ev_timer_mod(timer, (uint64_t)s->timers.maintain_interval * 1000000ULL);
}

static void session_generate_peer_id(uint8_t *id);

/* --- Pending connect handler --- */

static void
pending_connect_mark_failed(struct pending_connect *pc)
{
	struct mbt_session *s = pc->session;
	struct session_torrent *st;
	STAILQ_FOREACH(st, &s->torrents, next) {
		if (st->torrent != pc->torrent)
			continue;
		struct mbt_peer_entry *pe = mbt_peerdb_find(
			mbt_torrent_peerdb(st->torrent), pc->addr, pc->port);
		if (pe) {
			pe->state = MBT_PEERDB_FAILED;
			pe->failures++;
		}
		break;
	}
}

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
	pending_connect_mark_failed(pc);
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
	rv = mbt_torrent_add_peer(pc->torrent, conn, pc->source);
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
	pending_connect_mark_failed(pc);
	STAILQ_REMOVE(&s->pending, pc, pending_connect, next);
	free(pc);
}

/* --- Helpers --- */

static void
session_generate_peer_id(uint8_t *id)
{
	/* -MB0001- followed by 12 random bytes */
	memcpy(id, "-MB0001-", 8);
	if (getentropy(id + 8, 12) < 0) {}
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
	struct sockaddr_storage addr;
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

	/* Reject self-connections */
	if (memcmp(their_id, s->peer_id, MBT_PEER_ID_LEN) == 0) {
		char hex[MBT_PEER_ID_STRLEN + 1];
		mbt_hex_encode(s->peer_id, MBT_PEER_ID_LEN, hex, sizeof(hex));
		mbt_log_debug(MBT_LOG_SESSION, "self-connection rejected: peer_id=%s", hex);
		close(pa->fd);
		mbt_ev_io_free(pa->io);
		free(pa);
		return;
	}

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

	/* Reject duplicate peer_id (same peer connecting twice) */
	for (int i = 0; i < mbt_torrent_peer_count(st->torrent); i++) {
		struct mbt_peer *ep = mbt_torrent_peer_at(st->torrent, i);
		if (ep && mbt_peer_id_eq(mbt_peer_id(ep), their_id)) {
			close(pa->fd);
			mbt_ev_io_free(pa->io);
			free(pa);
			return;
		}
	}

	/* Enforce peer limit */
	if (mbt_torrent_peer_count(st->torrent) >= s->max_peers) {
		close(pa->fd);
		mbt_ev_io_free(pa->io);
		free(pa);
		return;
	}

	char paddr[INET6_ADDRSTRLEN];
	uint16_t pport;
	mbt_sockaddr_split(&pa->addr, paddr, sizeof(paddr), &pport);
	mbt_log_info(MBT_LOG_SESSION, "incoming peer %s:%hu", paddr, pport);

	mbt_ev_io_free(pa->io);

	struct mbt_conn *conn;
	int rv = mbt_conn_new(s->ev, &conn, pa->fd, NULL, NULL, NULL);
	if (rv < 0) { close(pa->fd); free(pa); return; }

	rv = mbt_torrent_add_peer_incoming(st->torrent, conn, ext, their_id);
	if (rv < 0)
		mbt_conn_free(conn);
	free(pa);
}

/* --- uTP incoming (async BT handshake over uTP conn) --- */

struct pending_utp_accept {
	struct mbt_session *session;
	struct mbt_conn    *conn;
	unsigned char       hs[HANDSHAKE_SIZE];
	size_t              hs_got;
};

static void
pending_utp_accept_free(struct pending_utp_accept *pa, int close_conn)
{
	if (close_conn)
		mbt_conn_free(pa->conn);
	free(pa);
}

static void
pending_utp_read(struct mbt_conn *c, void *v)
{
	struct pending_utp_accept *pa = v;
	struct mbt_session *s = pa->session;

	ssize_t n = mbt_conn_read(c, pa->hs + pa->hs_got,
	                          HANDSHAKE_SIZE - pa->hs_got);
	if (n <= 0)
		return;
	pa->hs_got += n;
	if (pa->hs_got < HANDSHAKE_SIZE)
		return;

	/* Validate BT handshake */
	if (pa->hs[0] != 19 || memcmp(pa->hs + 1, "BitTorrent protocol", 19) != 0) {
		pending_utp_accept_free(pa, 1);
		return;
	}

	uint8_t ext[8], hash[20], their_id[20];
	memcpy(ext,      pa->hs + 20, 8);
	memcpy(hash,     pa->hs + 28, 20);
	memcpy(their_id, pa->hs + 48, 20);

	if (memcmp(their_id, s->peer_id, MBT_PEER_ID_LEN) == 0) {
		pending_utp_accept_free(pa, 1);
		return;
	}

	struct session_torrent *st;
	STAILQ_FOREACH(st, &s->torrents, next) {
		if (memcmp(st->ih->hash_sha1, hash, 20) == 0)
			break;
	}
	if (!st) {
		pending_utp_accept_free(pa, 1);
		return;
	}

	for (int i = 0; i < mbt_torrent_peer_count(st->torrent); i++) {
		struct mbt_peer *ep = mbt_torrent_peer_at(st->torrent, i);
		if (ep && mbt_peer_id_eq(mbt_peer_id(ep), their_id)) {
			pending_utp_accept_free(pa, 1);
			return;
		}
	}

	if (mbt_torrent_peer_count(st->torrent) >= s->max_peers) {
		pending_utp_accept_free(pa, 1);
		return;
	}

	char paddr[INET6_ADDRSTRLEN];
	uint16_t pport;
	mbt_conn_peer_addr(pa->conn, paddr, sizeof(paddr), &pport);
	mbt_log_info(MBT_LOG_SESSION, "incoming uTP peer %s:%hu", paddr, pport);

	struct mbt_conn *conn = pa->conn;
	free(pa); /* conn ownership transferred to torrent */

	int rv = mbt_torrent_add_peer_incoming(st->torrent, conn, ext, their_id);
	if (rv < 0)
		mbt_conn_free(conn);
}

static void
pending_utp_event(struct mbt_conn *c, int err, void *v)
{
	struct pending_utp_accept *pa = v;
	(void)c; (void)err;
	pending_utp_accept_free(pa, 1);
}

static void
utp_accept_cb(struct mbt_utp_ctx *ctx, struct mbt_conn *conn,
              const struct sockaddr *addr, socklen_t addrlen, void *arg)
{
	struct mbt_session *s = arg;
	(void)ctx; (void)addr; (void)addrlen;

	struct pending_utp_accept *pa = calloc(1, sizeof(*pa));
	if (!pa) { mbt_conn_free(conn); return; }
	pa->session = s;
	pa->conn = conn;

	mbt_conn_set_watermark(conn, 1);
	mbt_conn_set_callbacks(conn, pending_utp_read, pending_utp_event, pa);
}

static void
listen_accept_cb(struct mbt_ev_io *io, int fd, uint32_t events, void *v)
{
	struct mbt_session *s = v;
	struct sockaddr_storage addr;
	socklen_t alen = sizeof(addr);
	int cfd;
	(void)io; (void)events;

	cfd = accept(fd, (struct sockaddr *)&addr, &alen);
	if (cfd >= 0)
		fcntl(cfd, F_SETFL, fcntl(cfd, F_GETFL) | O_NONBLOCK);
	if (cfd < 0)
		return;

	struct pending_accept *pa = calloc(1, sizeof(*pa));
	if (!pa) { close(cfd); return; }
	pa->session = s;
	pa->fd = cfd;
	memcpy(&pa->addr, &addr, sizeof(pa->addr));

	int rv = mbt_ev_io_new(s->ev, &pa->io, cfd, MBT_EV_READ, pending_accept_cb, pa);
	if (rv < 0) { close(cfd); free(pa); return; }
}
static int
session_listen(struct mbt_session *s)
{
	int rv = mbt_listen_tcp(&s->listener, s->ev, s->listen_port, listen_accept_cb, s);
	if (rv < 0) return rv;
	s->listen_port = s->listener.port; /* update with actual bound port */

	/* uTP on the same port number (UDP) */
	rv = mbt_utp_ctx_new(&s->utp_ctx, s->ev, s->listen_port,
	                     utp_accept_cb, s);
	if (rv < 0) {
		mbt_log_warn(MBT_LOG_SESSION, "uTP listen failed: %s", strerror(-rv));
		s->utp_ctx = NULL; /* non-fatal: TCP still works */
	} else {
		mbt_log_info(MBT_LOG_SESSION, "uTP listening on port %hu",
		             mbt_utp_ctx_port(s->utp_ctx));
	}

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
	s->default_storage = cfg->default_storage;
	s->default_data_dir = cfg->default_data_dir;
	s->state_ops = cfg->state_ops;
	s->state_ctx = cfg->state_ctx;
	s->picker = cfg->picker;
	s->picker_ctx = cfg->picker_ctx;
	s->listen_port = cfg->listen_port;
	s->timers = cfg->timers;
	if (cfg->upload_rate)
		mbt_ratelimit_new(&s->upload_limit, cfg->upload_rate, 0);
	if (cfg->download_rate)
		mbt_ratelimit_new(&s->download_limit, cfg->download_rate, 0);
	/* Apply defaults for zero values */
	if (!s->timers.maintain_interval) s->timers.maintain_interval = 5;
	if (!s->timers.choke_interval) s->timers.choke_interval = 10;
	if (!s->timers.pex_interval) s->timers.pex_interval = 60;
	if (!s->timers.request_timeout) s->timers.request_timeout = 15;
	if (!s->timers.keepalive_interval) s->timers.keepalive_interval = 120;
	if (!s->timers.snub_timeout) s->timers.snub_timeout = 60;
	if (!s->timers.dht_refresh) s->timers.dht_refresh = 900;
	if (!s->timers.lsd_interval) s->timers.lsd_interval = 300;
	if (!s->timers.tracker_interval) s->timers.tracker_interval = 1800;
	if (!s->timers.connect_timeout) s->timers.connect_timeout = 10;
	s->max_peers = cfg->max_peers ? cfg->max_peers : 30;
	s->max_pieces_per_peer = cfg->max_pieces_per_peer ? cfg->max_pieces_per_peer : 4;
	s->min_peers_before_download = cfg->min_peers_before_download;
	s->reqq = cfg->reqq ? cfg->reqq : 500;
	s->max_pipeline = cfg->max_pipeline;
	s->http = cfg->http;
	s->http_arg = cfg->http_arg;

	STAILQ_INIT(&s->torrents);
	STAILQ_INIT(&s->pending);

	session_generate_peer_id(s->peer_id);
	{
		char hex[41];
		mbt_hex_encode(s->peer_id, 20, hex, sizeof(hex));
		mbt_log_info(MBT_LOG_SESSION, "our peer_id: %s", hex);
	}

	/* Maintenance timer: refill peers from pool every 5s */
	mbt_ev_timer_new(s->ev, &s->maintain_timer, (uint64_t)s->timers.maintain_interval * 1000000ULL, session_maintain_cb, s);

	/* Listen socket for incoming peers */
	session_listen(s);

	/* LSD announcer: multicast our infohashes every 5 min */
	if (cfg->mbt_lsd_announce && s->lsd && s->listen_port > 0)
		mbt_ev_timer_new(s->ev, &s->lsd_announce_timer, 15 * 1000000ULL,
		                 session_lsd_announce_cb, s);

	/* PCP port mapping */
	if (cfg->enable_pcp && s->listen_port > 0) {
		s->enable_pcp = 1;
		s->pcp_gateway = cfg->pcp_gateway;
		session_pcp_start(s);
	}

	/* Restore persisted torrents */
	if (s->state_ops && s->state_ops->enumerate) {
		s->state_ops->enumerate(s->state_ctx,
		    session_state_restore_cb, s);
	}

	*out = s;
	return 0;
}

void
mbt_session_free(struct mbt_session *s)
{
	if (!s)
		return;

	if (s->utp_ctx)
		mbt_utp_ctx_free(s->utp_ctx);
	mbt_listener_close(&s->listener);
	if (s->lsd_announce_timer)
		mbt_ev_timer_free(s->lsd_announce_timer);
	if (s->lsd_io)
		mbt_ev_io_free(s->lsd_io);
	if (s->lsd)
		mbt_lsd_freep(&s->lsd);
	if (s->dht)
		mbt_dht_free(s->dht);
	if (s->pcp_retry_timer)
		mbt_ev_timer_free(s->pcp_retry_timer);
	if (s->pcp)
		mbt_pcp_free(s->pcp);
	if (s->pcp_udp)
		mbt_pcp_free(s->pcp_udp);

	/* Free pending connects */
	struct pending_connect *pc;
	while ((pc = STAILQ_FIRST(&s->pending))) {
		STAILQ_REMOVE_HEAD(&s->pending, next);
		mbt_ev_io_free(pc->io);
		close(pc->fd);
		free(pc);
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

	if (s->maintain_timer)
		mbt_ev_timer_free(s->maintain_timer);

	mbt_ratelimit_free(s->upload_limit);
	mbt_ratelimit_free(s->download_limit);
	if (s->state_ops && s->state_ops->close)
		s->state_ops->close(s->state_ctx);
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
return -ENOSYS; /* no built-in backend */
#endif
	if (rv < 0)
		return rv;

	struct mbt_session_config cfg = {
		.ev = ev,
		.default_storage = mbt_storage_file_new(path),
		.default_data_dir = path,
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

	mbt_log_debug(MBT_LOG_LSD, "LSD received from %s:%hu", info->remote, info->port);

	/* Ignore our own announcements (cookie match) */
	if (info->cookie && s->lsd && strcmp(info->cookie, s->lsd->cookie) == 0) {
		mbt_log_debug(MBT_LOG_LSD, "LSD self-filter: cookie=%s", info->cookie);
		return;
	}
	mbt_log_debug(MBT_LOG_LSD, "LSD cookie: ours=%s theirs=%s",
	              s->lsd ? s->lsd->cookie : "(null)",
	              info->cookie ? info->cookie : "(null)");

	STAILQ_FOREACH(ih, &info->infohashes, next) {
		struct session_torrent *st = session_find(s, ih);

		if (s->cb.on_lsd_announce) {
			if (s->cb.on_lsd_announce(s, ih, info->remote, info->port, s->cb.arg)) {
				if (!st) {
					mbt_session_add_infohash(s, ih, NULL);
					st = session_find(s, ih);
				}
				if (st)
					mbt_peerdb_insert(mbt_torrent_peerdb(st->torrent), info->remote, info->port, MBT_SOURCE_LSD);
				mbt_session_add_peer(s, ih, info->remote, info->port, MBT_SOURCE_LSD);
			}
		} else {
			if (st) {
				mbt_log_info(MBT_LOG_LSD, "LSD peer %s:%hu for known torrent", info->remote, info->port);
				mbt_peerdb_insert(mbt_torrent_peerdb(st->torrent), info->remote, info->port, MBT_SOURCE_LSD);
				mbt_session_add_peer(s, ih, info->remote, info->port, MBT_SOURCE_LSD);
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

	/* Start announce timer if we have a listen port */
	if (!s->lsd_announce_timer && s->listen_port > 0)
		mbt_ev_timer_new(s->ev, &s->lsd_announce_timer, 15 * 1000000ULL,
		                 session_lsd_announce_cb, s);

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

	/* Map UDP port if PCP is active */
	if (s->pcp_udp)
		; /* already mapped */
	else if (s->enable_pcp) {
		mbt_pcp_new(&s->pcp_udp, s->ev, s->pcp_gateway);
		if (s->pcp_udp)
			mbt_pcp_map(s->pcp_udp, 17 /* UDP */, s->listen_port, 300, NULL, NULL);
	}

	return 0;
}

int
mbt_session_enable_dht_local(struct mbt_session *s)
{
	if (s->dht)
		return 0;

	int rv = mbt_dht_new(&s->dht, s->ev, s->listen_port ? s->listen_port : 6881);
	if (rv < 0)
		return rv;

	/* No bootstrap — relies on PORT messages from peers */
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

uint16_t
mbt_session_dht_port(struct mbt_session *s)
{
	return (s && s->dht) ? s->listen_port : 0;
}

uint16_t
mbt_session_listen_port(struct mbt_session *s)
{
	return s ? s->listen_port : 0;
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
		mbt_session_add_peer(s, ih, addr, port, MBT_SOURCE_TRACKER);
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
	if (st)
		st->emergency_reannounce = 0; /* fresh peers — allow one more emergency if needed */
	if (st && interval > 0) {
		/* Use configured interval if shorter than server's */
		if (s->timers.tracker_interval > 0 && s->timers.tracker_interval < interval)
			interval = s->timers.tracker_interval;
		/* Clamp: min 5s, max 15min */
		if (interval < 5) interval = 5;
		if (interval > 900) interval = 900;
		st->announce_interval = interval;
		/* Add jitter: 75%-125% of interval to prevent thundering herd */
		uint64_t base = (uint64_t)interval * 1000000ULL;
		uint64_t jitter = base / 4;
		uint64_t delay = base - jitter / 2 + mbt_random_uniform(jitter);
		if (!st->reannounce_timer) {
			mbt_ev_timer_new(s->ev, &st->reannounce_timer,
			                 delay, session_reannounce_timer_cb, st);
		} else {
			mbt_ev_timer_mod(st->reannounce_timer, delay);
		}
		mbt_log_debug(MBT_LOG_SESSION, "next announce in %llums",
		              (unsigned long long)(delay / 1000));
	}
}

static void
session_do_announce(struct mbt_session *s, struct session_torrent *st)
{
	if (st->tracker_count == 0)
		return;

	struct mbt_torrent *t = st->torrent;
	uint64_t left = 0;
	int event = st->started_announced ? MBT_TRACKER_EVENT_NONE : MBT_TRACKER_EVENT_STARTED;
	if (!st->started_announced) st->started_announced = 1;

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

	for (int i = 0; i < st->tracker_count; i++) {
		mbt_log_debug(MBT_LOG_SESSION, "announcing to tracker[%d]: %s", i, st->trackers[i]);
		mbt_tracker_announce(st->trackers[i], &treq, s->http, s->http_arg);
	}
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

static int
session_state_restore_cb(const struct mbt_state_entry *e, void *arg)
{
	struct mbt_session *s = arg;
	if (!e->metadata || e->metadata_len == 0)
		return 0;
	struct mbt_add_torrent_cfg cfg = {
		.data_dir = e->data_dir,
		.storage  = e->data_dir ? mbt_storage_file_new(e->data_dir) : s->default_storage,
	};
	int rv = mbt_session_add_torrent_buf(s, e->metadata, e->metadata_len, &cfg);
	if (rv < 0)
		mbt_log_warn(MBT_LOG_SESSION, "state restore: failed to add torrent: %s", strerror(-rv));
	return 0; /* continue enumeration even on error */
}

static void
session_on_complete(struct mbt_torrent *t, void *arg)
{
	struct mbt_session *s = arg;
	if (s->state_ops && s->state_ops->save)
		s->state_ops->save(s->state_ctx, t);
	if (s->cb.on_torrent_complete)
		s->cb.on_torrent_complete(s, t, s->cb.arg);
}

static void
session_on_piece_complete(struct mbt_torrent *t, uint32_t piece, void *arg)
{
	(void)piece;
	struct mbt_session *s = arg;
	if (s->state_ops && s->state_ops->save)
		s->state_ops->save(s->state_ctx, t);
}

static void
session_on_error(struct mbt_torrent *t, int err, void *arg)
{
	struct mbt_session *s = arg;
	if (s->cb.on_torrent_error)
		s->cb.on_torrent_error(s, t, err, s->cb.arg);
}

int
mbt_session_add_infohash(struct mbt_session *s, struct mbt_infohash *ih, struct mbt_add_torrent_cfg *acfg)
{
	/* Already have it? */
	if (session_find(s, ih))
		return 0;

	struct session_torrent *st = calloc(1, sizeof(*st));
	if (!st)
		return -ENOMEM;

	struct mbt_storage stor = (acfg && acfg->storage.ops) ? acfg->storage : s->default_storage;
	const char *ddir = (acfg && acfg->data_dir) ? acfg->data_dir : s->default_data_dir;
	struct mbt_torrent_config tcfg = {
		.ev = s->ev,
		.ih = ih,
		.peer_id = s->peer_id,
		.storage = stor,
		.data_dir = ddir,
		.picker = s->picker,
		.picker_ctx = s->picker_ctx,
		.timers = s->timers,
	};

	int rv = mbt_torrent_new(&st->torrent, &tcfg);
	if (rv < 0) {
		free(st);
		return rv;
	}
	mbt_torrent_set_session(st->torrent, s);

	struct mbt_torrent_callbacks tcb = {
		.on_complete       = session_on_complete,
		.on_piece_complete = session_on_piece_complete,
		.on_error          = session_on_error,
	};
	mbt_torrent_set_callbacks(st->torrent, &tcb, s);

	st->session = s;
	st->ih = mbt_infohash_ref(ih);
	STAILQ_INSERT_TAIL(&s->torrents, st, next);

	if (s->dht && st->torrent && !mbt_torrent_is_private(st->torrent))
		mbt_session_dht_get_peers(s, ih);

	/* Immediate LSD announce for this infohash */
	if (s->lsd && s->listen_port > 0 && !mbt_torrent_is_private(st->torrent)) {
		char hex[MBT_MBT_INFOHASH_SHA1HEXLEN + 1];
		mbt_infohash_string(ih, hex, sizeof(hex));
		mbt_lsd_announce(s->lsd, hex, s->listen_port);
	}

	return 0;
}

int
mbt_session_add_torrent_buf(struct mbt_session *s, const void *data, size_t len, struct mbt_add_torrent_cfg *cfg)
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

	rv = mbt_session_add_infohash(s, ih, cfg);
	if (rv < 0) { mbt_metainfo_free(mi); return rv; }

	t = mbt_session_find_torrent(s, ih);
	if (t) {
		const void *raw = mbt_metainfo_raw_info(mi, &raw_len);
		mbt_torrent_set_metadata(t, raw, raw_len);
		if (s->state_ops && s->state_ops->save)
			s->state_ops->save(s->state_ctx, t);
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

	/* Bootstrap DHT from torrent's nodes list */
	if (s->dht) {
		for (int i = 0; i < mbt_metainfo_node_count(mi); i++)
			mbt_dht_bootstrap(s->dht, mbt_metainfo_node_host(mi, i),
			                  mbt_metainfo_node_port(mi, i));
	}

	mbt_metainfo_free(mi);
	return 0;
}

int
mbt_session_add_torrent_file(struct mbt_session *s, const char *path, struct mbt_add_torrent_cfg *cfg)
{
	FILE *fp = fopen(path, "rb");
	if (!fp) return -errno;

	unsigned char *buf = malloc(4 * 1024 * 1024);
	if (!buf) { fclose(fp); return -ENOMEM; }

	size_t len = fread(buf, 1, 4 * 1024 * 1024, fp);
	fclose(fp);

	if (len == 0) { free(buf); return -EIO; }

	int rv = mbt_session_add_torrent_buf(s, buf, len, cfg);
	free(buf);
	return rv;
}

int
mbt_session_add_magnet(struct mbt_session *s, const char *uri, struct mbt_add_torrent_cfg *cfg)
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

	rv = mbt_session_add_infohash(s, ih, cfg);
	if (rv < 0) {
		mbt_infohash_unref(ih);
		mbt_magnet_free(&m);
		return rv;
	}

	/* Connect peer hints (from x.pe in magnet URI) */
	for (int i = 0; i < m.peer_count; i++)
		mbt_session_add_peer(s, ih, m.peers[i].addr, m.peers[i].port, MBT_SOURCE_MAGNET);

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

static const char *mbt_fallback_trackers[] = {
	"udp://tracker.opentrackr.org:1337/announce",
	"udp://open.demonii.com:1337/announce",
	"udp://tracker.openbittorrent.com:6969/announce",
	"udp://exodus.desync.com:6969/announce",
	"udp://tracker.torrent.eu.org:451/announce",
	NULL,
};

int
mbt_session_add_tracker(struct mbt_session *s, struct mbt_torrent *t, const char *url)
{
	struct session_torrent *st;
	STAILQ_FOREACH(st, &s->torrents, next) {
		if (st->torrent != t) continue;
		if (st->tracker_count >= 16) return -ENOSPC;
		/* Skip if already present */
		for (int i = 0; i < st->tracker_count; i++)
			if (strcmp(st->trackers[i], url) == 0) return 0;
		char *u = strdup(url);
		if (!u) return -ENOMEM;
		st->trackers[st->tracker_count++] = u;
		return 0;
	}
	return -ENOENT;
}

void
mbt_session_add_tracker_fallbacks(struct mbt_session *s, struct mbt_torrent *t)
{
	for (int i = 0; mbt_fallback_trackers[i]; i++)
		mbt_session_add_tracker(s, t, mbt_fallback_trackers[i]);
}

int
mbt_session_torrent_tracker_count(struct mbt_session *s, struct mbt_torrent *t)
{
	struct session_torrent *st;
	STAILQ_FOREACH(st, &s->torrents, next)
		if (st->torrent == t) return st->tracker_count;
	return 0;
}

const char *
mbt_session_torrent_tracker(struct mbt_session *s, struct mbt_torrent *t, int idx)
{
	struct session_torrent *st;
	STAILQ_FOREACH(st, &s->torrents, next)
		if (st->torrent == t)
			return (idx < st->tracker_count) ? st->trackers[idx] : NULL;
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
                     const char *addr, uint16_t port, enum mbt_peer_source source)
{
	int rv, fd;
	struct session_torrent *st;
	struct pending_connect *pc;

	/* Find or create torrent */
	st = session_find(s, ih);
	if (!st) {
		rv = mbt_session_add_infohash(s, ih, NULL);
		if (rv < 0)
			return rv;
		st = session_find(s, ih);
		if (!st)
			return -ENOMEM;
	}

	/* Check peer limit (active + pending connects) */
	int pending = 0;

	/* Don't initiate outbound connections when seeding (except LSD) */
	if (mbt_torrent_complete(st->torrent) && source != MBT_SOURCE_LSD)
		return 0;

	struct pending_connect *pcc;
	STAILQ_FOREACH(pcc, &s->pending, next) {
		if (pcc->torrent == st->torrent)
			pending++;
	}
	if (mbt_torrent_peer_count(st->torrent) + pending >= s->max_peers)
		return 0;

	/* Skip self-connect (our own external address via PCP) */
	if (s->external_addr[0] && strcmp(addr, s->external_addr) == 0 && port == s->listen_port)
		return 0;

	/* Skip if already connected or connecting to this address */
	STAILQ_FOREACH(pcc, &s->pending, next) {
		if (pcc->torrent == st->torrent &&
		    strcmp(pcc->addr, addr) == 0 && pcc->port == port)
			return 0;
	}
	for (int i = 0; i < mbt_torrent_peer_count(st->torrent); i++) {
		struct mbt_peer *p = mbt_torrent_peer_at(st->torrent, i);
		uint16_t lp = mbt_peer_port(p);
		if (p && lp != 0 && lp == port && strcmp(mbt_peer_addr(p), addr) == 0)
			return 0;
	}

	/* Set up non-blocking connect */
	fd = mbt_connect_tcp(addr, port);
	if (fd < 0) {
		mbt_log_warn(MBT_LOG_SESSION, "connect to %s:%hu failed: %s", addr, port, strerror(-fd));
		return fd;
	}

	/* Enable TCP keepalive */
	int yes = 1;
	setsockopt(fd, SOL_SOCKET, SO_KEEPALIVE, &yes, sizeof(yes));

	mbt_log_debug(MBT_LOG_SESSION, "connecting to %s:%hu from %s", addr, port,
	              mbt_peer_source_str(source));

	/* Wait for writable (handles both instant connect and EINPROGRESS) */
	pc = calloc(1, sizeof(*pc));
	if (!pc) {
		close(fd);
		return -ENOMEM;
	}

	pc->session = s;
	pc->torrent = st->torrent;
	pc->fd = fd;
	pc->source = source;
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
	mbt_ev_timer_new(s->ev, &pc->timeout, (uint64_t)s->timers.connect_timeout * 1000000ULL, pending_connect_timeout, pc);

	STAILQ_INSERT_TAIL(&s->pending, pc, next);
	return 0;
}

struct mbt_ratelimit *
mbt_session_get_upload_limit(struct mbt_session *s)
{
	return s ? s->upload_limit : NULL;
}

struct mbt_ratelimit *
mbt_session_get_download_limit(struct mbt_session *s)
{
	return s ? s->download_limit : NULL;
}

int
mbt_session_max_pieces_per_peer(struct mbt_session *s)
{
	return s ? s->max_pieces_per_peer : 4;
}

int
mbt_session_min_peers_before_download(struct mbt_session *s)
{
	return s ? s->min_peers_before_download : 0;
}

int
mbt_session_reqq(struct mbt_session *s)
{
	return s ? s->reqq : 0;
}

int
mbt_session_max_pipeline(struct mbt_session *s)
{
	return (s && s->max_pipeline > 0) ? s->max_pipeline : 0;
}

void
mbt_session_set_upload_rate(struct mbt_session *s, uint64_t bytes_per_sec)
{
	if (!s->upload_limit && bytes_per_sec)
		mbt_ratelimit_new(&s->upload_limit, bytes_per_sec, 0);
	else if (s->upload_limit)
		mbt_ratelimit_set_rate(s->upload_limit, bytes_per_sec);
}

void
mbt_session_set_download_rate(struct mbt_session *s, uint64_t bytes_per_sec)
{
	if (!s->download_limit && bytes_per_sec)
		mbt_ratelimit_new(&s->download_limit, bytes_per_sec, 0);
	else if (s->download_limit)
		mbt_ratelimit_set_rate(s->download_limit, bytes_per_sec);
}

uint64_t
mbt_session_upload_rate(struct mbt_session *s)
{
	return mbt_ratelimit_rate(s->upload_limit);
}

uint64_t
mbt_session_download_rate(struct mbt_session *s)
{
	return mbt_ratelimit_rate(s->download_limit);
}
