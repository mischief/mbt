#pragma once

#include <sys/queue.h>

enum {
	INFOHASH_SHA1 = 1,
	INFOHASH_SHA1HEXLEN = 40,
};

struct infohash;

STAILQ_HEAD(infohash_head, infohash);

struct infohash
{
	int ref;

	STAILQ_ENTRY(infohash) next;

	int type;

	union {
		unsigned char hash_sha1[20];
	};
};

int infohash_new(struct infohash **ih, int type, const char *str);
struct infohash* infohash_ref(struct infohash *ih);
void infohash_unref(struct infohash *ih);
void infohash_unrefp(struct infohash **ih);
int infohash_string(struct infohash *ih, char *buf, size_t sz);
int infohash_cmp(struct infohash *a, struct infohash *b);

