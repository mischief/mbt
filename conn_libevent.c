// SPDX-License-Identifier: ISC
#include <sys/socket.h>
#include "net.h"
#include <netinet/in.h>
#include <arpa/inet.h>
#include <stdlib.h>
#include <unistd.h>
#include <errno.h>
#include <string.h>

#ifdef HAVE_LIBEVENT1
#include <event.h>
#define evbuffer_get_length(eb)      EVBUFFER_LENGTH(eb)
#define bufferevent_get_input(bev)   EVBUFFER_INPUT(bev)
#define BEV_EVENT_EOF                EVBUFFER_EOF
#define BEV_EVENT_ERROR              EVBUFFER_ERROR
#else
#include <event2/event.h>
#include <event2/bufferevent.h>
#include <event2/buffer.h>
#endif

#include "mbt/event.h"
#include "mbt/conn.h"

struct conn_lev {
	struct mbt_conn base;
	struct bufferevent *bev;
	size_t watermark;
	mbt_conn_read_cb readcb;
	mbt_conn_event_cb eventcb;
	void *arg;
	int fd;
};

/* Access the underlying event_base from our ev_libevent backend */
#include "ev_lev_internal.h"

static void
conn_lev_on_read(struct bufferevent *bev, void *v)
{
	struct conn_lev *c = v;
	struct evbuffer *input = bufferevent_get_input(bev);

	if (evbuffer_get_length(input) >= c->watermark) {
		if (c->readcb)
			c->readcb(&c->base, c->arg);
	}
}

static void
conn_lev_on_event(struct bufferevent *bev, short what, void *v)
{
	struct conn_lev *c = v;
	int err = 0;

	(void)bev;

	if (what & BEV_EVENT_EOF)
		err = 0;
	else if (what & BEV_EVENT_ERROR)
		err = -ECONNRESET;

	if (c->eventcb)
		c->eventcb(&c->base, err, c->arg);
}

static void
conn_lev_set_watermark(struct mbt_conn *conn, size_t low)
{
	struct conn_lev *c = (struct conn_lev *)conn;
	c->watermark = low;
	bufferevent_setwatermark(c->bev, EV_READ, low, 0);

	/* Re-check: if data already buffered >= new watermark, fire read */
	struct evbuffer *input = bufferevent_get_input(c->bev);
	if (c->readcb && evbuffer_get_length(input) >= low)
		c->readcb(&c->base, c->arg);
}

static size_t
conn_lev_available(struct mbt_conn *conn)
{
	struct conn_lev *c = (struct conn_lev *)conn;
	struct evbuffer *input = bufferevent_get_input(c->bev);
	return evbuffer_get_length(input);
}

static ssize_t
conn_lev_read(struct mbt_conn *conn, void *buf, size_t len)
{
	struct conn_lev *c = (struct conn_lev *)conn;
	struct evbuffer *input = bufferevent_get_input(c->bev);
	size_t avail = evbuffer_get_length(input);
	size_t toread = len < avail ? len : avail;

	if (toread == 0)
		return 0;

	return evbuffer_remove(input, buf, toread);
}

static ssize_t
conn_lev_write(struct mbt_conn *conn, const void *buf, size_t len)
{
	struct conn_lev *c = (struct conn_lev *)conn;

	if (bufferevent_write(c->bev, buf, len) < 0)
		return -EIO;

	return (ssize_t)len;
}

static void
conn_lev_free(struct mbt_conn *conn)
{
	struct conn_lev *c = (struct conn_lev *)conn;
	bufferevent_free(c->bev);
#ifdef HAVE_LIBEVENT1
	close(c->fd);
#endif
	free(c);
}

static void
conn_lev_set_callbacks(struct mbt_conn *conn, mbt_conn_read_cb readcb,
                       mbt_conn_event_cb eventcb, void *arg)
{
	struct conn_lev *c = (struct conn_lev *)conn;
	c->readcb = readcb;
	c->eventcb = eventcb;
	c->arg = arg;
}

static int
conn_lev_peer_addr(struct mbt_conn *conn, char *buf, size_t buflen, uint16_t *port)
{
	struct conn_lev *c = (struct conn_lev *)conn;
	int fd = c->fd;
	struct sockaddr_storage ss;
	socklen_t len = sizeof(ss);

	if (getpeername(fd, (struct sockaddr *)&ss, &len) < 0)
		return -errno;

	if (ss.ss_family == AF_INET || ss.ss_family == AF_INET6) {
		uint16_t p;
		mbt_sockaddr_split(&ss, buf, buflen, &p);
		if (port) *port = p;
	} else {
		return -EAFNOSUPPORT;
	}

	return 0;
}

static const struct mbt_conn_ops conn_lev_ops = {
	.set_watermark = conn_lev_set_watermark,
	.available     = conn_lev_available,
	.read          = conn_lev_read,
	.write         = conn_lev_write,
	.set_callbacks = conn_lev_set_callbacks,
	.peer_addr     = conn_lev_peer_addr,
	.free          = conn_lev_free,
};

int
mbt_conn_new(struct mbt_ev *ev, struct mbt_conn **out, int fd,
             mbt_conn_read_cb readcb, mbt_conn_event_cb eventcb, void *arg)
{
	struct conn_lev *c;
	struct ev_lev *l = (struct ev_lev *)ev;

	c = calloc(1, sizeof(*c));
	if (!c)
		return -ENOMEM;

	c->base.ops = &conn_lev_ops;
	c->readcb = readcb;
	c->eventcb = eventcb;
	c->arg = arg;
	c->watermark = 1;

	c->fd = fd;
#ifdef HAVE_LIBEVENT1
	c->bev = bufferevent_new(fd, conn_lev_on_read, NULL, conn_lev_on_event, c);
	if (!c->bev) {
		free(c);
		return -ENOMEM;
	}
	event_base_set(l->base_ev, &c->bev->ev_read);
	event_base_set(l->base_ev, &c->bev->ev_write);
#else
	c->bev = bufferevent_socket_new(l->base_ev, fd, BEV_OPT_CLOSE_ON_FREE);
	if (!c->bev) {
		free(c);
		return -ENOMEM;
	}
	bufferevent_setcb(c->bev, conn_lev_on_read, NULL, conn_lev_on_event, c);
#endif
	bufferevent_setwatermark(c->bev, EV_READ, 1, 0);
	bufferevent_enable(c->bev, EV_READ | EV_WRITE);

	*out = &c->base;
	return 0;
}
