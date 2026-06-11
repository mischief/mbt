// SPDX-License-Identifier: ISC
/*
 * net.c — dual-stack networking helpers
 *
 * Portable listener pairs (v4 + v6), outbound connect, UDP send.
 * Works on Linux (dual-stack optional) and OpenBSD (v6only mandatory).
 */
#define _GNU_SOURCE
#include <sys/socket.h>
#include <netinet/in.h>
#include <arpa/inet.h>
#include <netdb.h>
#include <unistd.h>
#include <fcntl.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <errno.h>

#include "net.h"
#include "mbt/event.h"

static int
make_nonblock(int fd)
{
	int flags = fcntl(fd, F_GETFL);
	if (flags < 0) return -1;
	return fcntl(fd, F_SETFL, flags | O_NONBLOCK | O_CLOEXEC);
}

static int
bind_listen_tcp(int family, uint16_t port)
{
	int fd = socket(family, SOCK_STREAM | SOCK_NONBLOCK | SOCK_CLOEXEC, 0);
	if (fd < 0) return -errno;

	int yes = 1;
	setsockopt(fd, SOL_SOCKET, SO_REUSEADDR, &yes, sizeof(yes));
	if (family == AF_INET6)
		setsockopt(fd, IPPROTO_IPV6, IPV6_V6ONLY, &yes, sizeof(yes));

	struct sockaddr_storage sa = {0};
	socklen_t salen;
	if (family == AF_INET) {
		struct sockaddr_in *s4 = (struct sockaddr_in *)&sa;
		s4->sin_family = AF_INET;
		s4->sin_port = htons(port);
		s4->sin_addr.s_addr = INADDR_ANY;
		salen = sizeof(*s4);
	} else {
		struct sockaddr_in6 *s6 = (struct sockaddr_in6 *)&sa;
		s6->sin6_family = AF_INET6;
		s6->sin6_port = htons(port);
		s6->sin6_addr = in6addr_any;
		salen = sizeof(*s6);
	}

	if (bind(fd, (struct sockaddr *)&sa, salen) < 0) {
		int e = errno; close(fd); return -e;
	}
	if (listen(fd, 128) < 0) {
		int e = errno; close(fd); return -e;
	}
	return fd;
}

static int
bind_udp(int family, uint16_t port)
{
	int fd = socket(family, SOCK_DGRAM | SOCK_NONBLOCK | SOCK_CLOEXEC, 0);
	if (fd < 0) return -errno;

	int yes = 1;
	setsockopt(fd, SOL_SOCKET, SO_REUSEADDR, &yes, sizeof(yes));
	if (family == AF_INET6)
		setsockopt(fd, IPPROTO_IPV6, IPV6_V6ONLY, &yes, sizeof(yes));

	struct sockaddr_storage sa = {0};
	socklen_t salen;
	if (family == AF_INET) {
		struct sockaddr_in *s4 = (struct sockaddr_in *)&sa;
		s4->sin_family = AF_INET;
		s4->sin_port = htons(port);
		s4->sin_addr.s_addr = INADDR_ANY;
		salen = sizeof(*s4);
	} else {
		struct sockaddr_in6 *s6 = (struct sockaddr_in6 *)&sa;
		s6->sin6_family = AF_INET6;
		s6->sin6_port = htons(port);
		s6->sin6_addr = in6addr_any;
		salen = sizeof(*s6);
	}

	if (bind(fd, (struct sockaddr *)&sa, salen) < 0) {
		int e = errno; close(fd); return -e;
	}
	return fd;
}

int
mbt_listen_tcp(struct mbt_listener *l, struct mbt_ev *ev, uint16_t port,
               mbt_ev_io_cb cb, void *arg)
{
	memset(l, 0, sizeof(*l));
	l->fd4 = -1; l->fd6 = -1;

	l->fd4 = bind_listen_tcp(AF_INET, port);
	/* If port=0, discover assigned port for v6 to match */
	if (l->fd4 >= 0 && port == 0) {
		struct sockaddr_in sa;
		socklen_t len = sizeof(sa);
		getsockname(l->fd4, (struct sockaddr *)&sa, &len);
		port = ntohs(sa.sin_port);
	}
	l->fd6 = bind_listen_tcp(AF_INET6, port);
	l->port = port;

	if (l->fd4 < 0 && l->fd6 < 0)
		return l->fd4; /* both failed, return first error */

	if (l->fd4 >= 0)
		mbt_ev_io_new(ev, &l->io4, l->fd4, MBT_EV_READ, cb, arg);
	if (l->fd6 >= 0)
		mbt_ev_io_new(ev, &l->io6, l->fd6, MBT_EV_READ, cb, arg);

	return 0;
}

int
mbt_listen_udp(struct mbt_listener *l, struct mbt_ev *ev, uint16_t port,
               mbt_ev_io_cb cb, void *arg)
{
	memset(l, 0, sizeof(*l));
	l->fd4 = -1; l->fd6 = -1;

	l->fd4 = bind_udp(AF_INET, port);
	if (l->fd4 >= 0 && port == 0) {
		struct sockaddr_in sa;
		socklen_t len = sizeof(sa);
		getsockname(l->fd4, (struct sockaddr *)&sa, &len);
		port = ntohs(sa.sin_port);
	}
	l->fd6 = bind_udp(AF_INET6, port);
	l->port = port;

	if (l->fd4 < 0 && l->fd6 < 0)
		return l->fd4;

	if (l->fd4 >= 0)
		mbt_ev_io_new(ev, &l->io4, l->fd4, MBT_EV_READ, cb, arg);
	if (l->fd6 >= 0)
		mbt_ev_io_new(ev, &l->io6, l->fd6, MBT_EV_READ, cb, arg);

	return 0;
}

void
mbt_listener_close(struct mbt_listener *l)
{
	if (l->io4) mbt_ev_io_free(l->io4);
	if (l->io6) mbt_ev_io_free(l->io6);
	if (l->fd4 >= 0) close(l->fd4);
	if (l->fd6 >= 0) close(l->fd6);
	l->io4 = l->io6 = NULL;
	l->fd4 = l->fd6 = -1;
}

int
mbt_connect_tcp(const char *addr, uint16_t port)
{
	struct sockaddr_storage sa = {0};
	socklen_t salen;
	struct sockaddr_in *s4 = (struct sockaddr_in *)&sa;
	struct sockaddr_in6 *s6 = (struct sockaddr_in6 *)&sa;

	if (inet_pton(AF_INET6, addr, &s6->sin6_addr) > 0) {
		s6->sin6_family = AF_INET6;
		s6->sin6_port = htons(port);
		salen = sizeof(*s6);
	} else if (inet_pton(AF_INET, addr, &s4->sin_addr) > 0) {
		s4->sin_family = AF_INET;
		s4->sin_port = htons(port);
		salen = sizeof(*s4);
	} else {
		/* Try DNS resolution */
		struct addrinfo hints = { .ai_socktype = SOCK_STREAM };
		struct addrinfo *res;
		char portstr[8];
		snprintf(portstr, sizeof(portstr), "%hu", port);
		if (getaddrinfo(addr, portstr, &hints, &res) != 0)
			return -ENOENT;
		memcpy(&sa, res->ai_addr, res->ai_addrlen);
		salen = res->ai_addrlen;
		freeaddrinfo(res);
	}

	int fd = socket(sa.ss_family, SOCK_STREAM | SOCK_NONBLOCK | SOCK_CLOEXEC, 0);
	if (fd < 0) return -errno;

	if (connect(fd, (struct sockaddr *)&sa, salen) < 0 && errno != EINPROGRESS) {
		int e = errno; close(fd); return -e;
	}
	return fd;
}

int
mbt_sendto(int fd4, int fd6, const void *buf, size_t len,
           const struct sockaddr *sa, socklen_t salen)
{
	int fd = (sa->sa_family == AF_INET6) ? fd6 : fd4;
	if (fd < 0) return -EAFNOSUPPORT;
	ssize_t rv = sendto(fd, buf, len, 0, sa, salen);
	return rv < 0 ? -errno : 0;
}

int
mbt_parse_endpoint(const char *str, uint16_t default_port,
                   struct sockaddr_storage *out, socklen_t *outlen)
{
	char buf[256];
	snprintf(buf, sizeof(buf), "%s", str);

	char *host = buf;
	char *portstr = NULL;
	uint16_t port = default_port;

	if (host[0] == '[') {
		/* [IPv6]:port */
		host++;
		char *end = strchr(host, ']');
		if (!end) return -1;
		*end = '\0';
		if (end[1] == ':') portstr = end + 2;
	} else {
		/* IPv4:port or hostname:port */
		char *colon = strrchr(host, ':');
		if (colon) { *colon = '\0'; portstr = colon + 1; }
	}
	if (portstr) port = atoi(portstr);

	struct sockaddr_in6 *s6 = (struct sockaddr_in6 *)out;
	struct sockaddr_in *s4 = (struct sockaddr_in *)out;

	memset(out, 0, sizeof(*out));
	if (inet_pton(AF_INET6, host, &s6->sin6_addr) > 0) {
		s6->sin6_family = AF_INET6;
		s6->sin6_port = htons(port);
		*outlen = sizeof(*s6);
	} else if (inet_pton(AF_INET, host, &s4->sin_addr) > 0) {
		s4->sin_family = AF_INET;
		s4->sin_port = htons(port);
		*outlen = sizeof(*s4);
	} else {
		return -1;
	}
	return 0;
}
