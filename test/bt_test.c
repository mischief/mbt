#include <fcntl.h>
#include <err.h>
#include <string.h>
#include <netinet/in.h>
#include <arpa/inet.h>
#include <assert.h>
#include <sysexits.h>
#include <stdio.h>
#include <signal.h>

#include "event.h"
#include "infohash.h"
#include <be.h>
#include "bt_legacy.h"
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
	struct mbt_infohash *ih;
	struct mbt_ev *ev = node_event(n);

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

		rv = remote_new(t, &r, mbt_infohash_ref(ih), ev);
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

struct lsd_ctx {
	struct lsd *lsd;
	struct mbt_ev *ev;
};

static void
lsd_handler(struct mbt_ev_io *io, int fd, uint32_t events, void *v)
{
	int rv;
	struct lsd_ctx *ctx = v;
	struct lsd *lsd = ctx->lsd;
	__attribute__((cleanup(lsd_frame_freep))) struct lsd_frame *frame = NULL;
	__attribute__((cleanup(lsd_frame_info_freep))) struct lsd_frame_info *info = NULL;
	struct mbt_infohash *ih;
	char hash[MBT_MBT_INFOHASH_SHA1HEXLEN+1];

	(void)io;
	(void)fd;
	(void)events;

	rv = lsd_recv(lsd, &frame);
	if(rv < 0){
		warnx("lsd_recv: %s", strerror(-rv));
		mbt_ev_stop(ctx->ev);
		return;
	}

	rv = lsd_frame_parse(frame, &info);
	if(rv < 0){
		warnx("lsd_frame_parse: %s", strerror(-rv));
		mbt_ev_stop(ctx->ev);
		return;
	}

	printf("received LSD frame from %s\n", info->remote);
	printf("bt port %hu\n", info->port);
	printf("cookie: %s\n", info->cookie);
	printf("hashes:\n");
	STAILQ_FOREACH(ih, &info->infohashes, next){
		if((rv = mbt_infohash_string(ih, hash, sizeof(hash))) < 0)
			warnx("mbt_infohash_string: %s", strerror(-rv));
		else
			printf("> %lu %s\n", strlen(hash), hash);
	}

	if(lsd->on_frame)
		lsd->on_frame(info, lsd->on_frame_arg);
}

int
main(int argc, char *argv[])
{
	int rv;
	struct mbt_ev *ev = NULL;
	__attribute__((cleanup(mbt_mbt_infohash_unrefp))) struct mbt_infohash *ih = NULL;
	__attribute__((cleanup(lsd_freep))) struct lsd *lsdfd = NULL;
	__attribute__((cleanup(node_freep))) struct node *node = NULL;
	__attribute__((cleanup(torrent_unrefp))) struct torrent *torrent = NULL;
	struct mbt_ev_io *lsd_io = NULL;

	struct remote *remote;

	(void)argc;
	(void)argv;

	const char hexstr[MBT_MBT_INFOHASH_SHA1HEXLEN+1] = "C8295CE630F2064F08440DB1534E4992CFE4862A";
	char hash[MBT_MBT_INFOHASH_SHA1HEXLEN+1];

	signal(SIGPIPE, SIG_IGN);

	if((rv = mbt_infohash_new(&ih, MBT_INFOHASH_SHA1, hexstr)) < 0)
		errx(1, "mbt_infohash_new: %s", strerror(-rv));

	if((rv = mbt_infohash_string(ih, hash, sizeof(hash))) < 0)
		warnx("mbt_infohash_string: %s", strerror(-rv));
	else
		printf("%s\n", hash);

	assert(strcmp(hexstr, hash) == 0);

#ifdef HAVE_SD_EVENT
	rv = mbt_ev_sd_new(&ev);
#elif defined(HAVE_LIBEVENT)
	rv = mbt_ev_libevent_new(&ev);
#else
#error "No event backend configured"
#endif
	if(rv < 0)
		errx(EX_OSERR, "mbt_ev_new: %s", strerror(-rv));

	if((rv = node_new(&node, 51412, ev)) < 0)
		errx(1, "node_new: %s", strerror(-rv));

	warnx("listening on 51412");

	if((rv = lsd_init(&lsdfd, O_NONBLOCK)) < 0)
		errx(1, "lsd_init: %s", strerror(-rv));

	lsdfd->on_frame = lsd_on_frame;
	lsdfd->on_frame_arg = node;

	struct lsd_ctx lsd_ctx = { .lsd = lsdfd, .ev = ev };

	if((rv = mbt_ev_io_new(ev, &lsd_io, lsdfd->fd, MBT_EV_READ, lsd_handler, &lsd_ctx)) < 0)
		errx(1, "mbt_ev_io_new: %s", strerror(-rv));

	if((rv = torrent_new(node, ih, &torrent)) < 0)
		errx(1, "torrent_new: %s", strerror(-rv));

	rv = remote_new(torrent, &remote, ih, ev);
	if(rv < 0)
		errx(1, "remote_new: %s", strerror(-rv));

	rv = remote_connect(remote, "192.168.0.10", 28888);
	if(rv < 0)
		errx(1, "remote_connect: %s", strerror(-rv));

	remote_unref(remote);
	remote = NULL;

	rv = mbt_ev_run(ev);
	if(rv < 0)
		errx(1, "mbt_ev_run: %s", strerror(-rv));

	mbt_ev_io_free(lsd_io);
	mbt_ev_destroy(ev);

	return 0;
}
