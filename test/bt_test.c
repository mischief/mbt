#include <fcntl.h>
#include <err.h>
#include <string.h>
#include <netinet/in.h>
#include <arpa/inet.h>
#include <assert.h>
#include <sysexits.h>
#include <stdio.h>

#include <systemd/sd-event.h>

#include "infohash.h"
#include <be.h>
#include <bt.h>
#include <remote.h>
#include <node.h>
#include <lsd.h>

int
lsd_on_frame(struct lsd_frame_info *info, void *v)
{
	int rv;
	struct node *n = v;
	struct remote *r;
	struct torrent *t;
	struct infohash *ih;
	__attribute__((cleanup(sd_event_unrefp))) sd_event *event = node_event_ref(n);

	warnx("%s called", __func__);

	STAILQ_FOREACH(ih, &info->infohashes, next){
		t = node_find_torrent(n, ih);
		if(!t){
			rv = torrent_new(n, ih, &t);
			if(rv < 0){
				warnx("%s: torrent_new: %s", __func__, strerror(-rv));
				continue;
			}

			node_add_torrent(n, t);
		}

		rv = remote_new(t, &r, infohash_ref(ih), event);
		if(rv < 0){
			warnx("remote_new4: %s", strerror(-rv));
			continue;
		}

		rv = remote_connect(r, info->remote, info->port);
		if(rv < 0){
			warnx("%s: remote_connect: %s", __func__, strerror(-rv));
			return 0;
		}
	}

	return 0;
}

static int
lsd_handler(sd_event_source *s, int fd, uint32_t revents, void *v)
{
	int rv;
	struct lsd *lsd = v;
	__attribute__((cleanup(lsd_frame_freep))) struct lsd_frame *frame = NULL;
	__attribute__((cleanup(lsd_frame_info_freep))) struct lsd_frame_info *info = NULL;
	struct infohash *ih;
	char hash[INFOHASH_SHA1HEXLEN+1];

	(void)s;
	(void)fd;
	(void)revents;

	rv = lsd_recv(lsd, &frame);
	if(rv < 0){
		warnx("lsd_recv: %s", strerror(-rv));
		sd_event_exit(sd_event_source_get_event(s), rv);
		return rv;
	}

	rv = lsd_frame_parse(frame, &info);
	if(rv < 0){
		warnx("lsd_frame_parse: %s", strerror(-rv));
		sd_event_exit(sd_event_source_get_event(s), rv);
		return rv;
	}

	printf("received LSD frame from %s\n", info->remote);
	printf("bt port %hu\n", info->port);
	printf("cookie: %s\n", info->cookie);
	printf("hashes:\n");
	STAILQ_FOREACH(ih, &info->infohashes, next){
		if((rv = infohash_string(ih, hash, sizeof(hash))) < 0)
			warnx("infohash_string: %s", strerror(-rv));
		else
			printf("> %lu %s\n", strlen(hash), hash);
	}

	if(lsd->on_frame)
		return lsd->on_frame(info, lsd->on_frame_arg);

	return 0;
}

int
main(int argc, char *argv[])
{
	int rv;
	__attribute__((cleanup(sd_event_unrefp))) sd_event *ev = NULL;
	__attribute__((cleanup(infohash_unrefp))) struct infohash *ih = NULL;
	__attribute__((cleanup(lsd_freep))) struct lsd *lsdfd = NULL;
	__attribute__((cleanup(node_freep))) struct node *node = NULL;
	__attribute__((cleanup(torrent_unrefp))) struct torrent *torrent = NULL;

	struct remote *remote;

	(void)argc;
	(void)argv;

	const char hexstr[INFOHASH_SHA1HEXLEN+1] = "6F4370DF4304609A8793CE2B59178DCC8FEBF5E2";
	char hash[INFOHASH_SHA1HEXLEN+1];

	signal(SIGPIPE, SIG_IGN);

	if((rv = infohash_new(&ih, INFOHASH_SHA1, hexstr)) < 0)
		errx(1, "infohash_new: %s", strerror(-rv));

	if((rv = infohash_string(ih, hash, sizeof(hash))) < 0)
		warnx("infohash_string: %s", strerror(-rv));
	else
		printf("%s\n", hash);

	assert(strcmp(hexstr, hash) == 0);

	if((rv = sd_event_default(&ev)) < 0)
		errx(EX_OSERR, "sd_event_default: %s", strerror(-rv));

        if((rv = sd_event_add_signal(ev, NULL, SIGTERM|SD_EVENT_SIGNAL_PROCMASK, NULL, NULL)) < 0)
                errx(EX_OSERR, "sd_event_add_signal: %s", strerror(-rv));

        if((rv = sd_event_add_signal(ev, NULL, SIGINT|SD_EVENT_SIGNAL_PROCMASK, NULL, NULL)) < 0)
                errx(EX_OSERR, "sd_event_add_signal: %s", strerror(-rv));

	if((rv = node_new(&node, 51412, ev)) < 0)
		errx(1, "node_new: %s", strerror(-rv));

	warnx("listening on 51412");

	if((rv = lsd_init(&lsdfd, O_NONBLOCK)) < 0)
		errx(1, "lsd_init: %s", strerror(-rv));

	lsdfd->on_frame = lsd_on_frame;
	lsdfd->on_frame_arg = node;

	if((rv = sd_event_add_io(ev, NULL, lsdfd->fd, EPOLLIN, lsd_handler, lsdfd)) < 0)
		errx(1, "sd_event_add_io: %s", strerror(-rv));

	if((rv = torrent_new(node, ih, &torrent)) < 0)
		errx(1, "torrent_new: %s", strerror(-rv));

	rv = remote_new(torrent, &remote, ih, ev);
	if(rv < 0)
		errx(1, "remote_new: %s", strerror(-rv));

	rv = remote_connect(remote, "192.168.0.12", 59677);
	if(rv < 0)
		errx(1, "remote_connect: %s", strerror(-rv));

	remote_unref(remote);
	remote = NULL;

	rv = sd_event_loop(ev);
	if(rv < 0)
		errx(1, "sd_event_loop: %s", strerror(-rv));

	return 0;
}
