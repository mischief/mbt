#include <sys/param.h>
#include <stdlib.h>
#include <string.h>
#include <errno.h>
#include <err.h>

#include <sha1.h>

#include "event.h"
#include "conn.h"
#include "infohash.h"
#include "peer.h"
#include "torrent.h"
#include "tbmap.h"
#include "be.h"

enum {
	BLOCK_SIZE = 16384,
	METADATA_BLOCK_SIZE = 16384,
	MAX_PEERS = 50,
	PIPELINE_DEPTH = 5,
};

/* Per-piece tracking */
struct piece_info {
	uint8_t *data;
	size_t length;
	size_t received;       /* bytes received so far */
	uint8_t hash[20];      /* expected SHA1 */
	enum mbt_piece_state state;
};

/* Per-peer entry in the torrent's peer list */
struct torrent_peer {
	struct mbt_peer *peer;
	struct torrent_peer *next;
	int outstanding;       /* in-flight requests */
};

struct mbt_torrent {
	struct mbt_ev *ev;
	struct infohash *ih;
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

	/* Have bitmap */
	struct tbmap *have;
	uint32_t have_count;

	/* Peers */
	struct torrent_peer *peers;
	int peer_count;
};

/* --- Forward declarations --- */
static void torrent_drive_peer(struct mbt_torrent *t, struct torrent_peer *tp);
static void torrent_request_metadata(struct mbt_torrent *t, struct torrent_peer *tp);

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
on_bitfield(struct mbt_peer *p, struct tbmap *bits, void *arg)
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

	if (pi->state == MBT_PIECE_COMPLETE)
		return; /* duplicate */

	if (offset + len > pi->length)
		return; /* out of bounds */

	/* Allocate piece buffer on first block */
	if (!pi->data) {
		pi->data = malloc(pi->length);
		if (!pi->data) return;
		pi->state = MBT_PIECE_ACTIVE;
	}

	memcpy(pi->data + offset, data, len);
	pi->received += len;

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
			/* Hash mismatch — reset piece */
			warnx("piece %u: hash mismatch, re-requesting", piece);
			pi->received = 0;
			pi->state = MBT_PIECE_PENDING;
			free(pi->data);
			pi->data = NULL;
		} else {
			/* Verified — store */
			pi->state = MBT_PIECE_COMPLETE;
			tbmap_set(t->have, piece);
			t->have_count++;

			if (t->storage_ops && t->storage_ops->write_piece)
				t->storage_ops->write_piece(t->storage_ctx, piece, pi->data, pi->length);

			/* Free piece buffer */
			free(pi->data);
			pi->data = NULL;

			if (t->cb.on_piece_complete)
				t->cb.on_piece_complete(t, piece, t->cb_arg);

			/* Check completion */
			if (t->have_count == t->piece_count) {
				if (t->cb.on_complete)
					t->cb.on_complete(t, t->cb_arg);
				return;
			}
		}
	}

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
			warnx("metadata hash mismatch, discarding");
			t->metadata_have = 0;
			memset(t->metadata, 0, t->metadata_len);
			return;
		}

		/* Parse and set metadata */
		mbt_torrent_set_metadata(t, t->metadata, t->metadata_len);

		/* Drive all peers now that we have metadata */
		struct torrent_peer *tp;
		for (tp = t->peers; tp; tp = tp->next) {
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

	while (tp->outstanding < PIPELINE_DEPTH) {
		int idx = t->picker(t, tp->peer, t->picker_ctx);
		if (idx < 0)
			break;

		struct piece_info *pi = &t->pieces[idx];
		if (pi->state == MBT_PIECE_COMPLETE)
			continue;

		if (!pi->data) {
			pi->data = malloc(pi->length);
			if (!pi->data) return;
			pi->state = MBT_PIECE_ACTIVE;
		}

		/* Request all blocks for this piece */
		size_t off;
		for (off = pi->received; off < pi->length; off += BLOCK_SIZE) {
			uint32_t req_len = MIN(BLOCK_SIZE, pi->length - off);
			mbt_peer_send_request(tp->peer, idx, off, req_len);
			tp->outstanding++;
		}

		break; /* one piece at a time per drive call */
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
	t->ih = infohash_ref(cfg->ih);
	memcpy(t->peer_id, cfg->peer_id, 20);
	t->storage_ops = cfg->storage_ops;
	t->storage_ctx = cfg->storage_ctx;
	t->picker = cfg->picker;
	t->picker_ctx = cfg->picker_ctx;

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

	tbmap_unref(t->have);
	free(t->metadata);
	infohash_unref(t->ih);
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
	size_t bitmap_bytes = (t->piece_count + 7) / 8;
	tbmap_new(&t->have, bitmap_bytes);

	/* Check storage for existing pieces (resume) */
	if (t->storage_ops && t->storage_ops->open)
		t->storage_ops->open(t->storage_ctx, t);

	if (t->storage_ops && t->storage_ops->have_piece) {
		for (uint32_t i = 0; i < t->piece_count; i++) {
			if (t->storage_ops->have_piece(t->storage_ctx, i) > 0) {
				t->pieces[i].state = MBT_PIECE_COMPLETE;
				tbmap_set(t->have, i);
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
	                  &torrent_peer_cbs, t);
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

struct infohash *
mbt_torrent_infohash(struct mbt_torrent *t)
{
	return t->ih;
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
