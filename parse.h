// SPDX-License-Identifier: ISC
#ifndef MBT_PARSE_H
#define MBT_PARSE_H

#include <errno.h>
#include <stdint.h>
#include <stdlib.h>

/*
 * Parse a whole decimal number in [min, max]. stop, if not 0, is one more
 * byte that may end the number, as '/' ends the port in a URL.
 */
static inline int
mbt_parse_int(const char *s, long long min, long long max, char stop, long long *out)
{
	char *end;
	long long v;

	if (*s < '0' || *s > '9') {
		if (*s != '-' || min >= 0)
			return -EINVAL;
	}
	errno = 0;
	v = strtoll(s, &end, 10);
	if (errno != 0 || end == s || (*end != '\0' && (stop == '\0' || *end != stop)))
		return -EINVAL;
	if (v < min || v > max)
		return -ERANGE;
	*out = v;
	return 0;
}

/* A port from 0 to 65535. */
static inline int
mbt_parse_port(const char *s, char stop, uint16_t *out)
{
	long long v;
	int rv = mbt_parse_int(s, 0, UINT16_MAX, stop, &v);

	if (rv == 0)
		*out = (uint16_t)v;
	return rv;
}

#endif
