// SPDX-License-Identifier: ISC
#include "mbt/compat.h"
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <stddef.h>
#include <errno.h>
#include <assert.h>
#include <stdint.h>
#include <sys/queue.h>
#include <sys/uio.h>

#include "mbt/be.h"

enum {
	/* 9,999,999 */
	BE_MAX_STR_LEN = 7,

	/* roughly int64 */
	BE_MAX_INT_LEN = 20,
};

static int
be_count_depth(struct mbt_be_value *be)
{
	int depth;

	depth = 0;

	if(!be)
		return 0;

	for(be = be->up; be; be = be->up)
		depth++;

	return depth;
}

/* Core free logic: walk tree, respecting refcounts on children */
static void
be_value_free_impl(struct mbt_be_value *v)
{
	struct mbt_be_value *ov, *head;

	for(; v;){
		switch(v->type){
		case BE_STRING:
			free(v->string.iov_base);
			break;
		case BE_INTEGER:
			break;

		case BE_DICTIONARY:
		case BE_LIST:
			head = TAILQ_FIRST(&v->listhead);
			if(head){
				TAILQ_REMOVE(&v->listhead, head, next);
				if(--head->n_ref > 0){
					/* still referenced elsewhere; detach and skip */
					head->up = NULL;
					continue;
				}
				v = head;
				continue;
			}

			break;

		default:
			break;
		}

		free(v->key);

		ov = v->up;
		free(v);
		v = ov;
	}
}

struct mbt_be_value *
mbt_be_ref(struct mbt_be_value *v)
{
	if(v)
		v->n_ref++;
	return v;
}

struct mbt_be_value *
mbt_be_unref(struct mbt_be_value *v)
{
	if(!v)
		return NULL;
	if(--v->n_ref <= 0)
		be_value_free_impl(v);
	return NULL;
}

void
mbt_be_unrefp(struct mbt_be_value **v)
{
	if(*v){
		mbt_be_unref(*v);
		*v = NULL;
	}
}


static int
be_alloc(int type, struct mbt_be_value *up, struct mbt_be_value **out)
{
	struct mbt_be_value *v;

	v = calloc(1, sizeof(*v));
	if(!v)
		return -ENOMEM;

	v->type = type;
	v->n_ref = 1;
	v->up = up;

	TAILQ_INIT(&v->listhead);

	*out = v;

	return 0;
}

enum {
	IN_DICT_FALSE,
	IN_DICT_KEY,
	IN_DICT_VALUE,
};

static int
be_value_parse_string(const unsigned char *buf, size_t sz, struct mbt_be_value **out)
{
	const unsigned char *p = buf, *cp;
	size_t spn;
	ptrdiff_t pdif;
	int rv;
	unsigned long insz;
	struct mbt_be_value *v;

	*out = NULL;

	cp = memchr(p, ':', MIN(BE_MAX_STR_LEN, sz));
	if(!cp)
		return -EINVAL;

	spn = strspn((const char *)p, "0123456789");
	pdif = cp - p;

	if(spn != (size_t)pdif)
		return -EINVAL;

	rv = be_alloc(BE_STRING, NULL, &v);
	if(rv < 0)
		return rv;

	insz = strtoul((const char *)p, NULL, 10);
	if(insz > sz){
		mbt_be_unref(v);
		return -ERANGE;
	}

	v->string.iov_base = malloc(insz+1);
	if(!v->string.iov_base){
		mbt_be_unref(v);
		return -ENOMEM;
	}

	v->string.iov_len = insz;

	memcpy(v->string.iov_base, cp+1, insz);
	memset(((char*)v->string.iov_base)+insz, '\0', 1);

	*out = v;
	return (cp+1-p) + insz;
}

static int
be_value_parse_number(const unsigned char *buf, size_t sz, struct mbt_be_value **out)
{
	const unsigned char *p = buf, *cp;
	size_t spn;
	ptrdiff_t pdif;
	int rv;
	struct mbt_be_value *v;

	cp = memchr(p, 'e', MIN(BE_MAX_INT_LEN, sz));
	if(!cp)
		return -EINVAL;

	spn = strspn((const char*)p+1, "-0123456789");
	pdif = cp - (p + 1);

	if(spn != (size_t)pdif)
		return -EINVAL;

	rv = be_alloc(BE_INTEGER, NULL, &v);
	if(rv < 0)
		return rv;

	v->integer = strtol((const char*)p+1, NULL, 10);

	*out = v;

	return cp+1-p;
}

static int be_value_parse_inner(int depth, const unsigned char *buf, size_t sz, struct mbt_be_value **out);

static void
be_insert(struct mbt_be_value *v, struct mbt_be_value *val)
{
	if (v->type == BE_DICTIONARY && val->key) {
		/* Insert sorted by key (BEP 3 requirement) */
		struct mbt_be_value *cur, *prev = NULL;
		TAILQ_FOREACH(cur, &v->listhead, next) {
			if (cur->key && strcmp(val->key, cur->key) < 0)
				break;
			prev = cur;
		}
		if (!prev)
			TAILQ_INSERT_HEAD(&v->listhead, val, next);
		else
			TAILQ_INSERT_AFTER(&v->listhead, prev, val, next);
	} else {
		TAILQ_INSERT_TAIL(&v->listhead, val, next);
	}
	val->up = v;
	mbt_be_ref(val); /* parent claims a ref */
}

static int
be_value_parse_list(int depth, const unsigned char *buf, size_t sz, struct mbt_be_value **out)
{
	const unsigned char *p = buf + 1;
	struct mbt_be_value *v, *inner;
	int rv, chr;

	chr = 1;

	rv = be_alloc(BE_LIST, NULL, &v);
	if(rv < 0)
		return rv;

	while(*p && *p != 'e'){
		rv = be_value_parse_inner(depth, p, sz - chr, &inner);
		if(rv < 0){
			mbt_be_unref(v);
			return rv;
		}

		be_insert(v, inner);
		mbt_be_unref(inner); /* drop creator ref; parent holds its own */
		p += rv;
		chr += rv;
	}

	*out = v;

	// +1 for the e
	chr++;

	return chr;
}

static int
be_value_parse_dictionary(int depth, const unsigned char *buf, size_t sz, struct mbt_be_value **out)
{
	const unsigned char *p = buf;
	struct mbt_be_value *v, *inner, *key;
	int rv, chr;

	if(sz < 2 || *p++ != 'd')
		return -EINVAL;

	chr = 1;

	rv = be_alloc(BE_DICTIONARY, NULL, &v);
	if(rv < 0)
		return rv;

	while(p < buf+sz && *p && *p != 'e'){
		switch(p[0]){
		case '0': case '1': case '2': case '3': 
		case '4': case '5': case '6': case '7': 
		case '8': case '9':
			rv = be_value_parse_string(p, sz - chr, &key);
			if(rv < 0){
				mbt_be_unref(v);
				return rv;
			}

			p += rv;
			chr += rv;

			break;
		default:
			mbt_be_unref(v);
			return -EINVAL;
		}

		rv = be_value_parse_inner(depth, p, sz - chr, &inner);
		if(rv < 0){
			mbt_be_unref(key);
			mbt_be_unref(v);
			return rv;
		}

		/* kidnap key */
		inner->key = key->string.iov_base;
		key->string.iov_base = NULL;
		mbt_be_unref(key);

		be_insert(v, inner);
		mbt_be_unref(inner); /* drop creator ref; parent holds its own */
		p += rv;
		chr += rv;
	}

	// +1 for the e
	chr++;

	*out = v;

	return chr;
}

static int
be_value_parse_inner(int depth, const unsigned char *buf, size_t sz, struct mbt_be_value **out)
{
	if(depth > 128)
		return -EINVAL;

	if(sz < 2)
		return -ENOMSG;

	depth++;

	switch(buf[0]){
	case '0': case '1': case '2': case '3': 
	case '4': case '5': case '6': case '7': 
	case '8': case '9':
		return be_value_parse_string(buf, sz, out);
	case 'i':
		return be_value_parse_number(buf, sz, out);
	case 'l':
		return be_value_parse_list(depth, buf, sz, out);
	case 'd':
		return be_value_parse_dictionary(depth, buf, sz, out);
	}

	return -EINVAL;
}

int
mbt_be_value_parse(const unsigned char *buf, size_t sz, struct mbt_be_value **out)
{
	return be_value_parse_inner(0, buf, sz, out);
}

int 
mbt_be_pp(struct mbt_be_value *be, void *arg)
{
	int depth;

	const char indent[] = "                    ";
	const int nindent = sizeof(indent);

	(void) arg;

	depth = be_count_depth(be);

	printf("%.*s", MIN(depth, nindent), indent);
	if(be->key)
		printf("%lu:%s = ", strlen(be->key), be->key);

	switch (be->type) {
	case BE_INTEGER:
		printf("i%llde\n", be->integer);
		break;
	case BE_STRING:
		printf("%lu:%s\n", be->string.iov_len, (char*)be->string.iov_base);
		break;
	case BE_LIST:
		printf("l\n");
		break;
	case BE_LIST_END:
		break;
	case BE_DICTIONARY:
		printf("d\n");
		break;
	case BE_DICTIONARY_END:
		break;
	default:
		assert(0);
	}

	return 0;
}

void
mbt_be_walk(struct mbt_be_value *be, int (*cb)(struct mbt_be_value *be, void *arg), void *arg)
{
	struct mbt_be_value *inner, fake = {0};

	if(!be)
		return;

	cb(be, arg);

	switch(be->type){
	case BE_LIST:
		[[fallthrough]];
	case BE_DICTIONARY:
		TAILQ_FOREACH(inner, &be->listhead, next){
			mbt_be_walk(inner, cb, arg);
		}
		break;
	}

	switch(be->type){
	case BE_LIST:
		fake.type = BE_LIST_END;
		cb(&fake, arg);
		break;

	case BE_DICTIONARY:
		fake.type = BE_DICTIONARY_END;
		cb(&fake, arg);
		break;
	}
}

struct mbt_be_value*
mbt_be_find(struct mbt_be_value *be, const char *key)
{
	struct mbt_be_value *inner;

	if(!be || !(be->type == BE_DICTIONARY))
		return NULL;

	TAILQ_FOREACH(inner, &be->listhead, next){
		if(strcmp(key, inner->key) == 0)
			return inner;
	}

	return NULL;
}

int
mbt_be_dict_new(struct mbt_be_value **out)
{
	return be_alloc(BE_DICTIONARY, NULL, out);
}

int
mbt_be_dict_append(struct mbt_be_value *dict, char *key, struct mbt_be_value *value)
{
	char *k;

	k = strdup(key);
	if(!k)
		return -ENOMEM;

	value->key = k;

	be_insert(dict, value);

	return 0;
}

int
mbt_be_dict_append_number(struct mbt_be_value *dict, char *key, int64_t num)
{
	int rv;
	struct mbt_be_value *v;

	rv = be_alloc(BE_INTEGER, dict, &v);
	if(rv < 0)
		return rv;

	v->integer = num;

	rv = mbt_be_dict_append(dict, key, v);
	mbt_be_unref(v); /* drop creator ref; parent holds its own */
	return rv;
}

int
mbt_be_dict_append_string(struct mbt_be_value *dict, char *key, const void *buf, size_t len)
{
	struct mbt_be_value *v;
	int rv = be_alloc(BE_STRING, dict, &v);
	if (rv < 0) return rv;
	v->string.iov_base = malloc(len);
	if (!v->string.iov_base) { mbt_be_unref(v); return -ENOMEM; }
	memcpy(v->string.iov_base, buf, len);
	v->string.iov_len = len;
	rv = mbt_be_dict_append(dict, key, v);
	mbt_be_unref(v); /* drop creator ref; parent holds its own */
	return rv;
}

int
mbt_be_string_new(struct mbt_be_value **out, const void *buf, size_t len)
{
	int rv = be_alloc(BE_STRING, NULL, out);
	if (rv < 0) return rv;
	(*out)->string.iov_base = malloc(len);
	if (!(*out)->string.iov_base) { mbt_be_unref(*out); *out = NULL; return -ENOMEM; }
	memcpy((*out)->string.iov_base, buf, len);
	(*out)->string.iov_len = len;
	return 0;
}

int
mbt_be_integer_new(struct mbt_be_value **out, int64_t val)
{
	int rv = be_alloc(BE_INTEGER, NULL, out);
	if (rv < 0) return rv;
	(*out)->integer = val;
	return 0;
}

int
mbt_be_list_new(struct mbt_be_value **out)
{
	return be_alloc(BE_LIST, NULL, out);
}

void
mbt_be_list_append(struct mbt_be_value *list, struct mbt_be_value *val)
{
	be_insert(list, val);
}

struct encode_state
{
	char *ptr;
	size_t len;
};

static int
encode_getsize_cb(struct mbt_be_value *v, void *arg)
{
	struct encode_state *e = arg;
	char buf[16];
	int slen;

	if(v->key){
		/* size:string */
		slen = snprintf(buf, sizeof(buf), "%lu", strlen(v->key));
		e->len += slen;
		e->len += 1;
		e->len += strlen(v->key);
	}

	switch(v->type){
	case BE_STRING:
		/* size:string */
		slen = snprintf(buf, sizeof(buf), "%lu", v->string.iov_len);
		e->len += slen;
		e->len += 1;
		e->len += v->string.iov_len;
		break;

	case BE_INTEGER:
		/* i1234e */
		slen = snprintf(buf, sizeof(buf), "%lld", v->integer);
		e->len += 1;
		e->len += slen;
		e->len += 1;
		break;

	case BE_LIST:
	case BE_LIST_END:
	case BE_DICTIONARY:
	case BE_DICTIONARY_END:
		/* [dl]..e */
		e->len += 1;
		break;
	}

	return 0;
}

static int
encode_cb(struct mbt_be_value *v, void *arg)
{
	struct encode_state *e = arg;
	int slen;

	if(v->key){
		/* size:string */
		slen = sprintf(e->ptr, "%lu:%s", strlen(v->key), v->key);
		e->ptr += slen;
	}

	switch(v->type){
	case BE_STRING:
		/* size:string */
		slen = sprintf(e->ptr, "%zu:", v->string.iov_len);
		e->ptr += slen;
		memcpy(e->ptr, v->string.iov_base, v->string.iov_len);
		e->ptr += v->string.iov_len;
		break;

	case BE_INTEGER:
		/* i1234e */
		slen = sprintf(e->ptr, "i%llde", v->integer);
		e->ptr += slen;
		break;

	case BE_LIST:
		*e->ptr++ = 'l';
		break;

	case BE_LIST_END:
		*e->ptr++ = 'e';
		break;

	case BE_DICTIONARY:
		*e->ptr++ = 'd';
		break;

	case BE_DICTIONARY_END:
		*e->ptr++ = 'e';
		break;
	}

	return 0;
}

int
mbt_be_value_encode(struct mbt_be_value *v, unsigned char **out, size_t *len)
{
	struct encode_state e = {0};
	unsigned char *buf;

	mbt_be_walk(v, encode_getsize_cb, &e);

	buf = malloc(e.len + 1);
	if(!buf)
		return -ENOMEM;

	e.ptr = (char*)buf;

	mbt_be_walk(v, encode_cb, &e);

	*out = buf;
	*len = e.len;

	return 0;
}
