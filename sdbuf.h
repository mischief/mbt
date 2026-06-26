// SPDX-License-Identifier: ISC
/*
#include <stdint.h>
*/

#include <stddef.h>
#include <stdint.h>
#include <sys/types.h>

enum {
	SDBUF_EVENT_ERROR = 1,
};

struct sdbuf;

typedef int (*sdbuf_cb)(struct sdbuf*, void*);
typedef void (*sdbuf_eventcb)(struct sdbuf*, int event, int e, void*);

int sdbuf_new(struct sdbuf **out, sdbuf_cb read, sdbuf_cb write, sdbuf_eventcb error, void *arg);
void sdbuf_free(struct sdbuf *b);

void sdbuf_set_watermark(struct sdbuf *b, size_t readlow, size_t writelow);

int sdbuf_io(struct sdbuf *b, int fd, uint32_t revents);

size_t sdbuf_read_available(struct sdbuf *b);
ssize_t sdbuf_discard(struct sdbuf *b, size_t sz);
ssize_t sdbuf_read(struct sdbuf *b, void *buf, size_t sz);
ssize_t sdbuf_write(struct sdbuf *b, const void *data, size_t sz);
int sdbuf_flush(struct sdbuf *b, int fd);
size_t sdbuf_write_pending(struct sdbuf *b);
