#include <sys/param.h>
#include <sys/epoll.h>
#include <unistd.h>
#include <stdlib.h>
#include <string.h>
#include <stddef.h>
#include <errno.h>

#include <sdbuf.h>

struct buf
{
	unsigned char *buf, *p;
	size_t len;

	size_t lowat;
	size_t hiwat;
};

struct sdbuf
{
	struct buf rbuf;
	struct buf wbuf;

	sdbuf_cb readcb;
	sdbuf_cb writecb;
	sdbuf_eventcb errorcb;

	void *cbarg;
};

int
sdbuf_new(struct sdbuf **out, sdbuf_cb read, sdbuf_cb write, sdbuf_eventcb error, void *arg)
{
	struct sdbuf *b;

	b = calloc(1, sizeof(*b));
	if(!b)
		return -ENOMEM;

	b->rbuf.len = 4096;
	b->rbuf.buf = malloc(b->rbuf.len);
	if(!b->rbuf.buf)
		goto serr;

	b->wbuf.len = 4096;
	b->wbuf.buf = malloc(b->wbuf.len);
	if(!b->wbuf.buf)
		goto rerr;

	b->wbuf.p = b->wbuf.buf;

	b->readcb = read;
	b->writecb = write;
	b->errorcb = error;
	b->cbarg = arg;

	*out = b;

	return 0;

rerr:
	free(b->rbuf.buf);
serr:
	free(b);
	return -ENOMEM;
}

void
sdbuf_free(struct sdbuf *b)
{
	free(b->rbuf.buf);
	free(b->wbuf.buf);
	free(b);
}

void
sdbuf_set_watermark(struct sdbuf *b, size_t readlow, size_t writelow)
{
	b->rbuf.lowat = readlow;
	b->wbuf.lowat = writelow;
}

static int
bufwant(struct buf *buf, size_t sz)
{
	ptrdiff_t diff;
	unsigned char *np;

	if(buf->p + sz < buf->buf + buf->len)
		return 0;

	diff = buf->p - buf->buf;

	np = realloc(buf->buf, buf->len * 2);
	if(!np)
		return -ENOMEM;

	buf->buf = np;
	buf->p = buf->buf + diff;
	buf->len = buf->len * 2;
	return 0;
}

static int
sdbuf_handle_read(struct sdbuf *b, int fd)
{
	ssize_t rv;
	struct buf *buf = &b->rbuf;

	if(!buf->p)
		buf->p = buf->buf;

	while(1){
		if(bufwant(buf, 4096) < 0){
			b->errorcb(b, SDBUF_EVENT_ERROR, -ENOMEM, b->cbarg);
			return -ENOMEM;
		}

		rv = read(fd, buf->p, 4096);
		if(rv == 0){
			b->errorcb(b, SDBUF_EVENT_ERROR, 0, b->cbarg);
			return -EIO;
		}

		if(rv < 0){
			if(errno == EINTR)
				continue;
			else if(errno == EAGAIN)
				break;
			else {
				b->errorcb(b, SDBUF_EVENT_ERROR, -errno, b->cbarg);
				return -errno;
			}
		}

		buf->p += rv;

		while(buf->p - buf->buf >= (ptrdiff_t)buf->lowat){
			if(b->readcb(b, b->cbarg) < 0)
				return -EIO;
		}
	}

	return 0;
}

int
sdbuf_io(struct sdbuf *b, int fd, uint32_t revents)
{
	if(revents & EPOLLIN){
		return sdbuf_handle_read(b, fd);
	}

	return 0;
}

size_t
sdbuf_read_available(struct sdbuf *b)
{
	if(!b->rbuf.p)
		return 0;

	return (size_t)(b->rbuf.p - b->rbuf.buf);
}

ssize_t
sdbuf_discard(struct sdbuf *b, size_t sz)
{
	size_t have, canread;
	struct buf *buf = &b->rbuf;

	if(!buf->p)
		return -EIO;

	have = (size_t)(buf->p - buf->buf);

	canread = MIN(sz, have);

	memmove(buf->buf, buf->buf+canread, have - canread);

	buf->p -= canread;

	return canread;
}

ssize_t
sdbuf_read(struct sdbuf *b, void *out, size_t sz)
{
	size_t have, canread;
	struct buf *buf = &b->rbuf;

	if(!buf->p)
		return -EIO;

	have = (size_t)(buf->p - buf->buf);

	canread = MIN(sz, have);

	if(canread == 0)
		return 0;

	memcpy(out, buf->buf, canread);

	memmove(buf->buf, buf->buf+canread, have - canread);

	buf->p -= canread;

	return canread;
}

ssize_t
sdbuf_write(struct sdbuf *b, void *buf, size_t sz)
{
	return 0;
}

