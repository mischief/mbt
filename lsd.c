#include <sys/types.h>
#include <sys/queue.h>
#include <arpa/inet.h>
#include <sys/socket.h>
#include <netinet/in.h>
#include <fcntl.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>
#include <assert.h>
#include <limits.h>
#include <errno.h>
#include <err.h>

#include "infohash.h"
#include "lsd.h"

void
lsd_freep(struct lsd **v)
{
	struct lsd *lsd = *v;

	if(!lsd)
		return;

	close(lsd->fd);
	free(lsd);
}

void
lsd_frame_freep(struct lsd_frame **v)
{
	if(*v)
		free(*v);
}

void
lsd_frame_info_freep(struct lsd_frame_info **v)
{
	struct lsd_frame_info *lfi;
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
lsd_init(struct lsd **out, int setfl)
{
	struct lsd *lsd;
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
	*out = lsd;

	return 0;
err:
	serr = errno;
	close(fd);
	return -serr;
}

int
lsd_recv(struct lsd *lsd, struct lsd_frame **out)
{
	int rv;
	socklen_t salen;
	struct lsd_frame *frame;

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
lsd_frame_parse(struct lsd_frame *frame, struct lsd_frame_info **out)
{
	int rv;
	unsigned long port;
	const char *p, *ep;
	struct lsd_frame_info *info;
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
	lsd_frame_info_freep(&info);
	return -ENOMEM;
}

int
lsd_announce(struct lsd *lsd, const char *infohash_hex, uint16_t port)
{
	char buf[256];
	int len;
	struct sockaddr_in dst = {
		.sin_family = AF_INET,
		.sin_port = htons(6771),
		.sin_addr.s_addr = inet_addr("239.192.152.143"),
	};

	len = snprintf(buf, sizeof(buf),
		"BT-SEARCH * HTTP/1.1\r\n"
		"Host: 239.192.152.143:6771\r\n"
		"Port: %hu\r\n"
		"Infohash: %s\r\n"
		"\r\n", port, infohash_hex);

	if (len < 0 || (size_t)len >= sizeof(buf))
		return -ENOMEM;

	ssize_t rv = sendto(lsd->fd, buf, len, 0, (struct sockaddr *)&dst, sizeof(dst));
	if (rv < 0)
		return -errno;
	return 0;
}
