// SPDX-License-Identifier: ISC
/*
 * resolve_blocking.c — synchronous DNS (fallback, blocks event loop)
 */
#include <stdlib.h>
#include <string.h>
#include <errno.h>
#include <netdb.h>
#include <stdio.h>

#include "mbt/resolve.h"
#include "mbt/event.h"

struct mbt_resolve {
	int cancelled;
};

int
mbt_resolve(struct mbt_ev *ev, const char *host, uint16_t port,
            mbt_resolve_cb cb, void *arg, struct mbt_resolve **out)
{
	(void)ev;
	struct addrinfo hints = { .ai_socktype = SOCK_DGRAM };
	struct addrinfo *res;
	char portstr[8];
	snprintf(portstr, sizeof(portstr), "%hu", port);

	int rv = getaddrinfo(host, portstr, &hints, &res);
	if (rv != 0) {
		cb(NULL, 0, -EHOSTUNREACH, arg);
		return 0;
	}

	struct sockaddr_storage ss = {0};
	socklen_t len = res->ai_addrlen;
	memcpy(&ss, res->ai_addr, len);
	freeaddrinfo(res);

	cb(&ss, len, 0, arg);
	if (out) *out = NULL;
	return 0;
}

void
mbt_resolve_cancel(struct mbt_resolve *r)
{
	(void)r;
}
