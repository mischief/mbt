// SPDX-License-Identifier: ISC
/*
 * uTP (BEP 29) — uTorrent Transport Protocol
 *
 * Reliable ordered stream over UDP with LEDBAT congestion control.
 * Each uTP connection implements mbt_conn_ops so the peer layer is unchanged.
 *
 * mbt_utp_ctx  — one per session; owns the UDP listener; demultiplexes by conn_id
 * mbt_utp_sock — one per connection; implements mbt_conn_ops
 *
 * Wire header (20 bytes, big-endian):
 *   type_ver(8) extension(8) conn_id(16) timestamp_us(32)
 *   timestamp_diff_us(32) wnd_size(32) seq_nr(16) ack_nr(16)
 *
 * Connection ID:
 *   initiator: recv_conn_id=rand, send_conn_id=recv_conn_id+1
 *   SYN carries recv_conn_id; subsequent outbound use send_conn_id
 *   acceptor:  recv_conn_id=SYN.conn_id+1, send_conn_id=SYN.conn_id
 *
 * Congestion control (LEDBAT, simplified):
 *   base_delay  — sliding minimum of timestamp_diff_us over 2 minutes
 *   our_delay   = timestamp_diff_us - base_delay
 *   off_target  = CCONTROL_TARGET(100ms) - our_delay
 *   max_window += MAX_CWND_INC * (off_target/TARGET) * (in_flight/max_window)
 *   On 3× dup-ack loss: max_window *= 0.5
 */

#include "mbt/utp.h"
#include "mbt/event.h"
#include "mbt/log.h"
#include "net.h"

#include <stdlib.h>
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
#define UTP_CCONTROL_TARGET  100000   /* µs target one-way delay */
#define UTP_MAX_CWND_INC     3000     /* bytes/RTT max window increase */
#define UTP_REORDER_THRESH   3
#define UTP_BASE_DELAY_HIST  120      /* seconds */
#define UTP_RECV_BUF         (256 * 1024)
#define UTP_SEND_BUF         (256 * 1024)
#define UTP_CONN_HASH_BITS   6
#define UTP_CONN_HASH_SIZE   (1 << UTP_CONN_HASH_BITS)

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

/* ---- one uTP stream ---- */
struct mbt_utp_sock {
	struct mbt_conn       conn;       /* MUST be first */
	struct mbt_utp_ctx   *ctx;

	enum utp_cs   state;
	uint16_t      recv_conn_id;
	uint16_t      send_conn_id;

	uint16_t      seq_nr;            /* next seq to send */
	uint16_t      ack_nr;            /* last seq received in order */

	/* congestion / flow control */
	uint32_t      max_window;
	uint32_t      peer_wnd;
	uint32_t      cur_window;        /* bytes in-flight (unacked sent data) */

	/* RTT */
	int32_t       rtt_us;
	int32_t       rtt_var_us;
	int32_t       timeout_ms;

	/* LEDBAT */
	uint32_t      reply_micro;       /* last received timestamp_diff to echo back */
	uint32_t      base_delay_us;
	uint64_t      base_delay_ts_us;

	/* peer address */
	struct sockaddr_storage peer_addr;
	socklen_t               peer_addrlen;

	/* receive buffer — in-order assembled bytes */
	uint8_t  *rbuf;
	size_t    rbuf_len;
	size_t    rbuf_cap;
	size_t    rbuf_watermark;

	/* send buffer — bytes waiting to be sent or acked */
	uint8_t  *sbuf;
	size_t    sbuf_len;
	size_t    sbuf_cap;
	size_t    sbuf_sent;             /* bytes sent but not yet acked */

	mbt_conn_read_cb  readcb;
	mbt_conn_event_cb eventcb;
	void             *cb_arg;

	struct mbt_ev_timer *rto_timer;

	SLIST_ENTRY(mbt_utp_sock) bucket_entry;
};

SLIST_HEAD(utp_sock_list, mbt_utp_sock);

/* ---- multiplexer ---- */
struct mbt_utp_ctx {
	struct mbt_ev      *ev;
	struct mbt_listener listener;

	mbt_utp_accept_cb accept_cb;
	void             *accept_arg;

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
}

static void
utp_ctx_remove(struct mbt_utp_ctx *ctx, struct mbt_utp_sock *s)
{
	struct utp_sock_list *h = &ctx->buckets[s->recv_conn_id & (UTP_CONN_HASH_SIZE - 1)];
	SLIST_REMOVE(h, s, mbt_utp_sock, bucket_entry);
}

/* ---- mbt_conn ops ---- */

static void
utp_conn_set_watermark(struct mbt_conn *c, size_t low)
{
	((struct mbt_utp_sock *)c)->rbuf_watermark = low;
}

static size_t
utp_conn_available(struct mbt_conn *c)
{
	return ((struct mbt_utp_sock *)c)->rbuf_len;
}

static ssize_t
utp_conn_read(struct mbt_conn *c, void *buf, size_t len)
{
	struct mbt_utp_sock *s = (struct mbt_utp_sock *)c;
	if (len > s->rbuf_len)
		len = s->rbuf_len;
	if (!len) return 0;
	memcpy(buf, s->rbuf, len);
	memmove(s->rbuf, s->rbuf + len, s->rbuf_len - len);
	s->rbuf_len -= len;
	return (ssize_t)len;
}

static int utp_flush(struct mbt_utp_sock *s);

static ssize_t
utp_conn_write(struct mbt_conn *c, const void *buf, size_t len)
{
	struct mbt_utp_sock *s = (struct mbt_utp_sock *)c;
	if (s->state != UTP_CS_CONNECTED)
		return -ENOTCONN;
	size_t space = s->sbuf_cap - s->sbuf_len;
	if (len > space)
		len = space;
	if (!len) return 0;
	memcpy(s->sbuf + s->sbuf_len, buf, len);
	s->sbuf_len += len;
	utp_flush(s);
	return (ssize_t)len;
}

static void
utp_conn_free(struct mbt_conn *c)
{
	struct mbt_utp_sock *s = (struct mbt_utp_sock *)c;
	if (!s) return;
	if (s->rto_timer) {
		mbt_ev_timer_free(s->rto_timer);
		s->rto_timer = NULL;
	}
	/* Remove from ctx hash table if still registered */
	if (s->ctx && s->state != UTP_CS_DESTROY) {
		utp_ctx_remove(s->ctx, s);
		s->state = UTP_CS_DESTROY;
	}
	free(s->rbuf);
	free(s->sbuf);
	free(s);
}

static void
utp_conn_set_callbacks(struct mbt_conn *c, mbt_conn_read_cb readcb,
                       mbt_conn_event_cb eventcb, void *arg)
{
	struct mbt_utp_sock *s = (struct mbt_utp_sock *)c;
	s->readcb   = readcb;
	s->eventcb  = eventcb;
	s->cb_arg   = arg;
}

static const struct mbt_conn_ops utp_conn_ops = {
	.set_watermark = utp_conn_set_watermark,
	.available     = utp_conn_available,
	.read          = utp_conn_read,
	.write         = utp_conn_write,
	.set_callbacks = utp_conn_set_callbacks,
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
	s->timeout_ms = UTP_INIT_TIMEOUT_MS;
	s->rbuf_watermark = 1;

	memcpy(&s->peer_addr, addr, addrlen);
	s->peer_addrlen = addrlen;

	s->rbuf = malloc(UTP_RECV_BUF);
	s->sbuf = malloc(UTP_SEND_BUF);
	if (!s->rbuf || !s->sbuf) {
		free(s->rbuf);
		free(s->sbuf);
		free(s);
		return NULL;
	}
	s->rbuf_cap = UTP_RECV_BUF;
	s->sbuf_cap = UTP_SEND_BUF;

	return s;
}

/* ---- wire send ---- */

static int
utp_send_raw(struct mbt_utp_sock *s, uint8_t type,
             uint16_t seq, uint16_t ack,
             const void *payload, size_t payload_len)
{
	uint8_t buf[UTP_HEADER_SIZE + UTP_MAX_PACKET_SIZE];
	struct utp_hdr *h = (struct utp_hdr *)buf;

	if (payload_len > UTP_MAX_PACKET_SIZE)
		payload_len = UTP_MAX_PACKET_SIZE;

	/*
	 * BEP 29: SYN is sent with recv_conn_id so the acceptor knows what
	 * conn_id to use when sending responses back.  All other packets use
	 * send_conn_id.
	 */
	uint16_t wire_conn_id = (type == ST_SYN) ? s->recv_conn_id : s->send_conn_id;

	h->type_ver             = (uint8_t)((type << 4) | UTP_VERSION);
	h->extension            = 0;
	h->conn_id              = htons(wire_conn_id);
	h->timestamp_us         = htonl(utp_now_us32());
	h->timestamp_diff_us    = htonl(s->reply_micro);
	h->wnd_size             = htonl((uint32_t)(s->rbuf_cap - s->rbuf_len));
	h->seq_nr               = htons(seq);
	h->ack_nr               = htons(ack);

	if (payload && payload_len)
		memcpy(buf + UTP_HEADER_SIZE, payload, payload_len);

	/* pick the right fd based on peer AF */
	int fd = (s->peer_addr.ss_family == AF_INET6)
	       ? s->ctx->listener.fd6 : s->ctx->listener.fd4;

	ssize_t n = sendto(fd, buf, UTP_HEADER_SIZE + payload_len, 0,
	                   (struct sockaddr *)&s->peer_addr, s->peer_addrlen);
	return n < 0 ? -errno : 0;
}

static int
utp_send_state(struct mbt_utp_sock *s)
{
	return utp_send_raw(s, ST_STATE, s->seq_nr, s->ack_nr, NULL, 0);
}

/* Push buffered send data within window constraints */
static int
utp_flush(struct mbt_utp_sock *s)
{
	if (s->state != UTP_CS_CONNECTED)
		return 0;

	while (s->sbuf_len > s->sbuf_sent) {
		uint32_t window = (s->max_window < s->peer_wnd) ? s->max_window : s->peer_wnd;
		if (window == 0) window = UTP_MIN_PACKET_SIZE;
		if (s->cur_window >= window)
			break;

		size_t avail  = s->sbuf_len - s->sbuf_sent;
		size_t space  = window - s->cur_window;
		size_t chunk  = avail < space ? avail : space;
		if (chunk > UTP_MAX_PACKET_SIZE)
			chunk = UTP_MAX_PACKET_SIZE;

		int rv = utp_send_raw(s, ST_DATA, s->seq_nr,
		                      s->ack_nr,
		                      s->sbuf + s->sbuf_sent, chunk);
		if (rv < 0) return rv;

		s->seq_nr++;
		s->sbuf_sent  += chunk;
		s->cur_window += chunk;
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
	double gain = (double)UTP_MAX_CWND_INC
	            * ((double)off_target / UTP_CCONTROL_TARGET)
	            * ((double)s->cur_window / s->max_window);

	int32_t nw = (int32_t)s->max_window + (int32_t)gain;
	s->max_window = nw < UTP_MIN_PACKET_SIZE ? UTP_MIN_PACKET_SIZE : (uint32_t)nw;
}

/* Ack bytes up through (and including) the given seq; returns bytes acked */
static size_t
utp_ack_up_to(struct mbt_utp_sock *s, uint16_t ack_seq)
{
	/*
	 * sbuf_sent tracks bytes sent (head of ring not yet acked).
	 * We keep it simple: one outstanding send window, ack consumes front.
	 * ack_seq tells us the last packet received; each packet was seq_nr-per-chunk.
	 * For now: ack everything sent up to sbuf_sent on any ack (simplification
	 * until we track per-packet seq → byte offsets).
	 */
	(void)ack_seq;
	size_t acked = s->sbuf_sent;
	if (acked > 0) {
		memmove(s->sbuf, s->sbuf + acked, s->sbuf_len - acked);
		s->sbuf_len  -= acked;
		s->sbuf_sent  = 0;
		s->cur_window = 0;
	}
	return acked;
}

/* ---- retransmit timer ---- */

static void utp_rto_cb(struct mbt_ev_timer *t, void *arg);

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
utp_rto_cb(struct mbt_ev_timer *t, void *arg)
{
	(void)t;
	struct mbt_utp_sock *s = arg;

	if (s->state == UTP_CS_SYN_SENT) {
		/* retransmit SYN */
		utp_send_raw(s, ST_SYN, 1, 0, NULL, 0);
		utp_arm_rto(s);
		return;
	}

	if (s->state == UTP_CS_CONNECTED && s->sbuf_sent > 0) {
		/* halve window, retransmit oldest unacked data */
		s->max_window /= 2;
		if (s->max_window < UTP_MIN_PACKET_SIZE)
			s->max_window = UTP_MIN_PACKET_SIZE;
		s->cur_window = 0;
		s->sbuf_sent  = 0; /* will resend from sbuf head */
		utp_flush(s);
		utp_arm_rto(s);
	}
}

/* ---- incoming packet handlers ---- */

static void
utp_handle_syn(struct mbt_utp_ctx *ctx,
               const struct utp_hdr *h,
               const struct sockaddr *src, socklen_t srclen)
{
	uint16_t recv_id = ntohs(h->conn_id) + 1;

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
	s->reply_micro  = utp_now_us32() - ntohl(h->timestamp_us);
	s->peer_wnd     = ntohl(h->wnd_size);

	utp_ctx_insert(ctx, s);

	if (utp_send_state(s) < 0) {
		utp_ctx_remove(ctx, s);
		utp_conn_free(&s->conn);
		return;
	}
	s->seq_nr++; /* SYN-ACK advances seq_nr per BEP 29 diagram */

	s->state = UTP_CS_CONNECTED;

	if (ctx->accept_cb)
		ctx->accept_cb(ctx, &s->conn, src, srclen, ctx->accept_arg);
}

static void
utp_handle_data(struct mbt_utp_sock *s, const struct utp_hdr *h,
                const void *payload, size_t plen)
{
	uint16_t pkt_seq = ntohs(h->seq_nr);

	s->reply_micro = utp_now_us32() - ntohl(h->timestamp_us);
	s->peer_wnd    = ntohl(h->wnd_size);
	utp_ledbat_update(s, ntohl(h->timestamp_diff_us));

	/* Only accept in-order packets for now */
	if (pkt_seq != (uint16_t)(s->ack_nr + 1))
		goto ack;

	s->ack_nr = pkt_seq;

	if (plen > 0 && s->rbuf_len + plen <= s->rbuf_cap) {
		memcpy(s->rbuf + s->rbuf_len, payload, plen);
		s->rbuf_len += plen;
	}

	if (s->readcb && s->rbuf_len >= s->rbuf_watermark)
		s->readcb(&s->conn, s->cb_arg);

ack:
	utp_send_state(s);
}

static void
utp_handle_state(struct mbt_utp_sock *s, const struct utp_hdr *h)
{
	s->reply_micro = utp_now_us32() - ntohl(h->timestamp_us);
	s->peer_wnd    = ntohl(h->wnd_size);

	if (s->state == UTP_CS_SYN_SENT) {
		s->ack_nr = ntohs(h->seq_nr);
		s->state  = UTP_CS_CONNECTED;
		if (s->rto_timer) {
			mbt_ev_timer_free(s->rto_timer);
			s->rto_timer = NULL;
		}
		if (s->eventcb)
			s->eventcb(&s->conn, 0, s->cb_arg);
		/* flush any queued send data */
		utp_flush(s);
		return;
	}

	uint32_t their_delay = ntohl(h->timestamp_diff_us);
	if (their_delay)
		utp_ledbat_update(s, their_delay);

	utp_ack_up_to(s, ntohs(h->ack_nr));

	if (s->sbuf_sent > 0)
		utp_arm_rto(s);
	else if (s->rto_timer) {
		mbt_ev_timer_free(s->rto_timer);
		s->rto_timer = NULL;
	}

	utp_flush(s);
}

static void
utp_handle_fin(struct mbt_utp_sock *s)
{
	s->state = UTP_CS_DESTROY;
	utp_send_state(s);
	if (s->eventcb)
		s->eventcb(&s->conn, 0, s->cb_arg); /* EOF */
}

static void
utp_handle_reset(struct mbt_utp_sock *s)
{
	s->state = UTP_CS_DESTROY;
	if (s->eventcb)
		s->eventcb(&s->conn, -ECONNRESET, s->cb_arg);
}

/* ---- UDP receive callback (registered with mbt_listener) ---- */

static void
utp_udp_recv(struct mbt_ev_io *io, int fd, uint32_t events, void *arg)
{
	(void)io; (void)events;
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
			s->state = UTP_CS_DESTROY;
			utp_conn_free(&s->conn);
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

	/* skip extension headers */
	size_t off = UTP_HEADER_SIZE;
	uint8_t ext = h->extension;
	while (ext != 0) {
		if (off + 2 > len) return -1;
		const uint8_t *ep = (const uint8_t *)buf + off;
		ext  = ep[0];
		off += 2 + ep[1];
		if (off > len) return -1;
	}

	const void *payload    = (const uint8_t *)buf + off;
	size_t      payload_len = len - off;

	if (type == ST_SYN) {
		utp_handle_syn(ctx, h, src, srclen);
		return 0;
	}

	uint16_t conn_id = ntohs(h->conn_id);
	struct mbt_utp_sock *s = utp_ctx_find(ctx, conn_id);
	if (!s) return -1;

	switch (type) {
	case ST_DATA:  utp_handle_data(s, h, payload, payload_len); break;
	case ST_STATE: utp_handle_state(s, h);                      break;
	case ST_FIN:   utp_handle_fin(s);                           break;
	case ST_RESET: utp_handle_reset(s);                         break;
	default:       return -1;
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

	/* pick recv_conn_id not already in use */
	uint16_t cid;
	do { cid = (uint16_t)(arc4random() & 0xFFFF); }
	while (utp_ctx_find(ctx, cid));

	s->recv_conn_id = cid;
	s->send_conn_id = cid + 1;
	s->seq_nr       = 1;
	s->state        = UTP_CS_SYN_SENT;

	utp_ctx_insert(ctx, s);

	/* SYN: send with recv_conn_id, seq=1 */
	int rv = utp_send_raw(s, ST_SYN, 1, 0, NULL, 0);
	if (rv < 0) {
		utp_ctx_remove(ctx, s);
		utp_conn_free(&s->conn);
		return rv;
	}
	s->seq_nr = 2; /* next outbound data starts at 2 */

	utp_arm_rto(s);

	*out = &s->conn;
	return 0;
}
