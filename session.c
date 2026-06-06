#include <sys/queue.h>
#include <sys/socket.h>
#include <netinet/in.h>
#include <arpa/inet.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>
#include <errno.h>
#include <err.h>
#include <fcntl.h>

#include "event.h"
#include "conn.h"
#include "infohash.h"
#include "torrent.h"
#include "session.h"
#include "lsd.h"

/* --- Torrent list entry --- */

struct session_torrent {
	struct mbt_torrent *torrent;
	struct infohash *ih;
	STAILQ_ENTRY(session_torrent) next;
};

STAILQ_HEAD(session_torrent_list, session_torrent);

/* --- Pending connect --- */

struct pending_connect {
	struct mbt_session *session;
	struct mbt_torrent *torrent;
	struct mbt_ev_io *io;
	int fd;
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
		warnx("connect failed: %s", strerror(error ? error : errno));
		goto fail;
	}

	warnx("connected to peer, creating conn");

	/* Free connect watcher before creating conn (conn registers its own) */
	mbt_ev_io_free(pc->io);
	pc->io = NULL;

	/* Connection succeeded — create mbt_conn */
	rv = mbt_conn_new(s->ev, &conn, pc->fd, NULL, NULL, NULL);
	if (rv < 0) {
		warnx("mbt_conn_new failed: %s", strerror(-rv));
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
session_find(struct mbt_session *s, struct infohash *ih)
{
	struct session_torrent *st;
	STAILQ_FOREACH(st, &s->torrents, next) {
		if (infohash_cmp(st->ih, ih) == 0)
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
		infohash_unref(st->ih);
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
		.picker = mbt_picker_random,
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
	struct infohash *ih;

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

/* --- Torrent management --- */

int
mbt_session_add_infohash(struct mbt_session *s, struct infohash *ih)
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
	};

	int rv = mbt_torrent_new(&st->torrent, &cfg);
	if (rv < 0) {
		free(st);
		return rv;
	}

	st->ih = infohash_ref(ih);
	STAILQ_INSERT_TAIL(&s->torrents, st, next);

	return 0;
}

int
mbt_session_add_torrent_file(struct mbt_session *s, const char *path)
{
	(void)s;
	(void)path;
	/* TODO: parse .torrent, extract infohash + info dict, call add_infohash + set_metadata */
	return -ENOTSUP;
}

int
mbt_session_add_magnet(struct mbt_session *s, const char *uri)
{
	(void)s;
	(void)uri;
	/* TODO: parse magnet URI, extract infohash, call add_infohash */
	return -ENOTSUP;
}

int
mbt_session_remove_torrent(struct mbt_session *s, struct infohash *ih)
{
	struct session_torrent *st = session_find(s, ih);
	if (!st)
		return -ENOENT;

	STAILQ_REMOVE(&s->torrents, st, session_torrent, next);
	mbt_torrent_free(st->torrent);
	infohash_unref(st->ih);
	free(st);
	return 0;
}

struct mbt_torrent *
mbt_session_find_torrent(struct mbt_session *s, struct infohash *ih)
{
	struct session_torrent *st = session_find(s, ih);
	return st ? st->torrent : NULL;
}

/* --- Peer connection --- */

int
mbt_session_add_peer(struct mbt_session *s, struct infohash *ih,
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
		close(fd);
		return -errno;
	}

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

	rv = mbt_ev_io_new(s->ev, &pc->io, fd, MBT_EV_WRITE, pending_connect_cb, pc);
	if (rv < 0) {
		close(fd);
		free(pc);
		return rv;
	}

	STAILQ_INSERT_TAIL(&s->pending, pc, next);
	return 0;
}
