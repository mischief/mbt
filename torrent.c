// SPDX-License-Identifier: ISC
#include <sys/param.h>
#include <sys/queue.h>
#include <sys/socket.h>
#include <arpa/inet.h>
#include <stdlib.h>
#include <string.h>
#include <errno.h>
#include <arpa/inet.h>
#include <time.h>

#include "mbt/sha1_compat.h"

#include "mbt/log.h"

#include "mbt/event.h"
#include "mbt/conn.h"
#include "mbt/infohash.h"
#include "mbt/peer.h"
#include "mbt/torrent.h"
#include "mbt/session.h"
#include "mbt/ratelimit.h"
#include "mbt/bitmap.h"
#include "mbt/peerdb.h"
#include "mbt/be.h"
#include "mbt/hex.h"
#include "internal.h"
#include "mbt/picker.h"

enum {
	BLOCK_SIZE = 16384,
	METADATA_BLOCK_SIZE = 16384,
	MAX_PEERS = 50,
	PIPELINE_DEPTH = 512,
};

/* Per-piece tracking */
struct piece_info {
	uint8_t *data;
	size_t length;
	size_t received;       /* bytes received so far */
	size_t requested;      /* bytes requested so far (>= received) */
	uint8_t hash[20];      /* expected SHA1 */
	struct mbt_bitmap *blocks; /* received block bitmap */
	enum mbt_piece_state state;
	struct torrent_peer *owner; /* peer downloading this piece, NULL if pending */
	uint64_t last_activity; /* monotonic time of last block received (usec) */
};

/* Per-peer entry in the torrent's peer list */
struct torrent_peer {
	SLIST_ENTRY(torrent_peer) entry;
	struct mbt_peer *peer;
	int outstanding;       /* in-flight requests */
	int hash_failures;     /* consecutive hash failures from this peer */
	int banned;
	int snubbed;           /* peer not sending data, skip in drive_peer */
	uint64_t bytes_from;   /* bytes received from peer in current interval */
	uint64_t bytes_to;     /* bytes sent to peer in current interval */
	int peer_interested;   /* peer wants data from us */
	int am_choking;        /* we are choking this peer */
	time_t last_recv;      /* last time we received piece data */
};

struct mbt_torrent {
	struct mbt_ev *ev;
	struct mbt_infohash *ih;
	struct mbt_session *session; /* owning session (NULL if standalone) */
	uint8_t peer_id[20];

	const struct mbt_storage_ops *storage_ops;
	void *storage_ctx;
	mbt_picker_fn picker;
	void *picker_ctx;

	struct mbt_torrent_callbacks cb;
	void *cb_arg;

	/* Metadata (info dict) */
	uint8_t *metadata;
	size_t metadata_len;
	size_t metadata_have;  /* bytes received */
	int metadata_pieces;   /* total metadata pieces expected */

	/* Piece map (populated after metadata) */
	struct piece_info *pieces;
	uint32_t piece_count;
	uint32_t piece_length;
	uint64_t total_length;
	char *name;

	/* File table */
	struct mbt_file *files;
	int file_count;

	/* Have bitmap */
	struct mbt_bitmap *have;
	uint32_t have_count;

	/* Peers */
	SLIST_HEAD(, torrent_peer) peers;
	int peer_count;

	/* Peer database (owned by torrent) */
	struct mbt_peerdb peerdb;

	/* Request timeout */
	struct mbt_ev_timer *timeout_timer;
	struct mbt_ev_timer *choke_timer;
	struct mbt_ev_timer *pex_timer;
	int choke_round;  /* increments every 10s, optimistic every 3rd round (30s) */
	struct mbt_timers timers;

	/* Endgame mode */
	int endgame;  /* 1 when all pieces are requested at least once */
	int is_private; /* BEP 27: no DHT/PEX/LSD for this torrent */
	int max_pieces_per_peer;

	/* Rate limiting */
	struct mbt_ratelimit *upload_limit;
	struct mbt_ratelimit *download_limit;
	struct mbt_ev_timer *ratelimit_timer;
};

enum {
	MAX_HASH_FAILURES = 5,   /* ban after this many consecutive failures */
	ENDGAME_THRESHOLD = 10,  /* enter endgame with this many pieces remaining */
};

/* --- Forward declarations --- */
static void torrent_drive_peer(struct mbt_torrent *t, struct torrent_peer *tp);
static void torrent_ratelimit_retry(struct mbt_ev_timer *timer, void *arg);
static void torrent_request_metadata(struct mbt_torrent *t, struct torrent_peer *tp);

static void
torrent_deferred_peer_free(struct mbt_ev_timer *timer, void *v)
{
	struct torrent_peer *tp = v;
	mbt_ev_timer_free(timer);
	if (tp->peer)
		mbt_peer_free(tp->peer);
	free(tp);
}

static uint64_t
now_usec(void)
{
	struct timespec ts;
	clock_gettime(CLOCK_MONOTONIC, &ts);
	return (uint64_t)ts.tv_sec * 1000000 + ts.tv_nsec / 1000;
}

/* --- Request timeout --- */

static void
torrent_timeout_cb(struct mbt_ev_timer *timer, void *v)
{
	struct mbt_torrent *t = v;
	uint64_t now = now_usec();
	uint64_t deadline = (uint64_t)t->timers.request_timeout * 1000000;
	int reset_count = 0;

	if (!t->pieces)
		goto reschedule;

	for (uint32_t i = 0; i < t->piece_count; i++) {
		struct piece_info *pi = &t->pieces[i];
		if (pi->state != MBT_PIECE_ACTIVE)
			continue;

		/* Reset if: owner is gone/banned/choked, or timed out */
		int stale = 0;
		if (!pi->owner || !pi->owner->peer || pi->owner->banned)
			stale = 1;
		else if (mbt_peer_choked(pi->owner->peer))
			stale = 1;
		else if (pi->last_activity && now - pi->last_activity >= deadline)
			stale = 1;

		if (!stale)
			continue;

		/* Timed out — just reset the piece, don't blame the peer.
		 * Timeouts happen naturally during choke/unchoke rotation.
		 * Only hash mismatches should lead to banning. */

		pi->state = MBT_PIECE_PENDING;
		pi->owner = NULL;
		pi->received = 0;
			if (pi->blocks) { mbt_bitmap_unref(pi->blocks); pi->blocks = NULL; }
		pi->requested = 0;
		pi->last_activity = 0;
		free(pi->data);
		pi->data = NULL;
		reset_count++;
	}

	/* Drive all unchoked peers if we freed up pieces */
	if (reset_count > 0) {
		struct torrent_peer *tp;
		SLIST_FOREACH(tp, &t->peers, entry) {
			if (!tp->banned && !mbt_peer_choked(tp->peer))
				torrent_drive_peer(t, tp);
		}
	}

	/* Anti-snubbing: mark peers with no data received for snub_timeout */
	{
		time_t now_t = time(NULL);
		struct torrent_peer *tp;
		SLIST_FOREACH(tp, &t->peers, entry) {
			if (tp->banned || tp->snubbed || !tp->peer)
				continue;
			if (tp->outstanding > 0 &&
			    now_t - tp->last_recv >= t->timers.snub_timeout) {
				tp->snubbed = 1;
				mbt_log_debug(MBT_LOG_TORRENT, "%s:%hu: snubbed (no data for %ds)",
				              mbt_peer_addr(tp->peer), mbt_peer_port(tp->peer),
				              t->timers.snub_timeout);
			}
		}
	}

reschedule:
	mbt_ev_timer_mod(timer, 5 * 1000000ULL); /* check every 5s */
}

/* --- Peer callbacks --- */

static struct torrent_peer *
find_torrent_peer(struct mbt_torrent *t, struct mbt_peer *p)
{
	struct torrent_peer *tp;
	SLIST_FOREACH(tp, &t->peers, entry)
		if (tp->peer == p)
			return tp;
	return NULL;
}

/* --- PEX send (BEP 11) — broadcast connected peers every 60s --- */

static void
torrent_pex_cb(struct mbt_ev_timer *timer, void *v)
{
	struct mbt_torrent *t = v;

	if (t->is_private) goto done; /* BEP 27: no PEX for private torrents */

	/* Build compact IPv4 "added" list from all connected peers */
	uint8_t added[50 * 6]; /* max 50 peers per BEP 11 */
	int count = 0;
	struct torrent_peer *tp;
	for (struct torrent_peer *tp = SLIST_FIRST(&t->peers); tp && count < 50; tp = SLIST_NEXT(tp, entry)) {
		if (!tp->peer) continue;
		const char *addr = mbt_peer_addr(tp->peer);
		uint16_t port = mbt_peer_port(tp->peer);
		struct in_addr in;
		if (inet_pton(AF_INET, addr, &in) <= 0) continue;
		memcpy(added + count * 6, &in, 4);
		uint16_t nport = htons(port);
		memcpy(added + count * 6 + 4, &nport, 2);
		count++;
	}

	if (count == 0) goto done;

	/* Build bencoded PEX message: d5:added<compact>e */
	struct mbt_be_value *msg = NULL;
	mbt_be_dict_new(&msg);
	mbt_be_dict_append_string(msg, "added", added, count * 6);

	unsigned char *enc; size_t enc_len;
	if (mbt_be_value_encode(msg, &enc, &enc_len) < 0) {
		mbt_be_value_free(msg);
		goto done;
	}
	mbt_be_value_free(msg);

	/* Send to all peers that support ut_pex (ext id 4 = our local id) */
	unsigned char *pex_msg = malloc(2 + enc_len);
	if (!pex_msg) { free(enc); goto done; }
	pex_msg[0] = 20;
	pex_msg[1] = 4; /* remote's ut_pex id — but we need THEIR id, not ours */

	/* Actually: we need each peer's ut_pex_id. Use mbt_peer_send_msg with the
	 * extension header. For simplicity, build per-peer: */
	free(pex_msg);

	SLIST_FOREACH(tp, &t->peers, entry) {
		if (!tp->peer) continue;
		uint8_t remote_pex_id = mbt_peer_ut_pex_id(tp->peer);
		if (!remote_pex_id) continue;
		unsigned char hdr[2] = { BT_EXTENSION, remote_pex_id };
		size_t total = 2 + enc_len;
		unsigned char *buf = malloc(total);
		if (!buf) continue;
		memcpy(buf, hdr, 2);
		memcpy(buf + 2, enc, enc_len);
		mbt_peer_send_msg(tp->peer, buf, total);
		free(buf);
	}
	free(enc);

done:
	mbt_ev_timer_mod(timer, (uint64_t)t->timers.pex_interval * 1000000ULL);
}

/* --- Choking algorithm (BEP 3) --- */

#define UNCHOKE_SLOTS 4

static void
torrent_choke_cb(struct mbt_ev_timer *timer, void *v)
{
	struct mbt_torrent *t = v;
	t->choke_round++;

	/* Count interested peers */
	struct torrent_peer *tp;
	struct torrent_peer *ranked[128];
	int n = 0;
	for (tp = SLIST_FIRST(&t->peers); tp && n < 128; tp = SLIST_NEXT(tp, entry))
		if (tp->peer && tp->peer_interested)
			ranked[n++] = tp;

	/* Sort by bytes_from (descending) — reward uploaders */
	for (int i = 0; i < n - 1; i++)
		for (int j = i + 1; j < n; j++)
			if (ranked[j]->bytes_from > ranked[i]->bytes_from) {
				struct torrent_peer *tmp = ranked[i];
				ranked[i] = ranked[j]; ranked[j] = tmp;
			}

	/* Unchoke top UNCHOKE_SLOTS */
	for (int i = 0; i < n; i++) {
		int should_unchoke = (i < UNCHOKE_SLOTS);

		/* Optimistic unchoke: every 3rd round (30s), pick one random from the rest */
		if (!should_unchoke && (t->choke_round % 3 == 0) && i == UNCHOKE_SLOTS)
			should_unchoke = 1;

		if (should_unchoke && ranked[i]->am_choking) {
			mbt_peer_send_choke(ranked[i]->peer, 0);
			ranked[i]->am_choking = 0;
		} else if (!should_unchoke && !ranked[i]->am_choking) {
			mbt_peer_send_choke(ranked[i]->peer, 1);
			ranked[i]->am_choking = 1;
		}
	}

	/* Reset counters for next interval */
	SLIST_FOREACH(tp, &t->peers, entry) {
		tp->bytes_from = 0;
		tp->bytes_to = 0;
	}

	mbt_ev_timer_mod(timer, (uint64_t)t->timers.choke_interval * 1000000ULL);
}

static void
on_active(struct mbt_peer *p, void *arg)
{
	struct mbt_torrent *t = arg;
	struct torrent_peer *tp = find_torrent_peer(t, p);
	if (!tp) return;

	mbt_log_debug(MBT_LOG_TORRENT, "on_active: metadata=%p metadata_size=%zu",
	              (void *)t->metadata, mbt_peer_metadata_size(p));

	/* Send bitfield if we have pieces */
	if (t->have && mbt_bitmap_count(t->have) > 0)
		mbt_peer_send_bitfield(p, t->have);

	/* Start unchoked — the choke timer will regulate later */
	struct torrent_peer *tp2 = find_torrent_peer(t, p);
	if (tp2) tp2->am_choking = 0;
	mbt_peer_send_choke(p, 0);

	/* Always express interest */
	mbt_peer_send_interested(p, 1);

	/* BEP 5: advertise our DHT port if DHT is active */
	if (t->session && (mbt_peer_flags(p) & MBT_PEER_FLAG_DHT)) {
		uint16_t dht_port = mbt_session_dht_port(t->session);
		if (dht_port)
			mbt_peer_send_port(p, dht_port);
	}

	/* If we need metadata, request it */
	if (mbt_peer_metadata_size(p) > 0 && t->piece_count == 0) {
		if (!t->metadata) {
			t->metadata_len = mbt_peer_metadata_size(p);
			t->metadata = calloc(1, t->metadata_len);
			t->metadata_pieces = (t->metadata_len + METADATA_BLOCK_SIZE - 1) / METADATA_BLOCK_SIZE;
		}
		torrent_request_metadata(t, tp);
	}
}

static void
on_choke(struct mbt_peer *p, int choked, void *arg)
{
	struct mbt_torrent *t = arg;
	struct torrent_peer *tp = find_torrent_peer(t, p);
	if (!tp) return;
	struct torrent_peer *op;

	struct mbt_bitmap *pb = mbt_peer_bitfield(p);
	mbt_log_debug(MBT_LOG_TORRENT, "%s:%hu: %s (peer has %zu/%u pieces)",
	              mbt_peer_addr(p), mbt_peer_port(p),
	              choked ? "choked us" : "unchoked us",
	              pb ? mbt_bitmap_count(pb) : 0, t->piece_count);

	if (!choked && t->pieces)
		torrent_drive_peer(t, tp);

	/* When choked, release pieces owned by this peer so others can take them */
	if (choked && t->pieces) {
		for (uint32_t i = 0; i < t->piece_count; i++) {
			struct piece_info *pi = &t->pieces[i];
			if (pi->owner == tp && pi->state == MBT_PIECE_ACTIVE) {
				pi->state = MBT_PIECE_PENDING;
				pi->owner = NULL;
				pi->requested = 0;
				pi->received = 0;
				if (pi->blocks) { mbt_bitmap_unref(pi->blocks); pi->blocks = NULL; }
				pi->last_activity = 0;
			}
		}
		tp->outstanding = 0;
		/* Drive other unchoked peers to pick up freed pieces */
		SLIST_FOREACH(op, &t->peers, entry)
			if (op != tp && !mbt_peer_choked(op->peer))
				torrent_drive_peer(t, op);
	}
}

static void
on_have(struct mbt_peer *p, uint32_t piece, void *arg)
{
	struct mbt_torrent *t = arg;
	struct torrent_peer *tp = find_torrent_peer(t, p);
	if (!tp) return;

	/* If we're unchoked and need this piece, maybe request it */
	if (!mbt_peer_choked(p) && t->pieces)
		torrent_drive_peer(t, tp);
}

static void
on_bitfield(struct mbt_peer *p, struct mbt_bitmap *bits, void *arg)
{
	struct mbt_torrent *t = arg;
	struct torrent_peer *tp = find_torrent_peer(t, p);
	if (!tp) return;

	mbt_log_debug(MBT_LOG_TORRENT, "%s:%hu: bitfield %zu/%u pieces",
	              mbt_peer_addr(p), mbt_peer_port(p),
	              mbt_bitmap_count(bits), t->piece_count);

	if (!mbt_peer_choked(p) && t->pieces)
		torrent_drive_peer(t, tp);
}

static void
on_piece(struct mbt_peer *p, uint32_t piece, uint32_t offset,
         const void *data, size_t len, void *arg)
{
	struct mbt_torrent *t = arg;
	struct torrent_peer *tp = find_torrent_peer(t, p);
	if (!tp) return;
	struct torrent_peer *hp;

	/* Download tokens consumed at request time (drive_peer) */

	tp->last_recv = time(NULL);
	if (tp->snubbed) {
		tp->snubbed = 0;
		mbt_log_debug(MBT_LOG_TORRENT, "%s:%hu: un-snubbed",
		              mbt_peer_addr(p), mbt_peer_port(p));
	}

	if (piece >= t->piece_count)
		return;

	/* Already have this piece — ignore late/unsolicited data */
	if (mbt_bitmap_isset(t->have, piece) > 0) {
		struct torrent_peer *tp2 = find_torrent_peer(t, p);
		if (tp2) if (tp2->outstanding > 0) tp2->outstanding--;
		return;
	}

	struct piece_info *pi = &t->pieces[piece];

	if (mbt_bitmap_isset(t->have, piece) > 0)
		return; /* duplicate */

	if (offset + len > pi->length)
		return; /* out of bounds */

	/* Allocate piece buffer on first block */
	if (!pi->data) {
		/* Only accept if piece is ACTIVE and owned by this peer */
		if (pi->state != MBT_PIECE_ACTIVE || pi->owner != tp) {
			if (tp->outstanding > 0) tp->outstanding--;
			return;
		}
		pi->data = calloc(1, pi->length);
		if (!pi->data) return;
	}

	/* Track block by index, ignore true duplicates */
	uint32_t block_idx = offset / (16 * 1024);
	if (pi->blocks && mbt_bitmap_isset(pi->blocks, block_idx) > 0)
		{ if (tp->outstanding > 0) tp->outstanding--; return; } /* duplicate block */

	memcpy(pi->data + offset, data, len);
	if (!pi->blocks) {
		uint32_t nblocks = (pi->length + 16*1024 - 1) / (16*1024);
		mbt_bitmap_new_bits(&pi->blocks, nblocks);
	}
	mbt_bitmap_set(pi->blocks, block_idx);
	pi->received += len;
	tp->bytes_from += len;
	pi->last_activity = now_usec();

	if (tp->outstanding > 0) tp->outstanding--;

	/* Piece complete? */
	if (pi->received >= pi->length) {
		/* SHA1 verify */
		SHA1_CTX ctx;
		uint8_t digest[20];

		SHA1Init(&ctx);
		SHA1Update(&ctx, pi->data, pi->length);
		SHA1Final(digest, &ctx);

		if (memcmp(digest, pi->hash, 20) != 0) {
			/* Hash mismatch — reset piece, blame peer */
			char got[41], want[41];
			mbt_hex_encode(digest, 20, got, sizeof(got));
			mbt_hex_encode(pi->hash, 20, want, sizeof(want));
			mbt_log_warn(MBT_LOG_TORRENT, "piece %u: hash mismatch from %s:%hu",
			             piece, mbt_peer_addr(p), mbt_peer_port(p));
			mbt_log_debug(MBT_LOG_TORRENT, "  got=%s want=%s len=%zu received=%zu blocks=0x%x",
			              got, want, pi->length, pi->received, pi->blocks ? mbt_bitmap_count(pi->blocks) : 0);
			pi->received = 0;
			if (pi->blocks) { mbt_bitmap_unref(pi->blocks); pi->blocks = NULL; }
			pi->requested = 0;
			pi->state = MBT_PIECE_PENDING;
			pi->owner = NULL;
			pi->last_activity = 0;
			pi->data = NULL;

			/* Ban peer after repeated failures */
			tp->hash_failures++;
			if (tp->hash_failures >= MAX_HASH_FAILURES) {
				mbt_log_warn(MBT_LOG_TORRENT, "banning %s:%hu after %d hash failures",
				             mbt_peer_addr(p), mbt_peer_port(p), tp->hash_failures);
				tp->banned = 1;
				mbt_peer_disconnect(p); /* triggers deferred on_error → cleanup */
			}
			/* Piece is now PENDING again — drive all peers to pick it up */
			struct torrent_peer *rtp;
			SLIST_FOREACH(rtp, &t->peers, entry) {
				if (rtp->peer && !rtp->banned && !mbt_peer_choked(rtp->peer))
					torrent_drive_peer(t, rtp);
			}
			return;
		} else {
			/* Verified — store */
			mbt_bitmap_set(t->have, piece);
			t->have_count++;
			pi->state = MBT_PIECE_PENDING;
			pi->owner = NULL;

			/* Reset failure count on success */
			tp->hash_failures = 0;

			if (t->storage_ops && t->storage_ops->write_piece)
				t->storage_ops->write_piece(t->storage_ctx, piece, pi->data, pi->length);

			/* Free piece buffer */
			free(pi->data);
			pi->data = NULL;
			if (pi->blocks) { mbt_bitmap_unref(pi->blocks); pi->blocks = NULL; }

			mbt_log_debug(MBT_LOG_TORRENT, "piece %u verified from %s:%hu",
			             piece, mbt_peer_addr(p), mbt_peer_port(p));

			/* Broadcast HAVE to all connected peers */
			SLIST_FOREACH(hp, &t->peers, entry)
				mbt_peer_send_have(hp->peer, piece);

			if (t->cb.on_piece_complete)
				t->cb.on_piece_complete(t, piece, t->cb_arg);

			/* Endgame: cancel this piece from all other peers */
			if (t->endgame) {
				struct torrent_peer *otp;
				SLIST_FOREACH(otp, &t->peers, entry) {
					if (otp == tp || !otp->peer || otp->banned)
						continue;
					for (size_t off = 0; off < pi->length; off += BLOCK_SIZE) {
						uint32_t clen = MIN(BLOCK_SIZE, pi->length - off);
						mbt_peer_send_cancel(otp->peer, piece, off, clen);
					}
				}
			}

			/* Check completion */
			if (t->have_count == t->piece_count) {
				if (t->cb.on_complete)
					t->cb.on_complete(t, t->cb_arg);
				return;
			}

			/* Drive peers to pick up new pieces */
			if (t->download_limit || mbt_session_get_download_limit(t->session)) {
				/* Rate-limited: only drive this peer; retry timer handles others */
				torrent_drive_peer(t, tp);
			} else {
				struct torrent_peer *otp;
				SLIST_FOREACH(otp, &t->peers, entry) {
					if (otp->peer && !otp->banned && !mbt_peer_choked(otp->peer))
						torrent_drive_peer(t, otp);
				}
			}
			return;
		}
	}

	/* Keep requesting */
	/* Keep requesting */
	torrent_drive_peer(t, tp);
}

static void
on_metadata_request(struct mbt_peer *p, int piece, void *arg)
{
	struct mbt_torrent *t = arg;
	mbt_log_debug(MBT_LOG_TORRENT, "on_metadata_request piece=%d metadata=%p len=%zu",
	              piece, (void*)t->metadata, t->metadata_len);
	if (!t->metadata || t->metadata_len == 0)
		return;

	size_t offset = (size_t)piece * METADATA_BLOCK_SIZE;
	if (offset >= t->metadata_len)
		return;
	size_t len = t->metadata_len - offset;
	if (len > METADATA_BLOCK_SIZE)
		len = METADATA_BLOCK_SIZE;

	mbt_peer_send_metadata_data(p, piece, t->metadata_len,
	                            t->metadata + offset, len);
}

static void
on_metadata(struct mbt_peer *p, int piece, const void *data, size_t len, void *arg)
{
	struct mbt_torrent *t = arg;

	if (!t->metadata)
		return;

	size_t offset = (size_t)piece * METADATA_BLOCK_SIZE;
	if (offset >= t->metadata_len)
		return;

	size_t copy_len = MIN(len, t->metadata_len - offset);
	memcpy(t->metadata + offset, data, copy_len);
	t->metadata_have += copy_len;

	/* All metadata received? */
	if (t->metadata_have >= t->metadata_len) {
		/* Verify SHA1 matches infohash */
		SHA1_CTX ctx;
		uint8_t digest[20];

		SHA1Init(&ctx);
		SHA1Update(&ctx, t->metadata, t->metadata_len);
		SHA1Final(digest, &ctx);

		/* Compare with our infohash */
		if (memcmp(digest, t->ih->hash_sha1, 20) != 0) {
			mbt_log_warn(MBT_LOG_TORRENT, "metadata hash mismatch, discarding");
			t->metadata_have = 0;
			memset(t->metadata, 0, t->metadata_len);
			return;
		}

		/* Parse and set metadata */
		mbt_log_info(MBT_LOG_TORRENT, "metadata complete from %s:%hu (%zu bytes)",
		             mbt_peer_addr(p), mbt_peer_port(p), t->metadata_len);
		mbt_torrent_set_metadata(t, t->metadata, t->metadata_len);

		if (t->pieces)
			mbt_log_info(MBT_LOG_TORRENT, "name=\"%s\" pieces=%u piece_length=%u total=%llu",
			             t->name ? t->name : "?", t->piece_count,
			             t->piece_length, (unsigned long long)t->total_length);

		/* Drive all peers now that we have metadata */
		struct torrent_peer *tp;
		SLIST_FOREACH(tp, &t->peers, entry) {
			/* Update piece_count — applies deferred HAVE_ALL */
			mbt_peer_set_piece_count(tp->peer, t->piece_count);
			/* Re-send interested in case it was missed */
			mbt_peer_send_interested(tp->peer, 1);
			if (!mbt_peer_choked(tp->peer))
				torrent_drive_peer(t, tp);
		}
	}
}

static void
on_interest(struct mbt_peer *p, int interested, void *arg)
{
	struct mbt_torrent *t = arg;
	struct torrent_peer *tp = find_torrent_peer(t, p);
	if (tp) tp->peer_interested = interested;
}

static void
on_request(struct mbt_peer *p, uint32_t piece, uint32_t offset, uint32_t length, void *arg)
{
	struct mbt_torrent *t = arg;

	/* Only serve if we have the piece */
	if (mbt_bitmap_isset(t->have, piece) <= 0)
		return;

	if (!t->storage_ops || !t->storage_ops->read_piece)
		return;

	/* Read full piece from storage, then send the requested block */
	uint32_t plen = t->piece_length;
	if (piece == t->piece_count - 1) {
		uint32_t rem = t->total_length % t->piece_length;
		if (rem) plen = rem;
	}
	if (offset + length > plen)
		return;

	uint8_t *buf = malloc(plen);
	if (!buf)
		return;

	int rv = t->storage_ops->read_piece(t->storage_ctx, piece, buf, plen);
	if (rv < 0) {
		free(buf);
		return;
	}

	/* Check upload rate limits (per-torrent and session) */
	if (mbt_ratelimit_delay(t->upload_limit, length) > 0 ||
	    mbt_ratelimit_delay(mbt_session_get_upload_limit(t->session), length) > 0) {
		free(buf);
		return; /* drop request — peer will re-request */
	}
	mbt_ratelimit_consume(t->upload_limit, length);
	mbt_ratelimit_consume(mbt_session_get_upload_limit(t->session), length);

	mbt_peer_send_piece(p, piece, offset, buf + offset, length);
	free(buf);
}

static void
on_pex(struct mbt_peer *p, const char *addr, uint16_t port, void *arg)
{
	(void)p;
	struct mbt_torrent *t = arg;
	if (t->is_private) return; /* BEP 27: no PEX for private torrents */
	/* Insert into peerdb */
	mbt_peerdb_insert(&t->peerdb, addr, port, MBT_SOURCE_PEX);
	mbt_log_debug(MBT_LOG_TORRENT, "PEX: %s:%hu", addr, port);
}

static void
on_reject(struct mbt_peer *p, uint32_t piece, uint32_t offset, uint32_t length, void *arg)
{
	struct mbt_torrent *t = arg;
	(void)offset; (void)length;
	struct torrent_peer *op;

	if (piece >= t->piece_count) return;
	struct piece_info *pi = &t->pieces[piece];

	/* Reset piece if this peer owned it */
	struct torrent_peer *tp = find_torrent_peer(t, p);
	if (tp && pi->owner == tp) {
		pi->state = MBT_PIECE_PENDING;
		pi->owner = NULL;
		pi->requested = 0;
		pi->received = 0;
			if (pi->blocks) { mbt_bitmap_unref(pi->blocks); pi->blocks = NULL; }
		if (tp->outstanding > 0) tp->outstanding--;
		/* Try to re-request from someone else */
		SLIST_FOREACH(op, &t->peers, entry) {
			if (op != tp && !mbt_peer_choked(op->peer))
				torrent_drive_peer(t, op);
		}
	}
}

static void
on_error(struct mbt_peer *p, int err, void *arg)
{
	struct mbt_torrent *t = arg;
	struct torrent_peer *tp = find_torrent_peer(t, p);
	if (!tp) return;

	/* Reset any pieces this peer was downloading */
	if (t->pieces) {
		for (uint32_t i = 0; i < t->piece_count; i++) {
			if (t->pieces[i].owner == tp) {
				t->pieces[i].state = MBT_PIECE_PENDING;
				t->pieces[i].owner = NULL;
				t->pieces[i].received = 0;
				t->pieces[i].requested = 0;
				free(t->pieces[i].data);
				t->pieces[i].data = NULL;
				if (t->pieces[i].blocks) { mbt_bitmap_unref(t->pieces[i].blocks); t->pieces[i].blocks = NULL; }
			}
		}
	}

	/* Remove peer from list */
	SLIST_REMOVE(&t->peers, tp, torrent_peer, entry);

	/* Don't free tp->peer here — we may be inside its callback chain.
	 * The peer is already CLOSED and will not deliver more callbacks.
	 * Free on next event loop iteration via deferred timer. */
	if (!tp->banned)
		tp->banned = 1; /* prevent re-use */

	/* Schedule deferred free */
	struct mbt_ev_timer *dtimer = NULL;
	mbt_ev_timer_new(t->ev, &dtimer, 0, torrent_deferred_peer_free, tp);

	t->peer_count--;
}

static void
on_port(struct mbt_peer *p, uint16_t dht_port, void *arg)
{
	struct mbt_torrent *t = arg;
	if (!t->session || dht_port == 0)
		return;
	/* Feed peer's DHT node into our routing table */
	mbt_session_dht_bootstrap(t->session, mbt_peer_addr(p), dht_port);
}

static struct mbt_peer_callbacks torrent_peer_cbs = {
	.on_active   = on_active,
	.on_choke    = on_choke,
	.on_have     = on_have,
	.on_bitfield = on_bitfield,
	.on_piece    = on_piece,
	.on_metadata = on_metadata,
	.on_metadata_request = on_metadata_request,
	.on_interest = on_interest,
	.on_request  = on_request,
	.on_reject   = on_reject,
	.on_error    = on_error,
	.on_pex      = on_pex,
	.on_port     = on_port,
};

/* --- Drive piece requests --- */

static void
torrent_drive_peer(struct mbt_torrent *t, struct torrent_peer *tp)
{
	if (!t->pieces || mbt_peer_choked(tp->peer))
		return;
	if (tp->snubbed)
		return;
	if (t->have_count == t->piece_count)
		return; /* complete — nothing to request */

	/* Throttle requests if download rate exceeded */
	uint64_t d1 = mbt_ratelimit_delay(t->download_limit, BLOCK_SIZE);
	uint64_t d2 = mbt_ratelimit_delay(mbt_session_get_download_limit(t->session), BLOCK_SIZE);
	uint64_t d = d1 > d2 ? d1 : d2;
	if (d > 0) {
		if (d < 10000) d = 10000; /* min 10ms to avoid busy-loop */
		if (!t->ratelimit_timer)
			mbt_ev_timer_new(t->ev, &t->ratelimit_timer, d, torrent_ratelimit_retry, t);
		return;
	}
	int limit = 64; /* default pipeline — keeps memory bounded */
	int peer_limit = mbt_peer_reqq(tp->peer);
	if (peer_limit > 0)
		limit = peer_limit;
	if (limit > 4096)
		limit = 4096;

	while (tp->outstanding < limit) {
		/* Prefer continuing a piece this peer already owns */
		int idx = -1;
		int owns_active = 0;
		for (uint32_t i = 0; i < t->piece_count; i++) {
			if (t->pieces[i].owner == tp && t->pieces[i].state == MBT_PIECE_ACTIVE) {
				if (t->pieces[i].requested < t->pieces[i].length) {
					idx = i;
					break;
				}
				owns_active++;
			}
		}
		/* If peer has max active pieces all fully-requested, wait */
		int max_pp = t->max_pieces_per_peer ? t->max_pieces_per_peer : 4;
		if (idx < 0 && owns_active >= max_pp)
			break;
		/* If no existing piece, pick a new one */
		if (idx < 0)
			idx = t->picker(t, tp->peer, t->picker_ctx);
		if (idx < 0) {
			/* Check if we should enter endgame */
			if (!t->endgame && t->piece_count > 0) {
				uint32_t remaining = t->piece_count - t->have_count;
				if (remaining <= ENDGAME_THRESHOLD && remaining > 0) {
					t->endgame = 1;
					mbt_log_info(MBT_LOG_TORRENT, "entering endgame mode (%u pieces remaining)", remaining);
					/* In endgame, request all remaining pieces from this peer too */
					for (uint32_t i = 0; i < t->piece_count && tp->outstanding < limit; i++) {
						if (mbt_bitmap_isset(t->have, i) > 0)
							continue;
						struct piece_info *epi = &t->pieces[i];
						if (!epi->data) continue; /* not yet allocated = not in progress */
						/* Re-request all blocks */
						for (size_t off = 0; off < epi->length && tp->outstanding < limit; off += BLOCK_SIZE) {
							uint32_t req_len = MIN(BLOCK_SIZE, epi->length - off);
							if (mbt_ratelimit_delay(t->download_limit, req_len) > 0 ||
							    mbt_ratelimit_delay(mbt_session_get_download_limit(t->session), req_len) > 0)
								break;
							mbt_ratelimit_consume(t->download_limit, req_len);
							mbt_ratelimit_consume(mbt_session_get_download_limit(t->session), req_len);
							mbt_peer_send_request(tp->peer, i, off, req_len);
							tp->outstanding++;
						}
					}
				}
			}
			break;
		}

		struct piece_info *pi = &t->pieces[idx];
		if (mbt_bitmap_isset(t->have, idx) > 0)
			continue;

		/* Already fully requested — skip */
		if (pi->requested >= pi->length)
			continue;

		if (pi->state != MBT_PIECE_ACTIVE) {
			pi->state = MBT_PIECE_ACTIVE;
			pi->owner = tp;
			pi->last_activity = now_usec();
		}

		/* Request unrequested blocks for this piece */
		size_t off;
		int blocked = 0;
		for (off = pi->requested; off < pi->length && tp->outstanding < limit; off += BLOCK_SIZE) {
			uint32_t req_len = MIN(BLOCK_SIZE, pi->length - off);
			if (mbt_ratelimit_delay(t->download_limit, req_len) > 0 ||
			    mbt_ratelimit_delay(mbt_session_get_download_limit(t->session), req_len) > 0) {
				blocked = 1;
				break;
			}
			mbt_ratelimit_consume(t->download_limit, req_len);
			mbt_ratelimit_consume(mbt_session_get_download_limit(t->session), req_len);
			mbt_peer_send_request(tp->peer, idx, off, req_len);
			tp->outstanding++;
		}
		pi->requested = off;
		if (blocked) break;
	}

}

static void
torrent_ratelimit_retry(struct mbt_ev_timer *timer, void *arg)
{
	struct mbt_torrent *t = arg;
	mbt_ev_timer_free(timer);
	t->ratelimit_timer = NULL;

	struct torrent_peer *tp;
	SLIST_FOREACH(tp, &t->peers, entry) {
		if (!tp->banned && tp->peer && !mbt_peer_choked(tp->peer))
			torrent_drive_peer(t, tp);
	}

	/* If still incomplete and no timer was scheduled, retry in 10ms */
	if (!t->ratelimit_timer && t->have_count < t->piece_count)
		mbt_ev_timer_new(t->ev, &t->ratelimit_timer, 10000, torrent_ratelimit_retry, t);
}

static void
torrent_request_metadata(struct mbt_torrent *t, struct torrent_peer *tp)
{
	mbt_log_debug(MBT_LOG_TORRENT, "requesting %d metadata pieces", t->metadata_pieces);
	for (int i = 0; i < t->metadata_pieces; i++) {
		int rv = mbt_peer_send_metadata_request(tp->peer, i);
		mbt_log_debug(MBT_LOG_TORRENT, "metadata request piece %d: rv=%d", i, rv);
	}
}

/* --- Public API --- */

int
mbt_torrent_new(struct mbt_torrent **out, struct mbt_torrent_config *cfg)
{
	struct mbt_torrent *t;

	t = calloc(1, sizeof(*t));
	if (!t)
		return -ENOMEM;

	t->ev = cfg->ev;
	t->ih = mbt_infohash_ref(cfg->ih);
	memcpy(t->peer_id, cfg->peer_id, 20);
	t->storage_ops = cfg->storage_ops;
	t->storage_ctx = cfg->storage_ctx;
	t->picker = cfg->picker ? cfg->picker : mbt_picker_rarest;
	t->picker_ctx = cfg->picker_ctx;
	t->timers = cfg->timers;

	mbt_peerdb_init(&t->peerdb);

	/* Start timeout scanner */
	mbt_ev_timer_new(t->ev, &t->timeout_timer, 5 * 1000000ULL, torrent_timeout_cb, t);
	mbt_ev_timer_new(t->ev, &t->choke_timer, (uint64_t)t->timers.choke_interval * 1000000ULL, torrent_choke_cb, t);
	mbt_ev_timer_new(t->ev, &t->pex_timer, (uint64_t)t->timers.pex_interval * 1000000ULL, torrent_pex_cb, t);

	*out = t;
	return 0;
}

void
mbt_torrent_free(struct mbt_torrent *t)
{
	if (!t)
		return;

	/* Free peers */
	while (!SLIST_EMPTY(&t->peers)) {
		struct torrent_peer *tp = SLIST_FIRST(&t->peers);
		SLIST_REMOVE_HEAD(&t->peers, entry);
		mbt_peer_free(tp->peer);
		free(tp);
	}

	/* Free pieces */
	if (t->pieces) {
		for (uint32_t i = 0; i < t->piece_count; i++) {
			free(t->pieces[i].data);
			if (t->pieces[i].blocks) mbt_bitmap_unref(t->pieces[i].blocks);
		}
		free(t->pieces);
	}

	if (t->timeout_timer)
		mbt_ev_timer_free(t->timeout_timer);
	if (t->choke_timer)
		mbt_ev_timer_free(t->choke_timer);
	if (t->pex_timer)
		mbt_ev_timer_free(t->pex_timer);
	if (t->ratelimit_timer)
		mbt_ev_timer_free(t->ratelimit_timer);
	mbt_peerdb_free(&t->peerdb);
	mbt_bitmap_unref(t->have);
	free(t->metadata);
	free(t->name);
	if (t->files) {
		for (int i = 0; i < t->file_count; i++)
			free(t->files[i].path);
		free(t->files);
	}
	mbt_infohash_unref(t->ih);
	mbt_ratelimit_free(t->upload_limit);
	mbt_ratelimit_free(t->download_limit);
	free(t);
}

void
mbt_torrent_set_callbacks(struct mbt_torrent *t, struct mbt_torrent_callbacks *cb, void *arg)
{
	t->cb = *cb;
	t->cb_arg = arg;
}

struct mbt_peerdb *
mbt_torrent_peerdb(struct mbt_torrent *t)
{
	return &t->peerdb;
}






int
mbt_torrent_set_metadata(struct mbt_torrent *t, const void *info, size_t len)
{
	__attribute__((cleanup(mbt_be_value_freep))) struct mbt_be_value *be = NULL;
	struct mbt_be_value *tmp;
	int rv;

	/* Already have metadata parsed — skip */
	if (t->pieces)
		return 0;

	/* Store raw metadata for BEP 9 serving */
	if (!t->metadata) {
		t->metadata = malloc(len);
		if (!t->metadata) return -ENOMEM;
		memcpy(t->metadata, info, len);
		t->metadata_len = len;
	}

	rv = mbt_be_value_parse(info, len, &be);
	if (rv < 0)
		return rv;

	/* Extract name */
	tmp = mbt_be_find(be, "name");
	if (tmp && tmp->type == BE_STRING && tmp->string.iov_len > 0) {
		free(t->name);
		t->name = strndup(tmp->string.iov_base, tmp->string.iov_len);
	}

	/* Extract piece length */
	tmp = mbt_be_find(be, "piece length");
	if (!tmp || tmp->type != BE_INTEGER)
		return -EINVAL;
	t->piece_length = (uint32_t)tmp->integer;


	/* BEP 27: private flag */
	tmp = mbt_be_find(be, "private");
	if (tmp && tmp->type == BE_INTEGER && tmp->integer == 1)
		t->is_private = 1;
	/* Extract total length (single-file) or compute from files list */
	tmp = mbt_be_find(be, "length");
	if (tmp && tmp->type == BE_INTEGER) {
		t->total_length = (uint64_t)tmp->integer;
		/* Single-file: one entry in file table */
		t->file_count = 1;
		t->files = calloc(1, sizeof(struct mbt_file));
		if (t->files) {
			t->files[0].path = t->name ? strdup(t->name) : strdup("download");
			t->files[0].offset = 0;
			t->files[0].length = t->total_length;
		}
	} else {
		/* Multi-file: build file table from files list */
		struct mbt_be_value *files = mbt_be_find(be, "files");
		if (!files || files->type != BE_LIST)
			return -EINVAL;

		/* Count files */
		struct mbt_be_value *f;
		int count = 0;
		TAILQ_FOREACH(f, &files->listhead, next)
			if (f->type == BE_DICTIONARY) count++;

		t->files = calloc(count, sizeof(struct mbt_file));
		if (!t->files)
			return -ENOMEM;

		uint64_t offset = 0;
		int idx = 0;
		TAILQ_FOREACH(f, &files->listhead, next) {
			if (f->type != BE_DICTIONARY) continue;
			struct mbt_be_value *flen = mbt_be_find(f, "length");
			if (!flen || flen->type != BE_INTEGER) continue;

			/* Build path from path list */
			struct mbt_be_value *path_list = mbt_be_find(f, "path");
			if (path_list && path_list->type == BE_LIST) {
				/* Join path components with / */
				char pathbuf[1024] = {0};
				size_t pos = 0;
				struct mbt_be_value *comp;
				TAILQ_FOREACH(comp, &path_list->listhead, next) {
					if (comp->type != BE_STRING) continue;
					if (pos > 0 && pos < sizeof(pathbuf) - 1)
						pathbuf[pos++] = '/';
					size_t clen = comp->string.iov_len;
					if (pos + clen < sizeof(pathbuf)) {
						memcpy(pathbuf + pos, comp->string.iov_base, clen);
						pos += clen;
					}
				}
				pathbuf[pos] = '\0';
				t->files[idx].path = strdup(pathbuf);
			}

			t->files[idx].offset = offset;
			t->files[idx].length = (uint64_t)flen->integer;
			offset += t->files[idx].length;
			idx++;
		}
		t->file_count = idx;
		t->total_length = offset;

		if (t->total_length == 0)
			return -EINVAL;
	}

	/* Extract piece hashes */
	tmp = mbt_be_find(be, "pieces");
	if (!tmp || tmp->type != BE_STRING)
		return -EINVAL;

	t->piece_count = (uint32_t)(tmp->string.iov_len / 20);

	/* Allocate piece tracking */
	t->pieces = calloc(t->piece_count, sizeof(struct piece_info));
	if (!t->pieces)
		return -ENOMEM;

	/* Set up each piece */
	uint8_t *hashes = tmp->string.iov_base;
	for (uint32_t i = 0; i < t->piece_count; i++) {
		memcpy(t->pieces[i].hash, hashes + i * 20, 20);
		if (i == t->piece_count - 1) {
			/* Last piece may be shorter */
			uint64_t remaining = t->total_length - (uint64_t)i * t->piece_length;
			t->pieces[i].length = (size_t)remaining;
		} else {
			t->pieces[i].length = t->piece_length;
		}
		t->pieces[i].state = MBT_PIECE_PENDING;
	}

	/* Allocate have bitmap */
	mbt_bitmap_new_bits(&t->have, t->piece_count);

	/* Check storage for existing pieces (resume) */
	if (t->storage_ops && t->storage_ops->open)
		t->storage_ops->open(t->storage_ctx, t);

	if (t->storage_ops && t->storage_ops->read_piece) {
		uint8_t *buf = malloc(t->piece_length);
		if (buf) {
			for (uint32_t i = 0; i < t->piece_count; i++) {
				uint32_t plen = t->piece_length;
				if (i == t->piece_count - 1) {
					uint32_t rem = t->total_length % t->piece_length;
					if (rem) plen = rem;
				}
				int rv2 = t->storage_ops->read_piece(t->storage_ctx, i, buf, plen);
				if (rv2 < 0)
					continue;

				SHA1_CTX ctx;
				uint8_t digest[20];
				SHA1Init(&ctx);
				SHA1Update(&ctx, buf, plen);
				SHA1Final(digest, &ctx);

				if (memcmp(digest, t->pieces[i].hash, 20) == 0) {
					mbt_bitmap_set(t->have, i);
					t->have_count++;
				}
			}
			free(buf);
			if (t->have_count > 0) {
				mbt_log_info(MBT_LOG_TORRENT, "resumed %u/%u pieces",
				             t->have_count, t->piece_count);
				/* Announce resumed pieces to already-connected peers */
				struct torrent_peer *tp;
				SLIST_FOREACH(tp, &t->peers, entry) {
					if (tp->banned || !tp->peer) continue;
					for (uint32_t i = 0; i < t->piece_count; i++) {
						if (mbt_bitmap_isset(t->have, i) > 0)
							mbt_peer_send_have(tp->peer, i);
					}
				}
			}
			if (t->have_count == t->piece_count) {
				mbt_log_info(MBT_LOG_TORRENT, "all pieces verified, nothing to download");
				if (t->cb.on_complete)
					t->cb.on_complete(t, t->cb_arg);
			}
		}
	}

	return 0;
}

int
mbt_torrent_add_peer(struct mbt_torrent *t, struct mbt_conn *conn)
{
	struct torrent_peer *tp;
	struct mbt_peer *peer;
	int rv;

	tp = calloc(1, sizeof(*tp));
	if (!tp)
		return -ENOMEM;

	rv = mbt_peer_new(&peer, t->ev, conn, t->ih, t->peer_id,
	                  t->piece_count, &torrent_peer_cbs, t);
	if (rv < 0) {
		free(tp);
		return rv;
	}

	mbt_peer_set_our_metadata_size(peer, t->metadata_len);

	tp->peer = peer;
	tp->last_recv = time(NULL);
	SLIST_INSERT_HEAD(&t->peers, tp, entry);
	t->peer_count++;

	return 0;
}

int
mbt_torrent_add_peer_incoming(struct mbt_torrent *t, struct mbt_conn *conn,
                              const uint8_t ext[8], const uint8_t their_id[20])
{
	struct torrent_peer *tp;
	struct mbt_peer *peer;
	int rv;

	tp = calloc(1, sizeof(*tp));
	if (!tp)
		return -ENOMEM;

	rv = mbt_peer_new_incoming(&peer, t->ev, conn, t->ih, t->peer_id,
	                           t->piece_count, ext, their_id,
	                           t->metadata_len,
	                           &torrent_peer_cbs, t);
	if (rv < 0) {
		free(tp);
		return rv;
	}

	tp->peer = peer;
	tp->last_recv = time(NULL);
	SLIST_INSERT_HEAD(&t->peers, tp, entry);
	t->peer_count++;

	return 0;
}

struct mbt_infohash *
mbt_torrent_infohash(struct mbt_torrent *t)
{
	return t->ih;
}

const char *
mbt_torrent_name(struct mbt_torrent *t)
{
	return t->name;
}

float
mbt_torrent_progress(struct mbt_torrent *t)
{
	if (!t->piece_count)
		return 0.0f;
	return (float)t->have_count / (float)t->piece_count;
}

int
mbt_torrent_complete(struct mbt_torrent *t)
{
	return t->piece_count > 0 && t->have_count == t->piece_count;
}

enum mbt_torrent_state
mbt_torrent_state(struct mbt_torrent *t)
{
	if (!t->pieces)
		return MBT_TORRENT_INIT;
	if (t->have_count == t->piece_count)
		return MBT_TORRENT_SEEDING;

	/* Check if any unchoked peer has a piece we need */
	struct torrent_peer *tp;
	SLIST_FOREACH(tp, &t->peers, entry) {
		if (tp->banned || !tp->peer) continue;
		if (mbt_peer_choked(tp->peer)) continue;
		struct mbt_bitmap *bf = mbt_peer_bitfield(tp->peer);
		if (!bf) continue;
		for (uint32_t i = 0; i < t->piece_count; i++) {
			if (mbt_bitmap_isset(t->have, i) <= 0 &&
			    mbt_bitmap_isset(bf, i) > 0)
				return MBT_TORRENT_DOWNLOADING;
		}
	}
	return MBT_TORRENT_STALLED;
}

int
mbt_torrent_is_private(struct mbt_torrent *t)
{
	return t->is_private;
}
uint32_t
mbt_torrent_piece_count(struct mbt_torrent *t)
{
	return t->piece_count;
}

uint32_t
mbt_torrent_piece_length(struct mbt_torrent *t)
{
	return t->piece_length;
}

uint32_t
mbt_torrent_have_count(struct mbt_torrent *t)
{
	return t->have_count;
}

int
mbt_torrent_peer_count(struct mbt_torrent *t)
{
	return t->peer_count;
}

struct mbt_peer *
mbt_torrent_peer_at(struct mbt_torrent *t, int idx)
{
	struct torrent_peer *tp = SLIST_FIRST(&t->peers);
	for (int i = 0; tp && i < idx; i++) tp = SLIST_NEXT(tp, entry);
	return tp ? tp->peer : NULL;
}
int
mbt_torrent_has_metadata(struct mbt_torrent *t)
{
	return t->pieces != NULL;
}

int
mbt_torrent_has_piece(struct mbt_torrent *t, uint32_t piece)
{
	if (!t->have || piece >= t->piece_count)
		return 0;
	return mbt_bitmap_isset(t->have, piece) > 0;
}

int
mbt_torrent_piece_active(struct mbt_torrent *t, uint32_t piece)
{
	if (!t->pieces || piece >= t->piece_count)
		return 0;
	return t->pieces[piece].state == MBT_PIECE_ACTIVE;
}

int
mbt_torrent_piece_fully_requested(struct mbt_torrent *t, uint32_t piece)
{
	if (!t->pieces || piece >= t->piece_count)
		return 0;
	return t->pieces[piece].requested >= t->pieces[piece].length;
}

void
mbt_torrent_piece_map(struct mbt_torrent *t, uint8_t *out, uint32_t count)
{
	uint32_t n = count < t->piece_count ? count : t->piece_count;
	for (uint32_t i = 0; i < n; i++) {
		if (mbt_bitmap_isset(t->have, i) > 0)
			out[i] = 2;
		else if (t->pieces && t->pieces[i].state == MBT_PIECE_ACTIVE)
			out[i] = 1;
		else
			out[i] = 0;
	}
}

int
mbt_torrent_file_count(struct mbt_torrent *t)
{
	return t->file_count;
}

const struct mbt_file *
mbt_torrent_file(struct mbt_torrent *t, int index)
{
	if (index < 0 || index >= t->file_count)
		return NULL;
	return &t->files[index];
}

void
mbt_torrent_set_session(struct mbt_torrent *t, struct mbt_session *s)
{
	t->session = s;
	t->max_pieces_per_peer = mbt_session_max_pieces_per_peer(s);
}

void
mbt_torrent_set_upload_rate(struct mbt_torrent *t, uint64_t bytes_per_sec)
{
	if (!t->upload_limit && bytes_per_sec)
		mbt_ratelimit_new(&t->upload_limit, bytes_per_sec, 0);
	else if (t->upload_limit)
		mbt_ratelimit_set_rate(t->upload_limit, bytes_per_sec);
}

void
mbt_torrent_set_download_rate(struct mbt_torrent *t, uint64_t bytes_per_sec)
{
	if (!t->download_limit && bytes_per_sec)
		mbt_ratelimit_new(&t->download_limit, bytes_per_sec, 0);
	else if (t->download_limit)
		mbt_ratelimit_set_rate(t->download_limit, bytes_per_sec);
}

uint64_t
mbt_torrent_upload_rate(struct mbt_torrent *t)
{
	return mbt_ratelimit_rate(t->upload_limit);
}

uint64_t
mbt_torrent_download_rate(struct mbt_torrent *t)
{
	return mbt_ratelimit_rate(t->download_limit);
}

int
mbt_torrent_peer_stats(struct mbt_torrent *t, int idx, struct mbt_peer_stats *out)
{
	struct torrent_peer *tp;
	int i = 0;
	SLIST_FOREACH(tp, &t->peers, entry) {
		if (tp->banned || !tp->peer) continue;
		if (i == idx) {
			out->addr = mbt_peer_addr(tp->peer);
			out->port = mbt_peer_port(tp->peer);
			out->choked = mbt_peer_choked(tp->peer);
			out->interested = mbt_peer_interested(tp->peer);
			out->snubbed = tp->snubbed;
			out->outstanding = tp->outstanding;
			out->hash_failures = tp->hash_failures;
			struct mbt_bitmap *bf = mbt_peer_bitfield(tp->peer);
			if (bf && t->piece_count > 0)
				out->done_pct = 100 * mbt_bitmap_count(bf) / t->piece_count;
			else
				out->done_pct = 0;
			return 0;
		}
		i++;
	}
	return -1;
}
