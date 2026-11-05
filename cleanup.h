// SPDX-License-Identifier: ISC
/* Cleanup attribute macros for automatic resource management. */
#ifndef MBT_CLEANUP_H
#define MBT_CLEANUP_H

#include <stdlib.h>
#include <unistd.h>

static inline void
autofree_fn(void *p)
{
	free(*(void **)p);
}

static inline void
autoclose_fn(int *fd)
{
	if (*fd >= 0)
		close(*fd);
}

/* Initialize these at the declaration: to NULL for autofree, to -1 for autoclose. */
#define autofree __attribute__((cleanup(autofree_fn)))
#define autoclose __attribute__((cleanup(autoclose_fn)))

#endif
