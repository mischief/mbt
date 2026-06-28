// SPDX-License-Identifier: ISC
#include <errno.h>
#include <fcntl.h>
#include <stdlib.h>
#include <stdint.h>
#include <sys/socket.h>
#include <unistd.h>

#include "net.h"
#include "mbt/conn.h"
#include "mbt/event.h"
#include "sdbuf.h"

struct conn_poll {
	struct mbt_conn base;
	struct mbt_ev *ev;
	struct mbt_ev_io *io;
	struct sdbuf *buf;
	int fd;
	mbt_conn_read_cb readcb;
	mbt_conn_event_cb eventcb;
	void *arg;
};

static void
conn_poll_io(struct mbt_ev_io *io, int fd, uint32_t events, void *v)
{
	struct conn_poll *c = v;
	(void)io;

	if (events & MBT_EV_WRITE) {
		int rv = sdbuf_flush(c->buf, fd);
		if (rv <= 0)
			mbt_ev_io_mod(c->io, MBT_EV_READ);
	}

	if (events & MBT_EV_READ)
		sdbuf_io(c->buf, fd, events);
}

static int
conn_poll_sdbuf_read(struct sdbuf *b, void *v)
{
	struct conn_poll *c = v;
	(void)b;
	if (c->readcb)
		c->readcb(&c->base, c->arg);
	return 0;
}

static int
conn_poll_sdbuf_write(struct sdbuf *b, void *v)
{
	(void)b;
	(void)v;
	return 0;
}

static void
conn_poll_sdbuf_event(struct sdbuf *b, int event, int err, void *v)
{
	struct conn_poll *c = v;
	(void)b;
	(void)event;
	if (c->eventcb)
		c->eventcb(&c->base, err, c->arg);
}

static void
conn_poll_set_watermark(struct mbt_conn *conn, size_t low)
{
	struct conn_poll *c = (struct conn_poll *)conn;
	sdbuf_set_watermark(c->buf, low, 0);
}

static size_t
conn_poll_available(struct mbt_conn *conn)
{
	struct conn_poll *c = (struct conn_poll *)conn;
	return sdbuf_read_available(c->buf);
}

static ssize_t
conn_poll_read(struct mbt_conn *conn, void *buf, size_t len)
{
	struct conn_poll *c = (struct conn_poll *)conn;
	return sdbuf_read(c->buf, buf, len);
}

static ssize_t
conn_poll_write(struct mbt_conn *conn, const void *buf, size_t len)
{
	struct conn_poll *c = (struct conn_poll *)conn;

	if (sdbuf_write_pending(c->buf) == 0) {
		const unsigned char *p = buf;
		size_t rem = len;
		while (rem > 0) {
			ssize_t n = write(c->fd, p, rem);
			if (n < 0) {
				if (errno == EINTR) continue;
				if (errno == EAGAIN) break;
				return -errno;
			}
			p += n;
			rem -= n;
		}
		if (rem == 0) return (ssize_t)len;
		sdbuf_write(c->buf, p, rem);
	} else {
		sdbuf_write(c->buf, buf, len);
	}

	mbt_ev_io_mod(c->io, MBT_EV_READ | MBT_EV_WRITE);
	return (ssize_t)len;
}

static void
conn_poll_free(struct mbt_conn *conn)
{
	struct conn_poll *c = (struct conn_poll *)conn;
	if (c->io)
		mbt_ev_io_free(c->io);
	sdbuf_free(c->buf);
	close(c->fd);
	free(c);
}

static void
conn_poll_set_callbacks(struct mbt_conn *conn, mbt_conn_read_cb readcb,
                        mbt_conn_event_cb eventcb, void *arg)
{
	struct conn_poll *c = (struct conn_poll *)conn;
	c->readcb = readcb;
	c->eventcb = eventcb;
	c->arg = arg;
}

static const struct mbt_conn_ops conn_poll_ops = {
	.set_watermark = conn_poll_set_watermark,
	.available     = conn_poll_available,
	.read          = conn_poll_read,
	.write         = conn_poll_write,
	.set_callbacks = conn_poll_set_callbacks,
	.free          = conn_poll_free,
};

int
mbt_conn_peer_addr(struct mbt_conn *conn, char *buf, size_t buflen, uint16_t *port)
{
	struct conn_poll *c = (struct conn_poll *)conn;
	struct sockaddr_storage ss;
	socklen_t len = sizeof(ss);

	if (getpeername(c->fd, (struct sockaddr *)&ss, &len) < 0)
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

int
mbt_conn_new(struct mbt_ev *ev, struct mbt_conn **out, int fd,
             mbt_conn_read_cb readcb, mbt_conn_event_cb eventcb, void *arg)
{
	struct conn_poll *c;
	int rv;

	c = calloc(1, sizeof(*c));
	if (!c)
		return -ENOMEM;

	c->base.ops = &conn_poll_ops;
	c->ev = ev;
	c->fd = fd;
	c->readcb = readcb;
	c->eventcb = eventcb;
	c->arg = arg;

	fcntl(fd, F_SETFL, fcntl(fd, F_GETFL) | O_NONBLOCK);

	rv = sdbuf_new(&c->buf, conn_poll_sdbuf_read, conn_poll_sdbuf_write,
	               conn_poll_sdbuf_event, c);
	if (rv < 0) {
		free(c);
		return rv;
	}

	rv = mbt_ev_io_new(ev, &c->io, fd, MBT_EV_READ, conn_poll_io, c);
	if (rv < 0) {
		sdbuf_free(c->buf);
		free(c);
		return rv;
	}

	*out = &c->base;
	return 0;
}
