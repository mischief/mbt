#include <sys/uio.h>
#include <sys/queue.h>
#include <inttypes.h>

enum {
	BE_STRING = 0,
	BE_INTEGER = 1,
	BE_LIST = 2,
	BE_LIST_END = 3,
	BE_DICTIONARY = 4,
	BE_DICTIONARY_END = 5,
};

struct be_value;

STAILQ_HEAD(be_value_head, be_value);

struct be_value
{
	int type;
	struct be_value *up;
	char *key;
	STAILQ_ENTRY(be_value) next;
	union {
		long long integer;
		struct iovec string;
		struct be_value_head listhead;
	};
};

void be_value_free(struct be_value *v);
void be_value_freep(struct be_value **v);
int be_value_parse(const unsigned char *buf, size_t sz, struct be_value **out);

int be_pp(struct be_value *be, void *arg);

void be_walk(struct be_value *be, int (*cb)(struct be_value *be, void *arg), void *arg);
struct be_value *be_find(struct be_value *be, const char *key);

int be_dict_new(struct be_value **out);
int be_dict_append(struct be_value *dict, char *key, struct be_value *value);
int be_dict_append_number(struct be_value *dict, char *key, int64_t num);

int
be_value_encode(struct be_value *v, unsigned char **out, size_t *len);

