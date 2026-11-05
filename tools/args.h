// SPDX-License-Identifier: ISC
#ifndef MBT_TOOLS_ARGS_H
#define MBT_TOOLS_ARGS_H

#include <err.h>

#include "parse.h"

/* A number from the command line. Exits on anything out of range. */
static inline int
mbt_arg_int(const char *s, int min, int max)
{
	long long v;

	if (mbt_parse_int(s, min, max, '\0', &v) < 0)
		errx(1, "bad number: %s", s);
	return (int)v;
}

#endif
