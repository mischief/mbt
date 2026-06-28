// SPDX-License-Identifier: ISC
#ifndef MBT_CONN_H
#define MBT_CONN_H

#include "export.h"

#include <stddef.h>
#include <stdint.h>
#include <sys/types.h>
#include <sys/uio.h>

struct mbt_conn;
struct mbt_ev;

typedef void (*mbt_conn_read_cb)(struct mbt_conn *c, void *arg);
typedef void (*mbt_conn_event_cb)(struct mbt_conn *c, int err, void *arg);
/* fired once when send buffer drains after a -ENOBUFS write */
typedef void (*mbt_conn_write_cb)(struct mbt_conn *c, void *arg);

/* err=0 means EOF, err<0 means -errno */

struct mbt_conn_ops {
	void (*set_watermark)(struct mbt_conn *c, size_t low);
	size_t (*available)(struct mbt_conn *c);
	ssize_t (*read)(struct mbt_conn *c, void *buf, size_t len);
	ssize_t (*write)(struct mbt_conn *c, const void *buf, size_t len);
	/* Optional: gather-write iovcnt buffers as one logical message.
	 * Must be all-or-nothing for transports that can report -ENOBUFS
	 * (e.g. uTP): either the whole vector is accepted or none of it is.
	 * NULL pointer = fall back to per-iovec write() in the wrapper, which
	 * is safe only for transports whose write() never partially fails
	 * (e.g. TCP, which buffers any remainder). */
	ssize_t (*writev)(struct mbt_conn *c, const struct iovec *iov, int iovcnt);
	void (*set_callbacks)(struct mbt_conn *c, mbt_conn_read_cb readcb,
	                      mbt_conn_event_cb eventcb, void *arg);
	/* Optional: register one-shot writecb for when send buffer drains.
	 * NULL pointer = transport always succeeds (e.g. TCP with kernel buf). */
	void (*set_writecb)(struct mbt_conn *c, mbt_conn_write_cb cb, void *arg);
	int (*peer_addr)(struct mbt_conn *c, char *buf, size_t buflen, uint16_t *port);
	void (*free)(struct mbt_conn *c);
};

struct mbt_conn {
	const struct mbt_conn_ops *ops;
};

/*
 * Create a connection from an already-connected fd.
 * The conn takes ownership of fd.
 */
MBT_API int mbt_conn_new(struct mbt_ev *ev, struct mbt_conn **out, int fd,
                 mbt_conn_read_cb readcb, mbt_conn_event_cb eventcb, void *arg);

/* Change callbacks (e.g. after handing conn to a peer) */
MBT_API void mbt_conn_set_callbacks(struct mbt_conn *c, mbt_conn_read_cb readcb,
                                     mbt_conn_event_cb eventcb, void *arg);

/* Get remote peer address. Returns 0 on success. */
MBT_API int mbt_conn_peer_addr(struct mbt_conn *c, char *buf, size_t buflen,
                                uint16_t *port);

/* Inline convenience wrappers */
static inline void
mbt_conn_set_watermark(struct mbt_conn *c, size_t low)
{
	c->ops->set_watermark(c, low);
}

static inline size_t
mbt_conn_available(struct mbt_conn *c)
{
	return c->ops->available(c);
}

static inline ssize_t
mbt_conn_read(struct mbt_conn *c, void *buf, size_t len)
{
	return c->ops->read(c, buf, len);
}

static inline ssize_t
mbt_conn_write(struct mbt_conn *c, const void *buf, size_t len)
{
	return c->ops->write(c, buf, len);
}

/*
 * Gather-write a vector of buffers as one logical message. Returns the
 * total bytes accepted on success, or a negative -errno. If the transport
 * provides a native writev it is all-or-nothing (see mbt_conn_ops.writev);
 * otherwise we fall back to writing each iovec in turn, which is only
 * correct for transports whose write() cannot partially fail.
 */
static inline ssize_t
mbt_conn_writev(struct mbt_conn *c, const struct iovec *iov, int iovcnt)
{
	if (c->ops->writev)
		return c->ops->writev(c, iov, iovcnt);
	ssize_t total = 0;
	for (int i = 0; i < iovcnt; i++) {
		if (iov[i].iov_len == 0)
			continue;
		ssize_t n = c->ops->write(c, iov[i].iov_base, iov[i].iov_len);
		if (n < 0)
			return n;
		total += n;
	}
	return total;
}

static inline void
mbt_conn_set_writecb(struct mbt_conn *c, mbt_conn_write_cb cb, void *arg)
{
	if (c->ops->set_writecb)
		c->ops->set_writecb(c, cb, arg);
}

static inline void
mbt_conn_free(struct mbt_conn *c)
{
	if (c)
		c->ops->free(c);
}

#endif
