#include <sys/queue.h>
#include <stdlib.h>
#include <string.h>
#include <errno.h>
#include <err.h>
#include <fcntl.h>

#include "event.h"
#include "conn.h"
#include "infohash.h"
#include "session.h"
#include "lsd.h"

struct mbt_session {
	struct mbt_ev *ev;

	struct mbt_storage_ops *storage_ops;
	void *storage_ctx;
	mbt_picker_fn picker;
	void *picker_ctx;

	uint16_t listen_port;

	struct mbt_session_callbacks cb;

	/* LSD */
	struct lsd *lsd;
	struct mbt_ev_io *lsd_io;

	/* TODO: torrent list, listen socket, DHT, etc. */
};

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

	*out = s;
	return 0;
}

void
mbt_session_free(struct mbt_session *s)
{
	if (!s)
		return;

	if (s->lsd_io)
		mbt_ev_io_free(s->lsd_io);
	/* lsd_freep handles lsd */

	free(s);
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

/* --- LSD integration --- */

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

	/* For each announced infohash, connect a peer */
	STAILQ_FOREACH(ih, &info->infohashes, next) {
		mbt_session_add_peer(s, ih, info->remote, info->port);
	}
}

int
mbt_session_enable_lsd(struct mbt_session *s)
{
	int rv;

	if (s->lsd)
		return 0; /* already enabled */

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

/* --- Torrent management (stubs) --- */

int
mbt_session_add_infohash(struct mbt_session *s, struct infohash *ih)
{
	(void)s;
	(void)ih;
	/* TODO: create torrent, add to list */
	return 0;
}

int
mbt_session_add_torrent_file(struct mbt_session *s, const char *path)
{
	(void)s;
	(void)path;
	/* TODO: parse .torrent, extract infohash + info dict, create torrent */
	return 0;
}

int
mbt_session_add_magnet(struct mbt_session *s, const char *uri)
{
	(void)s;
	(void)uri;
	/* TODO: parse magnet URI, extract infohash, create torrent (metadata-less) */
	return 0;
}

int
mbt_session_remove_torrent(struct mbt_session *s, struct infohash *ih)
{
	(void)s;
	(void)ih;
	/* TODO */
	return 0;
}

struct mbt_torrent*
mbt_session_find_torrent(struct mbt_session *s, struct infohash *ih)
{
	(void)s;
	(void)ih;
	/* TODO */
	return NULL;
}

int
mbt_session_add_peer(struct mbt_session *s, struct infohash *ih,
                     const char *addr, uint16_t port)
{
	(void)s;
	(void)ih;
	(void)addr;
	(void)port;
	/* TODO: find torrent for ih, async connect, create peer, hand to torrent */
	return 0;
}
