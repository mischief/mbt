#include <stdlib.h>
#include <unistd.h>
#include <errno.h>
#include <string.h>

#include <event2/event.h>
#include <event2/bufferevent.h>
#include <event2/buffer.h>

#include "event.h"
#include "conn.h"

struct conn_lev {
	struct mbt_conn base;
	struct bufferevent *bev;
	size_t watermark;
	mbt_conn_read_cb readcb;
	mbt_conn_event_cb eventcb;
	void *arg;
};

/* Access the underlying event_base from our ev_libevent backend */
struct ev_lev {
	struct mbt_ev base;
	struct event_base *base_ev;
	int owned;
};

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
	free(c);
}

static const struct mbt_conn_ops conn_lev_ops = {
	.set_watermark = conn_lev_set_watermark,
	.available     = conn_lev_available,
	.read          = conn_lev_read,
	.write         = conn_lev_write,
	.free          = conn_lev_free,
};

void
mbt_conn_set_callbacks(struct mbt_conn *conn, mbt_conn_read_cb readcb,
                       mbt_conn_event_cb eventcb, void *arg)
{
	struct conn_lev *c = (struct conn_lev *)conn;
	c->readcb = readcb;
	c->eventcb = eventcb;
	c->arg = arg;
}

int
mbt_conn_peer_addr(struct mbt_conn *conn, char *buf, size_t buflen, uint16_t *port)
{
	struct conn_lev *c = (struct conn_lev *)conn;
	evutil_socket_t fd = bufferevent_getfd(c->bev);
	struct sockaddr_storage ss;
	socklen_t len = sizeof(ss);

	if (getpeername(fd, (struct sockaddr *)&ss, &len) < 0)
		return -errno;

	if (ss.ss_family == AF_INET) {
		struct sockaddr_in *sa = (struct sockaddr_in *)&ss;
		evutil_inet_ntop(AF_INET, &sa->sin_addr, buf, buflen);
		if (port) *port = ntohs(sa->sin_port);
	} else if (ss.ss_family == AF_INET6) {
		struct sockaddr_in6 *sa6 = (struct sockaddr_in6 *)&ss;
		evutil_inet_ntop(AF_INET6, &sa6->sin6_addr, buf, buflen);
		if (port) *port = ntohs(sa6->sin6_port);
	} else {
		return -EAFNOSUPPORT;
	}

	return 0;
}

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

	c->bev = bufferevent_socket_new(l->base_ev, fd, BEV_OPT_CLOSE_ON_FREE);
	if (!c->bev) {
		free(c);
		return -ENOMEM;
	}

	bufferevent_setcb(c->bev, conn_lev_on_read, NULL, conn_lev_on_event, c);
	bufferevent_setwatermark(c->bev, EV_READ, 1, 0);
	bufferevent_enable(c->bev, EV_READ | EV_WRITE);

	*out = &c->base;
	return 0;
}
