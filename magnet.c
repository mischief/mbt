#include <stdlib.h>
#include <string.h>
#include <errno.h>
#include <ctype.h>

#include "magnet.h"

/* Percent-decode in-place. Returns decoded length. */
static size_t
pct_decode(char *s)
{
	char *r = s, *w = s;
	while (*r) {
		if (*r == '%' && isxdigit(r[1]) && isxdigit(r[2])) {
			char hex[3] = { r[1], r[2], 0 };
			*w++ = (char)strtol(hex, NULL, 16);
			r += 3;
		} else if (*r == '+') {
			*w++ = ' ';
			r++;
		} else {
			*w++ = *r++;
		}
	}
	*w = '\0';
	return (size_t)(w - s);
}

/* Parse addr:port or [ipv6]:port into peer struct */
static int
parse_peer(const char *s, struct mbt_magnet_peer *p)
{
	const char *colon;

	if (s[0] == '[') {
		/* [ipv6]:port */
		const char *close = strchr(s, ']');
		if (!close || close[1] != ':')
			return -EINVAL;
		size_t len = (size_t)(close - s - 1);
		if (len >= sizeof(p->addr))
			return -EINVAL;
		memcpy(p->addr, s + 1, len);
		p->addr[len] = '\0';
		p->port = (uint16_t)atoi(close + 2);
	} else {
		/* ipv4:port or hostname:port */
		colon = strrchr(s, ':');
		if (!colon)
			return -EINVAL;
		size_t len = (size_t)(colon - s);
		if (len >= sizeof(p->addr))
			return -EINVAL;
		memcpy(p->addr, s, len);
		p->addr[len] = '\0';
		p->port = (uint16_t)atoi(colon + 1);
	}

	return 0;
}

int
mbt_magnet_parse(const char *uri, struct mbt_magnet *out)
{
	const char *p, *end, *eq;
	char *param;
	size_t plen;

	memset(out, 0, sizeof(*out));

	/* Must start with magnet:? */
	if (strncmp(uri, "magnet:?", 8) != 0)
		return -EINVAL;

	p = uri + 8;

	while (*p) {
		/* Find end of this parameter */
		end = strchr(p, '&');
		if (!end)
			end = p + strlen(p);

		plen = (size_t)(end - p);

		/* Copy param for in-place decode */
		param = strndup(p, plen);
		if (!param)
			return -ENOMEM;

		eq = strchr(param, '=');
		if (!eq) {
			free(param);
			goto next;
		}

		/* Split key=value */
		size_t klen = (size_t)(eq - param);
		char *val = param + klen + 1;
		pct_decode(val);

		if (klen == 2 && memcmp(param, "xt", 2) == 0) {
			/* xt=urn:btih:<hash> */
			if (strncmp(val, "urn:btih:", 9) == 0) {
				const char *hash = val + 9;
				size_t hlen = strlen(hash);
				if (hlen == 40) {
					/* SHA1 hex */
					memcpy(out->infohash_hex, hash, 40);
					out->infohash_hex[40] = '\0';
					out->infohash_type = MBT_INFOHASH_SHA1;
					/* Uppercase for consistency */
					for (int i = 0; i < 40; i++)
						out->infohash_hex[i] = toupper(out->infohash_hex[i]);
				} else if (hlen == 32) {
					/* Base32 — TODO: decode to 20 bytes then hex-encode */
					free(param);
					return -ENOTSUP;
				} else {
					free(param);
					return -EINVAL;
				}
			}
		} else if (klen == 2 && memcmp(param, "dn", 2) == 0) {
			free(out->display_name);
			out->display_name = strdup(val);
		} else if (klen == 2 && memcmp(param, "tr", 2) == 0) {
			if (out->tracker_count < MBT_MAGNET_MAX_TRACKERS)
				out->trackers[out->tracker_count++] = strdup(val);
		} else if (klen == 4 && memcmp(param, "x.pe", 4) == 0) {
			if (out->peer_count < MBT_MAGNET_MAX_PEERS) {
				if (parse_peer(val, &out->peers[out->peer_count]) == 0)
					out->peer_count++;
			}
		}

		free(param);
next:
		p = *end ? end + 1 : end;
	}

	/* Must have an infohash */
	if (out->infohash_hex[0] == '\0')
		return -EINVAL;

	return 0;
}

void
mbt_magnet_free(struct mbt_magnet *m)
{
	free(m->display_name);
	m->display_name = NULL;

	for (int i = 0; i < m->tracker_count; i++) {
		free(m->trackers[i]);
		m->trackers[i] = NULL;
	}
	m->tracker_count = 0;
	m->peer_count = 0;
}
