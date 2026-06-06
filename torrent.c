#include <sys/param.h>
#include <stdlib.h>
#include <string.h>
#include <errno.h>
#include <time.h>

#include <sha1.h>

#include "log.h"

#include "event.h"
#include "conn.h"
#include "infohash.h"
#include "peer.h"
#include "torrent.h"
#include "bitmap.h"
#include "be.h"

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
	enum mbt_piece_state state;
	struct torrent_peer *owner; /* peer downloading this piece, NULL if pending */
	uint64_t last_activity; /* monotonic time of last block received (usec) */
};

/* Per-peer entry in the torrent's peer list */
struct torrent_peer {
	struct mbt_peer *peer;
	struct torrent_peer *next;
	int outstanding;       /* in-flight requests */
	int hash_failures;     /* consecutive hash failures from this peer */
	int banned;
};

struct mbt_torrent {
	struct mbt_ev *ev;
	struct mbt_infohash *ih;
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

	/* Have bitmap */
	struct mbt_bitmap *have;
	uint32_t have_count;

	/* Peers */
	struct torrent_peer *peers;
	int peer_count;

	/* Request timeout */
	struct mbt_ev_timer *timeout_timer;
	int request_timeout_sec; /* 0 = disabled */

	/* Endgame mode */
	int endgame;  /* 1 when all pieces are requested at least once */
};

enum {
	MAX_HASH_FAILURES = 3,   /* ban after this many consecutive failures */
	ENDGAME_THRESHOLD = 10,  /* enter endgame with this many pieces remaining */
};

/* --- Forward declarations --- */
static void torrent_drive_peer(struct mbt_torrent *t, struct torrent_peer *tp);
static void torrent_request_metadata(struct mbt_torrent *t, struct torrent_peer *tp);

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
	uint64_t deadline = (uint64_t)t->request_timeout_sec * 1000000;
	int reset_count = 0;

	if (!t->pieces)
		goto reschedule;

	for (uint32_t i = 0; i < t->piece_count; i++) {
		struct piece_info *pi = &t->pieces[i];
		if (pi->state != MBT_PIECE_ACTIVE)
			continue;
		if (pi->last_activity == 0)
			continue;
		if (now - pi->last_activity < deadline)
			continue;

		/* Timed out — reset piece */
		mbt_log_warn(MBT_LOG_TORRENT, "piece %u timed out from %s:%hu, resetting",
		             i, pi->owner ? mbt_peer_addr(pi->owner->peer) : "?",
		             pi->owner ? mbt_peer_port(pi->owner->peer) : 0);

		pi->state = MBT_PIECE_PENDING;
		pi->owner = NULL;
		pi->received = 0;
		pi->requested = 0;
		pi->last_activity = 0;
		free(pi->data);
		pi->data = NULL;
		reset_count++;
	}

	/* Drive all unchoked peers if we freed up pieces */
	if (reset_count > 0) {
		struct torrent_peer *tp;
		for (tp = t->peers; tp; tp = tp->next) {
			if (!tp->banned && !mbt_peer_choked(tp->peer))
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
	struct torrent_peer *tp;
	for (tp = t->peers; tp; tp = tp->next)
		if (tp->peer == p)
			return tp;
	return NULL;
}

static void
on_active(struct mbt_peer *p, void *arg)
{
	struct mbt_torrent *t = arg;
	struct torrent_peer *tp = find_torrent_peer(t, p);
	if (!tp) return;

	mbt_log_debug(MBT_LOG_TORRENT, "on_active: metadata=%p metadata_size=%zu",
	              (void *)t->metadata, mbt_peer_metadata_size(p));

	/* Always express interest */
	mbt_peer_send_interested(p, 1);

	/* If we need metadata, request it */
	if (!t->metadata && mbt_peer_metadata_size(p) > 0) {
		t->metadata_len = mbt_peer_metadata_size(p);
		t->metadata = calloc(1, t->metadata_len);
		t->metadata_pieces = (t->metadata_len + METADATA_BLOCK_SIZE - 1) / METADATA_BLOCK_SIZE;
		torrent_request_metadata(t, tp);
	}
}

static void
on_choke(struct mbt_peer *p, int choked, void *arg)
{
	struct mbt_torrent *t = arg;
	struct torrent_peer *tp = find_torrent_peer(t, p);
	if (!tp) return;

	mbt_log_debug(MBT_LOG_TORRENT, "on_choke: choked=%d has_pieces=%d", choked, t->pieces != NULL);

	if (!choked && t->pieces)
		torrent_drive_peer(t, tp);
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

	if (piece >= t->piece_count)
		return;

	struct piece_info *pi = &t->pieces[piece];

	if (mbt_bitmap_isset(t->have, piece) > 0)
		return; /* duplicate */

	if (offset + len > pi->length)
		return; /* out of bounds */

	/* Allocate piece buffer on first block */
	if (!pi->data) {
		pi->data = malloc(pi->length);
		if (!pi->data) return;
		pi->state = MBT_PIECE_ACTIVE;
		pi->owner = tp;
	}

	memcpy(pi->data + offset, data, len);
	pi->received += len;
	pi->last_activity = now_usec();

	tp->outstanding--;

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
			mbt_log_warn(MBT_LOG_TORRENT, "piece %u: hash mismatch from %s:%hu",
			             piece, mbt_peer_addr(p), mbt_peer_port(p));
			pi->received = 0;
			pi->requested = 0;
			pi->state = MBT_PIECE_PENDING;
			pi->owner = NULL;
			pi->last_activity = 0;
			free(pi->data);
			pi->data = NULL;

			/* Ban peer after repeated failures */
			tp->hash_failures++;
			if (tp->hash_failures >= MAX_HASH_FAILURES) {
				mbt_log_warn(MBT_LOG_TORRENT, "banning %s:%hu after %d hash failures",
				             mbt_peer_addr(p), mbt_peer_port(p), tp->hash_failures);
				tp->banned = 1;
				mbt_peer_free(tp->peer);
				tp->peer = NULL;
				return; /* peer is dead, don't continue */
			}
		} else {
			/* Verified — store */
			mbt_bitmap_set(t->have, piece);
			t->have_count++;
			pi->state = MBT_PIECE_PENDING;
			pi->owner = NULL;

			if (t->storage_ops && t->storage_ops->write_piece)
				t->storage_ops->write_piece(t->storage_ctx, piece, pi->data, pi->length);

			/* Free piece buffer */
			free(pi->data);
			pi->data = NULL;

			mbt_log_debug(MBT_LOG_TORRENT, "piece %u verified from %s:%hu",
			             piece, mbt_peer_addr(p), mbt_peer_port(p));

			if (t->cb.on_piece_complete)
				t->cb.on_piece_complete(t, piece, t->cb_arg);

			/* Endgame: cancel this piece from all other peers */
			if (t->endgame) {
				struct torrent_peer *otp;
				for (otp = t->peers; otp; otp = otp->next) {
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
		}
	}

	/* Keep requesting */
	/* Keep requesting */
	torrent_drive_peer(t, tp);
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
		for (tp = t->peers; tp; tp = tp->next) {
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
	/* TODO: seeding — track who wants data from us */
	(void)p; (void)interested; (void)arg;
}

static void
on_request(struct mbt_peer *p, uint32_t piece, uint32_t offset, uint32_t length, void *arg)
{
	/* TODO: seeding — serve piece from storage */
	(void)p; (void)piece; (void)offset; (void)length; (void)arg;
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
			}
		}
	}

	/* Remove peer from list */
	struct torrent_peer **pp;
	for (pp = &t->peers; *pp; pp = &(*pp)->next) {
		if (*pp == tp) {
			*pp = tp->next;
			break;
		}
	}

	mbt_peer_free(tp->peer);
	free(tp);
	t->peer_count--;
}

static struct mbt_peer_callbacks torrent_peer_cbs = {
	.on_active   = on_active,
	.on_choke    = on_choke,
	.on_have     = on_have,
	.on_bitfield = on_bitfield,
	.on_piece    = on_piece,
	.on_metadata = on_metadata,
	.on_interest = on_interest,
	.on_request  = on_request,
	.on_error    = on_error,
};

/* --- Drive piece requests --- */

static void
torrent_drive_peer(struct mbt_torrent *t, struct torrent_peer *tp)
{
	if (!t->pieces || mbt_peer_choked(tp->peer))
		return;

	int limit = PIPELINE_DEPTH;
	int peer_limit = mbt_peer_reqq(tp->peer);
	if (peer_limit > 0 && peer_limit < limit)
		limit = peer_limit;

	/* Spread work across peers — don't let one peer hog all pieces */
	if (t->peer_count > 1) {
		int per_peer = PIPELINE_DEPTH / t->peer_count;
		if (per_peer < 64)
			per_peer = 64;
		if (per_peer < limit)
			limit = per_peer;
	}

	while (tp->outstanding < limit) {
		int idx = t->picker(t, tp->peer, t->picker_ctx);
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

		if (!pi->data) {
			pi->data = malloc(pi->length);
			if (!pi->data) return;
			pi->state = MBT_PIECE_ACTIVE;
			pi->owner = tp;
			pi->last_activity = now_usec();
		}

		/* Request unrequested blocks for this piece */
		size_t off;
		for (off = pi->requested; off < pi->length && tp->outstanding < limit; off += BLOCK_SIZE) {
			uint32_t req_len = MIN(BLOCK_SIZE, pi->length - off);
			mbt_peer_send_request(tp->peer, idx, off, req_len);
			tp->outstanding++;
		}
		pi->requested = off;
	}
}

static void
torrent_request_metadata(struct mbt_torrent *t, struct torrent_peer *tp)
{
	for (int i = 0; i < t->metadata_pieces; i++)
		mbt_peer_send_metadata_request(tp->peer, i);
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
	t->picker = cfg->picker;
	t->picker_ctx = cfg->picker_ctx;
	t->request_timeout_sec = cfg->request_timeout_sec ? cfg->request_timeout_sec : 15;

	/* Start timeout scanner */
	mbt_ev_timer_new(t->ev, &t->timeout_timer, 5 * 1000000ULL, torrent_timeout_cb, t);

	*out = t;
	return 0;
}

void
mbt_torrent_free(struct mbt_torrent *t)
{
	if (!t)
		return;

	/* Free peers */
	struct torrent_peer *tp = t->peers;
	while (tp) {
		struct torrent_peer *next = tp->next;
		mbt_peer_free(tp->peer);
		free(tp);
		tp = next;
	}

	/* Free pieces */
	if (t->pieces) {
		for (uint32_t i = 0; i < t->piece_count; i++)
			free(t->pieces[i].data);
		free(t->pieces);
	}

	if (t->timeout_timer)
		mbt_ev_timer_free(t->timeout_timer);
	mbt_bitmap_unref(t->have);
	free(t->metadata);
	free(t->name);
	mbt_infohash_unref(t->ih);
	free(t);
}

void
mbt_torrent_set_callbacks(struct mbt_torrent *t, struct mbt_torrent_callbacks *cb, void *arg)
{
	t->cb = *cb;
	t->cb_arg = arg;
}

int
mbt_torrent_set_metadata(struct mbt_torrent *t, const void *info, size_t len)
{
	__attribute__((cleanup(be_value_freep))) struct be_value *be = NULL;
	struct be_value *tmp;
	int rv;

	rv = be_value_parse(info, len, &be);
	if (rv < 0)
		return rv;

	/* Extract name */
	tmp = be_find(be, "name");
	if (tmp && tmp->type == BE_STRING && tmp->string.iov_len > 0) {
		free(t->name);
		t->name = strndup(tmp->string.iov_base, tmp->string.iov_len);
	}

	/* Extract piece length */
	tmp = be_find(be, "piece length");
	if (!tmp || tmp->type != BE_INTEGER)
		return -EINVAL;
	t->piece_length = (uint32_t)tmp->integer;

	/* Extract total length (single-file) or compute from files list */
	tmp = be_find(be, "length");
	if (tmp && tmp->type == BE_INTEGER) {
		t->total_length = (uint64_t)tmp->integer;
	} else {
		/* Multi-file: sum lengths */
		/* TODO: proper multi-file support */
		return -ENOTSUP;
	}

	/* Extract piece hashes */
	tmp = be_find(be, "pieces");
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

	if (t->storage_ops && t->storage_ops->have_piece) {
		for (uint32_t i = 0; i < t->piece_count; i++) {
			if (t->storage_ops->have_piece(t->storage_ctx, i) > 0) {
				mbt_bitmap_set(t->have, i);
				t->have_count++;
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

	tp->peer = peer;
	tp->next = t->peers;
	t->peers = tp;
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
