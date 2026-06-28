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
/*
 * UTP_MSS: payload bytes per packet. 1400 is conservative enough to avoid IP
 * fragmentation under IPv6 (40 B) + UDP (8) + uTP (20) header overhead AND one
 * layer of encapsulation (PPPoE -8, VLAN -4, light tunnels). fragmentation
 * defeats LEDBAT's delay signal, so we stay well under the common 1500 MTU.
 */
#define UTP_MSS              1400
#define UTP_MAX_PACKET_SIZE  UTP_MSS   /* wire payload cap == MSS */
#define UTP_INIT_TIMEOUT_MS  1000
#define UTP_CCONTROL_TARGET  100000   /* us target one-way delay */
#define UTP_MAX_CWND_INC     3000     /* bytes/RTT max window increase */
#define UTP_REORDER_THRESH   3
#define UTP_BASE_DELAY_HIST  120      /* seconds */
#define UTP_CONN_HASH_BITS   6
#define UTP_CONN_HASH_SIZE   (1 << UTP_CONN_HASH_BITS)
#define UTP_FIN_WAIT_MS      4000

/*
 * Window source of truth.
 *
 * One parameter -- the in-flight window in packets -- drives the send queue,
 * the reorder queue, the SACK bitmask width, and the advertised receive window.
 * They MUST agree: if any of them is smaller than what we advertise, a packet
 * can land outside the SACK/reorder range, become un-ackable, and deadlock the
 * stream (see utp.md item 1). Deriving all four from window_pkts makes that
 * impossible by construction.
 *
 * window_pkts is a per-socket value resolved from session config, rounded up to
 * a power of two (queues are masked-indexed), and clamped to [MIN, MAX].
 */
#define UTP_WINDOW_MIN       8        /* packets; also covers bep 29 cong. floor */
#define UTP_WINDOW_MAX       1024
#define UTP_WINDOW_DEFAULT   32
/*
 * Initial congestion window. One packet (the old value) means the pipe cannot
 * fill enough to get ack feedback flowing under any delay/reorder, and
 * max_window only grows on acks -- so cwnd stays at one packet and throughput
 * crawls. Start at an RFC 6928 style IW10 (10 packets), capped at sock setup to
 * the configured window.
 */
#define UTP_INIT_CWND_PKTS   10
#define UTP_INIT_CWND        (UTP_INIT_CWND_PKTS * UTP_MSS)
/*
 * Congestion window floor: one full MSS. max_window must never drop below this
 * -- a sub-MSS window forces the sender to emit tiny fragmented packets and
 * throughput collapses to a crawl. (UTP_MIN_PACKET_SIZE, 150, is a wire packet
 * floor, not a congestion floor; do not use it to clamp max_window.)
 */
#define UTP_MIN_WINDOW       UTP_MSS

/*
 * rbuf/sbuf floor, independent of the window.
 *
 * The peer layer reads whole messages: it sets a read watermark of the full
 * message length, the largest being a piece block (4 len + 1 id + 4 index +
 * 4 begin + 16384 block = 16397 B). If a ring cannot hold one whole message the
 * reader is never woken, the ring never drains, the advertised window pins at 0
 * and the stream deadlocks. So the rings have a fixed floor >= the largest
 * message, decoupled from the window: a small window saves on the per-packet
 * queues but the rings stay large enough to never deadlock the message reader.
 */
#define UTP_MAX_MSG          (4 + 1 + 4 + 4 + (16 * 1024))   /* 16397 */
#define UTP_RING_MIN         (64 * 1024)                     /* pow2, >= UTP_MAX_MSG */
_Static_assert(UTP_RING_MIN >= UTP_MAX_MSG, "ring floor must hold a full message");

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
	uint16_t     last_loss_una; /* send_una at last loss response */
	int          loss_acted;    /* responded to loss at last_loss_una */

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

	/* window source of truth (resolved at sock creation) */
	uint32_t  window_pkts;      /* in-flight window, packets (power of 2) */
	uint32_t  queue_mask;       /* window_pkts - 1; indexes send/recv queues */
	uint32_t  ring_cap;         /* rbuf/sbuf capacity in bytes (power of 2) */
	uint32_t  ring_mask;        /* ring_cap - 1 */
	uint32_t  sack_bytes;       /* SACK bitmask width covering the window */

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

	struct utp_send_entry  *send_queue;   /* [window_pkts] */
	struct utp_recv_entry  *recv_queue;   /* [window_pkts] */

	mbt_conn_read_cb   readcb;
	mbt_conn_event_cb  eventcb;
	mbt_conn_write_cb  writecb;
	void              *cb_arg;
	void              *write_arg;

	struct mbt_ev_timer *rto_timer;
	struct mbt_ev_timer *destroy_timer;
	struct mbt_ev_timer *readcb_timer; /* deferred readcb notification */
	struct mbt_ev_timer *fin_timer;    /* deferred eventcb(0) after FIN before accept_cb */
	struct mbt_ev_timer *rtx_timer;    /* deferred fast-retransmit (breaks send->ack->rtx reentrancy) */

	int          got_fin;   /* FIN received before eventcb was installed */
	int          in_hash;   /* 1 if inserted into ctx->buckets */

	SLIST_ENTRY(mbt_utp_sock) bucket_entry;
};

SLIST_HEAD(utp_sock_list, mbt_utp_sock);

/* ---- multiplexer ---- */
struct mbt_utp_ctx {
	struct mbt_ev      *ev;
	struct mbt_listener listener;

	uint32_t          window_pkts;   /* resolved window for sockets on this ctx */

	mbt_utp_accept_cb accept_cb;
	void             *accept_arg;

	/* Test-only receive fault injection. Consulted for each datagram handed
	 * to mbt_utp_recv: return 0 to drop it, nonzero to deliver. Lets tests
	 * model loss/reorder over real UDP without faking the send path. */
	int (*rx_hook)(struct mbt_utp_ctx *ctx, const void *buf, size_t len,
	               const struct sockaddr *src, socklen_t srclen, void *arg);
	void *rx_hook_arg;

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

/* round v up to a power of two */
static uint32_t
utp_pow2_ceil(uint32_t v)
{
	uint32_t p = 1;
	while (p < v) p <<= 1;
	return p;
}

/*
 * Resolve a caller-requested window (in packets) to the value we will actually
 * use: 0 means "library default", otherwise clamp to [MIN, MAX] and round up to
 * a power of two so the send/recv queues can be mask-indexed.
 */
static uint32_t
utp_resolve_window(uint32_t requested)
{
	if (requested == 0)
		requested = UTP_WINDOW_DEFAULT;
	if (requested < UTP_WINDOW_MIN) requested = UTP_WINDOW_MIN;
	if (requested > UTP_WINDOW_MAX) requested = UTP_WINDOW_MAX;
	return utp_pow2_ceil(requested);
}

static const char *
utp_type_str(uint8_t type)
{
	switch (type) {
	case ST_DATA:  return "DATA";
	case ST_FIN:   return "FIN";
	case ST_STATE: return "STATE";
	case ST_RESET: return "RESET";
	case ST_SYN:   return "SYN";
	default:       return "?";
	}
}

/*
 * Trace one datagram at the wire boundary (sendto/recvfrom). dir is "tx"/"rx".
 * Decodes the fixed header rather than dumping hex: type, seq, ack, advertised
 * window, whether an extension (SACK) is attached, and the datagram size.
 * Cheap to call -- the format args are only evaluated when UTP trace is on.
 */
static void
utp_trace_pkt(const char *dir, const void *buf, size_t len)
{
	if (len < UTP_HEADER_SIZE) {
		mbt_log_trace(MBT_LOG_UTP, "%s runt len=%zu", dir, len);
		return;
	}
	const struct utp_hdr *h = buf;
	mbt_log_trace(MBT_LOG_UTP,
	    "%s %-5s cid=%u seq=%u ack=%u wnd=%u ext=%u len=%zu",
	    dir, utp_type_str(h->type_ver >> 4), ntohs(h->conn_id),
	    ntohs(h->seq_nr), ntohs(h->ack_nr), ntohl(h->wnd_size),
	    h->extension, len);
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
	return s->ring_cap - rbuf_available(s);
}

/*
 * Advertised receive window. Must never exceed what we can both buffer (rbuf
 * free space) AND reorder/SACK (one window of packets): if the peer is allowed
 * more in flight than the reorder buffer or SACK bitmask can represent, a
 * packet can land outside the window, become un-ackable, and deadlock the
 * stream (utp.md item 1). Capping here keeps the peer's in-flight <= window_pkts
 * by construction.
 */
static uint32_t
utp_adv_wnd(struct mbt_utp_sock *s)
{
	uint32_t by_rbuf    = (uint32_t)rbuf_space(s);
	uint32_t by_reorder = s->window_pkts * UTP_MSS;
	return by_rbuf < by_reorder ? by_rbuf : by_reorder;
}

/* Copy len bytes into rbuf starting at monotonic offset rbuf_wr */
static void
rbuf_produce(struct mbt_utp_sock *s, const void *data, size_t len)
{
	const uint8_t *src = data;
	size_t done = 0;
	while (done < len) {
		size_t idx = (s->rbuf_wr + done) & s->ring_mask;
		size_t chunk = s->ring_cap - idx;
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

static int utp_flush(struct mbt_utp_sock *s);
static int utp_send_ack(struct mbt_utp_sock *s);
static int utp_do_sendto(struct mbt_utp_sock *s, const void *buf, size_t len);

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
		size_t idx = (s->rbuf_rd + done) & s->ring_mask;
		size_t chunk = s->ring_cap - idx;
		if (chunk > len - done) chunk = len - done;
		memcpy(dst + done, s->rbuf + idx, chunk);
		done += chunk;
	}
	s->rbuf_rd += (uint32_t)len;
	/* Advertise the newly freed window space to the peer */
	utp_send_ack(s);
	return (ssize_t)len;
}

/* Copy len bytes into sbuf at monotonic offset wr (caller advances sbuf_wr). */
static void
sbuf_copy_in(struct mbt_utp_sock *s, uint32_t wr, const void *src, size_t len)
{
	const uint8_t *p = src;
	size_t done = 0;
	while (done < len) {
		size_t idx = (wr + done) & s->ring_mask;
		size_t chunk = s->ring_cap - idx;
		if (chunk > len - done) chunk = len - done;
		memcpy(s->sbuf + idx, p + done, chunk);
		done += chunk;
	}
}

static ssize_t
utp_conn_write(struct mbt_conn *c, const void *buf, size_t len)
{
	struct mbt_utp_sock *s = (struct mbt_utp_sock *)c;
	if (s->state != UTP_CS_CONNECTED)
		return -ENOTCONN;
	uint32_t space = s->ring_cap - (s->sbuf_wr - s->sbuf_una);
	if (len > space) return -ENOBUFS;
	sbuf_copy_in(s, s->sbuf_wr, buf, len);
	s->sbuf_wr += (uint32_t)len;
	utp_flush(s);
	return (ssize_t)len;
}

/*
 * Gather-write: accept the whole vector or none of it. Lets the peer layer
 * frame a message (length prefix + header + payload) as separate iovecs
 * without concatenating them into a heap buffer first.
 */
static ssize_t
utp_conn_writev(struct mbt_conn *c, const struct iovec *iov, int iovcnt)
{
	struct mbt_utp_sock *s = (struct mbt_utp_sock *)c;
	if (s->state != UTP_CS_CONNECTED)
		return -ENOTCONN;

	size_t total = 0;
	for (int i = 0; i < iovcnt; i++)
		total += iov[i].iov_len;

	uint32_t space = s->ring_cap - (s->sbuf_wr - s->sbuf_una);
	if (total > space) return -ENOBUFS;

	uint32_t wr = s->sbuf_wr;
	for (int i = 0; i < iovcnt; i++) {
		sbuf_copy_in(s, wr, iov[i].iov_base, iov[i].iov_len);
		wr += (uint32_t)iov[i].iov_len;
	}
	s->sbuf_wr += (uint32_t)total;
	utp_flush(s);
	return (ssize_t)total;
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
	if (s->rtx_timer) {
		mbt_ev_timer_free(s->rtx_timer);
		s->rtx_timer = NULL;
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

	/*
	 * The owning peer/torrent_peer is going away. A CONNECTED socket lingers
	 * in FIN_SENT to drain its FIN, and keeps processing inbound acks while it
	 * does -- so clear ALL app callbacks now, or a late ack that advances
	 * send_una will invoke writecb with a write_arg (the freed torrent_peer)
	 * and crash. readcb/eventcb are cleared in the FIN branch below too, but
	 * writecb must be cleared on every path.
	 */
	s->writecb   = NULL;
	s->write_arg = NULL;

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

		utp_do_sendto(s, buf, UTP_HEADER_SIZE);
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

static void
utp_conn_set_writecb(struct mbt_conn *c, mbt_conn_write_cb cb, void *arg)
{
	struct mbt_utp_sock *s = (struct mbt_utp_sock *)c;
	s->writecb   = cb;
	s->write_arg = arg;
}

static const struct mbt_conn_ops utp_conn_ops = {
	.name          = "utp",
	.set_watermark = utp_conn_set_watermark,
	.available     = utp_conn_available,
	.read          = utp_conn_read,
	.write         = utp_conn_write,
	.writev        = utp_conn_writev,
	.set_callbacks = utp_conn_set_callbacks,
	.set_writecb   = utp_conn_set_writecb,
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
	s->timeout_ms = UTP_INIT_TIMEOUT_MS;
	s->rbuf_watermark = 1;

	/* Window source of truth: everything derives from window_pkts. */
	s->window_pkts = utp_resolve_window(ctx ? ctx->window_pkts : 0);
	s->queue_mask  = s->window_pkts - 1;
	/* SACK bitmask must cover the whole window; bep 29 wants 4-byte units. */
	s->sack_bytes  = ((s->window_pkts + 31) / 32) * 4;

	/* Initial cwnd, capped to the window we can actually track. */
	{
		uint32_t win_bytes = s->window_pkts * UTP_MSS;
		s->max_window = UTP_INIT_CWND < win_bytes ? UTP_INIT_CWND : win_bytes;
		s->peer_wnd   = s->max_window;
	}
	/* rings hold at least one window of packets AND one whole message
	 * (UTP_RING_MIN), so a small window never deadlocks the message reader. */
	{
		uint32_t want = s->window_pkts * UTP_MSS;
		if (want < UTP_RING_MIN) want = UTP_RING_MIN;
		s->ring_cap  = utp_pow2_ceil(want);
		s->ring_mask = s->ring_cap - 1;
	}

	memcpy(&s->peer_addr, addr, addrlen);
	s->peer_addrlen = addrlen;

	s->rbuf       = malloc(s->ring_cap);
	s->sbuf       = malloc(s->ring_cap);
	s->send_queue = calloc(s->window_pkts, sizeof(*s->send_queue));
	s->recv_queue = calloc(s->window_pkts, sizeof(*s->recv_queue));
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

	utp_trace_pkt("tx", buf, len);

	int fd = (s->peer_addr.ss_family == AF_INET6)
	       ? ctx->listener.fd6 : ctx->listener.fd4;

	ssize_t n = sendto(fd, buf, len, 0,
	                   (struct sockaddr *)&s->peer_addr, s->peer_addrlen);
	return n < 0 ? -errno : 0;
}

/* Build SACK extension covering the whole window. bit i = recv_queue holds
 * ack_nr+2+i. Width is s->sack_bytes (4-byte units per bep 29), sized so every
 * in-window out-of-order packet is representable -- without this the sender
 * cannot learn about holes beyond the first 32 packets and the stream can
 * deadlock (utp.md item 1). */
static size_t
utp_build_sack(struct mbt_utp_sock *s, uint8_t *ext_buf, size_t ext_cap)
{
	size_t nbits = s->sack_bytes * 8;
	if (ext_cap < 2 + s->sack_bytes) return 0;

	uint8_t *bits = ext_buf + 2;
	memset(bits, 0, s->sack_bytes);
	int any = 0;
	for (size_t i = 0; i < nbits; i++) {
		uint16_t seq = (uint16_t)(s->ack_nr + 2 + i);
		struct utp_recv_entry *e = &s->recv_queue[seq & s->queue_mask];
		if (e->valid && e->seq_nr == seq) {
			bits[i / 8] |= (uint8_t)(1 << (i % 8));
			any = 1;
		}
	}
	if (!any) return 0;
	ext_buf[0] = 0;                      /* next extension = none */
	ext_buf[1] = (uint8_t)s->sack_bytes; /* bitmask length */
	return 2 + s->sack_bytes;
}

static int
utp_send_ack(struct mbt_utp_sock *s)
{
	/* ext = 2 header bytes + bitmask; bitmask <= UTP_WINDOW_MAX/8 bytes */
	uint8_t buf[UTP_HEADER_SIZE + 2 + UTP_WINDOW_MAX / 8];
	uint8_t ext_buf[2 + UTP_WINDOW_MAX / 8];
	size_t ext_len = utp_build_sack(s, ext_buf, sizeof(ext_buf));

	struct utp_hdr *h = (struct utp_hdr *)buf;
	h->type_ver          = (uint8_t)((ST_STATE << 4) | UTP_VERSION);
	h->extension         = ext_len ? 1 : 0;
	h->conn_id           = htons(s->send_conn_id);
	h->timestamp_us      = htonl(utp_now_us32());
	h->timestamp_diff_us = htonl(s->reply_micro);
	h->wnd_size          = htonl(utp_adv_wnd(s));
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
	h->wnd_size          = htonl(utp_adv_wnd(s));
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
		size_t idx = (off + done) & s->ring_mask;
		size_t chunk = s->ring_cap - idx;
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
		if (in_flight >= window) {
			mbt_log_trace(MBT_LOG_UTP,
			    "flush stop: window in_flight=%u window=%u maxw=%u peer_wnd=%u pending=%u",
			    in_flight, window, s->max_window, s->peer_wnd,
			    s->sbuf_wr - s->sbuf_snd);
			break;
		}

		/* Check send queue slot available */
		struct utp_send_entry *e = &s->send_queue[s->seq_nr & s->queue_mask];
		if (e->valid) {
			mbt_log_trace(MBT_LOG_UTP,
			    "flush stop: queue full seq_nr=%u send_una=%u",
			    s->seq_nr, s->send_una);
			break;  /* queue full */
		}

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
	/*
	 * Bound off_target to +/- one target. our_delay is derived from peer
	 * timestamps and can be wildly large (clock skew, jitter, a stale or
	 * bogus sample), which would otherwise produce a huge negative gain and
	 * collapse the window in a single update. LEDBAT only needs the sign and
	 * a bounded magnitude here. (utp.md item 9.)
	 */
	if (off_target < -UTP_CCONTROL_TARGET) off_target = -UTP_CCONTROL_TARGET;
	if (off_target >  UTP_CCONTROL_TARGET) off_target =  UTP_CCONTROL_TARGET;

	if (!s->max_window) return;

	uint32_t in_flight = s->sbuf_snd - s->sbuf_una;
	double gain = (double)UTP_MAX_CWND_INC
	            * ((double)off_target / UTP_CCONTROL_TARGET)
	            * ((double)in_flight / s->max_window);

	int32_t nw = (int32_t)s->max_window + (int32_t)gain;
	s->max_window = nw < (int32_t)UTP_MIN_WINDOW ? UTP_MIN_WINDOW : (uint32_t)nw;
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

/* Retransmit one packet (no bookkeeping caller-visible side effects). */
static void
utp_resend_entry(struct mbt_utp_sock *s, struct utp_send_entry *e)
{
	uint8_t payload[UTP_MAX_PACKET_SIZE];
	sbuf_copy_out(s, e->sbuf_off, payload, e->len);
	utp_send_data(s, e->seq_nr, payload, e->len);
	e->retransmits++;
	e->sent_us = utp_now_us();
}

/*
 * Fast retransmit: resend every still-unacked (valid && !sacked) packet in the
 * window, oldest first. The old code only resent send_una, so a lost packet
 * that was NOT the oldest unacked (a hole mid/tail-window while send_una keeps
 * getting re-acked) was never the one retransmitted -> the gap never filled and
 * the stream deadlocked (utp.md item 1b). Driving retransmit off the actual
 * unacked set, now visible because SACK covers the whole window, fixes that.
 */
static void
utp_retransmit(struct mbt_utp_sock *s)
{
	/*
	 * Resend only the OLDEST unacked packet (the hole at send_una), not the
	 * whole unacked window. send_una is the cumulative-ack boundary, so it is
	 * the first gap by definition. Resending the entire window on every loss
	 * episode amplifies enormously under load (each episode replays thousands
	 * of packets), which collapses throughput; standard fast retransmit sends
	 * one segment and lets the ack clock drive the rest. Further holes are
	 * picked up by subsequent acks/SACKs and the rto.
	 */
	for (uint16_t seq = s->send_una;
	     (int16_t)(seq - s->seq_nr) < 0;
	     seq++) {
		struct utp_send_entry *e = &s->send_queue[seq & s->queue_mask];
		if (!e->valid || e->seq_nr != seq)
			continue;
		if (!e->sacked) {
			utp_resend_entry(s, e);
			return;   /* one packet per loss event */
		}
	}
}

static void utp_rtx_cb(struct mbt_ev_timer *t, void *arg);

/*
 * Loss-episode gate. Pure reordering routinely SACKs several packets ahead of a
 * gap that is not actually lost; without gating, each such ack would halve the
 * window and trigger a retransmit, collapsing throughput under reorder (utp.md
 * item 2). Respond to loss at most once per episode: an episode is identified by
 * the current send_una, and loss_acted is cleared whenever send_una advances
 * (the advance loop). Returns 1 (and halves max_window once) the first time loss
 * is detected for a given send_una, 0 thereafter.
 */
static int
utp_enter_loss(struct mbt_utp_sock *s)
{
	if (s->loss_acted && s->last_loss_una == s->send_una)
		return 0;
	s->last_loss_una = s->send_una;
	s->loss_acted = 1;
	s->max_window /= 2;
	if (s->max_window < UTP_MIN_WINDOW)
		s->max_window = UTP_MIN_WINDOW;
	return 1;
}

/*
 * Schedule a fast-retransmit on a 0ms timer instead of calling utp_retransmit
 * directly from ack processing. ack processing can run synchronously inside a
 * send (a TX hook, or any path where the peer is on the same loop), so resending
 * inline would recurse send -> ack -> resend -> ack -> ... and blow the stack.
 * deferring runs it on a fresh stack; the "already armed" guard also coalesces a
 * burst of triggers into a single retransmit pass per loop turn.
 */
static void
utp_schedule_retransmit(struct mbt_utp_sock *s)
{
	if (s->rtx_timer)
		return;
	mbt_ev_timer_new(s->ctx->ev, &s->rtx_timer, 0, utp_rtx_cb, s);
}

static void
utp_rtx_cb(struct mbt_ev_timer *t, void *arg)
{
	struct mbt_utp_sock *s = arg;
	s->rtx_timer = NULL;
	mbt_ev_timer_free(t);
	if (s->state == UTP_CS_CONNECTED && s->send_una != s->seq_nr)
		utp_retransmit(s);
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
		h->wnd_size          = htonl(utp_adv_wnd(s));
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
		if (s->max_window < UTP_MIN_WINDOW)
			s->max_window = UTP_MIN_WINDOW;

		/*
		 * Retransmit the unacked window in place (utp_retransmit resends
		 * every valid && !sacked entry with its original seq and
		 * sbuf_off). seq numbers are immutable once assigned -- the
		 * receiver buffers out-of-order packets and SACKs them assuming
		 * a given seq always carries the same bytes -- so we must NOT
		 * re-segment on resend. seq_nr/sbuf_snd/queue are left intact so
		 * SACK bookkeeping stays valid.
		 */
		utp_retransmit(s);
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
		if (s->dup_acks >= UTP_REORDER_THRESH && utp_enter_loss(s)) {
			utp_schedule_retransmit(s);
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
		struct utp_send_entry *e = &s->send_queue[seq & s->queue_mask];
		if (e->valid && e->seq_nr == seq && !e->sacked) {
			if (e->retransmits == 0) {
				int32_t sample = (int32_t)(now - e->sent_us);
				if (sample > 0)
					utp_update_rtt(s, sample);
			}
			e->sacked = 1;
		}
	}

	/* Process SACK bitmask: bit i -> seq peer_ack_nr+2+i. honor the full
	 * advertised bitmask (sack_len bytes), not a fixed 4. */
	for (size_t bi = 0; bi < sack_len; bi++) {
		uint8_t byte = sack_data[bi];
		for (int bit = 0; bit < 8; bit++) {
			if (byte & (1 << bit)) {
				uint16_t seq = (uint16_t)(peer_ack_nr + 2 + bi * 8 + bit);
				struct utp_send_entry *e = &s->send_queue[seq & s->queue_mask];
				if (e->valid && e->seq_nr == seq)
					e->sacked = 1;
			}
		}
	}

	/* Advance send_una */
	uint32_t old_una = s->sbuf_una;
	while ((int16_t)(s->send_una - s->seq_nr) < 0) {
		struct utp_send_entry *e = &s->send_queue[s->send_una & s->queue_mask];
		if (!e->valid || e->seq_nr != s->send_una || !e->sacked)
			break;
		s->sbuf_una = e->sbuf_off + e->len;
		e->valid = 0;
		s->send_una++;
		/* forward progress -> a new loss episode may be detected */
		s->loss_acted = 0;
	}
	if (s->writecb && s->sbuf_una != old_una) {
		mbt_conn_write_cb cb = s->writecb;
		void *arg = s->write_arg;
		s->writecb   = NULL;
		s->write_arg = NULL;
		cb(&s->conn, arg);
	}

	/* Fast retransmit: sacked packets ahead of new send_una */
	int sacked_ahead = 0;
	for (uint16_t seq = (uint16_t)(s->send_una + 1);
	     (int16_t)(seq - s->seq_nr) < 0;
	     seq++) {
		struct utp_send_entry *e = &s->send_queue[seq & s->queue_mask];
		if (e->valid && e->seq_nr == seq && e->sacked)
			sacked_ahead++;
	}
	if (sacked_ahead >= UTP_REORDER_THRESH && utp_enter_loss(s)) {
		utp_schedule_retransmit(s);
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

/* Deliver data to rbuf; returns 0 on success, -1 if rbuf full (don't ACK). */
static int
utp_deliver_data(struct mbt_utp_sock *s, const void *data, size_t len, uint16_t seq)
{
	if (len > rbuf_space(s)) return -1;
	rbuf_produce(s, data, len);
	return 0;
}

static void
utp_handle_data(struct mbt_utp_sock *s, const struct utp_hdr *h,
                const void *payload, size_t plen)
{
	uint16_t pkt_seq = ntohs(h->seq_nr);

	s->reply_micro = utp_now_us32() - ntohl(h->timestamp_us);
	s->peer_wnd    = ntohl(h->wnd_size);

	mbt_log_trace(MBT_LOG_UTP, "  data seq=%u vs ack_nr=%u plen=%zu",
	    pkt_seq, s->ack_nr, plen);

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
		/* In-order: deliver — only advance ack_nr if data was stored */
		if (utp_deliver_data(s, payload, plen, pkt_seq) < 0) {
			/* rbuf full; ACK with current ack_nr so peer sees wnd=0 */
			utp_send_ack(s);
			return;
		}
		s->ack_nr++;

		/* Cascade from recv_queue */
		for (;;) {
			uint16_t next = (uint16_t)(s->ack_nr + 1);
			struct utp_recv_entry *e = &s->recv_queue[next & s->queue_mask];
			if (!e->valid || e->seq_nr != next)
				break;
			if (utp_deliver_data(s, e->data, e->len, next) < 0)
				break;
			e->valid = 0;
			s->ack_nr++;
		}

		if (s->readcb && rbuf_available(s) >= s->rbuf_watermark)
			s->readcb(&s->conn, s->cb_arg);
	} else {
		/*
		 * Out of order: buffer only if within the reorder window
		 * (ack_nr, ack_nr + window_pkts]. anything further ahead would
		 * alias a slot belonging to a different seq, so drop it -- we
		 * advertise a window <= window_pkts (utp_adv_wnd), so a
		 * conforming peer never sends beyond this; a packet past it is
		 * either a misbehaving peer or stale, and dropping it (no ack
		 * advance) just makes the peer retransmit in range.
		 */
		uint16_t ahead = (uint16_t)(pkt_seq - s->ack_nr);
		if (ahead >= 1 && ahead <= s->window_pkts) {
			struct utp_recv_entry *e = &s->recv_queue[pkt_seq & s->queue_mask];
			if (!e->valid) {
				e->valid  = 1;
				e->seq_nr = pkt_seq;
				e->len    = (uint16_t)(plen > UTP_MAX_PACKET_SIZE
				                     ? UTP_MAX_PACKET_SIZE : plen);
				memcpy(e->data, payload, e->len);
			}
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
mbt_utp_ctx_set_rx_hook(struct mbt_utp_ctx *ctx,
    int (*hook)(struct mbt_utp_ctx *ctx, const void *buf, size_t len,
                const struct sockaddr *src, socklen_t srclen, void *arg),
    void *arg)
{
	ctx->rx_hook     = hook;
	ctx->rx_hook_arg = arg;
}

void
mbt_utp_ctx_set_window(struct mbt_utp_ctx *ctx, uint32_t pkts)
{
	/* Stored raw; utp_resolve_window clamps to [MIN,MAX] and rounds to a
	 * power of two when each socket is created. 0 == library default. */
	ctx->window_pkts = pkts;
}

int
mbt_utp_recv(struct mbt_utp_ctx *ctx,
             const void *buf, size_t len,
             const struct sockaddr *src, socklen_t srclen)
{
	utp_trace_pkt("rx", buf, len);

	/* Test-only receive fault injection: 0 = drop this datagram. */
	if (ctx->rx_hook &&
	    ctx->rx_hook(ctx, buf, len, src, srclen, ctx->rx_hook_arg) == 0)
		return 0;

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
	h->wnd_size          = htonl(utp_adv_wnd(s));
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
