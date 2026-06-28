// SPDX-License-Identifier: ISC
/*
 * uTP (BEP 29) -- uTorrent Transport Protocol
 *
 * Reliable ordered stream over UDP with LEDBAT congestion control.
 * Each uTP connection implements mbt_conn_ops so the peer layer is unchanged.
 *
 * mbt_utp_ctx  -- one per session; owns the UDP listener; demultiplexes by conn_id
 * mbt_utp_sock -- one per connection; implements mbt_conn_ops
 *
 * Wire header (20 bytes, big-endian):
 *   type_ver(8) extension(8) conn_id(16) timestamp_us(32)
 *   timestamp_diff_us(32) wnd_size(32) seq_nr(16) ack_nr(16)
 */

#include "mbt/utp.h"
#include "mbt/event.h"
#include "mbt/log.h"
#include "net.h"

#include <stdlib.h>
#include <stdio.h>
#include <string.h>
#include <errno.h>
#include <time.h>
#include <unistd.h>
#include <assert.h>
#include <sys/queue.h>
#include <netinet/in.h>
#include <arpa/inet.h>

/* ---- constants ---- */

#define UTP_VERSION          1
#define UTP_HEADER_SIZE      20
#define UTP_MIN_PACKET_SIZE  150
#define UTP_MAX_PACKET_SIZE  1450
#define UTP_INIT_CWND        UTP_MAX_PACKET_SIZE
#define UTP_INIT_TIMEOUT_MS  1000
#define UTP_CCONTROL_TARGET  100000   /* us target one-way delay */
#define UTP_MAX_CWND_INC     3000     /* bytes/RTT max window increase */
#define UTP_REORDER_THRESH   3
#define UTP_BASE_DELAY_HIST  120      /* seconds */
#define UTP_CONN_HASH_BITS   6
#define UTP_CONN_HASH_SIZE   (1 << UTP_CONN_HASH_BITS)
#define UTP_FIN_WAIT_MS      4000

/* ring buffer capacities -- must be powers of 2 */
#define SBUF_CAP        (64 * 1024)
#define RBUF_CAP        (64 * 1024)
#define SEND_QUEUE_SIZE  64           /* power of 2 */
#define RECV_QUEUE_SIZE  64           /* power of 2 */
#define SEND_QUEUE_MASK  (SEND_QUEUE_SIZE - 1)
#define RECV_QUEUE_MASK  (RECV_QUEUE_SIZE - 1)

/* ---- packet types (high nibble of type_ver) ---- */
#define ST_DATA   0
#define ST_FIN    1
#define ST_STATE  2
#define ST_RESET  3
#define ST_SYN    4

/* ---- connection states ---- */
enum utp_cs {
	UTP_CS_IDLE,
	UTP_CS_SYN_SENT,
	UTP_CS_SYN_RECV,
	UTP_CS_CONNECTED,
	UTP_CS_FIN_SENT,
	UTP_CS_DESTROY,
};

/* ---- wire header ---- */
struct utp_hdr {
	uint8_t  type_ver;
	uint8_t  extension;
	uint16_t conn_id;
	uint32_t timestamp_us;
	uint32_t timestamp_diff_us;
	uint32_t wnd_size;
	uint16_t seq_nr;
	uint16_t ack_nr;
} __attribute__((packed));

/* ---- per-packet send tracking ---- */
struct utp_send_entry {
	uint16_t seq_nr;
	uint32_t sbuf_off;   /* absolute sbuf offset at time of send */
	uint16_t len;
	uint64_t sent_us;
	uint8_t  retransmits;
	uint8_t  sacked;     /* acked (cumulative or SACK) */
	uint8_t  valid;
};

/* ---- per-packet recv reorder buffer ---- */
struct utp_recv_entry {
	uint8_t  valid;
	uint16_t seq_nr;
	uint16_t len;
	uint8_t  data[UTP_MAX_PACKET_SIZE];
};

/* ---- one uTP stream ---- */
struct mbt_utp_sock {
	struct mbt_conn      conn;      /* MUST be first */
	struct mbt_utp_ctx  *ctx;
	enum utp_cs  state;
	uint16_t     recv_conn_id;
	uint16_t     send_conn_id;

	/* send sequence */
	uint16_t     seq_nr;        /* next seq to assign */
	uint16_t     send_una;      /* oldest unacked seq */
	int          dup_acks;      /* consecutive dup acks */

	/* recv sequence */
	uint16_t     ack_nr;        /* last in-order seq received */

	/* congestion */
	uint32_t     max_window;
	uint32_t     peer_wnd;

	/* RTT / LEDBAT */
	int32_t  rtt_us;
	int32_t  rtt_var_us;
	int32_t  timeout_ms;
	uint32_t reply_micro;
	uint32_t base_delay_us;
	uint64_t base_delay_ts_us;

	struct sockaddr_storage peer_addr;
	socklen_t               peer_addrlen;

	/* recv ring buffer */
	uint8_t  *rbuf;
	uint32_t  rbuf_rd;          /* total bytes consumed by app (monotonic) */
	uint32_t  rbuf_wr;          /* total bytes produced (monotonic) */
	size_t    rbuf_watermark;

	/* send ring buffer */
	uint8_t  *sbuf;
	uint32_t  sbuf_una;         /* total bytes cumulatively acked (monotonic) */
	uint32_t  sbuf_snd;         /* total bytes sent (monotonic) */
	uint32_t  sbuf_wr;          /* total bytes written by app (monotonic) */

	struct utp_send_entry  *send_queue;   /* [SEND_QUEUE_SIZE] */
	struct utp_recv_entry  *recv_queue;   /* [RECV_QUEUE_SIZE] */

	mbt_conn_read_cb   readcb;
	mbt_conn_event_cb  eventcb;
	void              *cb_arg;

	struct mbt_ev_timer *rto_timer;
	struct mbt_ev_timer *destroy_timer;
	struct mbt_ev_timer *readcb_timer; /* deferred readcb notification */
	struct mbt_ev_timer *fin_timer;    /* deferred eventcb(0) after FIN before accept_cb */

	int          got_fin;   /* FIN received before eventcb was installed */
	int          in_hash;   /* 1 if inserted into ctx->buckets */

	SLIST_ENTRY(mbt_utp_sock) bucket_entry;
};

SLIST_HEAD(utp_sock_list, mbt_utp_sock);

/* ---- multiplexer ---- */
struct mbt_utp_ctx {
	struct mbt_ev      *ev;
	struct mbt_listener listener;

	mbt_utp_accept_cb accept_cb;
	void             *accept_arg;

	int (*tx_hook)(struct mbt_utp_ctx *ctx, const void *buf, size_t len,
	               const struct sockaddr *dst, socklen_t dstlen, void *arg);
	void *tx_hook_arg;

	struct utp_sock_list buckets[UTP_CONN_HASH_SIZE];
};

/* ---- helpers ---- */

static uint64_t
utp_now_us(void)
{
	struct timespec ts;
	clock_gettime(CLOCK_MONOTONIC, &ts);
	return (uint64_t)ts.tv_sec * 1000000 + (uint64_t)ts.tv_nsec / 1000;
}

static uint32_t
utp_now_us32(void)
{
	return (uint32_t)(utp_now_us() & 0xFFFFFFFF);
}

static struct mbt_utp_sock *
utp_ctx_find(struct mbt_utp_ctx *ctx, uint16_t recv_id)
{
	struct utp_sock_list *h = &ctx->buckets[recv_id & (UTP_CONN_HASH_SIZE - 1)];
	struct mbt_utp_sock *s;
	SLIST_FOREACH(s, h, bucket_entry)
		if (s->recv_conn_id == recv_id)
			return s;
	return NULL;
}

static void
utp_ctx_insert(struct mbt_utp_ctx *ctx, struct mbt_utp_sock *s)
{
	struct utp_sock_list *h = &ctx->buckets[s->recv_conn_id & (UTP_CONN_HASH_SIZE - 1)];
	SLIST_INSERT_HEAD(h, s, bucket_entry);
	s->in_hash = 1;
}

static void
utp_ctx_remove(struct mbt_utp_ctx *ctx, struct mbt_utp_sock *s)
{
	if (!s->in_hash) return;
	struct utp_sock_list *h = &ctx->buckets[s->recv_conn_id & (UTP_CONN_HASH_SIZE - 1)];
	SLIST_REMOVE(h, s, mbt_utp_sock, bucket_entry);
	s->in_hash = 0;
}

/* ---- rbuf helpers ---- */

static size_t
rbuf_available(struct mbt_utp_sock *s)
{
	return s->rbuf_wr - s->rbuf_rd;
}

static size_t
rbuf_space(struct mbt_utp_sock *s)
{
	return RBUF_CAP - rbuf_available(s);
}

/* Copy len bytes into rbuf starting at monotonic offset rbuf_wr */
static void
rbuf_produce(struct mbt_utp_sock *s, const void *data, size_t len)
{
	const uint8_t *src = data;
	size_t done = 0;
	while (done < len) {
		size_t idx = (s->rbuf_wr + done) & (RBUF_CAP - 1);
		size_t chunk = RBUF_CAP - idx;
		if (chunk > len - done) chunk = len - done;
		memcpy(s->rbuf + idx, src + done, chunk);
		done += chunk;
	}
	s->rbuf_wr += (uint32_t)len;
}

/* ---- mbt_conn ops ---- */

static void utp_deferred_read_cb(struct mbt_ev_timer *t, void *arg);

static void utp_deferred_fin_cb(struct mbt_ev_timer *t, void *arg);

static void
utp_schedule_fin_notify(struct mbt_utp_sock *s)
{
	if (!s->eventcb || s->fin_timer) return;
	mbt_ev_timer_new(s->ctx->ev, &s->fin_timer, 0, utp_deferred_fin_cb, s);
}

static void
utp_deferred_fin_cb(struct mbt_ev_timer *t, void *arg)
{
	struct mbt_utp_sock *s = arg;
	s->fin_timer = NULL;
	mbt_ev_timer_free(t);
	if (s->eventcb)
		s->eventcb(&s->conn, 0, s->cb_arg);
}

static void
utp_schedule_readcb(struct mbt_utp_sock *s)
{
	if (!s->readcb || rbuf_available(s) < s->rbuf_watermark)
		return;
	if (s->readcb_timer) return; /* already scheduled */
	mbt_ev_timer_new(s->ctx->ev, &s->readcb_timer, 0, utp_deferred_read_cb, s);
}

static void
utp_deferred_read_cb(struct mbt_ev_timer *t, void *arg)
{
	struct mbt_utp_sock *s = arg;
	s->readcb_timer = NULL;
	mbt_ev_timer_free(t);
	if (s->readcb && rbuf_available(s) >= s->rbuf_watermark)
		s->readcb(&s->conn, s->cb_arg);
}

static void
utp_conn_set_watermark(struct mbt_conn *c, size_t low)
{
	struct mbt_utp_sock *s = (struct mbt_utp_sock *)c;
	s->rbuf_watermark = low;
	utp_schedule_readcb(s);
}

static size_t
utp_conn_available(struct mbt_conn *c)
{
	return rbuf_available((struct mbt_utp_sock *)c);
}

static ssize_t
utp_conn_read(struct mbt_conn *c, void *buf, size_t len)
{
	struct mbt_utp_sock *s = (struct mbt_utp_sock *)c;
	size_t avail = rbuf_available(s);
	if (len > avail) len = avail;
	if (!len) return 0;
	uint8_t *dst = buf;
	size_t done = 0;
	while (done < len) {
		size_t idx = (s->rbuf_rd + done) & (RBUF_CAP - 1);
		size_t chunk = RBUF_CAP - idx;
		if (chunk > len - done) chunk = len - done;
		memcpy(dst + done, s->rbuf + idx, chunk);
		done += chunk;
	}
	s->rbuf_rd += (uint32_t)len;
	return (ssize_t)len;
}

static int utp_flush(struct mbt_utp_sock *s);

static ssize_t
utp_conn_write(struct mbt_conn *c, const void *buf, size_t len)
{
	struct mbt_utp_sock *s = (struct mbt_utp_sock *)c;
	if (s->state != UTP_CS_CONNECTED)
		return -ENOTCONN;
	uint32_t space = SBUF_CAP - (s->sbuf_wr - s->sbuf_una);
	if (len > space) len = space;
	if (!len) return 0;
	const uint8_t *src = buf;
	size_t done = 0;
	while (done < len) {
		size_t idx = (s->sbuf_wr + done) & (SBUF_CAP - 1);
		size_t chunk = SBUF_CAP - idx;
		if (chunk > len - done) chunk = len - done;
		memcpy(s->sbuf + idx, src + done, chunk);
		done += chunk;
	}
	s->sbuf_wr += (uint32_t)len;
	utp_flush(s);
	return (ssize_t)len;
}

static void utp_destroy_cb(struct mbt_ev_timer *t, void *arg);
static void utp_rto_cb(struct mbt_ev_timer *t, void *arg);

static void
utp_sock_real_free(struct mbt_utp_sock *s)
{
	if (s->rto_timer) {
		mbt_ev_timer_free(s->rto_timer);
		s->rto_timer = NULL;
	}
	if (s->destroy_timer) {
		mbt_ev_timer_free(s->destroy_timer);
		s->destroy_timer = NULL;
	}
	if (s->readcb_timer) {
		mbt_ev_timer_free(s->readcb_timer);
		s->readcb_timer = NULL;
	}
	if (s->fin_timer) {
		mbt_ev_timer_free(s->fin_timer);
		s->fin_timer = NULL;
	}
	free(s->rbuf);
	free(s->sbuf);
	free(s->send_queue);
	free(s->recv_queue);
	free(s);
}

static void
utp_conn_free(struct mbt_conn *c)
{
	struct mbt_utp_sock *s = (struct mbt_utp_sock *)c;
	if (!s) return;

	if (s->state == UTP_CS_CONNECTED || s->state == UTP_CS_SYN_RECV) {
		/* Send FIN and wait for remote to ack */
		s->readcb  = NULL;
		s->eventcb = NULL;
		s->cb_arg  = NULL;
		/* Send FIN: uses current seq_nr */
		uint8_t buf[UTP_HEADER_SIZE];
		struct utp_hdr *h = (struct utp_hdr *)buf;
		h->type_ver          = (uint8_t)((ST_FIN << 4) | UTP_VERSION);
		h->extension         = 0;
		h->conn_id           = htons(s->send_conn_id);
		h->timestamp_us      = htonl(utp_now_us32());
		h->timestamp_diff_us = htonl(s->reply_micro);
		h->wnd_size          = 0;
		h->seq_nr            = htons(s->seq_nr);
		h->ack_nr            = htons(s->ack_nr);

		int fd = (s->peer_addr.ss_family == AF_INET6)
		       ? s->ctx->listener.fd6 : s->ctx->listener.fd4;
		if (s->ctx->tx_hook) {
			int r = s->ctx->tx_hook(s->ctx, buf, UTP_HEADER_SIZE,
			    (struct sockaddr *)&s->peer_addr, s->peer_addrlen,
			    s->ctx->tx_hook_arg);
			if (r == 0) goto arm_timer;
		}
		sendto(fd, buf, UTP_HEADER_SIZE, 0,
		       (struct sockaddr *)&s->peer_addr, s->peer_addrlen);
arm_timer:
		s->state = UTP_CS_FIN_SENT;
		if (!s->destroy_timer)
			mbt_ev_timer_new(s->ctx->ev, &s->destroy_timer,
			                 (uint64_t)UTP_FIN_WAIT_MS * 1000,
			                 utp_destroy_cb, s);
		return;
	}

	if (s->state == UTP_CS_FIN_SENT) {
		/* Already waiting; nothing to do -- destroy_timer will free */
		return;
	}

	/* Remove from ctx hash table if still registered */
	if (s->ctx)
		utp_ctx_remove(s->ctx, s);
	s->state = UTP_CS_DESTROY;
	utp_sock_real_free(s);
}

static void
utp_destroy_cb(struct mbt_ev_timer *t, void *arg)
{
	(void)t;
	struct mbt_utp_sock *s = arg;
	s->destroy_timer = NULL;
	if (s->ctx)
		utp_ctx_remove(s->ctx, s);
	s->state = UTP_CS_DESTROY;
	utp_sock_real_free(s);
}

static void
utp_conn_set_callbacks(struct mbt_conn *c, mbt_conn_read_cb readcb,
                       mbt_conn_event_cb eventcb, void *arg)
{
	struct mbt_utp_sock *s = (struct mbt_utp_sock *)c;
	s->readcb   = readcb;
	s->eventcb  = eventcb;
	s->cb_arg   = arg;
	/* Schedule deferred readcb if data already arrived */
	utp_schedule_readcb(s);
	/* Schedule deferred eventcb(0) if FIN arrived before callbacks were set */
	if (s->got_fin)
		utp_schedule_fin_notify(s);
}

static int
utp_conn_peer_addr(struct mbt_conn *c, char *buf, size_t buflen, uint16_t *port)
{
	struct mbt_utp_sock *s = (struct mbt_utp_sock *)c;
	uint16_t p = 0;
	mbt_sockaddr_split(&s->peer_addr, buf, buflen, &p);
	if (port) *port = p;
	return 0;
}

static const struct mbt_conn_ops utp_conn_ops = {
	.set_watermark = utp_conn_set_watermark,
	.available     = utp_conn_available,
	.read          = utp_conn_read,
	.write         = utp_conn_write,
	.set_callbacks = utp_conn_set_callbacks,
	.peer_addr     = utp_conn_peer_addr,
	.free          = utp_conn_free,
};

/* ---- socket allocation ---- */

static struct mbt_utp_sock *
utp_sock_new(struct mbt_utp_ctx *ctx,
             const struct sockaddr *addr, socklen_t addrlen,
             mbt_conn_read_cb readcb, mbt_conn_event_cb eventcb, void *arg)
{
	struct mbt_utp_sock *s = calloc(1, sizeof(*s));
	if (!s) return NULL;

	s->conn.ops   = &utp_conn_ops;
	s->ctx        = ctx;
	s->readcb     = readcb;
	s->eventcb    = eventcb;
	s->cb_arg     = arg;
	s->max_window = UTP_INIT_CWND;
	s->peer_wnd   = UTP_INIT_CWND;
	s->timeout_ms = UTP_INIT_TIMEOUT_MS;
	s->rbuf_watermark = 1;

	memcpy(&s->peer_addr, addr, addrlen);
	s->peer_addrlen = addrlen;

	s->rbuf       = malloc(RBUF_CAP);
	s->sbuf       = malloc(SBUF_CAP);
	s->send_queue = calloc(SEND_QUEUE_SIZE, sizeof(*s->send_queue));
	s->recv_queue = calloc(RECV_QUEUE_SIZE, sizeof(*s->recv_queue));
	if (!s->rbuf || !s->sbuf || !s->send_queue || !s->recv_queue) {
		free(s->rbuf);
		free(s->sbuf);
		free(s->send_queue);
		free(s->recv_queue);
		free(s);
		return NULL;
	}

	return s;
}

/* ---- wire send ---- */

static int
utp_do_sendto(struct mbt_utp_sock *s, const void *buf, size_t len)
{
	struct mbt_utp_ctx *ctx = s->ctx;

	if (ctx->tx_hook) {
		int r = ctx->tx_hook(ctx, buf, len,
		    (struct sockaddr *)&s->peer_addr, s->peer_addrlen,
		    ctx->tx_hook_arg);
		if (r == 0) return 0;
	}

	int fd = (s->peer_addr.ss_family == AF_INET6)
	       ? ctx->listener.fd6 : ctx->listener.fd4;

	ssize_t n = sendto(fd, buf, len, 0,
	                   (struct sockaddr *)&s->peer_addr, s->peer_addrlen);
	return n < 0 ? -errno : 0;
}

/* Build SACK extension for recv_queue gaps */
static size_t
utp_build_sack(struct mbt_utp_sock *s, uint8_t *ext_buf, size_t ext_cap)
{
	/* bits: bit i = recv_queue has ack_nr+2+i */
	uint8_t bits[4] = {0, 0, 0, 0};
	int any = 0;
	for (int i = 0; i < 32; i++) {
		uint16_t seq = (uint16_t)(s->ack_nr + 2 + i);
		struct utp_recv_entry *e = &s->recv_queue[seq & RECV_QUEUE_MASK];
		if (e->valid && e->seq_nr == seq) {
			bits[i / 8] |= (uint8_t)(1 << (i % 8));
			any = 1;
		}
	}
	if (!any) return 0;
	if (ext_cap < 6) return 0;
	ext_buf[0] = 0;    /* next extension = none */
	ext_buf[1] = 4;    /* length of bitmask */
	ext_buf[2] = bits[0];
	ext_buf[3] = bits[1];
	ext_buf[4] = bits[2];
	ext_buf[5] = bits[3];
	return 6;
}

static int
utp_send_ack(struct mbt_utp_sock *s)
{
	uint8_t buf[UTP_HEADER_SIZE + 6];
	uint8_t ext_buf[6];
	size_t ext_len = utp_build_sack(s, ext_buf, sizeof(ext_buf));

	struct utp_hdr *h = (struct utp_hdr *)buf;
	h->type_ver          = (uint8_t)((ST_STATE << 4) | UTP_VERSION);
	h->extension         = ext_len ? 1 : 0;
	h->conn_id           = htons(s->send_conn_id);
	h->timestamp_us      = htonl(utp_now_us32());
	h->timestamp_diff_us = htonl(s->reply_micro);
	h->wnd_size          = htonl((uint32_t)rbuf_space(s));
	h->seq_nr            = htons(s->seq_nr);
	h->ack_nr            = htons(s->ack_nr);

	size_t total = UTP_HEADER_SIZE;
	if (ext_len) {
		memcpy(buf + UTP_HEADER_SIZE, ext_buf, ext_len);
		total += ext_len;
	}

	return utp_do_sendto(s, buf, total);
}

static int
utp_send_data(struct mbt_utp_sock *s, uint16_t seq,
              const void *payload, size_t payload_len)
{
	uint8_t buf[UTP_HEADER_SIZE + UTP_MAX_PACKET_SIZE];
	struct utp_hdr *h = (struct utp_hdr *)buf;

	if (payload_len > UTP_MAX_PACKET_SIZE)
		payload_len = UTP_MAX_PACKET_SIZE;

	h->type_ver          = (uint8_t)((ST_DATA << 4) | UTP_VERSION);
	h->extension         = 0;
	h->conn_id           = htons(s->send_conn_id);
	h->timestamp_us      = htonl(utp_now_us32());
	h->timestamp_diff_us = htonl(s->reply_micro);
	h->wnd_size          = htonl((uint32_t)rbuf_space(s));
	h->seq_nr            = htons(seq);
	h->ack_nr            = htons(s->ack_nr);

	memcpy(buf + UTP_HEADER_SIZE, payload, payload_len);

	return utp_do_sendto(s, buf, UTP_HEADER_SIZE + payload_len);
}

/* Extract payload bytes from sbuf ring starting at absolute offset off */
static void
sbuf_copy_out(struct mbt_utp_sock *s, uint32_t off, void *dst, size_t len)
{
	uint8_t *d = dst;
	size_t done = 0;
	while (done < len) {
		size_t idx = (off + done) & (SBUF_CAP - 1);
		size_t chunk = SBUF_CAP - idx;
		if (chunk > len - done) chunk = len - done;
		memcpy(d + done, s->sbuf + idx, chunk);
		done += chunk;
	}
}

/* Push buffered send data within window constraints */
static int
utp_flush(struct mbt_utp_sock *s)
{
	if (s->state != UTP_CS_CONNECTED)
		return 0;

	for (;;) {
		if (s->sbuf_wr <= s->sbuf_snd)
			break;

		uint32_t window = s->max_window < s->peer_wnd
		                ? s->max_window : s->peer_wnd;
		if (window < UTP_MIN_PACKET_SIZE) window = UTP_MIN_PACKET_SIZE;

		uint32_t in_flight = s->sbuf_snd - s->sbuf_una;
		if (in_flight >= window)
			break;

		/* Check send queue slot available */
		struct utp_send_entry *e = &s->send_queue[s->seq_nr & SEND_QUEUE_MASK];
		if (e->valid)
			break;  /* queue full */

		uint32_t avail = s->sbuf_wr - s->sbuf_snd;
		uint32_t space = window - in_flight;
		uint32_t chunk = avail < space ? avail : space;
		if (chunk > UTP_MAX_PACKET_SIZE) chunk = UTP_MAX_PACKET_SIZE;

		/* Gather payload */
		uint8_t payload[UTP_MAX_PACKET_SIZE];
		sbuf_copy_out(s, s->sbuf_snd, payload, chunk);

		/* Record in send queue before sending; advance seq_nr/sbuf_snd
		 * first so that any synchronous ACK arriving inside utp_send_data
		 * (e.g. from a TX hook) can advance send_una past this packet. */
		uint16_t cur_seq = s->seq_nr;
		e->seq_nr      = cur_seq;
		e->sbuf_off    = s->sbuf_snd;
		e->len         = (uint16_t)chunk;
		e->sent_us     = utp_now_us();
		e->retransmits = 0;
		e->sacked      = 0;
		e->valid       = 1;

		s->seq_nr++;
		s->sbuf_snd += chunk;

		utp_send_data(s, cur_seq, payload, chunk);
	}
	return 0;
}

/* ---- congestion / RTT ---- */

static void
utp_update_rtt(struct mbt_utp_sock *s, int32_t sample_us)
{
	if (s->rtt_us == 0) {
		s->rtt_us     = sample_us;
		s->rtt_var_us = sample_us / 2;
	} else {
		int32_t delta  = s->rtt_us - sample_us;
		s->rtt_var_us += (abs(delta) - s->rtt_var_us) / 4;
		s->rtt_us     += (sample_us - s->rtt_us) / 8;
	}
	int32_t to = (s->rtt_us + s->rtt_var_us * 4) / 1000;
	s->timeout_ms = to < 500 ? 500 : to;
}

static void
utp_ledbat_update(struct mbt_utp_sock *s, uint32_t their_delay_us)
{
	uint64_t now = utp_now_us();
	if (s->base_delay_us == 0 || their_delay_us < s->base_delay_us ||
	    now - s->base_delay_ts_us > (uint64_t)UTP_BASE_DELAY_HIST * 1000000) {
		s->base_delay_us    = their_delay_us;
		s->base_delay_ts_us = now;
	}
	int32_t our_delay  = (int32_t)(their_delay_us - s->base_delay_us);
	if (our_delay < 0) our_delay = 0;
	int32_t off_target = UTP_CCONTROL_TARGET - our_delay;

	if (!s->max_window) return;

	uint32_t in_flight = s->sbuf_snd - s->sbuf_una;
	double gain = (double)UTP_MAX_CWND_INC
	            * ((double)off_target / UTP_CCONTROL_TARGET)
	            * ((double)in_flight / s->max_window);

	int32_t nw = (int32_t)s->max_window + (int32_t)gain;
	s->max_window = nw < UTP_MIN_PACKET_SIZE ? UTP_MIN_PACKET_SIZE : (uint32_t)nw;
}

/* ---- retransmit timer ---- */

static void
utp_arm_rto(struct mbt_utp_sock *s)
{
	uint64_t usec = (uint64_t)s->timeout_ms * 1000;
	if (!s->rto_timer)
		mbt_ev_timer_new(s->ctx->ev, &s->rto_timer, usec, utp_rto_cb, s);
	else
		mbt_ev_timer_mod(s->rto_timer, usec);
}

static void
utp_disarm_rto(struct mbt_utp_sock *s)
{
	if (s->rto_timer) {
		mbt_ev_timer_free(s->rto_timer);
		s->rto_timer = NULL;
	}
}

/* Retransmit the packet at send_una */
static void
utp_retransmit(struct mbt_utp_sock *s)
{
	struct utp_send_entry *e = &s->send_queue[s->send_una & SEND_QUEUE_MASK];
	if (!e->valid || e->seq_nr != s->send_una)
		return;

	uint8_t payload[UTP_MAX_PACKET_SIZE];
	sbuf_copy_out(s, e->sbuf_off, payload, e->len);
	utp_send_data(s, e->seq_nr, payload, e->len);
	e->retransmits++;
	e->sent_us = utp_now_us();
}

static void
utp_rto_cb(struct mbt_ev_timer *t, void *arg)
{
	(void)t;
	struct mbt_utp_sock *s = arg;

	if (s->state == UTP_CS_SYN_SENT) {
		/* retransmit SYN */
		uint8_t buf[UTP_HEADER_SIZE];
		struct utp_hdr *h = (struct utp_hdr *)buf;
		h->type_ver          = (uint8_t)((ST_SYN << 4) | UTP_VERSION);
		h->extension         = 0;
		h->conn_id           = htons(s->recv_conn_id);
		h->timestamp_us      = htonl(utp_now_us32());
		h->timestamp_diff_us = 0;
		h->wnd_size          = htonl((uint32_t)RBUF_CAP);
		h->seq_nr            = htons(1);
		h->ack_nr            = 0;
		utp_do_sendto(s, buf, UTP_HEADER_SIZE);
		utp_arm_rto(s);
		return;
	}

	if (s->state == UTP_CS_CONNECTED &&
	    s->send_una != s->seq_nr) {
		/* Halve window on loss. */
		s->max_window /= 2;
		if (s->max_window < UTP_MIN_PACKET_SIZE)
			s->max_window = UTP_MIN_PACKET_SIZE;

		/*
		 * Retransmit the unacked window in place. seq numbers are
		 * immutable once assigned: the receiver buffers out-of-order
		 * packets and reports them via SACK, so it assumes a given
		 * seq always carries the same bytes. The previous code rewound
		 * seq_nr/sbuf_snd to send_una and re-flushed from scratch,
		 * which re-segmented the stream against the just-halved window
		 * and reassigned different byte ranges to already-sent (and
		 * possibly already-SACKed) seq numbers. That desynced the
		 * receiver -- a block arrived with the right length but wrong
		 * bytes, then the next length prefix landed mid-payload. Resend
		 * each still-unacked entry with its original seq and sbuf_off
		 * instead, and leave seq_nr, sbuf_snd and the queue intact so
		 * SACK bookkeeping stays valid.
		 */
		for (uint16_t seq = s->send_una;
		     (int16_t)(seq - s->seq_nr) < 0;
		     seq++) {
			struct utp_send_entry *e =
			    &s->send_queue[seq & SEND_QUEUE_MASK];
			if (!e->valid || e->seq_nr != seq || e->sacked)
				continue;
			uint8_t payload[UTP_MAX_PACKET_SIZE];
			sbuf_copy_out(s, e->sbuf_off, payload, e->len);
			utp_send_data(s, e->seq_nr, payload, e->len);
			e->retransmits++;
			e->sent_us = utp_now_us();
		}
		utp_arm_rto(s);
	}
}

/* ---- ACK processing ---- */

static void
utp_process_ack(struct mbt_utp_sock *s, uint16_t peer_ack_nr,
                uint32_t their_delay_us,
                const uint8_t *sack_data, size_t sack_len)
{
	/* Dup-ack detection */
	if (peer_ack_nr == (uint16_t)(s->send_una - 1)) {
		s->dup_acks++;
		if (s->dup_acks >= UTP_REORDER_THRESH) {
			s->max_window = s->max_window / 2;
			if (s->max_window < UTP_MIN_PACKET_SIZE)
				s->max_window = UTP_MIN_PACKET_SIZE;
			utp_retransmit(s);
			utp_arm_rto(s);
		}
		return;
	}

	/* Only accept if peer_ack_nr is ahead of (or equal to) send_una - 1 */
	if ((int16_t)(peer_ack_nr - (uint16_t)(s->send_una - 1)) <= 0)
		return;

	s->dup_acks = 0;

	uint64_t now = utp_now_us();

	/* Mark cumulative acks */
	for (uint16_t seq = s->send_una;
	     (int16_t)(seq - (uint16_t)(peer_ack_nr + 1)) < 0;
	     seq++) {
		struct utp_send_entry *e = &s->send_queue[seq & SEND_QUEUE_MASK];
		if (e->valid && e->seq_nr == seq && !e->sacked) {
			if (e->retransmits == 0) {
				int32_t sample = (int32_t)(now - e->sent_us);
				if (sample > 0)
					utp_update_rtt(s, sample);
			}
			e->sacked = 1;
		}
	}

	/* Process SACK bitmask: bit i -> seq peer_ack_nr+2+i */
	for (size_t bi = 0; bi < sack_len && bi < 4; bi++) {
		uint8_t byte = sack_data[bi];
		for (int bit = 0; bit < 8; bit++) {
			if (byte & (1 << bit)) {
				uint16_t seq = (uint16_t)(peer_ack_nr + 2 + bi * 8 + bit);
				struct utp_send_entry *e = &s->send_queue[seq & SEND_QUEUE_MASK];
				if (e->valid && e->seq_nr == seq)
					e->sacked = 1;
			}
		}
	}

	/* Advance send_una */
	while ((int16_t)(s->send_una - s->seq_nr) < 0) {
		struct utp_send_entry *e = &s->send_queue[s->send_una & SEND_QUEUE_MASK];
		if (!e->valid || e->seq_nr != s->send_una || !e->sacked)
			break;
		s->sbuf_una = e->sbuf_off + e->len;
		e->valid = 0;
		s->send_una++;
	}

	/* Fast retransmit: sacked packets ahead of new send_una */
	int sacked_ahead = 0;
	for (uint16_t seq = (uint16_t)(s->send_una + 1);
	     (int16_t)(seq - s->seq_nr) < 0;
	     seq++) {
		struct utp_send_entry *e = &s->send_queue[seq & SEND_QUEUE_MASK];
		if (e->valid && e->seq_nr == seq && e->sacked)
			sacked_ahead++;
	}
	if (sacked_ahead >= UTP_REORDER_THRESH) {
		s->max_window = s->max_window / 2;
		if (s->max_window < UTP_MIN_PACKET_SIZE)
			s->max_window = UTP_MIN_PACKET_SIZE;
		utp_retransmit(s);
		utp_arm_rto(s);
	}

	/* LEDBAT */
	if (their_delay_us)
		utp_ledbat_update(s, their_delay_us);

	/* Rearm or disarm RTO */
	if (s->send_una != s->seq_nr)
		utp_arm_rto(s);
	else
		utp_disarm_rto(s);

	utp_flush(s);
}

/* ---- incoming packet handlers ---- */

static void
utp_handle_syn(struct mbt_utp_ctx *ctx,
               const struct utp_hdr *h,
               const struct sockaddr *src, socklen_t srclen)
{
	uint16_t recv_id = (uint16_t)(ntohs(h->conn_id) + 1);

	/* Silently drop if conn_id collides */
	if (utp_ctx_find(ctx, recv_id))
		return;

	struct mbt_utp_sock *s = utp_sock_new(ctx, src, srclen, NULL, NULL, NULL);
	if (!s) return;

	s->state        = UTP_CS_SYN_RECV;
	s->recv_conn_id = recv_id;
	s->send_conn_id = ntohs(h->conn_id);
	s->ack_nr       = ntohs(h->seq_nr);
	s->seq_nr       = (uint16_t)(arc4random() & 0xFFFF);
	s->send_una     = s->seq_nr;
	s->reply_micro  = utp_now_us32() - ntohl(h->timestamp_us);
	s->peer_wnd     = ntohl(h->wnd_size);

	utp_ctx_insert(ctx, s);

	/* Send SYN-ACK (STATE packet) */
	if (utp_send_ack(s) < 0) {
		utp_ctx_remove(ctx, s);
		utp_sock_real_free(s);
		return;
	}
	s->seq_nr++; /* SYN-ACK advances seq_nr per BEP 29 */
	s->send_una = s->seq_nr;

	s->state = UTP_CS_CONNECTED;

	if (ctx->accept_cb)
		ctx->accept_cb(ctx, &s->conn, src, srclen, ctx->accept_arg);
}

/* Deliver data to rbuf and cascade recv_queue */
static void
utp_deliver_data(struct mbt_utp_sock *s, const void *data, size_t len, uint16_t seq)
{
	if (len > rbuf_space(s)) return;  /* rbuf full, drop */
	rbuf_produce(s, data, len);
}

static void
utp_handle_data(struct mbt_utp_sock *s, const struct utp_hdr *h,
                const void *payload, size_t plen)
{
	uint16_t pkt_seq = ntohs(h->seq_nr);

	s->reply_micro = utp_now_us32() - ntohl(h->timestamp_us);
	s->peer_wnd    = ntohl(h->wnd_size);

	/* Process ACK embedded in data packet */
	uint32_t their_delay = ntohl(h->timestamp_diff_us);
	if (their_delay)
		utp_ledbat_update(s, their_delay);
	utp_process_ack(s, ntohs(h->ack_nr), their_delay, NULL, 0);

	/* Duplicate: already delivered */
	if ((int16_t)(pkt_seq - s->ack_nr) <= 0) {
		utp_send_ack(s);
		return;
	}

	if (pkt_seq == (uint16_t)(s->ack_nr + 1)) {
		/* In-order: deliver */
		utp_deliver_data(s, payload, plen, pkt_seq);
		s->ack_nr++;

		/* Cascade from recv_queue */
		for (;;) {
			uint16_t next = (uint16_t)(s->ack_nr + 1);
			struct utp_recv_entry *e = &s->recv_queue[next & RECV_QUEUE_MASK];
			if (!e->valid || e->seq_nr != next)
				break;
			utp_deliver_data(s, e->data, e->len, next);
			e->valid = 0;
			s->ack_nr++;
		}

		if (s->readcb && rbuf_available(s) >= s->rbuf_watermark)
			s->readcb(&s->conn, s->cb_arg);
	} else {
		/* Out of order: buffer if slot free */
		struct utp_recv_entry *e = &s->recv_queue[pkt_seq & RECV_QUEUE_MASK];
		if (!e->valid) {
			e->valid  = 1;
			e->seq_nr = pkt_seq;
			e->len    = (uint16_t)(plen > UTP_MAX_PACKET_SIZE
			                     ? UTP_MAX_PACKET_SIZE : plen);
			memcpy(e->data, payload, e->len);
		}
	}

	utp_send_ack(s);
}

static void
utp_handle_state(struct mbt_utp_sock *s, const struct utp_hdr *h,
                 const uint8_t *sack_data, size_t sack_len)
{
	s->reply_micro = utp_now_us32() - ntohl(h->timestamp_us);
	s->peer_wnd    = ntohl(h->wnd_size);

	if (s->state == UTP_CS_SYN_SENT) {
		s->ack_nr   = ntohs(h->seq_nr);
		s->state    = UTP_CS_CONNECTED;
		utp_disarm_rto(s);
		if (s->eventcb)
			s->eventcb(&s->conn, 0, s->cb_arg);
		utp_flush(s);
		return;
	}

	uint32_t their_delay = ntohl(h->timestamp_diff_us);
	utp_process_ack(s, ntohs(h->ack_nr), their_delay, sack_data, sack_len);
}

static void
utp_handle_fin(struct mbt_utp_sock *s, const struct utp_hdr *h)
{
	uint16_t fin_seq = ntohs(h->seq_nr);
	s->reply_micro = utp_now_us32() - ntohl(h->timestamp_us);
	s->peer_wnd    = ntohl(h->wnd_size);

	/* FIN occupies a seq_nr; treat ack_nr = fin_seq to allow ack */
	if ((int16_t)(fin_seq - s->ack_nr) > 0)
		s->ack_nr = fin_seq;

	/* Send final STATE ack */
	utp_send_ack(s);

	if (s->state == UTP_CS_FIN_SENT || s->state == UTP_CS_DESTROY)
		return;

	s->state = UTP_CS_DESTROY;
	if (s->eventcb)
		s->eventcb(&s->conn, 0, s->cb_arg); /* EOF */
	else {
		/* eventcb not installed yet (FIN arrived before accept_cb);
		 * set got_fin so set_callbacks can schedule a deferred notify. */
		s->got_fin = 1;
	}
}

static void
utp_handle_reset(struct mbt_utp_sock *s)
{
	if (s->state == UTP_CS_DESTROY || s->state == UTP_CS_FIN_SENT)
		return;
	s->state = UTP_CS_DESTROY;
	if (s->eventcb)
		s->eventcb(&s->conn, -ECONNRESET, s->cb_arg);
}

/* ---- UDP receive callback (registered with mbt_listener) ---- */

static void
utp_udp_recv(struct mbt_ev_io *io, int fd, uint32_t events, void *arg)
{
	(void)io; (void)events; (void)fd;
	struct mbt_utp_ctx *ctx = arg;

	uint8_t buf[65536];
	struct sockaddr_storage src;
	socklen_t srclen = sizeof(src);

	ssize_t n = recvfrom(fd, buf, sizeof(buf), 0,
	                     (struct sockaddr *)&src, &srclen);
	if (n <= 0) return;

	mbt_utp_recv(ctx, buf, (size_t)n, (struct sockaddr *)&src, srclen);
}

/* ---- public API ---- */

int
mbt_utp_ctx_new(struct mbt_utp_ctx **out, struct mbt_ev *ev,
                uint16_t port,
                mbt_utp_accept_cb accept_cb, void *accept_arg)
{
	struct mbt_utp_ctx *ctx = calloc(1, sizeof(*ctx));
	if (!ctx) return -ENOMEM;

	ctx->ev         = ev;
	ctx->accept_cb  = accept_cb;
	ctx->accept_arg = accept_arg;

	int rv = mbt_listen_udp(&ctx->listener, ev, port, utp_udp_recv, ctx);
	if (rv < 0) {
		free(ctx);
		return rv;
	}

	*out = ctx;
	return 0;
}

void
mbt_utp_ctx_free(struct mbt_utp_ctx *ctx)
{
	if (!ctx) return;
	for (int i = 0; i < UTP_CONN_HASH_SIZE; i++) {
		struct mbt_utp_sock *s;
		while ((s = SLIST_FIRST(&ctx->buckets[i])) != NULL) {
			SLIST_REMOVE_HEAD(&ctx->buckets[i], bucket_entry);
			s->in_hash = 0;
			s->state = UTP_CS_DESTROY;
			utp_sock_real_free(s);
		}
	}
	mbt_listener_close(&ctx->listener);
	free(ctx);
}

uint16_t
mbt_utp_ctx_port(struct mbt_utp_ctx *ctx)
{
	return ctx->listener.port;
}

int
mbt_utp_ctx_fd(struct mbt_utp_ctx *ctx)
{
	return ctx->listener.fd4;
}

void
mbt_utp_ctx_set_tx_hook(struct mbt_utp_ctx *ctx,
    int (*hook)(struct mbt_utp_ctx *ctx, const void *buf, size_t len,
                const struct sockaddr *dst, socklen_t dstlen, void *arg),
    void *arg)
{
	ctx->tx_hook     = hook;
	ctx->tx_hook_arg = arg;
}

int
mbt_utp_recv(struct mbt_utp_ctx *ctx,
             const void *buf, size_t len,
             const struct sockaddr *src, socklen_t srclen)
{
	if (len < UTP_HEADER_SIZE)
		return -1;

	const struct utp_hdr *h = buf;
	uint8_t type = h->type_ver >> 4;
	uint8_t ver  = h->type_ver & 0x0f;

	if (ver != UTP_VERSION)
		return -1;

	/* Parse extension headers; extract SACK if present */
	size_t off = UTP_HEADER_SIZE;
	uint8_t ext = h->extension;
	const uint8_t *sack_data = NULL;
	size_t sack_len = 0;

	while (ext != 0) {
		if (off + 2 > len) return -1;
		const uint8_t *ep = (const uint8_t *)buf + off;
		uint8_t next_ext = ep[0];
		uint8_t ext_size = ep[1];
		if (off + 2 + ext_size > len) return -1;
		if (ext == 1) {
			/* SACK bitmask */
			sack_data = ep + 2;
			sack_len  = ext_size;
		}
		ext  = next_ext;
		off += 2 + ext_size;
	}

	const void *payload     = (const uint8_t *)buf + off;
	size_t      payload_len = len - off;

	if (type == ST_SYN) {
		utp_handle_syn(ctx, h, src, srclen);
		return 0;
	}

	uint16_t conn_id = ntohs(h->conn_id);
	struct mbt_utp_sock *s = utp_ctx_find(ctx, conn_id);
	if (!s) return -1;

	switch (type) {
	case ST_DATA:
		utp_handle_data(s, h, payload, payload_len);
		break;
	case ST_STATE:
		utp_handle_state(s, h, sack_data, sack_len);
		break;
	case ST_FIN:
		utp_handle_fin(s, h);
		break;
	case ST_RESET:
		utp_handle_reset(s);
		break;
	default:
		return -1;
	}
	return 0;
}

int
mbt_utp_connect(struct mbt_utp_ctx *ctx,
                struct mbt_conn **out,
                const struct sockaddr *addr, socklen_t addrlen,
                mbt_conn_read_cb readcb,
                mbt_conn_event_cb eventcb,
                void *arg)
{
	struct mbt_utp_sock *s = utp_sock_new(ctx, addr, addrlen, readcb, eventcb, arg);
	if (!s) return -ENOMEM;

	/* Pick recv_conn_id not already in use */
	uint16_t cid;
	do { cid = (uint16_t)(arc4random() & 0xFFFF); }
	while (utp_ctx_find(ctx, cid));

	s->recv_conn_id = cid;
	s->send_conn_id = (uint16_t)(cid + 1);
	s->seq_nr       = 1;
	s->send_una     = 1;
	s->state        = UTP_CS_SYN_SENT;

	utp_ctx_insert(ctx, s);

	/* SYN: send with recv_conn_id, seq=1 */
	uint8_t buf[UTP_HEADER_SIZE];
	struct utp_hdr *h = (struct utp_hdr *)buf;
	h->type_ver          = (uint8_t)((ST_SYN << 4) | UTP_VERSION);
	h->extension         = 0;
	h->conn_id           = htons(s->recv_conn_id);
	h->timestamp_us      = htonl(utp_now_us32());
	h->timestamp_diff_us = 0;
	h->wnd_size          = htonl((uint32_t)RBUF_CAP);
	h->seq_nr            = htons(1);
	h->ack_nr            = 0;

	/*
	 * Advance seq_nr/send_una to 2 BEFORE calling utp_do_sendto.
	 * The TX hook may deliver the SYN synchronously, triggering the full
	 * handshake chain.  If client_eventcb fires and calls mbt_conn_write
	 * -> utp_flush before we return here, data must use seq >= 2, not 1.
	 */
	s->seq_nr   = 2;
	s->send_una = 2;

	int rv = utp_do_sendto(s, buf, UTP_HEADER_SIZE);
	if (rv < 0) {
		utp_ctx_remove(ctx, s);
		utp_sock_real_free(s);
		return rv;
	}

	utp_arm_rto(s);

	*out = &s->conn;
	return 0;
}
