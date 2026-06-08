// SPDX-License-Identifier: ISC
#include <errno.h>
#include <stddef.h>
#include <stdlib.h>
#include <assert.h>
#include <string.h>

#include "mbt/infohash.h"
#include "mbt/hex.h"

int
mbt_infohash_new(struct mbt_infohash **ih, int type, const char *str)
{
	int rv;
	struct mbt_infohash *infohash;

	infohash = calloc(1, sizeof(*infohash));
	if(!infohash)
		return -ENOMEM;

	infohash->ref = 1;

	infohash->type = type;

	rv = mbt_hex_decode(infohash->hash_sha1, sizeof(infohash->hash_sha1), str);
	if(rv < 0){
		free(infohash);
		return rv;
	}

	*ih = infohash;

	return 0;
}

struct mbt_infohash*
mbt_infohash_ref(struct mbt_infohash *ih)
{
	ih->ref++;
	return ih;
}

void
mbt_infohash_unref(struct mbt_infohash *ih)
{
	if(!ih)
		return;

	assert(ih->ref > 0);

	if(--ih->ref > 0)
		return;

	free(ih);
}

void
mbt_mbt_infohash_unrefp(struct mbt_infohash **ih)
{
	if(!*ih)
		return;

	mbt_infohash_unref(*ih);
}

int
mbt_infohash_string(struct mbt_infohash *ih, char *buf, size_t sz)
{
	if(sz < MBT_MBT_INFOHASH_SHA1HEXLEN+1)
		return -EINVAL;

	if(ih->type != MBT_INFOHASH_SHA1)
		return -EINVAL;

	if(mbt_hex_encode(ih->hash_sha1, sizeof(ih->hash_sha1), buf, sz) < 0)
		return -EINVAL;

	buf[sz-1] = 0;

	return 0;
}

int
mbt_infohash_cmp(struct mbt_infohash *a, struct mbt_infohash *b)
{
	if(a->type != b->type)
		return -1;

	switch(a->type){
	case MBT_INFOHASH_SHA1:
		return memcmp(a->hash_sha1, b->hash_sha1, sizeof(a->hash_sha1));
	}

	abort();
	return 0;
}

