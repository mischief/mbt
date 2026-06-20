// SPDX-License-Identifier: ISC
#include <sys/param.h>
#include <sys/socket.h>
#include <arpa/inet.h>
#include <assert.h>
#include <stdint.h>
#include <stdlib.h>
#include <string.h>
#include <errno.h>
#include "mbt/peer_source.h"

#include "mbt/log.h"
#include "mbt/event.h"
#include "mbt/conn.h"
#include "mbt/infohash.h"
#include "mbt/pack.h"
#include "mbt/peer.h"
#include "mbt/bitmap.h"
#include "mbt/be.h"
#include "internal.h"

/* Protocol constants */
enum {
	HANDSHAKE_SIZE = 1 + 19 + 8 + 20 + 20, /* 68 */
	METADATA_BLOCKSIZE = 1 << 14,
};

/* Handshake reserved byte flags */

struct mbt_peer {
	enum mbt_peer_state state;
	struct mbt_ev *ev;
	struct mbt_conn *conn;
	struct mbt_ev_timer *timer;
	struct mbt_ev_timer *defer;

	struct mbt_infohash *ih;
	uint8_t our_id[20];
	uint8_t their_id[20];
	uint8_t ext[8];
	uint32_t flags;
	char addr[INET6_ADDRSTRLEN];
	uint16_t port;
	enum mbt_peer_source source;  /* mbt_peer_source flags */

	struct mbt_peer_callbacks cb;
	void *cb_arg;

	/* choke/interest state */
	int peer_choking;   /* they are choking us */
	int peer_interested;
	int am_interested;
	int am_choking;

	/* extension protocol */
	uint8_t ut_metadata_id; /* their msg id for ut_metadata, 0=unsupported */
	uint8_t ut_pex_id;     /* their msg id for ut_pex, 0=unsupported */
	uint8_t have_all;      /* BEP 6: peer sent HAVE_ALL (deferred until piece_count known) */
	size_t metadata_size;
	int reqq;              /* peer's max outstanding requests (0=unknown) */

	/* bitfield */
	struct mbt_bitmap *bitfield;
	uint32_t piece_count;
	size_t our_metadata_size; /* metadata we can serve (0 = unknown) */
	int our_reqq;            /* max requests we accept (0 = don't advertise) */

	/* message framing */
	size_t msglen;
};

/* --- Internal helpers --- */

static ssize_t
peer_write(struct mbt_peer *p, const void *buf, size_t len)
{
	return mbt_conn_write(p->conn, buf, len);
}

static ssize_t
peer_write_msg(struct mbt_peer *p, const void *buf, size_t len)
{
	unsigned char prefix[4];
	mbt_pack(prefix, "l", (uint32_t)len);
	if (mbt_conn_write(p->conn, prefix, 4) < 0)
		return -EIO;
	if (mbt_conn_write(p->conn, buf, len) < 0)
		return -EIO;
	return 0;
}

static void
peer_error(struct mbt_peer *p, int err)
{
	mbt_log_warn(MBT_LOG_PEER, "%s:%hu: %s", p->addr, p->port,
	             err == 0 ? "EOF" : strerror(-err));
	p->state = MBT_PEER_CLOSED;
	if (p->cb.on_error)
		p->cb.on_error(p, err, p->cb_arg);
}

/* --- Keepalive timer --- */

static void
peer_keepalive_cb(struct mbt_ev_timer *timer, void *v)
{
	struct mbt_peer *p = v;
	mbt_ev_timer_mod(timer, 60 * 1000000ULL);
	peer_write(p, "\0\0\0\0", 4);
}

/* --- Handshake --- */

static int
peer_send_handshake(struct mbt_peer *p)
{
	unsigned char buf[HANDSHAKE_SIZE];
	unsigned char ext[8] = {0};
	int o = 0, rv;

	/* BEP 10 */
	ext[5] |= RESERVED_EXTENSION;
	/* BEP 6 Fast Extension */
	ext[7] |= RESERVED_FAST;

	rv = mbt_pack(buf, "b*", 19, (size_t)19, "BitTorrent protocol");
	o += rv;
	rv = mbt_pack(buf + o, "*", (size_t)8, ext);
	o += rv;
	rv = mbt_pack(buf + o, "*", (size_t)20, p->ih->hash_sha1);
	o += rv;
	rv = mbt_pack(buf + o, "*", (size_t)20, p->our_id);
	o += rv;

	if (peer_write(p, buf, o) < 0)
		return -EIO;

	return 0;
}

static int
peer_recv_handshake(struct mbt_peer *p)
{
	unsigned char buf[HANDSHAKE_SIZE];
	unsigned char proto[19], hash[20];
	uint32_t pstrlen = 0;
	int off = 0, rv;

	rv = mbt_conn_read(p->conn, buf, HANDSHAKE_SIZE);
	if (rv != HANDSHAKE_SIZE)
		return -ENOMSG;

	rv = mbt_unpack(buf, "b*", &pstrlen, 19, proto);
	off += rv;

	if (pstrlen != 19 || memcmp(proto, "BitTorrent protocol", 19) != 0)
		return -ENOMSG;

	rv = mbt_unpack(buf + off, "*", 8, p->ext);
	off += rv;

	/* Parse reserved byte flags */
	p->flags = 0;
	if (p->ext[7] & RESERVED_DHT) p->flags |= MBT_PEER_FLAG_DHT;
	if (p->ext[7] & RESERVED_FAST) p->flags |= MBT_PEER_FLAG_FAST;
	if (p->ext[5] & RESERVED_EXTENSION) p->flags |= MBT_PEER_FLAG_EXTENSION;

	rv = mbt_unpack(buf + off, "*", 20, hash);
	off += rv;

	if (memcmp(hash, p->ih->hash_sha1, 20) != 0)
		return -ENOMSG; /* wrong infohash */

	rv = mbt_unpack(buf + off, "*", 20, p->their_id);
	off += rv;

	return 0;
}

static int
peer_send_extension_handshake(struct mbt_peer *p)
{
	_cleanup_be_ struct mbt_be_value *be = NULL;
	unsigned char *encoded;
	unsigned char packed[64];
	size_t sz;
	int rv;

	_cleanup_be_ struct mbt_be_value *m = NULL;
	if (mbt_be_dict_new(&be) < 0) return -ENOMEM;
	if (mbt_be_dict_new(&m) < 0) return -ENOMEM;
	if (mbt_be_dict_append_number(m, "ut_metadata", 3) < 0) return -ENOMEM;
	if (mbt_be_dict_append_number(m, "ut_pex", 4) < 0) return -ENOMEM;
	if (mbt_be_dict_append(be, "m", m) < 0) return -ENOMEM;
	m = NULL;
	if (p->our_metadata_size > 0)
		if (mbt_be_dict_append_number(be, "metadata_size", (int64_t)p->our_metadata_size) < 0) return -ENOMEM;
	if (p->our_reqq > 0)
		if (mbt_be_dict_append_number(be, "reqq", p->our_reqq) < 0) return -ENOMEM;

	rv = mbt_be_value_encode(be, &encoded, &sz);
	if (rv < 0)
		return rv;

	rv = mbt_pack(packed, "bb*", (int)BT_EXTENSION, (int)0, sz, encoded);
	free(encoded);
	if (rv < 0)
		return rv;

	return peer_write_msg(p, packed, rv);
}

static int
peer_recv_extension_handshake(struct mbt_peer *p, size_t len)
{
	unsigned char buf[1024];
	_cleanup_be_ struct mbt_be_value *be = NULL;
	struct mbt_be_value *tmp;
	ssize_t rd;
	int rv;

	rd = mbt_conn_read(p->conn, buf, MIN(sizeof(buf), len));
	if (rd <= 0)
		return -ENOMSG;

	rv = mbt_be_value_parse(buf, rd, &be);
	if (rv < 0)
		return rv;

	tmp = mbt_be_find(be, "m");
	if (tmp) {
		struct mbt_be_value *utm = mbt_be_find(tmp, "ut_metadata");
		if (utm && utm->type == BE_INTEGER)
			p->ut_metadata_id = (uint8_t)utm->integer;

		struct mbt_be_value *utp = mbt_be_find(tmp, "ut_pex");
		if (utp && utp->type == BE_INTEGER)
			p->ut_pex_id = (uint8_t)utp->integer;
	}

	tmp = mbt_be_find(be, "metadata_size");
	if (tmp && tmp->type == BE_INTEGER && tmp->integer > 0)
		p->metadata_size = (size_t)tmp->integer;

	tmp = mbt_be_find(be, "reqq");
	if (tmp && tmp->type == BE_INTEGER && tmp->integer > 0)
		p->reqq = (int)tmp->integer;

	return 0;
}

/* --- Message dispatch (ACTIVE state) --- */

static int
peer_rx_message(struct mbt_peer *p)
{
	uint8_t cmd;
	ssize_t sz;
	uint32_t length, piece, offset, req_len;
	unsigned char lenbuf[4];
	unsigned char buf[16];
	int rv;

	if (!p->msglen) {
		sz = mbt_conn_read(p->conn, lenbuf, 4);
		if (sz != 4)
			return -EIO;
		mbt_unpack(lenbuf, "l", &length);

		if (length == 0)
			return 0; /* keepalive */

		if (length > (1 << 18)) {
			mbt_log_warn(MBT_LOG_PEER, "%s:%hu: msg too large: %u (raw: %02x%02x%02x%02x)",
			             p->addr, p->port, length,
			             lenbuf[0], lenbuf[1], lenbuf[2], lenbuf[3]);
			/* Peek at next bytes for debugging */
			unsigned char peek[8] = {0};
			mbt_conn_read(p->conn, peek, sizeof(peek));
			mbt_log_warn(MBT_LOG_PEER, "%s:%hu: next 8 bytes: %02x %02x %02x %02x %02x %02x %02x %02x",
			             p->addr, p->port, peek[0], peek[1], peek[2], peek[3],
			             peek[4], peek[5], peek[6], peek[7]);
			return -ENOMSG;
		}

		p->msglen = length;
		mbt_conn_set_watermark(p->conn, p->msglen);
		return 0;
	}

	sz = mbt_conn_read(p->conn, &cmd, 1);
	if (sz != 1)
		return -EIO;
	p->msglen -= 1;


	switch (cmd) {
	case BT_CHOKE:
		p->peer_choking = 1;
		if (p->cb.on_choke)
			p->cb.on_choke(p, 1, p->cb_arg);
		break;

	case BT_UNCHOKE:
		p->peer_choking = 0;
		if (p->cb.on_choke)
			p->cb.on_choke(p, 0, p->cb_arg);
		break;

	case BT_INTERESTED:
		p->peer_interested = 1;
		if (p->cb.on_interest)
			p->cb.on_interest(p, 1, p->cb_arg);
		break;

	case BT_NOT_INTERESTED:
		p->peer_interested = 0;
		if (p->cb.on_interest)
			p->cb.on_interest(p, 0, p->cb_arg);
		break;

	case BT_HAVE:
		sz = mbt_conn_read(p->conn, buf, 4);
		mbt_unpack(buf, "l", &piece);
		if (!p->bitfield && p->piece_count > 0)
			mbt_bitmap_new_bits(&p->bitfield, p->piece_count);
		if (p->bitfield)
			mbt_bitmap_set(p->bitfield, piece);
		if (p->cb.on_have)
			p->cb.on_have(p, piece, p->cb_arg);
		break;

	case BT_BITFIELD:
		{
		unsigned char *bfbuf = malloc(p->msglen);
		if (!bfbuf) return -ENOMEM;
		sz = mbt_conn_read(p->conn, bfbuf, p->msglen);
		mbt_bitmap_unref(p->bitfield);
		mbt_bitmap_fromarray(&p->bitfield, bfbuf, sz, p->piece_count);
		free(bfbuf);
		if (p->cb.on_bitfield)
			p->cb.on_bitfield(p, p->bitfield, p->cb_arg);
		break;
		}

	case BT_REQUEST:
		sz = mbt_conn_read(p->conn, buf, 12);
		mbt_unpack(buf, "lll", &piece, &offset, &req_len);
		if (p->cb.on_request)
			p->cb.on_request(p, piece, offset, req_len, p->cb_arg);
		break;

	case BT_PIECE: {
		unsigned char hdr[8];
		sz = mbt_conn_read(p->conn, hdr, 8);
		mbt_unpack(hdr, "ll", &piece, &offset);
		size_t data_len = p->msglen - 8;
		unsigned char *data = malloc(data_len);
		if (!data) return -ENOMEM;
		sz = mbt_conn_read(p->conn, data, data_len);
		if (p->cb.on_piece)
			p->cb.on_piece(p, piece, offset, data, data_len, p->cb_arg);
		free(data);
		break;
	}

	case BT_EXTENSION: {
		uint8_t extcmd;
		sz = mbt_conn_read(p->conn, &extcmd, 1);
		p->msglen -= 1;

		if (extcmd == 0) {
			/* extension handshake (can arrive again) */
			peer_recv_extension_handshake(p, p->msglen);
		} else if (p->ut_metadata_id && extcmd == 3) {
			/* ut_metadata message */
			unsigned char *mbuf = malloc(p->msglen);
			if (!mbuf) return -ENOMEM;
			sz = mbt_conn_read(p->conn, mbuf, p->msglen);

			_cleanup_be_ struct mbt_be_value *be = NULL;
			rv = mbt_be_value_parse(mbuf, sz, &be);
			if (rv > 0) {
				struct mbt_be_value *mt = mbt_be_find(be, "msg_type");
				struct mbt_be_value *pt = mbt_be_find(be, "piece");
				if (mt && mt->type == BE_INTEGER && pt && pt->type == BE_INTEGER) {
					if (mt->integer == 0) {
						/* Request — fire on_metadata_request */
						if (p->cb.on_metadata_request)
							p->cb.on_metadata_request(p, (int)pt->integer, p->cb_arg);
					} else if (mt->integer == 1) {
						/* Data response */
						size_t data_off = rv;
						if (p->cb.on_metadata)
							p->cb.on_metadata(p, (int)pt->integer,
							                  mbuf + data_off, sz - data_off, p->cb_arg);
					}
				}
			}
			free(mbuf);
		} else if (extcmd == 4) {
			/* ut_pex: peer exchange */
			unsigned char *pbuf = malloc(p->msglen);
			if (!pbuf) return -ENOMEM;
			sz = mbt_conn_read(p->conn, pbuf, p->msglen);

			_cleanup_be_ struct mbt_be_value *be = NULL;
			rv = mbt_be_value_parse(pbuf, sz, &be);
			if (rv > 0) {
				struct mbt_be_value *added = mbt_be_find(be, "added");
				if (added && added->type == BE_STRING && added->string.iov_len >= 6) {
					int count = (int)(added->string.iov_len / 6);
					uint8_t *d = added->string.iov_base;
					for (int i = 0; i < count; i++) {
						char addr[INET_ADDRSTRLEN];
						uint16_t port;
						inet_ntop(AF_INET, d + i * 6, addr, sizeof(addr));
						memcpy(&port, d + i * 6 + 4, 2);
						port = ntohs(port);
						if (p->cb.on_pex)
							p->cb.on_pex(p, addr, port, p->cb_arg);
					}
				}
			}
			free(pbuf);
		} else {
			/* unknown extension, discard */
			if (p->msglen > 0) {
				unsigned char discard[1024];
				size_t rem = p->msglen;
				while (rem > 0) {
					size_t chunk = rem < sizeof(discard) ? rem : sizeof(discard);
					mbt_conn_read(p->conn, discard, chunk);
					rem -= chunk;
				}
			}
		}
		break;
	}

	default:
		/* BEP 5: PORT message — peer's DHT port */
		if (cmd == BT_PORT && p->msglen >= 2) {
			uint8_t portbuf[2];
			mbt_conn_read(p->conn, portbuf, 2);
			p->msglen -= 2;
			uint16_t dht_port = (portbuf[0] << 8) | portbuf[1];
			if (p->cb.on_port)
				p->cb.on_port(p, dht_port, p->cb_arg);
			break;
		}
		/* BEP 6 Fast Extension messages */
		if (cmd == BT_HAVE_ALL && (p->flags & MBT_PEER_FLAG_FAST)) {
			/* HAVE_ALL — peer has everything */
			if (p->piece_count == 0) {
				p->have_all = 1; /* deferred until piece_count known */
				break;
			}
			if (p->bitfield)
				mbt_bitmap_unref(p->bitfield);
			mbt_bitmap_new_bits(&p->bitfield, p->piece_count);
			for (uint32_t i = 0; i < p->piece_count; i++)
				mbt_bitmap_set(p->bitfield, i);
			if (p->cb.on_bitfield)
				p->cb.on_bitfield(p, p->bitfield, p->cb_arg);
			break;
		}
		if (cmd == BT_HAVE_NONE && (p->flags & MBT_PEER_FLAG_FAST)) {
			/* HAVE_NONE — peer has nothing */
			if (p->piece_count == 0) break;
			if (p->bitfield)
				mbt_bitmap_unref(p->bitfield);
			mbt_bitmap_new_bits(&p->bitfield, p->piece_count);
			if (p->cb.on_bitfield)
				p->cb.on_bitfield(p, p->bitfield, p->cb_arg);
			break;
		}
		if (cmd == BT_REJECT && (p->flags & MBT_PEER_FLAG_FAST) && p->msglen >= 12) {
			/* REJECT_REQUEST — peer won't serve this block */
			unsigned char rj[12];
			mbt_conn_read(p->conn, rj, 12);
			p->msglen -= 12;
			uint32_t rpiece, roffset, rlen;
			mbt_unpack(rj, "lll", &rpiece, &roffset, &rlen);
			if (p->cb.on_reject)
				p->cb.on_reject(p, rpiece, roffset, rlen, p->cb_arg);
			break;
		}
		if (cmd == BT_ALLOWED_FAST && (p->flags & MBT_PEER_FLAG_FAST) && p->msglen >= 4) {
			/* ALLOWED_FAST — piece we can request while choked */
			unsigned char af[4];
			mbt_conn_read(p->conn, af, 4);
			p->msglen -= 4;
			/* Store or just log — for now ignore (we still request normally) */
			break;
		}

		/* unknown message, discard payload */
		if (p->msglen > 0) {
			unsigned char discard[1024];
			size_t rem = p->msglen;
			while (rem > 0) {
				size_t chunk = rem < sizeof(discard) ? rem : sizeof(discard);
				mbt_conn_read(p->conn, discard, chunk);
				rem -= chunk;
			}
		}
		break;
	}

	p->msglen = 0;
	mbt_conn_set_watermark(p->conn, 4);
	return 0;
}

/* --- Connection callbacks --- */

static void
peer_on_read(struct mbt_conn *c, void *v)
{
	struct mbt_peer *p = v;
	int rv;

	(void)c;


	if (p->state == MBT_PEER_CLOSED)
		return;

	switch (p->state) {
	case MBT_PEER_HANDSHAKE:
		rv = peer_recv_handshake(p);
		if (rv < 0) {
			mbt_log_debug(MBT_LOG_PEER, "%s:%hu: handshake failed: %s",
			              p->addr, p->port, strerror(-rv));
			peer_error(p, rv);
			return;
		}

		mbt_log_debug(MBT_LOG_PEER, "%s:%hu: handshake OK, flags=%s%s%s",
		              p->addr, p->port,
		              (p->flags & MBT_PEER_FLAG_DHT) ? "DHT " : "",
		              (p->flags & MBT_PEER_FLAG_FAST) ? "FAST " : "",
		              (p->flags & MBT_PEER_FLAG_EXTENSION) ? "EXT" : "");

		if (p->ext[5] & RESERVED_EXTENSION) {
			p->state = MBT_PEER_EXTENSION;
			peer_send_extension_handshake(p);
			mbt_conn_set_watermark(p->conn, 4);
		} else {
			p->state = MBT_PEER_ACTIVE;
			mbt_conn_set_watermark(p->conn, 4);
			if (p->cb.on_active)
				p->cb.on_active(p, p->cb_arg);
		}
		break;

	case MBT_PEER_EXTENSION: {
		/* Read the message, check if it's extension handshake */
		uint32_t length;
		uint8_t cmd, extcmd;
		unsigned char lenbuf[4];
		ssize_t sz;

		if (!p->msglen) {
			/* First pass: read length, set watermark for full message */
			sz = mbt_conn_read(p->conn, lenbuf, 4);
			if (sz != 4) { peer_error(p, -EIO); return; }
			mbt_unpack(lenbuf, "l", &length);

			if (length == 0) break; /* keepalive */

			if (length < 2) {
				unsigned char skip[16];
				if (length > 0)
					mbt_conn_read(p->conn, skip, length);
				p->state = MBT_PEER_ACTIVE;
				mbt_conn_set_watermark(p->conn, 4);
				if (p->cb.on_active)
					p->cb.on_active(p, p->cb_arg);
				break;
			}

			p->msglen = length;
			mbt_conn_set_watermark(p->conn, length);
			return; /* wait for full message */
		}

		/* Second pass: full message available */
		length = p->msglen;
		p->msglen = 0;

		sz = mbt_conn_read(p->conn, &cmd, 1);
		if (cmd != BT_EXTENSION) {
			/* Non-extension message — skip and go active */
			size_t rem = length - 1;
			while (rem > 0) {
				unsigned char discard[1024];
				size_t chunk = rem < sizeof(discard) ? rem : sizeof(discard);
				mbt_conn_read(p->conn, discard, chunk);
				rem -= chunk;
			}
			p->state = MBT_PEER_ACTIVE;
			mbt_conn_set_watermark(p->conn, 4);
			if (p->cb.on_active)
				p->cb.on_active(p, p->cb_arg);
			break;
		}

		sz = mbt_conn_read(p->conn, &extcmd, 1);
		if (extcmd == 0) {
			peer_recv_extension_handshake(p, length - 2);
		}

		p->state = MBT_PEER_ACTIVE;
		mbt_log_debug(MBT_LOG_PEER, "%s:%hu: ACTIVE (metadata_size=%zu reqq=%d)",
		              p->addr, p->port, p->metadata_size, p->reqq);
		mbt_conn_set_watermark(p->conn, 4);
		if (p->cb.on_active)
			p->cb.on_active(p, p->cb_arg);
		break;
	}

	case MBT_PEER_ACTIVE:
		rv = peer_rx_message(p);
		if (rv < 0) {
			peer_error(p, rv);
			return;
		}
		break;

	default:
		break;
	}
}

static void
peer_on_event(struct mbt_conn *c, int err, void *v)
{
	struct mbt_peer *p = v;
	(void)c;
	peer_error(p, err);
}

/* --- Public API --- */

int
mbt_peer_new(struct mbt_peer **out, struct mbt_ev *ev, struct mbt_conn *conn,
             struct mbt_infohash *ih, const uint8_t peer_id[20],
             uint32_t piece_count, enum mbt_peer_source source,
             struct mbt_peer_callbacks *cb, void *arg)
{
	struct mbt_peer *p;
	int rv;

	p = calloc(1, sizeof(*p));
	if (!p)
		return -ENOMEM;

	p->ev = ev;
	p->conn = conn;
	p->ih = mbt_infohash_ref(ih);
	memcpy(p->our_id, peer_id, 20);
	p->cb = *cb;
	p->cb_arg = arg;
	p->source = source;
	p->peer_choking = 1;
	p->am_choking = 1;
	p->piece_count = piece_count;
	p->state = MBT_PEER_HANDSHAKE;

	mbt_conn_peer_addr(conn, p->addr, sizeof(p->addr), &p->port);

	/* Point conn callbacks to this peer */
	mbt_conn_set_callbacks(p->conn, peer_on_read, peer_on_event, p);
	mbt_conn_set_watermark(p->conn, HANDSHAKE_SIZE);

	mbt_log_debug(MBT_LOG_PEER, "%s:%hu: sending handshake", p->addr, p->port);

	rv = peer_send_handshake(p);
	if (rv < 0) {
		free(p);
		return rv;
	}

	/* Keepalive timer */
	rv = mbt_ev_timer_new(ev, &p->timer, 120 * 1000000ULL, peer_keepalive_cb, p);
	if (rv < 0) {
		free(p);
		return rv;
	}

	*out = p;
	return 0;
}

int
mbt_peer_new_incoming(struct mbt_peer **out, struct mbt_ev *ev, struct mbt_conn *conn,
                      struct mbt_infohash *ih, const uint8_t peer_id[20],
                      uint32_t piece_count, enum mbt_peer_source source, const uint8_t ext[8], const uint8_t their_id[20],
                      size_t metadata_size,
                      struct mbt_peer_callbacks *cb, void *arg)
{
	struct mbt_peer *p;
	int rv;

	p = calloc(1, sizeof(*p));
	if (!p)
		return -ENOMEM;

	p->ev = ev;
	p->conn = conn;
	p->ih = mbt_infohash_ref(ih);
	memcpy(p->our_id, peer_id, 20);
	p->cb = *cb;
	p->cb_arg = arg;
	p->source = source;

	p->peer_choking = 1;
	p->am_choking = 1;
	p->piece_count = piece_count;

	memcpy(p->ext, ext, 8);
	memcpy(p->their_id, their_id, 20);
	p->flags = 0;
	if (ext[7] & 0x01) p->flags |= MBT_PEER_FLAG_DHT;
	if (ext[7] & 0x04) p->flags |= MBT_PEER_FLAG_FAST;
	if (ext[5] & 0x10) p->flags |= MBT_PEER_FLAG_EXTENSION;

	mbt_conn_peer_addr(conn, p->addr, sizeof(p->addr), &p->port);
	mbt_log_debug(MBT_LOG_PEER, "%s:%hu: incoming peer from %s", p->addr, p->port,
	              mbt_peer_source_str(source));
	mbt_conn_set_callbacks(p->conn, peer_on_read, peer_on_event, p);

	/* Set metadata size so extension handshake includes it */
	p->our_metadata_size = metadata_size;

	/* Send our handshake as response */
	rv = peer_send_handshake(p);
	if (rv < 0) { free(p); return rv; }

	/* Already past handshake — go to extension or active */
	if (p->flags & MBT_PEER_FLAG_EXTENSION) {
		p->state = MBT_PEER_EXTENSION;
		peer_send_extension_handshake(p);
		mbt_conn_set_watermark(p->conn, 4);
	} else {
		p->state = MBT_PEER_ACTIVE;
		mbt_conn_set_watermark(p->conn, 4);
		if (p->cb.on_active)
			p->cb.on_active(p, p->cb_arg);
	}

	rv = mbt_ev_timer_new(ev, &p->timer, 120 * 1000000ULL, peer_keepalive_cb, p);
	if (rv < 0) { free(p); return rv; }

	*out = p;
	return 0;
}

void
mbt_peer_set_our_metadata_size(struct mbt_peer *p, size_t sz)
{
	p->our_metadata_size = sz;
}

void
mbt_peer_set_our_reqq(struct mbt_peer *p, int reqq)
{
	p->our_reqq = reqq;
}

void
mbt_peer_set_piece_count(struct mbt_peer *p, uint32_t count)
{
	p->piece_count = count;
	/* Apply deferred HAVE_ALL */
	if (p->have_all && count > 0) {
		p->have_all = 0;
		if (p->bitfield)
			mbt_bitmap_unref(p->bitfield);
		mbt_bitmap_new_bits(&p->bitfield, count);
		for (uint32_t i = 0; i < count; i++)
			mbt_bitmap_set(p->bitfield, i);
		if (p->cb.on_bitfield)
			p->cb.on_bitfield(p, p->bitfield, p->cb_arg);
	}
}

void
mbt_peer_disconnect(struct mbt_peer *p)
{
	if (p && p->state != MBT_PEER_CLOSED)
		peer_error(p, -ECONNABORTED);
}

uint8_t
mbt_peer_ut_pex_id(struct mbt_peer *p)
{
	return p->ut_pex_id;
}
void
mbt_peer_free(struct mbt_peer *p)
{
	if (!p)
		return;

	if (p->timer)
		mbt_ev_timer_free(p->timer);
	if (p->defer)
		mbt_ev_timer_free(p->defer);
	if (p->conn)
		mbt_conn_free(p->conn);

	mbt_infohash_unref(p->ih);
	mbt_bitmap_unref(p->bitfield);
	free(p);
}

enum mbt_peer_state
mbt_peer_state(struct mbt_peer *p)
{
	return p->state;
}

int
mbt_peer_choked(struct mbt_peer *p)
{
	return p->peer_choking;
}

int
mbt_peer_interested(struct mbt_peer *p)
{
	return p->am_interested;
}

struct mbt_bitmap *
mbt_peer_bitfield(struct mbt_peer *p)
{
	return p->bitfield;
}

int
mbt_peer_has_extension(struct mbt_peer *p)
{
	return p->flags & MBT_PEER_FLAG_EXTENSION;
}

uint32_t
mbt_peer_flags(struct mbt_peer *p)
{
	return p->flags;
}

size_t
mbt_peer_metadata_size(struct mbt_peer *p)
{
	return p->metadata_size;
}

int
mbt_peer_reqq(struct mbt_peer *p)
{
	return p->reqq;
}

const char *
mbt_peer_addr(struct mbt_peer *p)
{
	return p->addr;
}

const uint8_t *
mbt_peer_id(struct mbt_peer *p)
{
	return p->their_id;
}
enum mbt_peer_source
mbt_peer_source(struct mbt_peer *p)
{
	return p->source;
}
const char *
mbt_peer_source_str(enum mbt_peer_source source)
{
	switch (source) {
	case MBT_SOURCE_TRACKER: return "tracker";
	case MBT_SOURCE_DHT: return "dht";
	case MBT_SOURCE_PEX: return "pex";
	case MBT_SOURCE_LSD: return "lsd";
	case MBT_SOURCE_INCOMING: return "incoming";
	case MBT_SOURCE_MANUAL: return "manual";
	case MBT_SOURCE_MAGNET: return "magnet";
	default: return "unknown";
	}
}
uint16_t
mbt_peer_port(struct mbt_peer *p)
{
	return p->port;
}

/* --- Send commands --- */

int
mbt_peer_send_interested(struct mbt_peer *p, int interested)
{
	uint8_t msg = interested ? 2 : 3;
	unsigned char buf[5];
	mbt_pack(buf, "lb", (uint32_t)1, (int)msg);
	p->am_interested = interested;
	return peer_write(p, buf, 5) < 0 ? -EIO : 0;
}

int
mbt_peer_send_choke(struct mbt_peer *p, int choke)
{
	uint8_t msg = choke ? 0 : 1;
	unsigned char buf[5];
	mbt_pack(buf, "lb", (uint32_t)1, (int)msg);
	p->am_choking = choke;
	return peer_write(p, buf, 5) < 0 ? -EIO : 0;
}

int
mbt_peer_send_request(struct mbt_peer *p, uint32_t piece, uint32_t offset, uint32_t length)
{
	unsigned char buf[1 + 4 + 4 + 4];
	int rv = mbt_pack(buf, "blll", (int)BT_REQUEST, piece, offset, length);
	return peer_write_msg(p, buf, rv);
}

int
mbt_peer_send_cancel(struct mbt_peer *p, uint32_t piece, uint32_t offset, uint32_t length)
{
	unsigned char buf[1 + 4 + 4 + 4];
	int rv = mbt_pack(buf, "blll", (int)8, piece, offset, length);
	return peer_write_msg(p, buf, rv);
}

int
mbt_peer_send_have(struct mbt_peer *p, uint32_t piece)
{
	unsigned char buf[1 + 4];
	int rv = mbt_pack(buf, "bl", (int)BT_HAVE, piece);
	return peer_write_msg(p, buf, rv);
}

int
mbt_peer_send_port(struct mbt_peer *p, uint16_t port)
{
	unsigned char buf[3];
	buf[0] = BT_PORT;
	buf[1] = port >> 8;
	buf[2] = port & 0xff;
	return peer_write_msg(p, buf, 3);
}

int
mbt_peer_send_bitfield(struct mbt_peer *p, struct mbt_bitmap *bm)
{
	size_t bytes = mbt_bitmap_bytes(bm);
	unsigned char *buf = malloc(1 + bytes);
	if (!buf) return -ENOMEM;
	buf[0] = BT_BITFIELD;
	mbt_bitmap_raw(bm, buf + 1, bytes);
	int rv = peer_write_msg(p, buf, 1 + bytes);
	free(buf);
	return rv;
}

int
mbt_peer_send_msg(struct mbt_peer *p, const void *buf, size_t len)
{
	return peer_write_msg(p, buf, len);
}

int
mbt_peer_send_piece(struct mbt_peer *p, uint32_t piece, uint32_t offset,
                    const void *data, size_t len)
{
	unsigned char hdr[4 + 1 + 4 + 4];
	uint32_t msg_len = 1 + 4 + 4 + len;
	mbt_pack(hdr, "lbll", msg_len, (int)BT_PIECE, piece, offset);
	if (mbt_conn_write(p->conn, hdr, sizeof(hdr)) < 0)
		return -EIO;
	if (mbt_conn_write(p->conn, data, len) < 0)
		return -EIO;
	return 0;
}

int
mbt_peer_send_metadata_request(struct mbt_peer *p, int piece)
{
	_cleanup_be_ struct mbt_be_value *v = NULL;
	unsigned char *encoded;
	unsigned char packed[64];
	size_t sz;
	int rv;

	if (!p->ut_metadata_id)
		return -ENOTSUP;

	if (mbt_be_dict_new(&v) < 0) return -ENOMEM;
	if (mbt_be_dict_append_number(v, "msg_type", 0) < 0) return -ENOMEM;
	if (mbt_be_dict_append_number(v, "piece", piece) < 0) return -ENOMEM;

	rv = mbt_be_value_encode(v, &encoded, &sz);
	if (rv < 0)
		return rv;

	rv = mbt_pack(packed, "bb*", (int)BT_EXTENSION, (int)p->ut_metadata_id, sz, encoded);
	free(encoded);
	if (rv < 0)
		return rv;

	return peer_write_msg(p, packed, rv);
}

int
mbt_peer_send_metadata_data(struct mbt_peer *p, int piece, size_t total_size,
                            const void *data, size_t len)
{
	_cleanup_be_ struct mbt_be_value *v = NULL;
	unsigned char *encoded;
	size_t sz;
	int rv;

	if (!p->ut_metadata_id)
		return -ENOTSUP;

	if (mbt_be_dict_new(&v) < 0) return -ENOMEM;
	if (mbt_be_dict_append_number(v, "msg_type", 1) < 0) return -ENOMEM;
	if (mbt_be_dict_append_number(v, "piece", piece) < 0) return -ENOMEM;
	if (mbt_be_dict_append_number(v, "total_size", (int64_t)total_size) < 0) return -ENOMEM;

	rv = mbt_be_value_encode(v, &encoded, &sz);
	if (rv < 0)
		return rv;

	/* Message: ext_id(1) + ut_metadata_id(1) + bencode + raw data */
	size_t msglen = 2 + sz + len;
	unsigned char *msg = malloc(msglen);
	if (!msg) { free(encoded); return -ENOMEM; }
	msg[0] = BT_EXTENSION;
	msg[1] = p->ut_metadata_id;
	memcpy(msg + 2, encoded, sz);
	memcpy(msg + 2 + sz, data, len);
	free(encoded);

	rv = peer_write_msg(p, msg, msglen);
	free(msg);
	return rv;
}
