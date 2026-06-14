// SPDX-License-Identifier: ISC
#include <sys/types.h>
#include <sys/queue.h>
#include <arpa/inet.h>
#include <sys/socket.h>
#include <netinet/in.h>
#include <fcntl.h>
#include <stdlib.h>
#include <stdio.h>
#include <string.h>
#include <unistd.h>
#include <assert.h>
#include <limits.h>
#include <errno.h>
#include <err.h>

#include "mbt/infohash.h"
#include "mbt/lsd.h"

void
mbt_lsd_freep(struct mbt_lsd **v)
{
	struct mbt_lsd *lsd = *v;

	if(!lsd)
		return;

	close(lsd->fd);
	if (lsd->fd6 >= 0) close(lsd->fd6);
	free(lsd);
}

void
mbt_lsd_frame_freep(struct mbt_lsd_frame **v)
{
	if(*v)
		free(*v);
}

void
mbt_lsd_frame_info_freep(struct mbt_lsd_frame_info **v)
{
	struct mbt_lsd_frame_info *lfi;
	struct mbt_infohash *ih, *next;

	lfi = *v;

	if(!lfi)
		return;

	ih = STAILQ_FIRST(&lfi->infohashes);
	while(ih){
		next = STAILQ_NEXT(ih, next);
		mbt_infohash_unref(ih);
		ih = next;
	}

	free(lfi->cookie);
	free(lfi);
}

int
mbt_lsd_init(struct mbt_lsd **out, int setfl)
{
	struct mbt_lsd *lsd;
	int fd, serr, y = 1;
	struct sockaddr_in sa;
	struct ip_mreq mreq;

	fd = socket(AF_INET, SOCK_DGRAM, 0);
	if(fd < 0)
		return -errno;

	if(fcntl(fd, F_SETFD, FD_CLOEXEC) < 0)
		goto err;

	if(setfl)
		if(fcntl(fd, F_SETFL, setfl) < 0)
			goto err;

	if(setsockopt(fd, SOL_SOCKET, SO_REUSEADDR, &y, sizeof(y)) < 0)
		goto err;

	//memset(&sa, 0, sizeof(sa));
	sa = (struct sockaddr_in){
		.sin_family = AF_INET,
		.sin_addr = (struct in_addr){ .s_addr = htonl(INADDR_ANY) },
		.sin_port = htons(6771),
	};

	if(bind(fd, (struct sockaddr*)&sa, sizeof(sa)) < 0)
		goto err;

	mreq = (struct ip_mreq){
		.imr_multiaddr = (struct in_addr){ .s_addr = inet_addr("239.192.152.143") },
		.imr_interface = (struct in_addr){ .s_addr = htonl(INADDR_ANY) },
	};

	if(setsockopt(fd, IPPROTO_IP, IP_ADD_MEMBERSHIP, (char*) &mreq, sizeof(mreq)) < 0)
		goto err;

	lsd = calloc(1, sizeof(*lsd));
	if(!lsd)
		goto err;

	lsd->fd = fd;
	lsd->fd6 = -1;

	/* Optional: IPv6 multicast (ff15::efc0:988f port 6771) */
	{
		int fd6 = socket(AF_INET6, SOCK_DGRAM, 0);
		if (fd6 >= 0) {
			fcntl(fd6, F_SETFD, FD_CLOEXEC);
			if (setfl) fcntl(fd6, F_SETFL, setfl);
			int y6 = 1;
			setsockopt(fd6, SOL_SOCKET, SO_REUSEADDR, &y6, sizeof(y6));
			setsockopt(fd6, IPPROTO_IPV6, IPV6_V6ONLY, &y6, sizeof(y6));
			struct sockaddr_in6 sa6 = { .sin6_family = AF_INET6, .sin6_port = htons(6771) };
			if (bind(fd6, (struct sockaddr *)&sa6, sizeof(sa6)) == 0) {
				struct ipv6_mreq mreq6;
				inet_pton(AF_INET6, "ff15::efc0:988f", &mreq6.ipv6mr_multiaddr);
				mreq6.ipv6mr_interface = 0;
				setsockopt(fd6, IPPROTO_IPV6, IPV6_JOIN_GROUP, &mreq6, sizeof(mreq6));
				lsd->fd6 = fd6;
			} else {
				close(fd6);
			}
		}
	}

	/* Generate random cookie to identify our own announces */
	uint32_t r;
	getentropy(&r, sizeof(r));
	snprintf(lsd->cookie, sizeof(lsd->cookie), "%08x", r);

	*out = lsd;

	return 0;
err:
	serr = errno;
	close(fd);
	return -serr;
}

int
mbt_lsd_recv(struct mbt_lsd *lsd, struct mbt_lsd_frame **out)
{
	int rv;
	socklen_t salen;
	struct mbt_lsd_frame *frame;

	assert(lsd);
	assert(out);

	frame = calloc(1, sizeof(*frame));
	if(!frame)
		return -ENOMEM;

	salen = sizeof(frame->sa);
	rv = recvfrom(lsd->fd, frame->frame, sizeof(frame->frame)-1, 0, (struct sockaddr*)&frame->sa, &salen);
	if(rv < 0){
		free(frame);
		return -errno;
	}

	frame->len = rv;
	frame->frame[rv] = '\0';

	assert(strlcpy(frame->remote, "Unknown", sizeof(frame->remote)) < sizeof(frame->remote));

	switch(frame->sa.ss_family){
	case AF_INET:
		inet_ntop(frame->sa.ss_family, &((struct sockaddr_in*)&frame->sa)->sin_addr, frame->remote, salen);
		break;
	case AF_INET6:
		inet_ntop(frame->sa.ss_family, &((struct sockaddr_in6*)&frame->sa)->sin6_addr, frame->remote, salen);
		break;
	}

	*out = frame;

	return 0;
}

int
mbt_lsd_frame_parse(struct mbt_lsd_frame *frame, struct mbt_lsd_frame_info **out)
{
	int rv;
	unsigned long port;
	const char *p, *ep;
	struct mbt_lsd_frame_info *info;
	struct mbt_infohash *ih;

	p = frame->frame;

	ep = strstr(p, "\r\n");
	if(!ep)
		return -ENOMSG;

	if(strncmp(p, "BT-SEARCH", 9) != 0)
		return -ENOMSG;

	p = ep+2;

	ep = strstr(p, "\r\n");
	if(!ep)
		return -ENOMSG;

	if(strncmp(p, "Host: ", 6) != 0)
		return -ENOMSG;

	p = ep+2;

	ep = strstr(p, "\r\n");
	if(!ep)
		return -ENOMSG;

	if(strncmp(p, "Port: ", 6) != 0)
		return -ENOMSG;

	p += 6;

	port = strtoul(p, NULL, 10);
	if(port > USHRT_MAX)
		return -ERANGE;

	p = ep+2;

	info = calloc(1, sizeof(*info));

	info->sa = frame->sa;
	assert(strlcpy(info->remote, frame->remote, sizeof(info->remote)) <= sizeof(info->remote));
	info->port = port;

	STAILQ_INIT(&info->infohashes);

	while(p && *p){
		ep = strstr(p, "\r\n");
		if(!ep)
			break;

		if(strncmp(p, "Infohash: ", 10) == 0){
			if(ep - (p+10) != MBT_MBT_INFOHASH_SHA1HEXLEN)
				goto next;

			rv = mbt_infohash_new(&ih, MBT_INFOHASH_SHA1, p+10);
			if(rv < 0){
				warnx("mbt_infohash_new: %s", strerror(-rv));
				goto err;
			}

			STAILQ_INSERT_TAIL(&info->infohashes, ih, next);
		} else if(strncmp(p, "cookie: ", 8) == 0 && !info->cookie){
			info->cookie = strndup(p+8, ep - (p+8));
		}

next:
		p = ep+2;
	}

	/* if no infohash, it's invalid */
	if(!(ih = STAILQ_FIRST(&info->infohashes)))
		goto err;

	*out = info;

	return 0;

err:
	mbt_lsd_frame_info_freep(&info);
	return -ENOMEM;
}

int
mbt_lsd_announce(struct mbt_lsd *lsd, const char *infohash_hex, uint16_t port)
{
	return mbt_lsd_announce_multi(lsd, &infohash_hex, 1, port);
}

int
mbt_lsd_announce_multi(struct mbt_lsd *lsd, const char **hashes, int count, uint16_t port)
{
	char buf[4096];
	int off = 0;
	struct sockaddr_in dst = {
		.sin_family = AF_INET,
		.sin_port = htons(6771),
		.sin_addr.s_addr = inet_addr("239.192.152.143"),
	};

	off += snprintf(buf + off, sizeof(buf) - off,
		"BT-SEARCH * HTTP/1.1\r\n"
		"Host: 239.192.152.143:6771\r\n"
		"Port: %hu\r\n"
		"cookie: %s\r\n", port, lsd->cookie);

	for (int i = 0; i < count && off < (int)sizeof(buf) - 60; i++)
		off += snprintf(buf + off, sizeof(buf) - off, "Infohash: %s\r\n", hashes[i]);

	off += snprintf(buf + off, sizeof(buf) - off, "\r\n");

	if (off < 0 || (size_t)off >= sizeof(buf))
		return -ENOMEM;

	sendto(lsd->fd, buf, off, 0, (struct sockaddr *)&dst, sizeof(dst));

	/* Also send on IPv6 multicast if available */
	if (lsd->fd6 >= 0) {
		char buf6[4096];
		int off6 = 0;
		struct sockaddr_in6 dst6 = { .sin6_family = AF_INET6, .sin6_port = htons(6771) };
		inet_pton(AF_INET6, "ff15::efc0:988f", &dst6.sin6_addr);

		off6 += snprintf(buf6 + off6, sizeof(buf6) - off6,
			"BT-SEARCH * HTTP/1.1\r\n"
			"Host: [ff15::efc0:988f]:6771\r\n"
			"Port: %hu\r\n"
			"cookie: %s\r\n", port, lsd->cookie);

		for (int i = 0; i < count && off6 < (int)sizeof(buf6) - 60; i++)
			off6 += snprintf(buf6 + off6, sizeof(buf6) - off6, "Infohash: %s\r\n", hashes[i]);

		off6 += snprintf(buf6 + off6, sizeof(buf6) - off6, "\r\n");

		if (off6 > 0 && (size_t)off6 < sizeof(buf6))
			sendto(lsd->fd6, buf6, off6, 0, (struct sockaddr *)&dst6, sizeof(dst6));
	}

	return 0;
}
