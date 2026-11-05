// SPDX-License-Identifier: ISC
#include "export.h"
#include <sys/queue.h>
#include <inttypes.h>
#include <stddef.h>
#include <stdint.h>
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
	int n_ref;
	struct mbt_be_value *up;
	char *key;       /* NUL-terminated, but a key can hold NUL: see key_len */
	size_t key_len;
	/* Parsed dict values only: encoded bytes, relative to the start of the dict. */
	size_t src_off, src_len;
	TAILQ_ENTRY(mbt_be_value) next;
	union {
		long long integer;
		struct { uint8_t *buf; size_t len; } string;
		struct mbt_be_value_head listhead;
	};
};

MBT_API int mbt_be_value_parse(const unsigned char *buf, size_t sz, struct mbt_be_value **out);

/*
 * Bencode writer: a cursor over a caller's buffer, with no allocation. Keys
 * must come in sorted order. The first failure stays in err, later calls do
 * nothing, and mbt_bew_finish reports it: -EMSGSIZE for a small buffer, -EILSEQ
 * for keys out of order, -ERANGE for deep nesting, -EINVAL for misuse.
 */
#define MBT_BEW_MAX_DEPTH 8

struct mbt_bew {
	unsigned char *start, *p, *end;
	int err;
	int depth;
	struct {
		int is_dict;
		int want_value;
		const char *last_key; /* the caller's string, valid until the next key */
	} level[MBT_BEW_MAX_DEPTH];
};

MBT_API void mbt_bew_init(struct mbt_bew *w, void *buf, size_t len);
MBT_API int mbt_bew_dict(struct mbt_bew *w);
MBT_API int mbt_bew_list(struct mbt_bew *w);
MBT_API int mbt_bew_end(struct mbt_bew *w);
MBT_API int mbt_bew_key(struct mbt_bew *w, const char *key);
MBT_API int mbt_bew_str(struct mbt_bew *w, const void *data, size_t len);
MBT_API int mbt_bew_int(struct mbt_bew *w, int64_t v);
/* One value that is already bencoded. */
MBT_API int mbt_bew_raw(struct mbt_bew *w, const void *data, size_t len);
MBT_API int mbt_bew_finish(struct mbt_bew *w, size_t *out) __attribute__((warn_unused_result));

MBT_API int mbt_be_pp(struct mbt_be_value *be, void *arg);

MBT_API void mbt_be_walk(struct mbt_be_value *be, int (*cb)(struct mbt_be_value *be, void *arg), void *arg);
MBT_API struct mbt_be_value *mbt_be_find(struct mbt_be_value *be, const char *key);

MBT_API struct mbt_be_value *mbt_be_ref(struct mbt_be_value *v);
MBT_API struct mbt_be_value *mbt_be_unref(struct mbt_be_value *v);
MBT_API void mbt_be_unrefp(struct mbt_be_value **v);

#define _cleanup_be_ __attribute__((cleanup(mbt_be_unrefp)))

MBT_API int mbt_be_dict_new(struct mbt_be_value **out);
MBT_API int mbt_be_dict_append(struct mbt_be_value *dict, char *key, struct mbt_be_value *value);
MBT_API int mbt_be_dict_append_key(struct mbt_be_value *dict, const void *key, size_t key_len,
                                   struct mbt_be_value *value);
MBT_API int mbt_be_dict_append_number(struct mbt_be_value *dict, char *key, int64_t num);
MBT_API int mbt_be_dict_append_string(struct mbt_be_value *dict, char *key, const void *buf, size_t len);
MBT_API int mbt_be_string_new(struct mbt_be_value **out, const void *buf, size_t len);
MBT_API int mbt_be_integer_new(struct mbt_be_value **out, int64_t val);
MBT_API int mbt_be_list_new(struct mbt_be_value **out);
MBT_API void mbt_be_list_append(struct mbt_be_value *list, struct mbt_be_value *val);

MBT_API int
mbt_be_value_encode(struct mbt_be_value *v, unsigned char **out, size_t *len);

/* Check if a bencode value is a string equal to the given C string */
static inline int
mbt_be_streq(struct mbt_be_value *v, const char *s)
{
	size_t slen = strlen(s);
	return v && v->type == BE_STRING &&
	       v->string.len == slen &&
	       memcmp(v->string.buf, s, slen) == 0;
}
