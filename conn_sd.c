#include <stdlib.h>
#include <unistd.h>
#include <errno.h>
#include <fcntl.h>
#include <sys/socket.h>
#include <netinet/in.h>
#include <arpa/inet.h>

#include <systemd/sd-event.h>

#include "event.h"
#include "conn.h"
#include "sdbuf.h"

/* Access the underlying sd_event* from our ev_sd backend */
struct ev_sd {
	struct mbt_ev base;
	sd_event *loop;
	int owned;
};

struct conn_sd {
	struct mbt_conn base;
	struct mbt_ev *ev;
	struct sdbuf *buf;
	sd_event_source *src;
	int fd;
	mbt_conn_read_cb readcb;
	mbt_conn_event_cb eventcb;
	void *arg;
};

static int
conn_sd_io(sd_event_source *s, int fd, uint32_t revents, void *v)
{
	struct conn_sd *c = v;
	(void)s;
	return sdbuf_io(c->buf, fd, revents);
}

static int
conn_sd_sdbuf_read(struct sdbuf *b, void *v)
{
	struct conn_sd *c = v;
	(void)b;
	if (c->readcb)
		c->readcb(&c->base, c->arg);
	return 0;
}

static int
conn_sd_sdbuf_write(struct sdbuf *b, void *v)
{
	(void)b;
	(void)v;
	return 0;
}

static void
conn_sd_sdbuf_event(struct sdbuf *b, int event, int err, void *v)
{
	struct conn_sd *c = v;
	(void)b;
	(void)event;
	if (c->eventcb)
		c->eventcb(&c->base, err, c->arg);
}

static void
conn_sd_set_watermark(struct mbt_conn *conn, size_t low)
{
	struct conn_sd *c = (struct conn_sd *)conn;

	/* Lazily register IO source on first watermark set */
	if (!c->src) {
		struct ev_sd *s = (struct ev_sd *)c->ev;
		sd_event_add_io(s->loop, &c->src, c->fd, EPOLLIN, conn_sd_io, c);
	}

	sdbuf_set_watermark(c->buf, low, 0);
}

static size_t
conn_sd_available(struct mbt_conn *conn)
{
	struct conn_sd *c = (struct conn_sd *)conn;
	return sdbuf_read_available(c->buf);
}

static ssize_t
conn_sd_read(struct mbt_conn *conn, void *buf, size_t len)
{
	struct conn_sd *c = (struct conn_sd *)conn;
	return sdbuf_read(c->buf, buf, len);
}

static ssize_t
conn_sd_write(struct mbt_conn *conn, const void *buf, size_t len)
{
	struct conn_sd *c = (struct conn_sd *)conn;
	return write(c->fd, buf, len);
}

static void
conn_sd_free(struct mbt_conn *conn)
{
	struct conn_sd *c = (struct conn_sd *)conn;
	sd_event_source_unref(c->src);
	sdbuf_free(c->buf);
	close(c->fd);
	free(c);
}

static const struct mbt_conn_ops conn_sd_ops = {
	.set_watermark = conn_sd_set_watermark,
	.available     = conn_sd_available,
	.read          = conn_sd_read,
	.write         = conn_sd_write,
	.free          = conn_sd_free,
};

void
mbt_conn_set_callbacks(struct mbt_conn *conn, mbt_conn_read_cb readcb,
                       mbt_conn_event_cb eventcb, void *arg)
{
	struct conn_sd *c = (struct conn_sd *)conn;
	c->readcb = readcb;
	c->eventcb = eventcb;
	c->arg = arg;
}

int
mbt_conn_peer_addr(struct mbt_conn *conn, char *buf, size_t buflen, uint16_t *port)
{
	struct conn_sd *c = (struct conn_sd *)conn;
	struct sockaddr_storage ss;
	socklen_t len = sizeof(ss);

	if (getpeername(c->fd, (struct sockaddr *)&ss, &len) < 0)
		return -errno;

	if (ss.ss_family == AF_INET) {
		struct sockaddr_in *sa = (struct sockaddr_in *)&ss;
		inet_ntop(AF_INET, &sa->sin_addr, buf, buflen);
		if (port) *port = ntohs(sa->sin_port);
	} else if (ss.ss_family == AF_INET6) {
		struct sockaddr_in6 *sa6 = (struct sockaddr_in6 *)&ss;
		inet_ntop(AF_INET6, &sa6->sin6_addr, buf, buflen);
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
	struct conn_sd *c;
	struct ev_sd *s = (struct ev_sd *)ev;
	int rv;

	c = calloc(1, sizeof(*c));
	if (!c)
		return -ENOMEM;

	c->base.ops = &conn_sd_ops;
	c->ev = ev;
	c->fd = fd;
	c->readcb = readcb;
	c->eventcb = eventcb;
	c->arg = arg;

	fcntl(fd, F_SETFL, fcntl(fd, F_GETFL) | O_NONBLOCK);

	rv = sdbuf_new(&c->buf, conn_sd_sdbuf_read, conn_sd_sdbuf_write, conn_sd_sdbuf_event, c);
	if (rv < 0) {
		free(c);
		return rv;
	}

	rv = sd_event_add_io(s->loop, &c->src, fd, EPOLLIN, conn_sd_io, c);
	if (rv < 0) {
		sdbuf_free(c->buf);
		free(c);
		return rv;
	}

	*out = &c->base;
	return 0;
}
