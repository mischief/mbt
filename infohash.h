#pragma once

#include <sys/queue.h>

enum {
	MBT_INFOHASH_SHA1 = 1,
	MBT_MBT_INFOHASH_SHA1HEXLEN = 40,
};

struct mbt_infohash;

STAILQ_HEAD(mbt_infohash_head, mbt_infohash);

struct mbt_infohash
{
	int ref;

	STAILQ_ENTRY(mbt_infohash) next;

	int type;

	union {
		unsigned char hash_sha1[20];
	};
};

int mbt_infohash_new(struct mbt_infohash **ih, int type, const char *str);
struct mbt_infohash* mbt_infohash_ref(struct mbt_infohash *ih);
void mbt_infohash_unref(struct mbt_infohash *ih);
void mbt_mbt_infohash_unrefp(struct mbt_infohash **ih);
int mbt_infohash_string(struct mbt_infohash *ih, char *buf, size_t sz);
int mbt_infohash_cmp(struct mbt_infohash *a, struct mbt_infohash *b);

