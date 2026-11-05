// SPDX-License-Identifier: ISC
/* The curl client: replies, failures, loop progress, teardown and a real tracker. */
#include <arpa/inet.h>
#include <assert.h>
#include <netinet/in.h>
#include <signal.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/socket.h>
#include <sys/stat.h>
#include <sys/wait.h>
#include <time.h>
#include <unistd.h>

#include "mbt/event.h"
#include "mbt/http_curl.h"
#include "mbt/metainfo.h"
#include "mbt/session.h"
#include "mbt/storage.h"
#include "mbt/torrent.h"
#include "mbt/tracker_server.h"
#include "test_util.h"

#define WAIT_SEC 20
#define PIECE_LEN 16384
#define FILE_LEN (PIECE_LEN * 3)
#define TICK_USEC 5000
#define MIN_TICKS 5

static struct mbt_ev *ev;

/* One-connection server. It reads a request and sends reply, or stays silent. */
struct server {
	int lfd, cfd;
	uint16_t port;
	struct mbt_ev_io *lio, *cio;
	const char *reply;
};

static void
client_cb(struct mbt_ev_io *io, int fd, uint32_t events, void *arg)
{
	struct server *sv = arg;
	char buf[4096];

	(void)io; (void)events;
	if (read(fd, buf, sizeof(buf)) <= 0 || sv->reply == NULL)
		return;
	assert(write(fd, sv->reply, strlen(sv->reply)) == (ssize_t)strlen(sv->reply));
	sv->reply = NULL;
	shutdown(fd, SHUT_WR);
}

static void
accept_cb(struct mbt_ev_io *io, int fd, uint32_t events, void *arg)
{
	struct server *sv = arg;

	(void)io; (void)events;
	sv->cfd = accept(fd, NULL, NULL);
	assert(sv->cfd >= 0);
	assert(mbt_ev_io_new(ev, &sv->cio, sv->cfd, MBT_EV_READ, client_cb, sv) == 0);
}

static void
server_start(struct server *sv, const char *reply)
{
	struct sockaddr_in sin = {.sin_family = AF_INET};
	socklen_t len = sizeof(sin);

	memset(sv, 0, sizeof(*sv));
	sv->cfd = -1;
	sv->reply = reply;
	sin.sin_addr.s_addr = htonl(INADDR_LOOPBACK);
	assert((sv->lfd = socket(AF_INET, SOCK_STREAM, 0)) >= 0);
	assert(bind(sv->lfd, (struct sockaddr *)&sin, sizeof(sin)) == 0);
	assert(listen(sv->lfd, 4) == 0);
	assert(getsockname(sv->lfd, (struct sockaddr *)&sin, &len) == 0);
	sv->port = ntohs(sin.sin_port);
	assert(mbt_ev_io_new(ev, &sv->lio, sv->lfd, MBT_EV_READ, accept_cb, sv) == 0);
}

static void
server_stop(struct server *sv)
{
	if (sv->cio != NULL) {
		mbt_ev_io_free(sv->cio);
		close(sv->cfd);
	}
	mbt_ev_io_free(sv->lio);
	close(sv->lfd);
}

struct result {
	int done;
	char *data;
	size_t len;
};

static void
done_cb(struct mbt_http_response *resp, void *arg)
{
	struct result *r = arg;

	r->done++;
	r->data = resp->data;
	r->len = resp->len;
}

static void
get(struct mbt_http_curl *h, uint16_t port, struct result *r)
{
	char url[64];

	snprintf(url, sizeof(url), "http://127.0.0.1:%hu/announce", port);
	struct mbt_http_request req = {.url = url, .method = "GET"};
	memset(r, 0, sizeof(*r));
	assert(mbt_http_curl_fn(&req, done_cb, r, h) == 0);
}

static void
wait_done(struct result *r)
{
	time_t end = time(NULL) + WAIT_SEC;

	while (r->done == 0) {
		assert(time(NULL) < end);
		test_ev_step(ev, 10);
	}
	assert(r->done == 1);
}

static void
test_reply(struct mbt_http_curl *h)
{
	struct server sv;
	struct result r;

	server_start(&sv, "HTTP/1.0 200 OK\r\nContent-Length: 5\r\n\r\nhello");
	get(h, sv.port, &r);
	wait_done(&r);
	assert(r.len == 5 && memcmp(r.data, "hello", 5) == 0);
	free(r.data);
	server_stop(&sv);
}

static void
test_error_status(struct mbt_http_curl *h)
{
	struct server sv;
	struct result r;

	server_start(&sv, "HTTP/1.0 404 Not Found\r\nContent-Length: 3\r\n\r\nno!");
	get(h, sv.port, &r);
	wait_done(&r);
	assert(r.data == NULL && r.len == 0);
	server_stop(&sv);
}

static void
test_refused(struct mbt_http_curl *h)
{
	struct server sv;
	struct result r;
	uint16_t port;

	server_start(&sv, NULL);
	port = sv.port;
	server_stop(&sv);
	get(h, port, &r);
	wait_done(&r);
	assert(r.data == NULL);
}

static void
tick_cb(struct mbt_ev_timer *timer, void *arg)
{
	int *ticks = arg;

	(*ticks)++;
	mbt_ev_timer_mod(timer, TICK_USEC);
}

/* A silent server does not stop the loop, and free fails the request. */
static void
test_loop_runs_and_free_cancels(void)
{
	struct mbt_http_curl *h;
	struct mbt_ev_timer *timer;
	struct server sv;
	struct result r;
	int ticks = 0;

	assert(mbt_http_curl_new(&h, ev) == 0);
	server_start(&sv, NULL);
	assert(mbt_ev_timer_new(ev, &timer, TICK_USEC, tick_cb, &ticks) == 0);
	get(h, sv.port, &r);
	while (ticks < MIN_TICKS)
		test_ev_step(ev, 10);
	assert(r.done == 0);
	mbt_http_curl_free(h);
	assert(r.done == 1 && r.data == NULL);
	mbt_ev_timer_free(timer);
	server_stop(&sv);
}

static void *
make_torrent(const char *dir, const char *announce, size_t *lenp)
{
	static uint8_t data[FILE_LEN];
	struct mbt_metainfo *mi;
	unsigned char *enc;
	char path[256];
	FILE *fp;

	snprintf(path, sizeof(path), "%s/x.bin", dir);
	for (int i = 0; i < FILE_LEN; i++)
		data[i] = (uint8_t)(i * 3 + 1);
	assert((fp = fopen(path, "wb")) != NULL);
	assert(fwrite(data, 1, FILE_LEN, fp) == FILE_LEN);
	fclose(fp);
	assert(mbt_metainfo_new(&mi) == 0);
	assert(mbt_metainfo_set_content(mi, path, PIECE_LEN) == 0);
	assert(mbt_metainfo_add_tracker(mi, announce) == 0);
	assert(mbt_metainfo_encode(mi, &enc, lenp) == 0);
	mbt_metainfo_free(mi);
	return enc;
}

static struct mbt_session *
session_on(const char *dir)
{
	struct mbt_http_curl *h;
	struct mbt_session *s;
	struct mbt_storage *st = mbt_storage_file_new(dir);

	assert(st);
	assert(mbt_http_curl_new(&h, ev) == 0);
	struct mbt_session_config cfg = {
		.ev = ev, .default_storage = st, .disable_utp = 1,
		.http = mbt_http_curl_fn, .http_arg = h, .http_free = mbt_http_curl_free,
	};
	assert(mbt_session_new(&s, &cfg) == 0);
	mbt_storage_unref(st);
	return s;
}

static uint16_t
free_port(void)
{
	struct sockaddr_in sin = {.sin_family = AF_INET};
	socklen_t len = sizeof(sin);
	int fd;

	sin.sin_addr.s_addr = htonl(INADDR_LOOPBACK);
	assert((fd = socket(AF_INET, SOCK_STREAM, 0)) >= 0);
	assert(bind(fd, (struct sockaddr *)&sin, sizeof(sin)) == 0);
	assert(getsockname(fd, (struct sockaddr *)&sin, &len) == 0);
	close(fd);
	return ntohs(sin.sin_port);
}

/*
 * The tracker reads each request with a blocking read, so it needs its own
 * process. Fork before the parent makes its loop: sd-event does not cross fork.
 */
static pid_t
tracker_start(uint16_t port)
{
	int fds[2];
	char ready;
	pid_t pid;

	assert(pipe(fds) == 0);
	assert((pid = fork()) >= 0);
	if (pid == 0) {
		struct mbt_ev *tev;
		struct mbt_tracker_store *store;
		struct mbt_tracker_http *tracker;

		assert(mbt_ev_new_default(&tev) == 0);
		assert(mbt_tracker_store_new(&store) == 0);
		assert(mbt_tracker_http_new(&tracker, tev, port, store) == 0);
		assert(write(fds[1], "x", 1) == 1);
		mbt_ev_run(tev);
		_exit(0);
	}
	assert(read(fds[0], &ready, 1) == 1);
	close(fds[0]);
	close(fds[1]);
	return pid;
}

/* Two sessions find each other through an HTTP tracker and transfer. */
static void
test_tracker_transfer(uint16_t tracker_port)
{
	char tmp[] = "/tmp/mbt_http_XXXXXX", seed_dir[64], leech_dir[64], announce[64];
	struct mbt_torrent *st = NULL, *lt = NULL;
	char cmd[128];
	size_t len;

	assert(mkdtemp(tmp));
	snprintf(seed_dir, sizeof(seed_dir), "%s/seed", tmp);
	snprintf(leech_dir, sizeof(leech_dir), "%s/leech", tmp);
	assert(mkdir(seed_dir, 0755) == 0);
	assert(mkdir(leech_dir, 0755) == 0);
	snprintf(announce, sizeof(announce), "http://127.0.0.1:%hu/announce", tracker_port);

	void *enc = make_torrent(seed_dir, announce, &len);
	struct mbt_session *seed = session_on(seed_dir);
	struct mbt_session *leech = session_on(leech_dir);

	assert(mbt_session_add_torrent_buf(seed, enc, len, NULL, &st) == 0);
	test_wait_verified(ev, st);
	assert(mbt_torrent_complete(st));
	assert(mbt_session_add_torrent_buf(leech, enc, len, NULL, &lt) == 0);

	time_t end = time(NULL) + WAIT_SEC;
	while (!mbt_torrent_complete(lt)) {
		assert(time(NULL) < end);
		test_ev_step(ev, 10);
	}
	mbt_session_free(leech);
	mbt_session_free(seed);
	free(enc);
	snprintf(cmd, sizeof(cmd), "rm -rf %s", tmp);
	assert(system(cmd) == 0);
}

/* A session freed with an announce pending must not get its callback later. */
static void
test_session_free_pending(void)
{
	char tmp[] = "/tmp/mbt_http_XXXXXX", announce[64], cmd[128];
	struct server sv;
	size_t len;

	assert(mkdtemp(tmp));
	server_start(&sv, NULL);
	snprintf(announce, sizeof(announce), "http://127.0.0.1:%hu/announce", sv.port);

	void *enc = make_torrent(tmp, announce, &len);
	struct mbt_session *s = session_on(tmp);

	assert(mbt_session_add_torrent_buf(s, enc, len, NULL, NULL) == 0);
	while (sv.cfd < 0)
		test_ev_step(ev, 10);
	mbt_session_free(s);
	for (int i = 0; i < 5; i++)
		test_ev_step(ev, 10);
	server_stop(&sv);
	free(enc);
	snprintf(cmd, sizeof(cmd), "rm -rf %s", tmp);
	assert(system(cmd) == 0);
}

int
main(void)
{
	struct mbt_http_curl *h;
	uint16_t tracker_port = free_port();
	pid_t tracker = tracker_start(tracker_port);

	assert(mbt_ev_new_default(&ev) == 0);
	assert(mbt_http_curl_new(&h, ev) == 0);
	test_reply(h);
	test_error_status(h);
	test_refused(h);
	mbt_http_curl_free(h);
	test_loop_runs_and_free_cancels();
	test_tracker_transfer(tracker_port);
	test_session_free_pending();
	mbt_ev_destroy(ev);
	kill(tracker, SIGKILL);
	waitpid(tracker, NULL, 0);
	fprintf(stderr, "http_curl_test: PASS\n");
	return 0;
}
