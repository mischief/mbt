// SPDX-License-Identifier: ISC
#include "export.h"
#include <sys/uio.h>
#include <sys/queue.h>
#include <inttypes.h>
#include <string.h>

enum {
	BE_STRING = 0,
	BE_INTEGER = 1,
	BE_LIST = 2,
	BE_LIST_END = 3,
	BE_DICTIONARY = 4,
	BE_DICTIONARY_END = 5,
};

struct mbt_be_value;

TAILQ_HEAD(mbt_be_value_head, mbt_be_value);

struct mbt_be_value
{
	int type;
	struct mbt_be_value *up;
	char *key;
	TAILQ_ENTRY(mbt_be_value) next;
	union {
		long long integer;
		struct iovec string;
		struct mbt_be_value_head listhead;
	};
};

MBT_API void mbt_be_value_free(struct mbt_be_value *v);
MBT_API void mbt_be_value_freep(struct mbt_be_value **v);
MBT_API int mbt_be_value_parse(const unsigned char *buf, size_t sz, struct mbt_be_value **out);

MBT_API int mbt_be_pp(struct mbt_be_value *be, void *arg);

MBT_API void mbt_be_walk(struct mbt_be_value *be, int (*cb)(struct mbt_be_value *be, void *arg), void *arg);
MBT_API struct mbt_be_value *mbt_be_find(struct mbt_be_value *be, const char *key);

MBT_API int mbt_be_dict_new(struct mbt_be_value **out);
MBT_API int mbt_be_dict_append(struct mbt_be_value *dict, char *key, struct mbt_be_value *value);
MBT_API int mbt_be_dict_append_number(struct mbt_be_value *dict, char *key, int64_t num);
MBT_API int mbt_be_dict_append_string(struct mbt_be_value *dict, char *key, const void *buf, size_t len);
MBT_API void mbt_be_string_new(struct mbt_be_value **out, const void *buf, size_t len);
MBT_API void mbt_be_list_new(struct mbt_be_value **out);
MBT_API void mbt_be_list_append(struct mbt_be_value *list, struct mbt_be_value *val);

MBT_API int
mbt_be_value_encode(struct mbt_be_value *v, unsigned char **out, size_t *len);

/* Check if a bencode value is a string equal to the given C string */
static inline int
mbt_be_streq(struct mbt_be_value *v, const char *s)
{
	size_t slen = strlen(s);
	return v && v->type == BE_STRING &&
	       v->string.iov_len == slen &&
	       memcmp(v->string.iov_base, s, slen) == 0;
}
