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

#include "event.h"
#include "conn.h"
#include "infohash.h"
#include "hex.h"
#include "be.h"
#include "torrent.h"
#include "session.h"
#include "log.h"
#include "lsd.h"
#include "magnet.h"
#include "tracker.h"

/* --- Torrent list entry --- */

struct session_torrent {
	struct mbt_torrent *torrent;
	struct mbt_infohash *ih;
	STAILQ_ENTRY(session_torrent) next;
};

STAILQ_HEAD(session_torrent_list, session_torrent);

/* --- Pending connect --- */

struct pending_connect {
	struct mbt_session *session;
	struct mbt_torrent *torrent;
	struct mbt_ev_io *io;
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
	uint8_t peer_id[20];

	struct mbt_session_callbacks cb;

	/* LSD */
	struct lsd *lsd;
	struct mbt_ev_io *lsd_io;

	/* Torrents */
	struct session_torrent_list torrents;

	/* Pending outgoing connections */
	struct pending_connect_list pending;
};

/* --- Forward declarations --- */
static void session_generate_peer_id(uint8_t *id);

/* --- Pending connect handler --- */

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
		mbt_log_warn(MBT_LOG_SESSION, "connect failed: %s", strerror(error ? error : errno));
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
	STAILQ_REMOVE(&s->pending, pc, pending_connect, next);
	free(pc);
	return;

fail:
	close(pc->fd);
	if (pc->io)
		mbt_ev_io_free(pc->io);
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

	STAILQ_INIT(&s->torrents);
	STAILQ_INIT(&s->pending);

	session_generate_peer_id(s->peer_id);

	*out = s;
	return 0;
}

void
mbt_session_free(struct mbt_session *s)
{
	if (!s)
		return;

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
		free(st);
	}

	if (s->lsd_io)
		mbt_ev_io_free(s->lsd_io);

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
	__attribute__((cleanup(lsd_frame_freep))) struct lsd_frame *frame = NULL;
	__attribute__((cleanup(lsd_frame_info_freep))) struct lsd_frame_info *info = NULL;
	struct mbt_infohash *ih;

	(void)io;
	(void)fd;
	(void)events;

	rv = lsd_recv(s->lsd, &frame);
	if (rv < 0)
		return;

	rv = lsd_frame_parse(frame, &info);
	if (rv < 0)
		return;

	STAILQ_FOREACH(ih, &info->infohashes, next) {
		mbt_session_add_peer(s, ih, info->remote, info->port);
	}
}

int
mbt_session_enable_lsd(struct mbt_session *s)
{
	int rv;

	if (s->lsd)
		return 0;

	rv = lsd_init(&s->lsd, O_NONBLOCK);
	if (rv < 0)
		return rv;

	rv = mbt_ev_io_new(s->ev, &s->lsd_io, s->lsd->fd, MBT_EV_READ,
	                   session_lsd_io_cb, s);
	if (rv < 0) {
		lsd_freep(&s->lsd);
		return rv;
	}

	return 0;
}

/* --- Tracker callbacks --- */

static void
session_tracker_peer_cb(struct mbt_infohash *ih, const char *addr, uint16_t port, void *arg)
{
	struct mbt_session *s = arg;
	mbt_session_add_peer(s, ih, addr, port);
}

static void
session_tracker_done_cb(struct mbt_infohash *ih, int err, int interval, void *arg)
{
	(void)ih;
	(void)interval;
	(void)arg;
	if (err < 0)
		mbt_log_warn(MBT_LOG_SESSION, "tracker announce failed: %s", strerror(-err));
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

	st->ih = mbt_infohash_ref(ih);
	STAILQ_INSERT_TAIL(&s->torrents, st, next);

	return 0;
}

int
mbt_session_add_torrent_file(struct mbt_session *s, const char *path)
{
	FILE *fp;
	unsigned char *buf;
	size_t len;
	int rv;
	struct be_value *be = NULL, *info;
	struct mbt_infohash *ih = NULL;
	struct mbt_torrent *t;
	SHA1_CTX ctx;
	uint8_t digest[20];
	char hexstr[41];
	unsigned char *info_encoded = NULL;
	size_t info_len;

	fp = fopen(path, "rb");
	if (!fp)
		return -errno;

	buf = malloc(4 * 1024 * 1024); /* 4MB max torrent file */
	if (!buf) {
		fclose(fp);
		return -ENOMEM;
	}

	len = fread(buf, 1, 4 * 1024 * 1024, fp);
	fclose(fp);

	if (len == 0) {
		free(buf);
		return -EIO;
	}

	rv = be_value_parse(buf, len, &be);
	free(buf);
	if (rv < 0)
		return rv;

	info = be_find(be, "info");
	if (!info) {
		be_value_free(be);
		return -EINVAL;
	}

	/* Encode info dict to get its SHA1 (= infohash) */
	rv = be_value_encode(info, &info_encoded, &info_len);
	if (rv < 0) {
		be_value_free(be);
		return rv;
	}

	SHA1Init(&ctx);
	SHA1Update(&ctx, info_encoded, info_len);
	SHA1Final(digest, &ctx);

	/* Create infohash from hex */
	mbintohex(digest, 20, hexstr, sizeof(hexstr));
	rv = mbt_infohash_new(&ih, MBT_INFOHASH_SHA1, hexstr);
	if (rv < 0) {
		free(info_encoded);
		be_value_free(be);
		return rv;
	}

	/* Add torrent */
	rv = mbt_session_add_infohash(s, ih);
	if (rv < 0) {
		mbt_infohash_unref(ih);
		free(info_encoded);
		be_value_free(be);
		return rv;
	}

	/* Set metadata on it */
	t = mbt_session_find_torrent(s, ih);
	if (t)
		mbt_torrent_set_metadata(t, info_encoded, info_len);

	/* Announce to tracker if present */
	struct be_value *announce = be_find(be, "announce");
	if (announce && announce->type == BE_STRING && t) {
		char *url = strndup(announce->string.iov_base, announce->string.iov_len);
		if (url) {
			if (strncmp(url, "udp://", 6) == 0) {
				struct mbt_tracker_request treq = {
					.ev = s->ev,
					.ih = ih,
					.peer_id = s->peer_id,
					.listen_port = s->listen_port,
					.left = 0, /* TODO: compute from metadata */
					.peer_cb = session_tracker_peer_cb,
					.done_cb = session_tracker_done_cb,
					.arg = s,
				};
				mbt_tracker_udp_announce(url, &treq);
			}
			/* TODO: http:// tracker */
			free(url);
		}
	}

	mbt_infohash_unref(ih);
	free(info_encoded);
	be_value_free(be);

	return 0;
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

	/* TODO: announce to trackers once tracker client is implemented */

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

	/* Set up non-blocking connect */
	sa.sin_family = AF_INET;
	rv = inet_pton(AF_INET, addr, &sa.sin_addr);
	if (rv <= 0)
		return -EINVAL;
	sa.sin_port = htons(port);

	fd = socket(AF_INET, SOCK_STREAM | SOCK_NONBLOCK | SOCK_CLOEXEC, 0);
	if (fd < 0)
		return -errno;

	rv = connect(fd, (struct sockaddr *)&sa, sizeof(sa));
	if (rv < 0 && errno != EINPROGRESS) {
		mbt_log_warn(MBT_LOG_SESSION, "connect to %s:%hu failed: %s", addr, port, strerror(errno));
		close(fd);
		return -errno;
	}

	mbt_log_debug(MBT_LOG_SESSION, "connecting to %s:%hu (rv=%d errno=%d)", addr, port, rv, errno);

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

	STAILQ_INSERT_TAIL(&s->pending, pc, next);
	return 0;
}
