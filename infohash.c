#include <errno.h>
#include <stddef.h>
#include <stdlib.h>
#include <assert.h>
#include <string.h>

#include "infohash.h"
#include "hex.h"

int
infohash_new(struct infohash **ih, int type, const char *str)
{
	int rv;
	struct infohash *infohash;

	infohash = calloc(1, sizeof(*infohash));
	if(!infohash)
		return -ENOMEM;

	infohash->ref = 1;

	infohash->type = type;

	rv = mhextobin(infohash->hash_sha1, sizeof(infohash->hash_sha1), str);
	if(rv < 0){
		free(infohash);
		return rv;
	}

	*ih = infohash;

	return 0;
}

struct infohash*
infohash_ref(struct infohash *ih)
{
	ih->ref++;
	return ih;
}

void
infohash_unref(struct infohash *ih)
{
	if(!ih)
		return;

	assert(ih->ref > 0);

	if(--ih->ref > 0)
		return;

	free(ih);
}

void
infohash_unrefp(struct infohash **ih)
{
	if(!*ih)
		return;

	infohash_unref(*ih);
}

int
infohash_string(struct infohash *ih, char *buf, size_t sz)
{
	if(sz < INFOHASH_SHA1HEXLEN+1)
		return -EINVAL;

	if(ih->type != INFOHASH_SHA1)
		return -EINVAL;

	if(mbintohex(ih->hash_sha1, sizeof(ih->hash_sha1), buf, sz) < 0)
		return -EINVAL;

	buf[sz-1] = 0;

	return 0;
}

int
infohash_cmp(struct infohash *a, struct infohash *b)
{
	if(a->type != b->type)
		return -1;

	switch(a->type){
	case INFOHASH_SHA1:
		return memcmp(a->hash_sha1, b->hash_sha1, sizeof(a->hash_sha1));
	}

	abort();
	return 0;
}

