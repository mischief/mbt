#include <sys/queue.h>
#include <netinet/in.h>
#include <assert.h>
#include <errno.h>
#include <string.h>
#include <inttypes.h>
#include <stdlib.h>
#include <err.h>

#include <sha1.h>

#include "event.h"
#include "infohash.h"
#include <be.h>
#include "bt_legacy.h"
#include <remote.h>
#include <hex.h>
#include <tbmap.h>

struct piece {
	SPLAY_ENTRY(piece) next;
	size_t index;
	size_t length;
	size_t have;
	uint8_t hash[SHA1_DIGEST_LENGTH];

	/* piece size / BTBLOCKSIZE bits */
	struct tbmap *blockbits;
	uint8_t data[];
};

static int
piece_new(struct piece **out, size_t index, size_t length, unsigned char *hash)
{
	int rv;
	struct piece *p;

	assert(out);
	assert(length > 0);
	assert(hash);

	p = calloc(1, sizeof(*p) + length);
	if(!p)
		return -ENOMEM;

	p->index = index;
	p->length = length;
	p->have = 0;
	memcpy(p->hash, hash, sizeof(p->hash));

	if((rv = tbmap_new(&p->blockbits, length / BTBLOCKSIZE)) < 0){
		free(p);
		return -ENOMEM;
	}
	
	*out = p;

	return 0;
}

static int
piece_full(struct piece *p)
{
	size_t blocks;

	blocks = p->length/BTBLOCKSIZE;

	return p->have == blocks;
}

static int
piece_verify(struct piece *p)
{
	SHA1_CTX ctx;
	uint8_t hash[SHA1_DIGEST_LENGTH];

	if(!piece_full(p))
		return -EINVAL;

	SHA1Init(&ctx);
	SHA1Update(&ctx, p->data, p->length);
	SHA1Final(hash, &ctx);

	if(memcmp(hash, p->hash, sizeof(hash)) != 0)
		return -EBADMSG;

	return 0;
}

static int
piece_put_block(struct piece *p, size_t offset, unsigned char *buf, size_t sz)
{
	assert(p);
	assert(buf);

	if(offset >= p->length)
		return -ERANGE;

	if(offset % BTBLOCKSIZE != 0)
		return -EINVAL;

	if(sz > BTBLOCKSIZE)
		return -EIO;

	memcpy(p->data + offset, buf, sz);

	tbmap_set(p->blockbits, offset/BTBLOCKSIZE);

	p->have++;

	return 0;
}

static int piece_cmp(struct piece *a, struct piece *b);

SPLAY_HEAD(piece_head, piece);
SPLAY_PROTOTYPE(piece_head, piece, next, piece_cmp)
SPLAY_GENERATE(piece_head, piece, next, piece_cmp)

static int
piece_cmp(struct piece *a, struct piece *b)
{
	return (a->index < b->index ? -1 : a->index > b->index);
}

struct torrent
{
	int ref;
	RB_ENTRY(torrent) entry;

	struct node *node;

	struct infohash *ih;

	/* info part of torrent metadata */
	struct be_value *info;

	size_t length;
	size_t npieces;
	size_t piece_length;

	struct piece_head want_pieces;
	struct piece_head done_pieces;

	struct remote_head peers;
};

int
torrent_new(struct node *node, struct infohash *ih, struct torrent **out)
{
	struct torrent *t;

	t = calloc(1, sizeof(*t));
	if(!t)
		return -ENOMEM;

	t->ref = 1;

	t->node = node;

	t->ih = infohash_ref(ih);

	LIST_INIT(&t->peers);

	*out = t;
	return 0;
}

struct torrent*
torrent_ref(struct torrent *t)
{
	t->ref++;
	return t;
}

void
torrent_unref(struct torrent *t)
{
	struct remote *r, *next;

	assert(t->ref > 0);
	if(--t->ref > 0)
		return;

	infohash_unref(t->ih);

	r = LIST_FIRST(&t->peers);
	while(r){
		next = LIST_NEXT(r, next);
		remote_unref(r);
		r = next;
	}

	free(t);
}

void
torrent_unrefp(struct torrent **t)
{
	if(!*t)
		return;

	torrent_unref(*t);
}

int
torrent_cmp(struct torrent *a, struct torrent *b)
{
	assert(a);
	assert(b);

	assert(a->ih);
	assert(b->ih);

	return infohash_cmp(a->ih, b->ih);
}

struct node*
torrent_node(struct torrent *t)
{
	assert(t);
	assert(t->node);

	return t->node;
}

void
torrent_add_remote(struct torrent *t, struct remote *r)
{
	struct remote *ref = remote_ref(r);

	LIST_INSERT_HEAD(&t->peers, ref, next);
}

void
torrent_remove_remote(struct torrent *t, struct remote *r)
{
	LIST_REMOVE(r, next);
	remote_unref(r);
}

static void
torrent_request_random_piece(struct torrent *t)
{
	int rv;
	size_t piece;
	struct remote *r;

	piece = arc4random_uniform(t->npieces);

	r = LIST_FIRST(&t->peers);
	assert(r != 0);
	rv = remote_request_blocks_for_piece(r, piece);
	assert(rv >= 0);
}

/* TODO: better validation */
void
torrent_set_info(struct torrent *t, struct be_value *info)
{
	int rv;
	size_t i, plv;
	unsigned char *p;
	struct be_value *len, *pl, *pieces;
	struct piece *piece;

	len = be_find(info, "length");
	if(!len || len->type != BE_INTEGER || len->integer <= 0)
		errx(1, "fixme: not a single file torrent or missing length");

	pl = be_find(info, "piece length");
	if(!pl || pl->type != BE_INTEGER || pl->integer < (1<<14) || (pl->integer & (pl->integer - 1)) != 0)
		errx(1, "fixme: missing or bogus piece length");

	pieces = be_find(info, "pieces");
	if(!pieces || pieces->type != BE_STRING || pieces->string.iov_len % SHA1_DIGEST_LENGTH)
		errx(1, "fixme: missing or bogus pieces");

	t->length = (size_t)len->integer;
	t->npieces = (size_t)(pieces->string.iov_len / SHA1_DIGEST_LENGTH);
	t->piece_length = (size_t)pl->integer;

	SPLAY_INIT(&t->want_pieces);
	SPLAY_INIT(&t->done_pieces);

	for(i = 0; i < t->npieces; i++){
		p = pieces->string.iov_base;

		plv = t->piece_length;
		if(i == t->npieces - 1 && (t->length % t->piece_length) != 0)
			plv = (t->length % t->piece_length);

		rv = piece_new(&piece, i, plv, p+(i*SHA1_DIGEST_LENGTH));
		if(rv < 0)
			err(1, "%s: piece_new: %s", __func__, strerror(-rv));

		SPLAY_INSERT(piece_head, &t->want_pieces, piece);
	}

	torrent_request_random_piece(t);
}

size_t
torrent_piece_length(struct torrent *t)
{
	return t->piece_length;
}

int
torrent_put_piece_block(struct torrent *t, size_t piece_index, size_t offset, unsigned char *buf, size_t sz)
{
	int rv;
	struct piece find, *piece;

	assert(buf);
	assert(sz <= BTBLOCKSIZE);

	find.index = piece_index;

	piece = SPLAY_FIND(piece_head, &t->want_pieces, &find);
	if(!piece){
		warnx("%s: tried to put non-existing piece %zu", __func__, piece_index);
		return -EINVAL;
	}

	rv = piece_put_block(piece, offset, buf, sz);
	if(rv < 0)
		return rv;

	if(piece_full(piece)){
		rv = piece_verify(piece);
		if(rv < 0){
			warnx("%s: corrupt piece %zu", __func__, piece_index);
			tbmap_clear(piece->blockbits);
		}

		//torrent_request_random_piece(t);
	}

	return 0;
}

RB_GENERATE(torrent_tree, torrent, entry, torrent_cmp)
