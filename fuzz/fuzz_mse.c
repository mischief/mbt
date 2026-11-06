// SPDX-License-Identifier: ISC
/*
 * Fuzz target: the MSE handshake parser and stream filter. The first input
 * byte picks the role and the methods. The rest is the bytes from the other
 * side, cut into chunks. AFL builds run without DH and RC4: the shared secret
 * is zero, and the parser sees the input in the clear.
 */
#include <errno.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>

#include <mbt/conn.h>
#include <mbt/event.h>
#include <mbt/mse.h>

#ifndef __AFL_FUZZ_TESTCASE_LEN
  ssize_t fuzz_len;
  #define __AFL_FUZZ_TESTCASE_LEN fuzz_len
  unsigned char fuzz_buf[1024000];
  #define __AFL_FUZZ_TESTCASE_BUF fuzz_buf
  #define __AFL_FUZZ_INIT() void sync(void);
  #define __AFL_LOOP(x) ((fuzz_len = read(0, fuzz_buf, sizeof(fuzz_buf))) > 0 ? 1 : 0)
  #define __AFL_INIT() sync()
#endif

__AFL_FUZZ_INIT()

/* An inner conn that serves one chunk of input at a time. */
struct mem_conn {
	struct mbt_conn base;
	const unsigned char *p;
	size_t len;
	int refuse;
	mbt_conn_read_cb readcb;
	mbt_conn_event_cb eventcb;
	void *arg;
};

static void mem_watermark(struct mbt_conn *c, size_t low) { (void)c; (void)low; }
static size_t mem_available(struct mbt_conn *c) { return ((struct mem_conn *)c)->len; }

static ssize_t
mem_read(struct mbt_conn *c, void *buf, size_t len)
{
	struct mem_conn *m = (struct mem_conn *)c;

	if (len > m->len)
		len = m->len;
	memcpy(buf, m->p, len);
	m->p += len;
	m->len -= len;
	return (ssize_t)len;
}

static ssize_t
mem_write(struct mbt_conn *c, const void *buf, size_t len)
{
	(void)buf;
	return ((struct mem_conn *)c)->refuse ? -ENOBUFS : (ssize_t)len;
}

static void
mem_callbacks(struct mbt_conn *c, mbt_conn_read_cb r, mbt_conn_event_cb e, void *arg)
{
	struct mem_conn *m = (struct mem_conn *)c;

	m->readcb = r;
	m->eventcb = e;
	m->arg = arg;
}

static int
mem_addr(struct mbt_conn *c, char *buf, size_t n, uint16_t *port)
{
	(void)c; (void)buf; (void)n; (void)port;
	return -1;
}

static void mem_free(struct mbt_conn *c) { free(c); }

static const struct mbt_conn_ops mem_ops = {
	.name = "mem", .set_watermark = mem_watermark, .available = mem_available,
	.read = mem_read, .write = mem_write, .set_callbacks = mem_callbacks,
	.peer_addr = mem_addr, .free = mem_free,
};

static int
lookup(const uint8_t hash[20], uint8_t skey[20], void *arg)
{
	(void)arg;
	if (hash[0] == 0xff)
		return -2;
	memcpy(skey, hash, 20);
	return 0;
}

static int closed;
static int freed;
static struct mem_conn *cur;

/* Read in a pattern that follows the first byte, as the peer layer does. */
static void
on_read(struct mbt_conn *c, void *arg)
{
	unsigned char buf[97];
	ssize_t n;

	(void)arg;
	n = mbt_conn_read(c, buf, sizeof(buf));
	if (n <= 0)
		return;
	mbt_conn_set_watermark(c, buf[0] % 16);
	cur->refuse = buf[0] == 0xed;
	if (buf[0] == 0xee || buf[0] == 0xed)
		mbt_conn_write(c, buf, (size_t)n);
	if (buf[0] == 0xdd) {
		/* The session can free the conn from this callback. */
		mbt_conn_free(c);
		freed = closed = 1;
	}
}

static void
on_event(struct mbt_conn *c, int err, void *arg)
{
	(void)c; (void)err; (void)arg;
	closed = 1;
}

static void
run_input(struct mbt_ev *ev, const unsigned char *data, size_t len)
{
	static const uint8_t hash[20] = {1, 2, 3};
	struct mem_conn *m;
	struct mbt_conn *c;
	unsigned mode;
	int rv;

	if (len < 2 || !(m = calloc(1, sizeof(*m))))
		return;
	mode = data[0];
	data++;
	len--;
	m->base.ops = &mem_ops;
	cur = m;
	closed = freed = 0;
	if (mode & 4) {
		/* The session gives the server the bytes it read to detect MSE. */
		size_t seed = (mode & 8) ? (len < 68 ? len : 68) : 0;

		rv = mbt_mse_server(ev, &m->base, data, seed, lookup, NULL, (mode & 3) ? mode & 3 : 3, &c);
		data += seed;
		len -= seed;
	} else
		rv = mbt_mse_client(ev, &m->base, hash, (mode & 3) ? mode & 3 : 3, &c);
	if (rv < 0) {
		free(m);
		return;
	}
	mbt_conn_set_callbacks(c, on_read, on_event, NULL);
	mbt_conn_write(c, data, len < 68 ? len : 68);

	/* Chunk size comes from the mode, so a run covers split reads. */
	size_t chunk = (mode >> 3) ? (size_t)(mode >> 3) * 32 : len;

	while (len && !closed) {
		size_t n = len < chunk ? len : chunk;

		m->p = data;
		m->len = n;
		data += n;
		len -= n;
		m->readcb(&m->base, m->arg);
		m->len = 0;
	}
	if (freed) {
		mbt_ev_step(ev, 1);
		return;
	}
	if (m->eventcb && !closed)
		m->eventcb(&m->base, 0, m->arg);
	mbt_conn_free(c);
}

int
main(void)
{
	struct mbt_ev *ev;

#ifdef __AFL_HAVE_MANUAL_CONTROL
	__AFL_INIT();
#endif
	if (mbt_ev_new_default(&ev) < 0)
		return 1;
	unsigned char *buf = __AFL_FUZZ_TESTCASE_BUF;

	while (__AFL_LOOP(100000))
		run_input(ev, buf, (size_t)__AFL_FUZZ_TESTCASE_LEN);
	mbt_ev_destroy(ev);
	return 0;
}
