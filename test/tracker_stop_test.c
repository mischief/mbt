// SPDX-License-Identifier: ISC
/* A client that leaves tells its tracker, and the tracker forgets it. */
#include <arpa/inet.h>
#include <assert.h>
#include <netinet/in.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/socket.h>
#include <unistd.h>

#include "mbt/event.h"
#include "mbt/infohash.h"
#include "mbt/metainfo.h"
#include "mbt/session.h"
#include "mbt/storage.h"
#include "mbt/torrent.h"
#include "mbt/tracker_server.h"

#define WAIT_STEPS 1000          /* 10 seconds at 10 ms */

static struct mbt_ev *ev;
static struct mbt_tracker_server *ts;
static char tmpdir[] = "/tmp/mbt_stop_XXXXXX";

static int
wait_peers(int want)
{
	for (int i = 0; i < WAIT_STEPS; i++) {
		if (mbt_tracker_server_num_peers(ts) == want)
			return 1;
		mbt_ev_step(ev, 10);
	}
	return 0;
}

static void
encode(const char *name, unsigned char **enc, size_t *len)
{
	struct mbt_metainfo *mi;
	char path[256], url[64];
	FILE *fp;

	snprintf(path, sizeof(path), "%s/%s", tmpdir, name);
	fp = fopen(path, "wb");
	assert(fp);
	fputs(name, fp);
	fclose(fp);
	snprintf(url, sizeof(url), "udp://127.0.0.1:%hu/announce", mbt_tracker_server_port(ts));
	assert(mbt_metainfo_new(&mi) == 0);
	assert(mbt_metainfo_set_content(mi, path, 16384) == 0);
	assert(mbt_metainfo_add_tracker(mi, url) == 0);
	assert(mbt_metainfo_encode(mi, enc, len) == 0);
	mbt_metainfo_free(mi);
}

static struct mbt_session *
new_session(struct mbt_storage_memory *m)
{
	struct mbt_session_config cfg = {.ev = ev, .disable_utp = 1,
	    .default_storage = mbt_storage_memory_storage(m)};
	struct mbt_session *s;

	assert(mbt_session_new(&s, &cfg) == 0);
	return s;
}

/* Send one raw announce from a fixed client port, and let the server run. */
static void
http_announce(struct sockaddr_in *sa, const char *event)
{
	char req[256], resp[1024];
	int fd = socket(AF_INET, SOCK_STREAM, 0);

	assert(fd >= 0);
	assert(connect(fd, (struct sockaddr *)sa, sizeof(*sa)) == 0);
	snprintf(req, sizeof(req), "GET /announce?info_hash=%%AB%%AB%%AB%%AB%%AB%%AB%%AB%%AB%%AB%%AB"
	         "%%AB%%AB%%AB%%AB%%AB%%AB%%AB%%AB%%AB%%AB&port=6000&left=0%s HTTP/1.0\r\n\r\n", event);
	assert(send(fd, req, strlen(req), 0) == (ssize_t)strlen(req));
	for (int i = 0; i < 50; i++)
		mbt_ev_step(ev, 10);
	assert(recv(fd, resp, sizeof(resp), MSG_DONTWAIT) > 0);
	close(fd);
}

static void
test_http(void)
{
	struct mbt_tracker_http *th;
	struct sockaddr_in sa = {.sin_family = AF_INET};

	/* The server has no port getter, so bind one from the kernel first. */
	int probe = socket(AF_INET, SOCK_STREAM, 0);
	socklen_t len = sizeof(sa);

	sa.sin_addr.s_addr = htonl(INADDR_LOOPBACK);
	assert(bind(probe, (struct sockaddr *)&sa, sizeof(sa)) == 0);
	assert(getsockname(probe, (struct sockaddr *)&sa, &len) == 0);
	close(probe);
	assert(mbt_tracker_http_new(&th, ev, ntohs(sa.sin_port), NULL) == 0);
	http_announce(&sa, "");
	assert(mbt_tracker_http_num_peers(th) == 1);
	http_announce(&sa, "&event=stopped");
	assert(mbt_tracker_http_num_peers(th) == 0);
	mbt_tracker_http_free(th);
}

int
main(void)
{
	struct mbt_storage_memory *m = mbt_storage_memory_new(), *m2 = mbt_storage_memory_new();
	struct mbt_session *s, *s2;
	struct mbt_torrent *t;
	unsigned char *a, *b;
	size_t alen, blen;
	char cmd[300];

	assert(mkdtemp(tmpdir) && m && m2);
	assert(mbt_ev_new_default(&ev) == 0);
	assert(mbt_tracker_server_new(&ts, ev, 0, NULL) == 0);
	encode("a", &a, &alen);
	encode("b", &b, &blen);

	/* Removing a torrent announces stopped. */
	s = new_session(m);
	assert(mbt_session_add_torrent_buf(s, a, alen, NULL, &t) == 0);
	assert(wait_peers(1));
	assert(mbt_session_remove_torrent(s, mbt_torrent_infohash(t)) == 0);
	assert(wait_peers(0));


	/* A session stops all of its torrents and waits for replies. */
	s2 = new_session(m2);
	assert(mbt_session_add_torrent_buf(s2, b, blen, NULL, &t) == 0);
	assert(wait_peers(1));
	assert(mbt_session_stop_announce(s2) == 1);
	assert(mbt_session_stop_announce(s2) == 1); /* repeating sends nothing */
	for (int i = 0; i < WAIT_STEPS && mbt_session_stops_pending(s2) > 0; i++)
		mbt_ev_step(ev, 10);
	assert(mbt_session_stops_pending(s2) == 0);
	assert(mbt_tracker_server_num_peers(ts) == 0);

	/* A torrent that stopped does not announce again. */
	mbt_session_reannounce(s2, mbt_torrent_infohash(t));
	for (int i = 0; i < 100; i++)
		mbt_ev_step(ev, 10);
	assert(mbt_tracker_server_num_peers(ts) == 0);

	mbt_session_free(s2);
	mbt_session_free(s);
	mbt_tracker_server_free(ts);
	test_http();
	mbt_storage_memory_destroy(m);
	mbt_storage_memory_destroy(m2);
	mbt_ev_destroy(ev);
	free(a);
	free(b);
	snprintf(cmd, sizeof(cmd), "rm -rf %s", tmpdir);
	assert(system(cmd) == 0);
	puts("tracker_stop: ok");
	return 0;
}
