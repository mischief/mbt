// SPDX-License-Identifier: ISC
#include "mbt/compat.h"
#include <sys/queue.h>
#include <sys/socket.h>
#include <arpa/inet.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <errno.h>
#include <time.h>
#include <netinet/in.h>
#include <sha1.h>
#include <stdint.h>

#include "mbt/log.h"
#include "mbt/event.h"
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
#include "mbt/peer_source.h"
#include "mbt/storage.h"
#include "mbt/timers.h"

struct mbt_bitmap;
struct mbt_conn;
struct mbt_ev_timer;
struct mbt_peer;
struct mbt_session;

enum {
	BLOCK_SIZE = 16384,
	METADATA_BLOCK_SIZE = 16384,
	MAX_PEERS = 50,
	PIPELINE_DEPTH = 512,  /* default max per-peer pipeline */
	PIPELINE_INIT = 4,     /* slow-start initial window */
};

/* Per-piece tracking (only allocated for active pieces) */
struct piece_info {
	uint32_t piece;
	uint8_t *data;
	size_t received;       /* bytes received so far */
	size_t requested;      /* bytes requested so far (>= received) */
	struct mbt_bitmap *blocks; /* received block bitmap */
	struct torrent_peer *owner; /* peer downloading this piece */
	uint64_t last_activity; /* monotonic time of last block received (usec) */
	SLIST_ENTRY(piece_info) owner_entry; /* linkage in owner->active_pieces */
	SLIST_ENTRY(piece_info) table_entry; /* linkage in torrent->piece_table */
};

enum { PIECE_TABLE_BUCKETS = 128 };

struct piece_entry {
	uint16_t avail;
	uint8_t in_bucket;
	LIST_ENTRY(piece_entry) bucket_entry;
};

/* Per-peer entry in the torrent's peer list */
struct torrent_peer {
	SLIST_ENTRY(torrent_peer) entry;
	SLIST_HEAD(, piece_info) active_pieces; /* pieces currently assigned to this peer */
	int active_count;      /* length of active_pieces */
	struct mbt_peer *peer;
	int outstanding;       /* in-flight requests */
	int reqq_window;       /* dynamic pipeline target, grows on delivery */
	int hash_failures;     /* consecutive hash failures from this peer */
	int failures;          /* peer penalty score (decays over time) */
	int snubbed;           /* peer not sending data, skip in drive_peer */
	uint64_t bytes_from;   /* bytes received from peer in current interval */
	uint64_t bytes_to;     /* bytes sent to peer in current interval */
	uint64_t total_down;   /* lifetime bytes received */
	uint64_t total_up;     /* lifetime bytes sent */
	int peer_interested;   /* peer wants data from us */
	int am_choking;        /* we are choking this peer */
	time_t last_recv;      /* last time we received piece data */
};

struct mbt_torrent {
	struct mbt_ev *ev;
	struct mbt_infohash *ih;
	struct mbt_session *session; /* owning session (NULL if standalone) */
	uint8_t peer_id[20];

	struct mbt_storage storage;
	char *data_dir;
	mbt_picker_fn picker;
	void *picker_ctx;

	struct mbt_torrent_callbacks cb;
	void *cb_arg;

	/* Metadata (info dict) — immutable after mbt_torrent_set_metadata returns */
	uint8_t *metadata;
	size_t metadata_len;
	const uint8_t *pieces_hash;     /* pointer into metadata; NULL until metadata parsed */
	size_t pieces_hash_len;         /* == piece_count * 20 */
	size_t metadata_have;  /* bytes received */
	int metadata_pieces;   /* total metadata pieces expected */
	struct mbt_bitmap *metadata_recv; /* which pieces have been received */

	/* Active piece hash table (separate chaining, PIECE_TABLE_BUCKETS buckets) */
	SLIST_HEAD(piece_bucket, piece_info) piece_table[PIECE_TABLE_BUCKETS];
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

	/* Bucket queue for O(1) piece picking (allocated after metadata parsed, freed on completion) */
	struct piece_entry *piece_entries;   /* [piece_count], avail + list linkage */
	LIST_HEAD(avail_bucket, piece_entry) *avail_buckets; /* [peer_count_max+2] */
	int avail_buckets_n;                 /* allocated bucket count */


	/* Peers */
	SLIST_HEAD(, torrent_peer) peers;
	int peer_count;
	int peers_with_avail; /* peers that have sent a bitfield or HAVE_ALL */

	/* Peer database (owned by torrent) */
	struct mbt_peerdb peerdb;

	/* Request timeout */
	struct mbt_ev_timer *timeout_timer;
	struct mbt_ev_timer *choke_timer;
	struct mbt_ev_timer *pex_timer;
	int choke_round;  /* increments every 10s, optimistic every 3rd round (30s) */
	struct mbt_timers timers;

	/* Endgame mode */
	int endgame;
	int verifying; /* 1 while initial SHA1 verify is running */
	int is_private; /* BEP 27: no DHT/PEX/LSD for this torrent */
	int max_pieces_per_peer;

	/* Async verify state */
	uint32_t verify_cursor;
	uint8_t *verify_buf;
	struct mbt_ev_timer *verify_timer;

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
static const uint8_t *piece_hash(struct mbt_torrent *t, uint32_t i) {
	size_t off = (size_t)i * 20;
	if (!t->pieces_hash || off + 20 > t->pieces_hash_len)
		return NULL;
	return t->pieces_hash + off;
}

static void torrent_drive_peer(struct mbt_torrent *t, struct torrent_peer *tp);
static void torrent_drive_all_peers(struct mbt_torrent *t);
static void torrent_ratelimit_retry(struct mbt_ev_timer *timer, void *arg);
static void torrent_request_metadata(struct mbt_torrent *t, struct torrent_peer *tp);

static size_t
piece_length_for(struct mbt_torrent *t, uint32_t piece)
{
	if (piece == t->piece_count - 1) {
		uint64_t rem = t->total_length % t->piece_length;
		return rem ? (size_t)rem : (size_t)t->piece_length;
	}
	return (size_t)t->piece_length;
}

static struct piece_info *
piece_info_find(struct mbt_torrent *t, uint32_t piece)
{
	struct piece_info *pi;
	SLIST_FOREACH(pi, &t->piece_table[piece % PIECE_TABLE_BUCKETS], table_entry)
		if (pi->piece == piece)
			return pi;
	return NULL;
}

static struct piece_info *
piece_info_alloc(struct mbt_torrent *t, uint32_t piece)
{
	struct piece_info *pi = calloc(1, sizeof(*pi));
	if (!pi) return NULL;
	pi->piece = piece;
	SLIST_INSERT_HEAD(&t->piece_table[piece % PIECE_TABLE_BUCKETS], pi, table_entry);
	return pi;
}

static void
piece_assign(struct piece_info *pi, struct torrent_peer *tp)
{
	pi->owner = tp;
	SLIST_INSERT_HEAD(&tp->active_pieces, pi, owner_entry);
	tp->active_count++;
}

static void
piece_release(struct piece_info *pi)
{
	struct torrent_peer *tp = pi->owner;
	if (!tp) return;
	SLIST_REMOVE(&tp->active_pieces, pi, piece_info, owner_entry);
	tp->active_count--;
	pi->owner = NULL;
}

/* Release and free an active piece_info (removes from table and owner). */
static void
piece_info_free_one(struct mbt_torrent *t, struct piece_info *pi)
{
	if (pi->owner)
		piece_release(pi);
	SLIST_REMOVE(&t->piece_table[pi->piece % PIECE_TABLE_BUCKETS],
	             pi, piece_info, table_entry);
	free(pi->data);
	if (pi->blocks) mbt_bitmap_unref(pi->blocks);
	free(pi);
}

/* --- Bucket queue helpers --- */

static void
bucket_init(struct mbt_torrent *t)
{
	int n = MAX_PEERS + 2;
	t->avail_buckets = calloc(n, sizeof(*t->avail_buckets));
	t->avail_buckets_n = n;
	if (!t->avail_buckets) return;
	for (int i = 0; i < n; i++)
		LIST_INIT(&t->avail_buckets[i]);
	for (uint32_t i = 0; i < t->piece_count; i++) {
		LIST_INSERT_HEAD(&t->avail_buckets[0], &t->piece_entries[i], bucket_entry);
		t->piece_entries[i].in_bucket = 1;
	}
}

static void
bucket_free(struct mbt_torrent *t)
{
	free(t->avail_buckets);
	t->avail_buckets = NULL;
	free(t->piece_entries);
	t->piece_entries = NULL;
}

static void
bucket_piece_inc(struct mbt_torrent *t, uint32_t piece)
{
	if (!t->avail_buckets || piece >= t->piece_count) return;
	struct piece_entry *e = &t->piece_entries[piece];
	if (e->in_bucket) LIST_REMOVE(e, bucket_entry);
	e->avail++;
	int bkt = e->avail < (uint16_t)(t->avail_buckets_n - 1) ? e->avail : t->avail_buckets_n - 1;
	if (e->in_bucket)
		LIST_INSERT_HEAD(&t->avail_buckets[bkt], e, bucket_entry);
}

static void
bucket_piece_dec(struct mbt_torrent *t, uint32_t piece)
{
	if (!t->avail_buckets || piece >= t->piece_count) return;
	struct piece_entry *e = &t->piece_entries[piece];
	if (e->avail == 0) return;
	if (e->in_bucket) LIST_REMOVE(e, bucket_entry);
	e->avail--;
	int bkt = e->avail < (uint16_t)(t->avail_buckets_n - 1) ? e->avail : t->avail_buckets_n - 1;
	if (e->in_bucket)
		LIST_INSERT_HEAD(&t->avail_buckets[bkt], e, bucket_entry);
}

static void
bucket_reinsert(struct mbt_torrent *t, uint32_t piece)
{
	if (!t->avail_buckets || piece >= t->piece_count) return;
	struct piece_entry *e = &t->piece_entries[piece];
	if (e->in_bucket) return;
	if (mbt_bitmap_isset(t->have, piece) > 0) return;
	int bkt = e->avail < (uint16_t)(t->avail_buckets_n - 1) ? e->avail : t->avail_buckets_n - 1;
	LIST_INSERT_HEAD(&t->avail_buckets[bkt], e, bucket_entry);
	e->in_bucket = 1;
}

static void
bucket_piece_complete(struct mbt_torrent *t, uint32_t piece)
{
	if (!t->avail_buckets || piece >= t->piece_count) return;
	struct piece_entry *e = &t->piece_entries[piece];
	if (!e->in_bucket) return;
	LIST_REMOVE(e, bucket_entry);
	e->in_bucket = 0;
}

static void verify_tick(struct mbt_ev_timer *timer, void *arg);

static void
verify_complete(struct mbt_torrent *t)
{
	mbt_ev_timer_free(t->verify_timer);
	t->verify_timer = NULL;
	free(t->verify_buf);
	t->verify_buf = NULL;
	t->verifying = 0;

	if (t->cb.on_verify_complete)
		t->cb.on_verify_complete(t, t->cb_arg);

	if (t->have_count > 0) {
		mbt_log_info(MBT_LOG_TORRENT, "resumed %u/%u pieces",
		             t->have_count, t->piece_count);
		struct torrent_peer *tp;
		SLIST_FOREACH(tp, &t->peers, entry) {
			if (tp->failures >= 10 || !tp->peer) continue;
			for (uint32_t i = 0; i < t->piece_count; i++)
				if (mbt_bitmap_isset(t->have, i) > 0)
					mbt_peer_send_have(tp->peer, i);
		}
	}
	if (t->have_count == t->piece_count) {
		mbt_log_info(MBT_LOG_TORRENT, "all pieces verified, nothing to download");
		struct torrent_peer *np;
		SLIST_FOREACH(np, &t->peers, entry)
			if (np->peer && np->failures < 10)
				mbt_peer_send_interested(np->peer, 0);
		bucket_free(t);
		if (t->cb.on_complete)
			t->cb.on_complete(t, t->cb_arg);
	} else {
		/* Drive connected peers now that we know what we have */
		struct torrent_peer *tp;
		SLIST_FOREACH(tp, &t->peers, entry)
			if (!mbt_peer_choked(tp->peer))
				torrent_drive_peer(t, tp);
	}
}

static void
verify_tick(struct mbt_ev_timer *timer, void *arg)
{
	struct mbt_torrent *t = arg;
	struct timespec t0;
	clock_gettime(CLOCK_MONOTONIC, &t0);

	while (t->verify_cursor < t->piece_count) {
		uint32_t i = t->verify_cursor++;
		size_t plen = piece_length_for(t, i);

		int rv = t->storage.ops->read_piece(t->storage.ctx, i, t->verify_buf, plen);
		if (rv >= 0) {
			SHA1_CTX ctx;
			uint8_t digest[20];
			SHA1Init(&ctx);
			SHA1Update(&ctx, t->verify_buf, plen);
			SHA1Final(digest, &ctx);
			int have = memcmp(digest, piece_hash(t, i), 20) == 0;
			if (have && mbt_bitmap_isset(t->have, i) <= 0) {
				mbt_bitmap_set(t->have, i);
				t->have_count++;
				bucket_piece_complete(t, i);
			}
			if (t->cb.on_verify_progress)
				t->cb.on_verify_progress(t, i, have, t->cb_arg);
		}

		struct timespec now;
		clock_gettime(CLOCK_MONOTONIC, &now);
		long elapsed_ms = (now.tv_sec - t0.tv_sec) * 1000 +
		                  (now.tv_nsec - t0.tv_nsec) / 1000000;
		if (elapsed_ms >= 10)
			break;
	}

	if (t->verify_cursor < t->piece_count)
		mbt_ev_timer_mod(t->verify_timer, 1000);
	else
		verify_complete(t);
}

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

	if (t->piece_count == 0)
		goto reschedule;

	for (int _b = 0; _b < PIECE_TABLE_BUCKETS; _b++) {
		struct piece_info *pi, *_npi;
		SLIST_FOREACH_SAFE(pi, &t->piece_table[_b], table_entry, _npi) {

		/* Reset if: owner is gone/penalized/choked, or timed out */
		int stale = 0;
		if (!pi->owner || !pi->owner->peer || pi->owner->failures >= 10)
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
		piece_info_free_one(t, pi);
		reset_count++;
		} /* SLIST_FOREACH_SAFE */
	} /* bucket loop */

	/* Drive all unchoked peers if we freed up pieces */
	if (reset_count > 0) {
		struct torrent_peer *tp;
		SLIST_FOREACH(tp, &t->peers, entry) {
			if (tp->failures < 10 && !mbt_peer_choked(tp->peer))
				torrent_drive_peer(t, tp);
		}
	}

	/* Anti-snubbing: mark peers with no data received for snub_timeout */
	{
		time_t now_t = time(NULL);
		struct torrent_peer *tp;
		SLIST_FOREACH(tp, &t->peers, entry) {
			if (tp->failures >= 10 || tp->snubbed || !tp->peer)
				continue;
			if ((tp->outstanding > 0 || mbt_peer_choked(tp->peer)) &&
			    tp->last_recv > 0 &&
			    now_t - tp->last_recv >= t->timers.snub_timeout) {
				tp->snubbed = 1;
				mbt_log_debug(MBT_LOG_TORRENT, "%s:%hu: snubbed (%s, no data for %ds)",
				              mbt_peer_addr(tp->peer), mbt_peer_conn_port(tp->peer),
				              mbt_peer_choked(tp->peer) ? "choked" : "no response",
				              t->timers.snub_timeout);
				/* Cancel outstanding requests and release owned pieces */
				{
					struct piece_info *spi, *snpi;
					SLIST_FOREACH_SAFE(spi, &tp->active_pieces, owner_entry, snpi) {
						size_t plen = piece_length_for(t, spi->piece);
						size_t clen = BLOCK_SIZE;
						for (size_t off = spi->received; off < spi->requested; off += clen) {
							if (off + clen > plen) clen = plen - off;
							mbt_peer_send_cancel(tp->peer, spi->piece, off, clen);
						}
						if (spi->requested >= piece_length_for(t, spi->piece) &&
						    !mbt_torrent_has_piece(t, spi->piece))
							bucket_reinsert(t, spi->piece);
						spi->requested = spi->received;
						piece_release(spi);
					}
				}
				tp->outstanding = 0;
				tp->reqq_window = PIPELINE_INIT;
			}
		}
	}

	/* In endgame, re-drive idle unchoked peers every tick so duplicate
	 * requests keep going out even when no piece completions trigger it. */
	if (t->endgame && t->have_count < t->piece_count) {
		struct torrent_peer *tp;
		SLIST_FOREACH(tp, &t->peers, entry) {
			if (tp->failures < 10 && tp->peer &&
			    !mbt_peer_choked(tp->peer) && !tp->snubbed &&
			    tp->outstanding == 0)
				torrent_drive_peer(t, tp);
		}
	}

reschedule:
	mbt_ev_timer_mod(timer, 5 * 1000000ULL); /* check every 5s */
}

/* --- Peer callbacks --- */

static struct torrent_peer *
find_torrent_peer(struct mbt_torrent *t, struct mbt_peer *p)
{
	(void)t;
	return mbt_peer_user(p);
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
		mbt_be_unref(msg);
		goto done;
	}
	mbt_be_unref(msg);

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
		/* Decay failures every 3rd round (30s) */
		if (t->choke_round % 3 == 0 && tp->failures > 0)
			tp->failures--;
		/* Choke peers not interested in us */
		if (tp->peer && tp->failures < 10 && !tp->peer_interested && !tp->am_choking) {
			mbt_peer_send_choke(tp->peer, 1);
			tp->am_choking = 1;
		}
		/* Disconnect seed-to-seed: neither side interested */
		if (tp->peer && tp->failures < 10 && !tp->peer_interested &&
		    t->have_count == t->piece_count) {
			mbt_peer_disconnect(tp->peer);
			tp->failures += 5;
		}
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

	/* Express interest only if we need pieces (skip if no metadata yet) */
	if (t->piece_count > 0)
		mbt_peer_send_interested(p, t->have_count < t->piece_count ? 1 : 0);

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
			mbt_bitmap_new_bits(&t->metadata_recv, t->metadata_pieces);
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
	              mbt_peer_addr(p), mbt_peer_conn_port(p),
	              choked ? "choked us" : "unchoked us",
	              pb ? mbt_bitmap_count(pb) : 0, t->piece_count);

	if (!choked && t->piece_count > 0)
		torrent_drive_peer(t, tp);

	/* When choked, release pieces owned by this peer so others can take them */
	if (choked && t->piece_count > 0) {
		struct piece_info *cpi, *cnpi;
		SLIST_FOREACH_SAFE(cpi, &tp->active_pieces, owner_entry, cnpi)
			piece_info_free_one(t, cpi);
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

	bucket_piece_inc(t, piece);

	/* Become interested if peer has a piece we need */
	if (t->have && mbt_bitmap_isset(t->have, piece) <= 0 &&
	    !mbt_peer_interested(p))
		mbt_peer_send_interested(p, 1);

	/* If we're unchoked and need this piece, maybe request it */
	if (!mbt_peer_choked(p) && t->piece_count > 0 &&
	    t->have_count < t->piece_count)
		torrent_drive_peer(t, tp);
}

static void
on_bitfield(struct mbt_peer *p, struct mbt_bitmap *bits, void *arg)
{
	struct mbt_torrent *t = arg;
	struct torrent_peer *tp = find_torrent_peer(t, p);
	if (!tp) return;

	mbt_log_debug(MBT_LOG_TORRENT, "%s:%hu: bitfield %zu/%u pieces",
	              mbt_peer_addr(p), mbt_peer_conn_port(p),
	              mbt_bitmap_count(bits), t->piece_count);

	for (uint32_t i = 0; i < t->piece_count; i++)
		if (mbt_bitmap_isset(bits, i) > 0)
			bucket_piece_inc(t, i);

	/* Only count this peer's avail contribution if we had metadata to process it */
	if (t->piece_count > 0) {
		t->peers_with_avail++;
		int min_peers = mbt_session_min_peers_before_download(t->session);
		if (min_peers > 0 && t->peers_with_avail == min_peers) {
			torrent_drive_all_peers(t);
			return;
		}
	}
	if (!mbt_peer_choked(p) && t->piece_count > 0)
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
		              mbt_peer_addr(p), mbt_peer_conn_port(p));
	}

	if (piece >= t->piece_count)
		return;

	/* Already have this piece — ignore late/unsolicited data */
	if (mbt_bitmap_isset(t->have, piece) > 0) {
		struct torrent_peer *tp2 = find_torrent_peer(t, p);
		if (tp2) if (tp2->outstanding > 0) tp2->outstanding--;
		return;
	}

	struct piece_info *pi = piece_info_find(t, piece);
	size_t plen = piece_length_for(t, piece);

	if (offset + len > plen)
		return; /* out of bounds */

	/* Allocate piece buffer on first block */
	if (!pi || !pi->data) {
		/* In endgame, accept data from any peer; otherwise only the owner
		 * can trigger buffer allocation to avoid spurious piece assignments. */
		if (!pi || (!t->endgame && pi->owner != tp)) {
			if (tp->outstanding > 0) tp->outstanding--;
			return;
		}
		if (!pi->data) {
			pi->data = calloc(1, plen);
			if (!pi->data) return;
		}
	}

	/* Track block by index, ignore true duplicates */
	uint32_t block_idx = offset / (16 * 1024);
	if (pi->blocks && mbt_bitmap_isset(pi->blocks, block_idx) > 0)
		{ if (tp->outstanding > 0) tp->outstanding--; return; } /* duplicate block */

	memcpy(pi->data + offset, data, len);
	if (!pi->blocks) {
		uint32_t nblocks = (plen + 16*1024 - 1) / (16*1024);
		mbt_bitmap_new_bits(&pi->blocks, nblocks);
	}
	mbt_bitmap_set(pi->blocks, block_idx);
	pi->received += len;
	tp->bytes_from += len;
	tp->total_down += len;
	pi->last_activity = now_usec();

	if (tp->outstanding > 0) tp->outstanding--;
	if (tp->reqq_window < PIPELINE_DEPTH)
		tp->reqq_window++;

	/* Piece complete? */
	if (pi->received >= plen) {
		/* SHA1 verify */
		SHA1_CTX ctx;
		uint8_t digest[20];

		SHA1Init(&ctx);
		SHA1Update(&ctx, pi->data, plen);
		SHA1Final(digest, &ctx);

		if (memcmp(digest, piece_hash(t, piece), 20) != 0) {
			/* Hash mismatch — reset piece, blame peer */
			char got[41], want[41];
			mbt_hex_encode(digest, 20, got, sizeof(got));
			mbt_hex_encode((unsigned char *)piece_hash(t, piece), 20, want, sizeof(want));
			mbt_log_warn(MBT_LOG_TORRENT, "piece %u: hash mismatch from %s:%hu",
			             piece, mbt_peer_addr(p), mbt_peer_conn_port(p));
			mbt_log_debug(MBT_LOG_TORRENT, "  got=%s want=%s len=%zu received=%zu blocks=0x%x",
			              got, want, plen, pi->received, pi->blocks ? mbt_bitmap_count(pi->blocks) : 0);
			piece_info_free_one(t, pi);

			/* Penalize peer for hash failure */
			tp->hash_failures++;
			tp->failures += 3;
			if (tp->hash_failures >= MAX_HASH_FAILURES) {
				mbt_log_warn(MBT_LOG_TORRENT, "penalizing %s:%hu after %d hash failures",
				             mbt_peer_addr(p), mbt_peer_conn_port(p), tp->hash_failures);
				tp->failures += 5;
				mbt_peer_disconnect(p); /* triggers deferred on_error → cleanup */
			}
			/* Piece is now PENDING again — drive all peers to pick it up */
			struct torrent_peer *rtp;
			SLIST_FOREACH(rtp, &t->peers, entry) {
				if (rtp->peer && rtp->failures < 10 && !mbt_peer_choked(rtp->peer))
					torrent_drive_peer(t, rtp);
			}
			return;
		} else {
			/* Verified — store */
			mbt_bitmap_set(t->have, piece);
			t->have_count++;
			if (tp && tp->failures > 0) tp->failures--;
			tp->hash_failures = 0;

			if (t->storage.ops && t->storage.ops->write_piece)
				t->storage.ops->write_piece(t->storage.ctx, piece, pi->data, plen);

			piece_info_free_one(t, pi);
			bucket_piece_complete(t, piece);

			mbt_log_debug(MBT_LOG_TORRENT, "piece %u verified from %s:%hu outstanding=%d reqq_window=%d",
			             piece, mbt_peer_addr(p), mbt_peer_conn_port(p),
			             tp->outstanding, tp->reqq_window);

			/* Broadcast HAVE to all connected peers */
			SLIST_FOREACH(hp, &t->peers, entry)
				mbt_peer_send_have(hp->peer, piece);

			if (t->cb.on_piece_complete)
				t->cb.on_piece_complete(t, piece, t->cb_arg);

			/* Endgame: cancel this piece from all other peers */
			if (t->endgame) {
				struct torrent_peer *otp;
				SLIST_FOREACH(otp, &t->peers, entry) {
					if (otp == tp || !otp->peer || otp->failures >= 10)
						continue;
					for (size_t off = 0; off < plen; off += BLOCK_SIZE) {
						uint32_t clen = MIN(BLOCK_SIZE, plen - off);
						mbt_peer_send_cancel(otp->peer, piece, off, clen);
					}
				}
			}

			/* Check completion */
			if (t->have_count == t->piece_count) {
				/* Tell all peers we're no longer interested */
				struct torrent_peer *np;
				SLIST_FOREACH(np, &t->peers, entry) {
					if (np->peer && np->failures < 10)
						mbt_peer_send_interested(np->peer, 0);
				}
				bucket_free(t);
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
					if (otp->peer && otp->failures < 10 && !mbt_peer_choked(otp->peer))
						torrent_drive_peer(t, otp);
				}
			}
			return;
		}
	}

	/* During slow-start, pipeline may not cover a full piece yet — refill per block.
	 * Once reqq_window exceeds one piece worth of blocks, piece completion handles it. */
	{
		int bpp = t->piece_length ? (int)((t->piece_length + BLOCK_SIZE - 1) / BLOCK_SIZE) : 1;
		if (tp->reqq_window < bpp)
			torrent_drive_peer(t, tp);
	}
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
	/* Only count this piece once — duplicate delivery from multiple peers */
	if (!t->metadata_recv || !mbt_bitmap_isset(t->metadata_recv, piece)) {
		memcpy(t->metadata + offset, data, copy_len);
		t->metadata_have += copy_len;
		if (t->metadata_recv)
			mbt_bitmap_set(t->metadata_recv, piece);
	}

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
			char got[41], want[41];
			mbt_hex_encode(digest, 20, got, sizeof(got));
			mbt_hex_encode(t->ih->hash_sha1, 20, want, sizeof(want));
			mbt_log_warn(MBT_LOG_TORRENT,
			    "metadata hash mismatch: got=%s want=%s len=%zu have=%zu",
			    got, want, t->metadata_len, t->metadata_have);
			t->metadata_have = 0;
			memset(t->metadata, 0, t->metadata_len);
			if (t->metadata_recv)
				mbt_bitmap_clear(t->metadata_recv);
			return;
		}

		/* Parse and set metadata */
		mbt_log_info(MBT_LOG_TORRENT, "metadata complete from %s:%hu (%zu bytes)",
		             mbt_peer_addr(p), mbt_peer_conn_port(p), t->metadata_len);
		mbt_torrent_set_metadata(t, t->metadata, t->metadata_len);

		if (t->piece_count > 0)
			mbt_log_info(MBT_LOG_TORRENT, "name=\"%s\" pieces=%u piece_length=%u total=%llu",
			             t->name ? t->name : "?", t->piece_count,
			             t->piece_length, (unsigned long long)t->total_length);

		/* Drive all peers now that we have metadata */
		struct torrent_peer *tp;
		int npeers = 0;
		SLIST_FOREACH(tp, &t->peers, entry) {
			npeers++;
			/* Update piece_count — applies deferred HAVE_ALL */
			mbt_peer_set_piece_count(tp->peer, t->piece_count);
			/* Re-send interested in case it was missed */
			mbt_peer_send_interested(tp->peer, 1);
			mbt_log_debug(MBT_LOG_TORRENT, "post-metadata: peer %s:%hu choked=%d pieces=%p",
			              mbt_peer_addr(tp->peer), mbt_peer_port(tp->peer),
			              mbt_peer_choked(tp->peer), (void*)(uintptr_t)t->piece_count);
			if (!mbt_peer_choked(tp->peer) && t->piece_count > 0)
				torrent_drive_peer(t, tp);
		}
		mbt_log_debug(MBT_LOG_TORRENT, "post-metadata: %d peers in list", npeers);
	}
}

static void
on_interest(struct mbt_peer *p, int interested, void *arg)
{
	struct mbt_torrent *t = arg;
	struct torrent_peer *tp = find_torrent_peer(t, p);
	mbt_log_debug(MBT_LOG_TORRENT, "%s:%hu: %s",
	              mbt_peer_addr(p), mbt_peer_conn_port(p),
	              interested ? "peer INTERESTED" : "peer NOT_INTERESTED");
	if (tp) tp->peer_interested = interested;
}

static void
on_request(struct mbt_peer *p, uint32_t piece, uint32_t offset, uint32_t length, void *arg)
{
	struct mbt_torrent *t = arg;
	struct torrent_peer *tp = find_torrent_peer(t, p);

	mbt_log_debug(MBT_LOG_TORRENT, "%s:%hu: on_request piece=%u off=%u len=%u",
	              mbt_peer_addr(p), mbt_peer_conn_port(p), piece, offset, length);

	/* Only serve if we have the piece */
	if (mbt_bitmap_isset(t->have, piece) <= 0)
		return;

	if (!t->storage.ops || !t->storage.ops->read_piece)
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

	int rv = t->storage.ops->read_piece(t->storage.ctx, piece, buf, plen);
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
	if (tp) {
		tp->bytes_to += length;
		tp->total_up += length;
	}
	free(buf);
}

static void
on_pex(struct mbt_peer *p, const char *addr, uint16_t port, void *arg)
{
	(void)p;
	struct mbt_torrent *t = arg;
	if (t->is_private) return; /* BEP 27: no PEX for private torrents */
	if (port == 0) return;    /* port 0 is invalid */
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
	struct piece_info *pi = piece_info_find(t, piece);

	/* Reset piece if this peer owned it */
	struct torrent_peer *tp = find_torrent_peer(t, p);
	if (tp && pi && pi->owner == tp) {
		piece_info_free_one(t, pi);
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
	(void)err;
	struct mbt_torrent *t = arg;
	struct torrent_peer *tp = find_torrent_peer(t, p);
	if (!tp) return;

	/* Decrement availability for all pieces this peer had */
	{
		struct mbt_bitmap *pb = mbt_peer_bitfield(p);
		if (pb)
			for (uint32_t i = 0; i < t->piece_count; i++)
				if (mbt_bitmap_isset(pb, i) > 0)
					bucket_piece_dec(t, i);
	}

	/* Reset any pieces this peer was downloading */
	{
		struct piece_info *pi, *npi;
		SLIST_FOREACH_SAFE(pi, &tp->active_pieces, owner_entry, npi)
			piece_info_free_one(t, pi);
	}

	/* Remove peer from list */
	SLIST_REMOVE(&t->peers, tp, torrent_peer, entry);

	/* Don't free tp->peer here — we may be inside its callback chain.
	 * The peer is already CLOSED and will not deliver more callbacks.
	 * Free on next event loop iteration via deferred timer. */
	if (tp->failures < 10)
		tp->failures += 5; /* prevent re-use */

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
torrent_drive_all_peers(struct mbt_torrent *t)
{
	struct torrent_peer *tp;
	SLIST_FOREACH(tp, &t->peers, entry)
		torrent_drive_peer(t, tp);
}

static void
torrent_drive_peer(struct mbt_torrent *t, struct torrent_peer *tp)
{
	if (t->piece_count == 0 || mbt_peer_choked(tp->peer) || t->verifying) {
		mbt_log_debug(MBT_LOG_TORRENT, "drive_peer: skip (no_metadata=%d choked=%d verifying=%d)",
		              t->piece_count == 0, mbt_peer_choked(tp->peer), t->verifying);
		return;
	}
	int min_peers = mbt_session_min_peers_before_download(t->session);
	if (min_peers > 0 && t->peers_with_avail < min_peers)
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
	if (tp->reqq_window == 0)
		tp->reqq_window = PIPELINE_INIT;
	int limit = tp->reqq_window;
	int peer_limit = mbt_peer_reqq(tp->peer);
	if (peer_limit > 0 && peer_limit < limit)
		limit = peer_limit;
	int max_pipeline = mbt_session_max_pipeline(t->session);
	if (max_pipeline == 0) max_pipeline = PIPELINE_DEPTH;
	if (limit > max_pipeline)
		limit = max_pipeline;

	while (tp->outstanding < limit) {
		/* Prefer continuing a piece this peer already owns */
		int idx = -1;
		struct piece_info *api;
		SLIST_FOREACH(api, &tp->active_pieces, owner_entry) {
			if (api->requested < piece_length_for(t, api->piece)) {
				idx = (int)api->piece;
				break;
			}
		}
		/* Don't pick a new piece if already at cap; finish requesting existing ones */
		int max_pp = t->max_pieces_per_peer ? t->max_pieces_per_peer : 4;
		if (t->piece_length > 0) {
			int bpp = (int)((t->piece_length + BLOCK_SIZE - 1) / BLOCK_SIZE);
			int needed = (limit + bpp - 1) / bpp;
			if (needed > max_pp) max_pp = needed;
		}
		if (idx < 0 && tp->active_count >= max_pp)
			break;
		/* If no existing piece, pick a new one */
		if (idx < 0)
			idx = t->picker(t, tp->peer, t->picker_ctx);
		if (idx < 0) {
			if (mbt_peer_bitfield(tp->peer))
				mbt_log_debug(MBT_LOG_TORRENT, "drive_peer: picker returned -1 (have=%u/%u)",
				              t->have_count, t->piece_count);
			/* Endgame: picker returned -1 meaning all remaining pieces are
			 * fully requested by their owners. If this peer is idle, send
			 * duplicate requests for in-flight pieces so a slow owner doesn't
			 * stall completion. Trigger on any remaining count when idle
			 * (not just the small threshold) — outstanding==0 prevents spam. */
			if (t->piece_count > 0) {
				uint32_t remaining = t->piece_count - t->have_count;
				if (remaining > 0 && (remaining <= ENDGAME_THRESHOLD || tp->outstanding == 0)) {
					if (!t->endgame) {
						t->endgame = 1;
						mbt_log_info(MBT_LOG_TORRENT, "entering endgame mode (%u pieces remaining)", remaining);
					}
					struct mbt_bitmap *peer_bits = mbt_peer_bitfield(tp->peer);
					for (int _eb = 0; _eb < PIECE_TABLE_BUCKETS && tp->outstanding < limit; _eb++) {
						struct piece_info *epi;
						SLIST_FOREACH(epi, &t->piece_table[_eb], table_entry) {
						if (mbt_bitmap_isset(t->have, epi->piece) > 0)
							continue;
						if (peer_bits && mbt_bitmap_isset(peer_bits, epi->piece) <= 0)
							continue;
						size_t eplen = piece_length_for(t, epi->piece);
						for (size_t off = 0; off < eplen && tp->outstanding < limit; off += BLOCK_SIZE) {
							uint32_t req_len = MIN(BLOCK_SIZE, eplen - off);
							if (mbt_ratelimit_delay(t->download_limit, req_len) > 0 ||
							    mbt_ratelimit_delay(mbt_session_get_download_limit(t->session), req_len) > 0)
								break;
							mbt_ratelimit_consume(t->download_limit, req_len);
							mbt_ratelimit_consume(mbt_session_get_download_limit(t->session), req_len);
							mbt_peer_send_request(tp->peer, epi->piece, off, req_len);
							tp->outstanding++;
						}
						} /* SLIST_FOREACH epi */
					} /* bucket loop */
				}
			}
			break;
		}

		if (mbt_bitmap_isset(t->have, idx) > 0)
			continue;

		struct piece_info *pi = piece_info_find(t, (uint32_t)idx);
		size_t pi_len = piece_length_for(t, (uint32_t)idx);

		/* Already fully requested — skip */
		if (pi && pi->requested >= pi_len)
			continue;

		if (!pi) {
			pi = piece_info_alloc(t, (uint32_t)idx);
			if (!pi) break;
			piece_assign(pi, tp);
			pi->last_activity = now_usec();
		}

		/* Request unrequested blocks for this piece */
		size_t off;
		int blocked = 0;
		for (off = pi->requested; off < pi_len && tp->outstanding < limit; off += BLOCK_SIZE) {
			uint32_t req_len = MIN(BLOCK_SIZE, pi_len - off);
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
		if (pi->requested >= piece_length_for(t, pi->piece))
			bucket_piece_complete(t, pi->piece);
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
		if (tp->failures < 10 && tp->peer && !mbt_peer_choked(tp->peer))
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
	t->storage = cfg->storage;
	if (cfg->data_dir)
		t->data_dir = strdup(cfg->data_dir);
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

	/* Free active pieces */
	for (int _b = 0; _b < PIECE_TABLE_BUCKETS; _b++) {
		struct piece_info *pi, *npi;
		SLIST_FOREACH_SAFE(pi, &t->piece_table[_b], table_entry, npi) {
			free(pi->data);
			if (pi->blocks) mbt_bitmap_unref(pi->blocks);
			free(pi);
		}
	}
	bucket_free(t);

	if (t->timeout_timer)
		mbt_ev_timer_free(t->timeout_timer);
	if (t->choke_timer)
		mbt_ev_timer_free(t->choke_timer);
	if (t->pex_timer)
		mbt_ev_timer_free(t->pex_timer);
	if (t->ratelimit_timer)
		mbt_ev_timer_free(t->ratelimit_timer);
	if (t->verify_timer)
		mbt_ev_timer_free(t->verify_timer);
	free(t->verify_buf);
	mbt_peerdb_free(&t->peerdb);
	mbt_bitmap_unref(t->have);
	mbt_bitmap_unrefp(&t->metadata_recv);
	free(t->metadata);
	free(t->name);
	if (t->files) {
		for (int i = 0; i < t->file_count; i++)
			free(t->files[i].path);
		free(t->files);
	}
	free(t->data_dir);
	mbt_infohash_unref(t->ih);
	mbt_ratelimit_free(t->upload_limit);
	mbt_ratelimit_free(t->download_limit);
	if (t->storage.ops && t->storage.ops->close)
		t->storage.ops->close(t->storage.ctx);
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
	_cleanup_be_ struct mbt_be_value *be = NULL;
	struct mbt_be_value *tmp;
	int rv;

	/* Already have metadata parsed — skip */
	if (t->piece_count > 0)
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
	if (tmp && tmp->type == BE_STRING && tmp->string.len > 0) {
		free(t->name);
		t->name = strndup((char *)tmp->string.buf, tmp->string.len);
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
					size_t clen = comp->string.len;
					if (pos + clen < sizeof(pathbuf)) {
						memcpy(pathbuf + pos, comp->string.buf, clen);
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

	t->piece_count = (uint32_t)(tmp->string.len / 20);
	t->pieces_hash_len = tmp->string.len;

	/* Locate the pieces data in the raw metadata buffer.
	 * Bencoded format: "6:pieces" + "<len>:" + <data>. */
	{
		char needle[32];
		int nlen = snprintf(needle, sizeof(needle), "6:pieces%zu:", tmp->string.len);
		const uint8_t *p = memmem(info, len, needle, nlen);
		if (!p)
			return -EINVAL;
		/* p + nlen points to the hash data within the info buffer;
		 * t->metadata is a copy of info at the same layout. */
		t->pieces_hash = t->metadata + (p + nlen - (const uint8_t *)info);
	}

	/* Allocate bucket queue entries */
	t->piece_entries = calloc(t->piece_count, sizeof(*t->piece_entries));
	if (t->piece_entries)
		bucket_init(t);

	/* Allocate have bitmap */
	mbt_bitmap_new_bits(&t->have, t->piece_count);

	/* Open storage for this torrent */
	if (t->storage.ops && t->storage.ops->open) {
		int srv = t->storage.ops->open(t->storage.ctx, t);
		if (srv < 0) {
			mbt_log_warn(MBT_LOG_STORAGE, "storage open failed: %s", strerror(-srv));
			return srv;
		}
	}

	if (t->storage.ops && t->storage.ops->read_piece && !t->verifying) {
		t->verify_buf = malloc(t->piece_length);
		if (t->verify_buf) {
			t->verifying = 1;
			t->verify_cursor = 0;
			mbt_ev_timer_new(t->ev, &t->verify_timer, 0, verify_tick, t);
		}
	}

	return 0;
}

int
mbt_torrent_add_peer(struct mbt_torrent *t, struct mbt_conn *conn, enum mbt_peer_source source)
{
	struct torrent_peer *tp;
	struct mbt_peer *peer;
	int rv;

	tp = calloc(1, sizeof(*tp));
	if (!tp)
		return -ENOMEM;

	rv = mbt_peer_new(&peer, t->ev, conn, t->ih, t->peer_id,
	                  t->piece_count, source, &torrent_peer_cbs, t);
	if (rv < 0) {
		free(tp);
		return rv;
	}

	mbt_peer_set_our_metadata_size(peer, t->metadata_len);
	if (mbt_session_reqq(t->session) > 0)
		mbt_peer_set_our_reqq(peer, mbt_session_reqq(t->session));
	if (mbt_session_listen_port(t->session) > 0)
		mbt_peer_set_our_listen_port(peer, mbt_session_listen_port(t->session));

	tp->peer = peer;
	mbt_peer_set_user(peer, tp);
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
	                           t->piece_count, MBT_SOURCE_INCOMING, ext, their_id,
	                           t->metadata_len, mbt_session_listen_port(t->session),
	                           &torrent_peer_cbs, t);
	if (rv < 0) {
		free(tp);
		return rv;
	}

	tp->peer = peer;
	mbt_peer_set_user(peer, tp);
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

const char *
mbt_torrent_data_dir(struct mbt_torrent *t)
{
	return t->data_dir;
}

const void *
mbt_torrent_metadata(struct mbt_torrent *t, size_t *len_out)
{
	if (len_out) *len_out = t->metadata_len;
	return t->metadata;
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
	if (t->piece_count == 0)
		return MBT_TORRENT_INIT;
	if (t->verifying)
		return MBT_TORRENT_VERIFYING;
	if (t->have_count == t->piece_count)
		return MBT_TORRENT_SEEDING;

	/* Check if any unchoked peer has a piece we need */
	struct torrent_peer *tp;
	SLIST_FOREACH(tp, &t->peers, entry) {
		if (tp->failures >= 10 || !tp->peer) continue;
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

uint64_t
mbt_torrent_bytes_downloaded(struct mbt_torrent *t)
{
	uint64_t total = 0;
	struct torrent_peer *tp;
	SLIST_FOREACH(tp, &t->peers, entry)
		total += tp->total_down;
	return total;
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
	return t->piece_count > 0;
}

int
mbt_torrent_has_piece(struct mbt_torrent *t, uint32_t piece)
{
	if (!t->have || piece >= t->piece_count)
		return 0;
	return mbt_bitmap_isset(t->have, piece) > 0;
}

struct mbt_bitmap *
mbt_torrent_have_bitmap(struct mbt_torrent *t)
{
	return t ? t->have : NULL;
}

int
mbt_torrent_piece_active(struct mbt_torrent *t, uint32_t piece)
{
	if (piece >= t->piece_count) return 0;
	return piece_info_find(t, piece) != NULL;
}

int
mbt_torrent_piece_fully_requested(struct mbt_torrent *t, uint32_t piece)
{
	if (piece >= t->piece_count) return 0;
	struct piece_info *pi = piece_info_find(t, piece);
	if (!pi) return 0;
	return pi->requested >= piece_length_for(t, piece);
}

int
mbt_torrent_piece_avail(struct mbt_torrent *t, uint32_t piece)
{
	if (!t->piece_entries || piece >= t->piece_count)
		return 0;
	return t->piece_entries[piece].avail;
}

void
mbt_torrent_piece_map(struct mbt_torrent *t, uint8_t *out, uint32_t count)
{
	uint32_t n = count < t->piece_count ? count : t->piece_count;
	for (uint32_t i = 0; i < n; i++) {
		if (mbt_bitmap_isset(t->have, i) > 0)
			out[i] = 2;
		else if (piece_info_find(t, i) != NULL)
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
		if (tp->failures >= 10 || !tp->peer) continue;
		if (i == idx) {
			out->addr = mbt_peer_addr(tp->peer);
			out->port = mbt_peer_port(tp->peer);
			out->conn_port = mbt_peer_conn_port(tp->peer);
			out->choked = mbt_peer_choked(tp->peer);
			out->interested = mbt_peer_interested(tp->peer);
			out->snubbed = tp->snubbed;
			out->outstanding = tp->outstanding;
			out->hash_failures = tp->hash_failures;
			out->score = tp->failures;
			out->source = mbt_peer_source(tp->peer);
			out->client_version = mbt_peer_version(tp->peer);
			out->downloaded = tp->total_down;
			out->uploaded = tp->total_up;
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

int
mbt_picker_rarest_bucket(struct mbt_torrent *t, void *peer, void *ctx)
{
	(void)ctx;
	struct mbt_peer *p = peer;
	struct mbt_bitmap *peer_bits = mbt_peer_bitfield(p);
	if (!peer_bits || !t->avail_buckets || t->piece_count == 0) return -1;

	if (mbt_torrent_have_count(t) == 0)
		return mbt_picker_random(t, peer, ctx);

	for (int b = 0; b < t->avail_buckets_n; b++) {
		struct piece_entry *e;
		LIST_FOREACH(e, &t->avail_buckets[b], bucket_entry) {
			uint32_t piece = (uint32_t)(e - t->piece_entries);
			if (mbt_torrent_has_piece(t, piece)) continue;
			if (mbt_bitmap_isset(peer_bits, piece) <= 0) continue;
			if (mbt_torrent_piece_fully_requested(t, piece)) continue;
			return (int)piece;
		}
	}
	return -1;
}
