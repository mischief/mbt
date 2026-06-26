// SPDX-License-Identifier: ISC
#pragma once

/* MIN/MAX: provided by <sys/param.h> on Linux/BSDs, not on Solaris */
#ifndef MIN
#define MIN(a, b) ((a) < (b) ? (a) : (b))
#endif
#ifndef MAX
#define MAX(a, b) ((a) > (b) ? (a) : (b))
#endif

/* SLIST_FOREACH_SAFE: BSD extension, missing from glibc's sys/queue.h */
#ifndef SLIST_FOREACH_SAFE
#define SLIST_FOREACH_SAFE(var, head, field, tvar) \
	for ((var) = SLIST_FIRST(head); \
	     (var) && ((tvar) = SLIST_NEXT(var, field), 1); \
	     (var) = (tvar))
#endif
