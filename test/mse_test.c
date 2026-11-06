// SPDX-License-Identifier: ISC
/* MSE/PE: run both ends over a socketpair. */
#include <assert.h>
#include <errno.h>
#include <fcntl.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/socket.h>
#include <unistd.h>

#include "mbt/conn.h"
#include "mbt/event.h"
#include "mbt/mse.h"
#include "mbt/sha1_compat.h"

#define BIG 200000

static const uint8_t hash_a[20] = "aaaaaaaaaaaaaaaaaaa";
static const uint8_t hash_b[20] = "bbbbbbbbbbbbbbbbbbb";

static struct mbt_ev *ev;

struct side {
	struct mbt_conn *c;
	uint8_t got[BIG];
	size_t len;
	int err;
	int failed;
};

static void
wait_for(int *flag, size_t *have, size_t want)
{
	for (int i = 0; i < 2000; i++) {
		if ((flag && *flag) || (have && *have >= want))
			return;
		mbt_ev_step(ev, 10);
	}
	assert(!"timed out");
}

static void
on_read(struct mbt_conn *c, void *arg)
{
	struct side *s = arg;
	size_t avail = mbt_conn_available(c);
	ssize_t n;

	if (avail > BIG - s->len)
		avail = BIG - s->len;
	n = mbt_conn_read(c, s->got + s->len, avail);
	assert(n >= 0);
	s->len += (size_t)n;
}

static void
on_event(struct mbt_conn *c, int err, void *arg)
{
	struct side *s = arg;

	(void)c;
	s->err = err;
	s->failed = 1;
}

static int
lookup(const uint8_t hash[20], uint8_t skey[20], void *arg)
{
	uint8_t want[20];
	SHA1_CTX ctx;

	(void)arg;
	SHA1Init(&ctx);
	SHA1Update(&ctx, (const uint8_t *)"req2", 4);
	SHA1Update(&ctx, hash_a, 20);
	SHA1Final(want, &ctx);
	if (memcmp(hash, want, 20) != 0)
		return -ENOENT;
	memcpy(skey, hash_a, 20);
	return 0;
}

static void
fill(uint8_t *p, size_t n, unsigned seed)
{
	for (size_t i = 0; i < n; i++)
		p[i] = (uint8_t)(i * 31 + i / 251 + seed);
}

static void
make_inner(struct mbt_conn **a, struct mbt_conn **b)
{
	int sv[2];

	assert(socketpair(AF_UNIX, SOCK_STREAM, 0, sv) == 0);
	assert(fcntl(sv[0], F_SETFL, O_NONBLOCK) == 0);
	assert(fcntl(sv[1], F_SETFL, O_NONBLOCK) == 0);
	assert(mbt_conn_new(ev, a, sv[0], NULL, NULL, NULL) == 0);
	assert(mbt_conn_new(ev, b, sv[1], NULL, NULL, NULL) == 0);
}

static void
start(struct side *a, struct side *b, const uint8_t *skey, unsigned pa, unsigned pb)
{
	struct mbt_conn *ia, *ib;

	memset(a, 0, sizeof(*a));
	memset(b, 0, sizeof(*b));
	make_inner(&ia, &ib);
	assert(mbt_mse_client(ev, ia, skey, pa, &a->c) == 0);
	mbt_conn_set_callbacks(a->c, on_read, on_event, a);
	assert(mbt_mse_server(ev, ib, NULL, 0, lookup, NULL, pb, &b->c) == 0);
	mbt_conn_set_callbacks(b->c, on_read, on_event, b);
}

/* Bytes written before the handshake ends and bytes written after it. */
static void
test_exchange(unsigned pa, unsigned pb, unsigned expect)
{
	static uint8_t data[BIG];
	static struct side a, b;
	size_t early = 68;

	fill(data, sizeof(data), expect);
	start(&a, &b, hash_a, pa, pb);
	assert(mbt_mse_incomplete(a.c));
	assert(mbt_mse_selected(a.c) == 0);
	assert(mbt_conn_write(a.c, data, early) == (ssize_t)early);

	wait_for(NULL, &b.len, early);
	assert(memcmp(b.got, data, early) == 0);
	assert(mbt_mse_selected(b.c) == expect);
	assert(mbt_conn_write(b.c, data, 1000) == 1000);
	wait_for(NULL, &a.len, 1000);
	assert(memcmp(a.got, data, 1000) == 0);
	assert(mbt_mse_selected(a.c) == expect);
	assert(!mbt_mse_incomplete(a.c));

	assert(mbt_conn_write(a.c, data + early, BIG - early) == (ssize_t)(BIG - early));
	wait_for(NULL, &b.len, BIG);
	assert(memcmp(b.got, data, BIG) == 0);

	struct iovec iov[2] = {
		{.iov_base = data, .iov_len = 10},
		{.iov_base = data + 10, .iov_len = 90},
	};
	assert(mbt_conn_writev(b.c, iov, 2) == 100);
	wait_for(NULL, &a.len, 1100);
	assert(memcmp(a.got + 1000, data, 100) == 0);
	mbt_conn_free(a.c);
	mbt_conn_free(b.c);
}

static void
test_failures(void)
{
	static struct side a, b;
	static uint8_t junk[4096];
	struct mbt_conn *ia, *ib;

	/* No torrent for this infohash. */
	start(&a, &b, hash_b, MBT_MSE_RC4, MBT_MSE_RC4);
	wait_for(&b.failed, NULL, 0);
	assert(b.err == -ENOENT);
	assert(mbt_mse_incomplete(b.c));
	mbt_conn_free(a.c);
	mbt_conn_free(b.c);

	/* No common method. */
	start(&a, &b, hash_a, MBT_MSE_PLAINTEXT, MBT_MSE_RC4);
	wait_for(&b.failed, NULL, 0);
	assert(b.err == -EPROTO);
	mbt_conn_free(b.c);
	wait_for(&a.failed, NULL, 0);
	assert(mbt_mse_incomplete(a.c));
	mbt_conn_free(a.c);

	/* Random bytes never hold the marker: stop at the end of the pad. */
	memset(&a, 0, sizeof(a));
	fill(junk, sizeof(junk), 7);
	make_inner(&ia, &ib);
	assert(mbt_mse_client(ev, ia, hash_a, MBT_MSE_RC4, &a.c) == 0);
	mbt_conn_set_callbacks(a.c, on_read, on_event, &a);
	assert(mbt_conn_write(ib, junk, sizeof(junk)) == (ssize_t)sizeof(junk));
	wait_for(&a.failed, NULL, 0);
	assert(a.err == -EPROTO);
	assert(mbt_conn_write(a.c, "x", 1) == -EPIPE);
	mbt_conn_free(a.c);
	mbt_conn_free(ib);

}

/* An inner conn that can refuse a write, as uTP does when its window is full. */
struct flaky {
	struct mbt_conn base;
	struct mbt_conn_ops ops;
	struct mbt_conn *real;
	int refuse;
};

static ssize_t
flaky_write(struct mbt_conn *c, const void *buf, size_t len)
{
	struct flaky *f = (struct flaky *)c;

	return f->refuse ? -ENOBUFS : mbt_conn_write(f->real, buf, len);
}

static size_t
flaky_available(struct mbt_conn *c)
{
	return mbt_conn_available(((struct flaky *)c)->real);
}

static ssize_t
flaky_read(struct mbt_conn *c, void *buf, size_t len)
{
	return mbt_conn_read(((struct flaky *)c)->real, buf, len);
}

static void
flaky_watermark(struct mbt_conn *c, size_t low)
{
	mbt_conn_set_watermark(((struct flaky *)c)->real, low);
}

struct flaky_cb {
	struct flaky *f;
	mbt_conn_read_cb read;
	mbt_conn_event_cb event;
	void *arg;
} flaky_up;

static void
flaky_on_read(struct mbt_conn *c, void *arg)
{
	(void)c; (void)arg;
	if (flaky_up.read)
		flaky_up.read(&flaky_up.f->base, flaky_up.arg);
}

static void
flaky_on_event(struct mbt_conn *c, int err, void *arg)
{
	(void)c; (void)arg;
	if (flaky_up.event)
		flaky_up.event(&flaky_up.f->base, err, flaky_up.arg);
}

static void
flaky_callbacks(struct mbt_conn *c, mbt_conn_read_cb r, mbt_conn_event_cb e, void *arg)
{
	struct flaky *f = (struct flaky *)c;

	flaky_up = (struct flaky_cb){f, r, e, arg};
	mbt_conn_set_callbacks(f->real, flaky_on_read, flaky_on_event, NULL);
}

static void
flaky_free(struct mbt_conn *c)
{
	mbt_conn_free(((struct flaky *)c)->real);
	free(c);
}

static void
test_refused_write(void)
{
	static struct side a, b;
	struct mbt_conn *ia, *ib;
	struct flaky *f = calloc(1, sizeof(*f));

	assert(f);
	memset(&a, 0, sizeof(a));
	memset(&b, 0, sizeof(b));
	make_inner(&ia, &ib);
	f->ops = (struct mbt_conn_ops){
		.name = "flaky", .set_watermark = flaky_watermark, .available = flaky_available,
		.read = flaky_read, .write = flaky_write, .set_callbacks = flaky_callbacks,
		.free = flaky_free,
	};
	f->base.ops = &f->ops;
	f->real = ia;
	assert(mbt_mse_client(ev, &f->base, hash_a, MBT_MSE_RC4, &a.c) == 0);
	assert(strcmp(mbt_conn_transport_name(a.c), "flaky") == 0);
	mbt_conn_set_callbacks(a.c, on_read, on_event, &a);
	assert(mbt_mse_server(ev, ib, NULL, 0, lookup, NULL, MBT_MSE_RC4, &b.c) == 0);
	mbt_conn_set_callbacks(b.c, on_read, on_event, &b);

	while (mbt_mse_selected(a.c) == 0 || mbt_mse_selected(b.c) == 0)
		mbt_ev_step(ev, 10);
	if (mbt_mse_selected(b.c))
		assert(mbt_conn_write(b.c, "s", 1) == 1);
	wait_for(NULL, &a.len, 1);

	/* A refused write must not use up keystream. */
	f->refuse = 1;
	assert(mbt_conn_write(a.c, "first", 5) == -ENOBUFS);
	f->refuse = 0;
	assert(mbt_conn_write(a.c, "second", 6) == 6);
	wait_for(NULL, &b.len, 6);
	assert(memcmp(b.got, "second", 6) == 0);
	mbt_conn_free(a.c);
	mbt_conn_free(b.c);
}

/* The upper layer frees the conn from its read callback, as the session does. */
static void
free_on_read(struct mbt_conn *c, void *arg)
{
	int *fired = arg;

	(*fired)++;
	mbt_conn_free(c);
}

static void
test_free_in_callback(void)
{
	static struct side a, b;
	int fired = 0;

	start(&a, &b, hash_a, MBT_MSE_RC4, MBT_MSE_RC4);
	mbt_conn_set_callbacks(b.c, free_on_read, NULL, &fired);
	assert(mbt_conn_write(a.c, "hello", 5) == 5);
	wait_for(&fired, NULL, 0);
	assert(fired == 1);
	for (int i = 0; i < 10; i++)
		mbt_ev_step(ev, 1);
	assert(fired == 1);
	mbt_conn_free(a.c);
	mbt_ev_step(ev, 1);
}

int
main(void)
{
	assert(mbt_ev_new_default(&ev) == 0);

	test_exchange(MBT_MSE_RC4 | MBT_MSE_PLAINTEXT, MBT_MSE_RC4 | MBT_MSE_PLAINTEXT, MBT_MSE_RC4);
	test_exchange(MBT_MSE_RC4, MBT_MSE_RC4, MBT_MSE_RC4);
	test_exchange(MBT_MSE_PLAINTEXT, MBT_MSE_RC4 | MBT_MSE_PLAINTEXT, MBT_MSE_PLAINTEXT);
	test_failures();
	test_refused_write();
	test_free_in_callback();

	mbt_ev_destroy(ev);
	puts("mse: ok");
	return 0;
}
