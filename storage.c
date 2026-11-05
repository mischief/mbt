// SPDX-License-Identifier: ISC
#include <stdlib.h>

#include "mbt/storage.h"

struct mbt_storage *
mbt_storage_new(const struct mbt_storage_ops *ops, void *ctx)
{
	struct mbt_storage *s = calloc(1, sizeof(*s));

	if (!s)
		return NULL;
	s->ref = 1;
	s->ops = ops;
	s->ctx = ctx;
	return s;
}

struct mbt_storage *
mbt_storage_ref(struct mbt_storage *s)
{
	if (s)
		s->ref++;
	return s;
}

void
mbt_storage_unref(struct mbt_storage *s)
{
	if (!s || --s->ref > 0)
		return;
	if (s->ops->destroy)
		s->ops->destroy(s->ctx);
	free(s);
}
