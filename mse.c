// SPDX-License-Identifier: ISC
/*
 * MSE/PE handshake and stream filter. See mbt/mse.h.
 *
 *   A->B: Ya, PadA
 *   B->A: Yb, PadB
 *   A->B: HASH('req1', S), HASH('req2', SKEY) ^ HASH('req3', S),
 *         ENCRYPT(VC, crypto_provide, len(PadC), PadC, len(IA)), ENCRYPT(IA)
 *   B->A: ENCRYPT(VC, crypto_select, len(PadD), PadD), ENCRYPT2(payload)
 *
 * Pads have no length prefix. The reader finds the end of PadA by the req1
 * hash, and the end of PadB by the encrypted VC.
 */
#include "mbt/compat.h"
#include "mbt/event.h"
#include "mbt/mse.h"
#include "mbt/sha1_compat.h"

#include <errno.h>
#include <stdlib.h>
#include <string.h>

#include "banned.h"

#define KEY_LEN 96
#define PRIV_LEN 20
#define HASH_LEN 20
#define VC_LEN 8
#define PAD_MAX 512
#define IA_MAX 1024              /* a BitTorrent handshake needs 68 */
#define HEAD_LEN (VC_LEN + 4 + 2)
#define HS_MAX (KEY_LEN + PAD_MAX + 2 * HASH_LEN + HEAD_LEN + PAD_MAX + 2 + IA_MAX)
#define OUT_MAX IA_MAX
#define TIMEOUT_USEC (20 * 1000000ULL)
#define LIMBS (KEY_LEN / 4)

/* The fuzz target must reach the parser, so it runs without DH or RC4. */
#ifdef FUZZING_BUILD_MODE_UNSAFE_FOR_PRODUCTION
#define FUZZ_NO_CRYPTO 1
#else
#define FUZZ_NO_CRYPTO 0
#endif

static const uint8_t mse_prime[KEY_LEN] = {
	0xff,0xff,0xff,0xff,0xff,0xff,0xff,0xff, 0xc9,0x0f,0xda,0xa2,0x21,0x68,0xc2,0x34,
	0xc4,0xc6,0x62,0x8b,0x80,0xdc,0x1c,0xd1, 0x29,0x02,0x4e,0x08,0x8a,0x67,0xcc,0x74,
	0x02,0x0b,0xbe,0xa6,0x3b,0x13,0x9b,0x22, 0x51,0x4a,0x08,0x79,0x8e,0x34,0x04,0xdd,
	0xef,0x95,0x19,0xb3,0xcd,0x3a,0x43,0x1b, 0x30,0x2b,0x0a,0x6d,0xf2,0x5f,0x14,0x37,
	0x4f,0xe1,0x35,0x6d,0x6d,0x51,0xc2,0x45, 0xe4,0x85,0xb5,0x76,0x62,0x5e,0x7e,0xc6,
	0xf4,0x4c,0x42,0xe9,0xa6,0x3a,0x36,0x21, 0x00,0x00,0x00,0x00,0x00,0x09,0x05,0x63,
};

/* --- RC4 --- */

struct rc4 {
	uint8_t s[256];
	uint8_t i, j;
};

static void
rc4_init(struct rc4 *c, const uint8_t *key, size_t keylen)
{
	uint8_t j = 0;

	for (int n = 0; n < 256; n++)
		c->s[n] = (uint8_t)n;
	for (int n = 0; n < 256; n++) {
		uint8_t t = c->s[n];

		j = (uint8_t)(j + t + key[(size_t)n % keylen]);
		c->s[n] = c->s[j];
		c->s[j] = t;
	}
	c->i = c->j = 0;
}

/* XOR len bytes with the keystream. buf can be NULL to discard keystream. */
static void
rc4_crypt(struct rc4 *c, uint8_t *buf, size_t len)
{
	uint8_t i = c->i, j = c->j;

	if (FUZZ_NO_CRYPTO)
		return;
	for (size_t n = 0; n < len; n++) {
		uint8_t si, sj;

		i = (uint8_t)(i + 1);
		si = c->s[i];
		j = (uint8_t)(j + si);
		sj = c->s[j];
		c->s[i] = sj;
		c->s[j] = si;
		if (buf)
			buf[n] ^= c->s[(uint8_t)(si + sj)];
	}
	c->i = i;
	c->j = j;
}

/* --- 768-bit modular exponentiation --- */

static void
limbs_from_be(uint32_t r[LIMBS], const uint8_t b[KEY_LEN])
{
	for (int i = 0; i < LIMBS; i++) {
		const uint8_t *p = b + (LIMBS - 1 - i) * 4;

		r[i] = (uint32_t)p[0] << 24 | (uint32_t)p[1] << 16 |
		       (uint32_t)p[2] << 8 | p[3];
	}
}

static void
limbs_to_be(uint8_t b[KEY_LEN], const uint32_t a[LIMBS])
{
	for (int i = 0; i < LIMBS; i++) {
		uint8_t *p = b + (LIMBS - 1 - i) * 4;

		p[0] = (uint8_t)(a[i] >> 24);
		p[1] = (uint8_t)(a[i] >> 16);
		p[2] = (uint8_t)(a[i] >> 8);
		p[3] = (uint8_t)a[i];
	}
}

static int
limbs_ge(const uint32_t a[LIMBS], const uint32_t b[LIMBS])
{
	for (int i = LIMBS - 1; i >= 0; i--)
		if (a[i] != b[i])
			return a[i] > b[i];
	return 1;
}

static void
limbs_sub(uint32_t a[LIMBS], const uint32_t b[LIMBS])
{
	uint64_t borrow = 0;

	for (int i = 0; i < LIMBS; i++) {
		uint64_t d = (uint64_t)a[i] - b[i] - borrow;

		a[i] = (uint32_t)d;
		borrow = (d >> 32) & 1;
	}
}

/* r = a * b / 2^768 mod p, by word-serial Montgomery multiplication. */
static void
mont_mul(uint32_t r[LIMBS], const uint32_t a[LIMBS], const uint32_t b[LIMBS],
         const uint32_t p[LIMBS], uint32_t n0)
{
	uint32_t t[LIMBS + 2] = {0};

	for (int i = 0; i < LIMBS; i++) {
		uint64_t cs = 0;

		for (int j = 0; j < LIMBS; j++) {
			cs = (uint64_t)t[j] + (uint64_t)a[j] * b[i] + (cs >> 32);
			t[j] = (uint32_t)cs;
		}
		cs = (uint64_t)t[LIMBS] + (cs >> 32);
		t[LIMBS] = (uint32_t)cs;
		t[LIMBS + 1] = (uint32_t)(cs >> 32);

		uint32_t m = t[0] * n0;

		cs = (uint64_t)t[0] + (uint64_t)m * p[0];
		for (int j = 1; j < LIMBS; j++) {
			cs = (uint64_t)t[j] + (uint64_t)m * p[j] + (cs >> 32);
			t[j - 1] = (uint32_t)cs;
		}
		cs = (uint64_t)t[LIMBS] + (cs >> 32);
		t[LIMBS - 1] = (uint32_t)cs;
		t[LIMBS] = t[LIMBS + 1] + (uint32_t)(cs >> 32);
	}
	if (t[LIMBS] || limbs_ge(t, p))
		limbs_sub(t, p);
	memcpy(r, t, LIMBS * sizeof(*r));
}

/* out = base ^ exp mod p. exp is big endian. */
static void
modexp(uint8_t out[KEY_LEN], const uint8_t base[KEY_LEN], const uint8_t exp[PRIV_LEN])
{
	uint32_t p[LIMBS], b[LIMBS], r2[LIMBS] = {1}, x[LIMBS], one[LIMBS] = {1};
	uint32_t inv;

	limbs_from_be(p, mse_prime);
	limbs_from_be(b, base);

	/* -1/p mod 2^32, by Newton iteration. */
	inv = p[0];
	for (int i = 0; i < 4; i++)
		inv *= 2 - p[0] * inv;
	inv = 0 - inv;

	/* 2^1536 mod p, to convert values to Montgomery form. */
	for (int i = 0; i < KEY_LEN * 16; i++) {
		uint32_t top = r2[LIMBS - 1] >> 31;

		for (int j = LIMBS - 1; j > 0; j--)
			r2[j] = r2[j] << 1 | r2[j - 1] >> 31;
		r2[0] <<= 1;
		if (top || limbs_ge(r2, p))
			limbs_sub(r2, p);
	}

	mont_mul(b, b, r2, p, inv);
	mont_mul(x, one, r2, p, inv);
	for (int i = 0; i < PRIV_LEN * 8; i++) {
		mont_mul(x, x, x, p, inv);
		if (exp[i / 8] >> (7 - i % 8) & 1)
			mont_mul(x, x, b, p, inv);
	}
	mont_mul(x, x, one, p, inv);
	limbs_to_be(out, x);
}

/* --- Key derivation --- */

static void
hash2(uint8_t out[HASH_LEN], const char tag[4], const uint8_t *a, size_t alen)
{
	SHA1_CTX ctx;

	SHA1Init(&ctx);
	SHA1Update(&ctx, (const uint8_t *)tag, 4);
	SHA1Update(&ctx, a, alen);
	SHA1Final(out, &ctx);
}

static void
stream_key(struct rc4 *c, const char tag[4], const uint8_t s[KEY_LEN],
           const uint8_t skey[HASH_LEN])
{
	uint8_t key[HASH_LEN];
	SHA1_CTX ctx;

	SHA1Init(&ctx);
	SHA1Update(&ctx, (const uint8_t *)tag, 4);
	SHA1Update(&ctx, s, KEY_LEN);
	SHA1Update(&ctx, skey, HASH_LEN);
	SHA1Final(key, &ctx);
	rc4_init(c, key, sizeof(key));
	rc4_crypt(c, NULL, 1024);
}

/* --- Conn --- */

enum mse_state {
	MSE_YB,          /* A: wait for Yb */
	MSE_FIND_VC,     /* A: find the end of PadB */
	MSE_SELECT,      /* A: crypto_select, len(PadD) */
	MSE_PAD_D,       /* A: PadD */
	MSE_YA,          /* B: wait for Ya */
	MSE_FIND_REQ1,   /* B: find the end of PadA */
	MSE_REQ,         /* B: SKEY hash, VC, crypto_provide, len(PadC) */
	MSE_PAD_C,       /* B: PadC, len(IA) */
	MSE_IA,          /* B: IA */
	MSE_DONE,
	MSE_FAILED,
};

struct mbt_mse {
	struct mbt_conn base;       /* first: this is an mbt_conn */
	struct mbt_conn_ops ops;    /* mse_ops, with the name of inner */
	struct mbt_conn *inner;
	struct mbt_ev *ev;
	struct mbt_ev_timer *timer;

	enum mse_state state;
	unsigned provide;
	unsigned selected;
	uint8_t skey[HASH_LEN];
	mbt_mse_skey_lookup lookup;
	void *lookup_arg;

	uint8_t priv[PRIV_LEN];
	uint8_t pub[KEY_LEN];
	uint8_t secret[KEY_LEN];
	uint8_t marker[VC_LEN > HASH_LEN ? VC_LEN : HASH_LEN];
	struct rc4 rx, tx;

	/* Handshake input. After MSE_DONE it holds plain bytes to give up. */
	uint8_t *hs;
	size_t hs_off, hs_len;
	size_t need;                /* length of the pad or IA in progress */

	uint8_t *out;               /* upper writes held until MSE_DONE */
	size_t out_len;
	uint8_t *txbuf;
	size_t txcap;

	mbt_conn_read_cb up_read;
	mbt_conn_event_cb up_event;
	void *up_arg;
	mbt_conn_write_cb up_write;
	void *up_write_arg;
	size_t low;

	int in_cb;                  /* inner must outlive its own callback */
	int dead;
};

static const struct mbt_conn_ops mse_ops;

static size_t
mse_prefix(const struct mbt_mse *m)
{
	return m->state == MSE_DONE ? m->hs_len - m->hs_off : 0;
}

/* Fire the inner read callback at the right time for bytes in the prefix. */
static void
mse_sync_watermark(struct mbt_mse *m)
{
	size_t have = mse_prefix(m);

	mbt_conn_set_watermark(m->inner, m->low > have ? m->low - have : 1);
}

static size_t
mse_available(struct mbt_conn *c)
{
	struct mbt_mse *m = (struct mbt_mse *)c;

	if (m->state != MSE_DONE)
		return 0;
	return mse_prefix(m) + mbt_conn_available(m->inner);
}

static ssize_t
mse_read(struct mbt_conn *c, void *buf, size_t len)
{
	struct mbt_mse *m = (struct mbt_mse *)c;
	uint8_t *p = buf;
	size_t done = mse_prefix(m);
	size_t avail;

	if (m->state != MSE_DONE)
		return 0;
	if (done > len)
		done = len;
	if (done) {
		memcpy(p, m->hs + m->hs_off, done);
		m->hs_off += done;
		if (m->hs_off == m->hs_len) {
			free(m->hs);
			m->hs = NULL;
			m->hs_off = m->hs_len = 0;
		}
		mse_sync_watermark(m);
	}
	avail = mbt_conn_available(m->inner);
	if (avail > len - done)
		avail = len - done;
	if (avail) {
		ssize_t n = mbt_conn_read(m->inner, p + done, avail);

		if (n < 0)
			return done ? (ssize_t)done : n;
		if (m->selected == MBT_MSE_RC4)
			rc4_crypt(&m->rx, p + done, (size_t)n);
		done += (size_t)n;
	}
	return (ssize_t)done;
}

/* Send on the established stream. The keystream advances only for bytes that
 * inner accepts, so a -ENOBUFS caller can retry. */
static ssize_t
mse_send(struct mbt_mse *m, const struct iovec *iov, int iovcnt)
{
	struct rc4 saved;
	struct iovec v;
	size_t total = 0;
	ssize_t n;

	if (m->selected != MBT_MSE_RC4)
		return mbt_conn_writev(m->inner, iov, iovcnt);
	for (int i = 0; i < iovcnt; i++)
		total += iov[i].iov_len;
	if (total > m->txcap) {
		uint8_t *p = realloc(m->txbuf, total);

		if (!p)
			return -ENOMEM;
		m->txbuf = p;
		m->txcap = total;
	}
	v.iov_base = m->txbuf;
	v.iov_len = 0;
	for (int i = 0; i < iovcnt; i++) {
		if (iov[i].iov_len)
			memcpy(m->txbuf + v.iov_len, iov[i].iov_base, iov[i].iov_len);
		v.iov_len += iov[i].iov_len;
	}
	saved = m->tx;
	rc4_crypt(&m->tx, m->txbuf, total);
	n = mbt_conn_writev(m->inner, &v, 1);
	if (n < 0 || (size_t)n < total) {
		m->tx = saved;
		if (n > 0)
			rc4_crypt(&m->tx, NULL, (size_t)n);
	}
	return n;
}

static ssize_t
mse_writev(struct mbt_conn *c, const struct iovec *iov, int iovcnt)
{
	struct mbt_mse *m = (struct mbt_mse *)c;
	size_t total = 0;

	if (m->state == MSE_DONE)
		return mse_send(m, iov, iovcnt);
	if (m->state == MSE_FAILED)
		return -EPIPE;
	for (int i = 0; i < iovcnt; i++)
		total += iov[i].iov_len;
	if (total > OUT_MAX - m->out_len)
		return -ENOBUFS;
	if (!m->out && !(m->out = malloc(OUT_MAX)))
		return -ENOMEM;
	for (int i = 0; i < iovcnt; i++) {
		if (iov[i].iov_len)
			memcpy(m->out + m->out_len, iov[i].iov_base, iov[i].iov_len);
		m->out_len += iov[i].iov_len;
	}
	return (ssize_t)total;
}

static ssize_t
mse_write(struct mbt_conn *c, const void *buf, size_t len)
{
	struct iovec v = {.iov_base = (void *)(uintptr_t)buf, .iov_len = len};

	return mse_writev(c, &v, 1);
}

static void
mse_set_watermark(struct mbt_conn *c, size_t low)
{
	struct mbt_mse *m = (struct mbt_mse *)c;

	m->low = low;
	if (m->state == MSE_DONE)
		mse_sync_watermark(m);
}

static void
mse_set_callbacks(struct mbt_conn *c, mbt_conn_read_cb readcb,
                  mbt_conn_event_cb eventcb, void *arg)
{
	struct mbt_mse *m = (struct mbt_mse *)c;

	m->up_read = readcb;
	m->up_event = eventcb;
	m->up_arg = arg;
}

static void
mse_inner_writecb(struct mbt_conn *c, void *arg)
{
	struct mbt_mse *m = arg;

	(void)c;
	if (m->up_write && !m->dead)
		m->up_write(&m->base, m->up_write_arg);
}

static void
mse_set_writecb(struct mbt_conn *c, mbt_conn_write_cb cb, void *arg)
{
	struct mbt_mse *m = (struct mbt_mse *)c;

	m->up_write = cb;
	m->up_write_arg = arg;
	mbt_conn_set_writecb(m->inner, cb ? mse_inner_writecb : NULL, m);
}

static int
mse_peer_addr(struct mbt_conn *c, char *buf, size_t buflen, uint16_t *port)
{
	return mbt_conn_peer_addr(((struct mbt_mse *)c)->inner, buf, buflen, port);
}

static void
mse_destroy(struct mbt_mse *m)
{
	if (m->timer)
		mbt_ev_timer_free(m->timer);
	mbt_conn_free(m->inner);
	free(m->hs);
	free(m->out);
	free(m->txbuf);
	free(m);
}

static void
mse_destroy_cb(struct mbt_ev_timer *timer, void *arg)
{
	(void)timer;
	mse_destroy(arg);
}

static void
mse_free(struct mbt_conn *c)
{
	struct mbt_mse *m = (struct mbt_mse *)c;

	if (!m->in_cb) {
		mse_destroy(m);
		return;
	}
	/* The caller is under an inner callback, so free inner later. */
	m->dead = 1;
	m->up_read = NULL;
	m->up_event = NULL;
	m->up_write = NULL;
	mbt_conn_set_callbacks(m->inner, NULL, NULL, NULL);
	mbt_conn_set_writecb(m->inner, NULL, NULL);
	if (m->timer)
		mbt_ev_timer_free(m->timer);
	m->timer = NULL;
	if (mbt_ev_timer_new(m->ev, &m->timer, 1, mse_destroy_cb, m) < 0)
		m->timer = NULL;
}

static const struct mbt_conn_ops mse_ops = {
	.set_watermark = mse_set_watermark,
	.available = mse_available,
	.read = mse_read,
	.write = mse_write,
	.writev = mse_writev,
	.set_callbacks = mse_set_callbacks,
	.set_writecb = mse_set_writecb,
	.peer_addr = mse_peer_addr,
	.free = mse_free,
};

/* --- Handshake --- */

static void
mse_fail(struct mbt_mse *m, int err)
{
	if (m->state == MSE_DONE || m->state == MSE_FAILED)
		return;
	m->state = MSE_FAILED;
	if (m->up_event)
		m->up_event(&m->base, err, m->up_arg);
}

static void
mse_timeout(struct mbt_ev_timer *timer, void *arg)
{
	struct mbt_mse *m = arg;

	mbt_ev_timer_free(timer);
	m->timer = NULL;
	m->in_cb++;
	mse_fail(m, -ETIMEDOUT);
	m->in_cb--;
}

static int
mse_keypair(struct mbt_mse *m)
{
	static const uint8_t two[KEY_LEN] = {[KEY_LEN - 1] = 2};

	if (FUZZ_NO_CRYPTO)
		return 0;
	arc4random_buf(m->priv, sizeof(m->priv));
	modexp(m->pub, two, m->priv);
	return 0;
}

static void
mse_shared_secret(struct mbt_mse *m, const uint8_t *peer)
{
	if (!FUZZ_NO_CRYPTO)
		modexp(m->secret, peer, m->priv);
}

static int
mse_send_pub(struct mbt_mse *m)
{
	uint8_t buf[KEY_LEN + PAD_MAX];
	size_t pad = arc4random_uniform(PAD_MAX + 1);
	ssize_t n;

	memcpy(buf, m->pub, KEY_LEN);
	arc4random_buf(buf + KEY_LEN, pad);
	n = mbt_conn_write(m->inner, buf, KEY_LEN + pad);
	return n < 0 ? (int)n : 0;
}

/* Find marker in the bytes after hs_off, and skip past it. Returns 1 if
 * found, 0 if more bytes can still reveal it, or -EPROTO. */
static int
mse_find(struct mbt_mse *m, const uint8_t *marker, size_t len)
{
	size_t avail = m->hs_len - m->hs_off;

	for (size_t i = 0; i + len <= avail && i <= PAD_MAX; i++) {
		if (memcmp(m->hs + m->hs_off + i, marker, len) == 0) {
			m->hs_off += i + len;
			return 1;
		}
	}
	return avail >= PAD_MAX + len ? -EPROTO : 0;
}

static int
mse_send_req(struct mbt_mse *m)
{
	uint8_t buf[2 * HASH_LEN + HEAD_LEN + 2 + IA_MAX] = {0};
	uint8_t req2[HASH_LEN], req3[HASH_LEN];
	uint8_t *p = buf + 2 * HASH_LEN;
	size_t ia = m->out_len;
	ssize_t n;

	hash2(buf, "req1", m->secret, KEY_LEN);
	hash2(req2, "req2", m->skey, HASH_LEN);
	hash2(req3, "req3", m->secret, KEY_LEN);
	for (int i = 0; i < HASH_LEN; i++)
		buf[HASH_LEN + i] = req2[i] ^ req3[i];

	/* VC is zero. len(PadC) is zero. */
	p[VC_LEN + 3] = (uint8_t)m->provide;
	p[HEAD_LEN] = (uint8_t)(ia >> 8);
	p[HEAD_LEN + 1] = (uint8_t)ia;
	if (ia)
		memcpy(p + HEAD_LEN + 2, m->out, ia);
	m->out_len = 0;
	rc4_crypt(&m->tx, p, HEAD_LEN + 2 + ia);
	n = mbt_conn_write(m->inner, buf, 2 * HASH_LEN + HEAD_LEN + 2 + ia);
	return n < 0 ? (int)n : 0;
}

static int
mse_send_select(struct mbt_mse *m)
{
	uint8_t buf[HEAD_LEN] = {0};
	ssize_t n;

	buf[VC_LEN + 3] = (uint8_t)m->selected;
	rc4_crypt(&m->tx, buf, sizeof(buf));
	n = mbt_conn_write(m->inner, buf, sizeof(buf));
	return n < 0 ? (int)n : 0;
}

/* The rest of hs is stream data, and skip counts leading plain bytes in it. */
static int
mse_established(struct mbt_mse *m, size_t skip)
{
	if (m->selected == MBT_MSE_RC4)
		rc4_crypt(&m->rx, m->hs + m->hs_off + skip, m->hs_len - m->hs_off - skip);
	m->state = MSE_DONE;
	if (m->timer) {
		mbt_ev_timer_free(m->timer);
		m->timer = NULL;
	}
	if (m->out_len) {
		struct iovec v = {.iov_base = m->out, .iov_len = m->out_len};
		ssize_t n = mse_send(m, &v, 1);

		m->out_len = 0;
		if (n < 0)
			return (int)n;
	}
	free(m->out);
	m->out = NULL;
	mse_sync_watermark(m);
	return 0;
}

/* Returns 1 after progress, 0 if more bytes are needed, or -errno. */
static int
mse_step(struct mbt_mse *m)
{
	size_t avail = m->hs_len - m->hs_off;
	uint8_t *p = m->hs + m->hs_off;
	uint8_t hash[HASH_LEN];
	int rv;

	switch (m->state) {
	case MSE_YB:
		if (avail < KEY_LEN)
			return 0;
		mse_shared_secret(m, p);
		m->hs_off += KEY_LEN;
		stream_key(&m->tx, "keyA", m->secret, m->skey);
		stream_key(&m->rx, "keyB", m->secret, m->skey);
		if ((rv = mse_send_req(m)) < 0)
			return rv;
		memset(m->marker, 0, VC_LEN);
		rc4_crypt(&m->rx, m->marker, VC_LEN);
		m->state = MSE_FIND_VC;
		return 1;

	case MSE_FIND_VC:
		if ((rv = mse_find(m, m->marker, VC_LEN)) <= 0)
			return rv;
		m->state = MSE_SELECT;
		return 1;

	case MSE_SELECT: {
		unsigned sel;

		if (avail < 6)
			return 0;
		rc4_crypt(&m->rx, p, 6);
		sel = (unsigned)p[0] << 24 | (unsigned)p[1] << 16 | (unsigned)p[2] << 8 | p[3];
		m->need = (size_t)p[4] << 8 | p[5];
		m->hs_off += 6;
		if ((sel != MBT_MSE_RC4 && sel != MBT_MSE_PLAINTEXT) ||
		    !(sel & m->provide) || m->need > PAD_MAX)
			return -EPROTO;
		m->selected = sel;
		m->state = MSE_PAD_D;
		return 1;
	}

	case MSE_PAD_D:
		if (avail < m->need)
			return 0;
		rc4_crypt(&m->rx, p, m->need);
		m->hs_off += m->need;
		if ((rv = mse_established(m, 0)) < 0)
			return rv;
		return 1;

	case MSE_YA:
		if (avail < KEY_LEN)
			return 0;
		mse_shared_secret(m, p);
		m->hs_off += KEY_LEN;
		if ((rv = mse_send_pub(m)) < 0)
			return rv;
		hash2(m->marker, "req1", m->secret, KEY_LEN);
		m->state = MSE_FIND_REQ1;
		return 1;

	case MSE_FIND_REQ1:
		if ((rv = mse_find(m, m->marker, HASH_LEN)) <= 0)
			return rv;
		m->state = MSE_REQ;
		return 1;

	case MSE_REQ: {
		unsigned peer;

		if (avail < HASH_LEN + HEAD_LEN)
			return 0;
		hash2(hash, "req3", m->secret, KEY_LEN);
		for (int i = 0; i < HASH_LEN; i++)
			hash[i] ^= p[i];
		if ((rv = m->lookup(hash, m->skey, m->lookup_arg)) < 0)
			return rv;
		stream_key(&m->tx, "keyB", m->secret, m->skey);
		stream_key(&m->rx, "keyA", m->secret, m->skey);
		p += HASH_LEN;
		rc4_crypt(&m->rx, p, HEAD_LEN);
		for (int i = 0; i < VC_LEN; i++)
			if (p[i])
				return -EPROTO;
		peer = (unsigned)p[VC_LEN + 2] << 8 | p[VC_LEN + 3];
		m->need = (size_t)p[VC_LEN + 4] << 8 | p[VC_LEN + 5];
		m->hs_off += HASH_LEN + HEAD_LEN;
		if (m->need > PAD_MAX)
			return -EPROTO;
		peer &= m->provide;
		if (peer & MBT_MSE_RC4)
			m->selected = MBT_MSE_RC4;
		else if (peer & MBT_MSE_PLAINTEXT)
			m->selected = MBT_MSE_PLAINTEXT;
		else
			return -EPROTO;
		m->state = MSE_PAD_C;
		return 1;
	}

	case MSE_PAD_C:
		if (avail < m->need + 2)
			return 0;
		rc4_crypt(&m->rx, p, m->need + 2);
		p += m->need;
		m->hs_off += m->need + 2;
		m->need = (size_t)p[0] << 8 | p[1];
		if (m->need > IA_MAX)
			return -EPROTO;
		m->state = MSE_IA;
		return 1;

	case MSE_IA:
		if (avail < m->need)
			return 0;
		rc4_crypt(&m->rx, p, m->need);    /* IA is always encrypted */
		if ((rv = mse_send_select(m)) < 0)
			return rv;
		if ((rv = mse_established(m, m->need)) < 0)
			return rv;
		return 1;

	default:
		return 0;
	}
}

static int
mse_input(struct mbt_mse *m)
{
	int rv;

	for (;;) {
		size_t room = HS_MAX - m->hs_len;
		size_t avail = mbt_conn_available(m->inner);

		if (avail > room)
			avail = room;
		if (avail) {
			ssize_t n = mbt_conn_read(m->inner, m->hs + m->hs_len, avail);

			if (n < 0)
				return (int)n;
			m->hs_len += (size_t)n;
		}
		while ((rv = mse_step(m)) > 0 && m->state != MSE_DONE)
			;
		if (rv < 0 || m->state == MSE_DONE)
			return rv < 0 ? rv : 0;
		if (mbt_conn_available(m->inner) == 0)
			return 0;
		if (m->hs_len == HS_MAX)
			return -EPROTO;
	}
}

/* Run the upper read callback while it can make progress. */
static void
mse_pump(struct mbt_mse *m)
{
	while (m->state == MSE_DONE && m->up_read) {
		size_t have = mse_available(&m->base);

		if (have == 0 || have < m->low)
			break;
		m->up_read(&m->base, m->up_arg);
		if (m->dead || mse_available(&m->base) >= have)
			break;
	}
}

static void
mse_inner_read(struct mbt_conn *c, void *arg)
{
	struct mbt_mse *m = arg;
	int rv;

	(void)c;
	if (m->dead || m->state == MSE_FAILED)
		return;
	m->in_cb++;
	if (m->state != MSE_DONE && (rv = mse_input(m)) < 0)
		mse_fail(m, rv);
	if (!m->dead)
		mse_pump(m);
	m->in_cb--;
}

static void
mse_inner_event(struct mbt_conn *c, int err, void *arg)
{
	struct mbt_mse *m = arg;

	(void)c;
	if (m->dead || m->state == MSE_FAILED)
		return;
	m->in_cb++;
	if (m->state != MSE_DONE)
		mse_fail(m, err < 0 ? err : -ECONNRESET);
	else if (m->up_event)
		m->up_event(&m->base, err, m->up_arg);
	m->in_cb--;
}

static int
mse_new(struct mbt_mse **out, struct mbt_ev *ev, struct mbt_conn *inner, unsigned provide)
{
	struct mbt_mse *m;
	int rv;

	if (!(provide & (MBT_MSE_PLAINTEXT | MBT_MSE_RC4)))
		return -EINVAL;
	if (!(m = calloc(1, sizeof(*m))) || !(m->hs = malloc(HS_MAX))) {
		free(m);
		return -ENOMEM;
	}
	m->ops = mse_ops;
	m->ops.name = inner->ops->name;
	m->base.ops = &m->ops;
	m->inner = inner;
	m->ev = ev;
	m->provide = provide;
	m->low = 1;
	if ((rv = mbt_ev_timer_new(ev, &m->timer, TIMEOUT_USEC, mse_timeout, m)) < 0) {
		free(m->hs);
		free(m);
		return rv;
	}
	mse_keypair(m);
	*out = m;
	return 0;
}

/* Undo mse_new without freeing inner, which still belongs to the caller. */
static void
mse_abort(struct mbt_mse *m)
{
	mbt_ev_timer_free(m->timer);
	free(m->hs);
	free(m);
}

int
mbt_mse_client(struct mbt_ev *ev, struct mbt_conn *inner, const uint8_t infohash[20],
               unsigned provide, struct mbt_conn **out)
{
	struct mbt_mse *m;
	int rv;

	if ((rv = mse_new(&m, ev, inner, provide)) < 0)
		return rv;
	memcpy(m->skey, infohash, HASH_LEN);
	m->state = MSE_YB;
	if ((rv = mse_send_pub(m)) < 0) {
		mse_abort(m);
		return rv;
	}
	mbt_conn_set_watermark(inner, 1);
	mbt_conn_set_callbacks(inner, mse_inner_read, mse_inner_event, m);
	*out = &m->base;
	return 0;
}

int
mbt_mse_server(struct mbt_ev *ev, struct mbt_conn *inner, const void *seed, size_t seedlen,
               mbt_mse_skey_lookup lookup, void *arg, unsigned provide,
               struct mbt_conn **out)
{
	struct mbt_mse *m;
	int rv;

	if (seedlen > HS_MAX)
		return -EINVAL;
	if ((rv = mse_new(&m, ev, inner, provide)) < 0)
		return rv;
	m->lookup = lookup;
	m->lookup_arg = arg;
	m->state = MSE_YA;
	if (seedlen)
		memcpy(m->hs, seed, seedlen);
	m->hs_len = seedlen;
	if ((rv = mse_input(m)) < 0) {
		mse_abort(m);
		return rv;
	}
	mbt_conn_set_watermark(inner, 1);
	mbt_conn_set_callbacks(inner, mse_inner_read, mse_inner_event, m);
	*out = &m->base;
	return 0;
}

unsigned
mbt_mse_selected(struct mbt_conn *c)
{
	if (!c || c->ops->free != mse_free || ((struct mbt_mse *)c)->state != MSE_DONE)
		return 0;
	return ((struct mbt_mse *)c)->selected;
}

int
mbt_mse_incomplete(struct mbt_conn *c)
{
	return c && c->ops->free == mse_free && ((struct mbt_mse *)c)->state != MSE_DONE;
}
