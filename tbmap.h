/*
#include <stdint.h>
#include <stddef.h>

#include <tbmap.h>
*/

struct tbmap;

int tbmap_new(struct tbmap **out, size_t sz);
int tbmap_fromarray(struct tbmap **out, uint8_t *buf, size_t len);
void tbmap_unref(struct tbmap *tb);
void tbmap_unrefp(struct tbmap **tb);

size_t tbmap_bits(struct tbmap *tb);
size_t tbmap_bytes(struct tbmap *tb);

void tbmap_set(struct tbmap *tb, size_t bit);
int tbmap_isset(struct tbmap *tb, size_t bit);

void tbmap_clear(struct tbmap *tb);

int tbmap_union(struct tbmap *a, struct tbmap *b, struct tbmap *out);
int tbmap_intersection(struct tbmap *a, struct tbmap *b, struct tbmap *out);

