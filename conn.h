#ifndef MBT_CONN_H
#define MBT_CONN_H

#include <stddef.h>
#include <sys/types.h>

struct mbt_conn;
struct mbt_ev;

typedef void (*mbt_conn_read_cb)(struct mbt_conn *c, void *arg);
typedef void (*mbt_conn_event_cb)(struct mbt_conn *c, int err, void *arg);

/* err=0 means EOF, err<0 means -errno */

struct mbt_conn_ops {
	void (*set_watermark)(struct mbt_conn *c, size_t low);
	size_t (*available)(struct mbt_conn *c);
	ssize_t (*read)(struct mbt_conn *c, void *buf, size_t len);
	ssize_t (*write)(struct mbt_conn *c, const void *buf, size_t len);
	void (*free)(struct mbt_conn *c);
};

struct mbt_conn {
	const struct mbt_conn_ops *ops;
};

/*
 * Create a connection from an already-connected fd.
 * The conn takes ownership of fd.
 */
int mbt_conn_new(struct mbt_ev *ev, struct mbt_conn **out, int fd,
                 mbt_conn_read_cb readcb, mbt_conn_event_cb eventcb, void *arg);

/* Change callbacks (e.g. after handing conn to a peer) */
void mbt_conn_set_callbacks(struct mbt_conn *c, mbt_conn_read_cb readcb,
                            mbt_conn_event_cb eventcb, void *arg);

/* Get remote peer address (via getpeername). Returns 0 on success. */
int mbt_conn_peer_addr(struct mbt_conn *c, char *buf, size_t buflen, uint16_t *port);

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

static inline void
mbt_conn_free(struct mbt_conn *c)
{
	if (c)
		c->ops->free(c);
}

#endif
