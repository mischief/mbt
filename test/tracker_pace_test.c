// SPDX-License-Identifier: ISC
/*
 * Tracker announces of many torrents go out at the session rate, new
 * torrents first, and all UDP tracker traffic uses the session port.
 */
#include <assert.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>
#include <unistd.h>
#include <sys/socket.h>
#include <netinet/in.h>
#include <arpa/inet.h>

#include "mbt/event.h"
#include "mbt/infohash.h"
#include "mbt/metainfo.h"
#include "mbt/session.h"
#include "mbt/tracker.h"

#define N 100
#define RATE 50

static struct mbt_ev *ev;
static char tmpdir[] = "/tmp/mbt_pace_XXXXXX";
static int calls;
static double first_call, last_call, min_gap = 1e9;
static int started_at;       /* call number of the last event=started */

/* UDP tracker */
static int udp_connects, udp_announces, udp_ports_differ;
static uint16_t udp_port;
static int udp_refuse;       /* answer connects with an error */
static int done_ok, done_err;

static double
now(void)
{
	struct timespec ts;

	clock_gettime(CLOCK_MONOTONIC, &ts);
	return ts.tv_sec + ts.tv_nsec / 1e9;
}

static int
fake_http(struct mbt_http_request *req, mbt_http_done_fn done, void *done_arg, void *arg)
{
	static const char reply[] = "d8:intervali900e5:peers0:e";
	struct mbt_http_response resp = {0};
	double t = now();

	(void)arg;
	if (calls == 0)
		first_call = t;
	else if (t - last_call < min_gap)
		min_gap = t - last_call;
	last_call = t;
	calls++;
	if (strstr(req->url, "event=started"))
		started_at = calls;
	resp.data = strdup(reply);
	resp.len = sizeof(reply) - 1;
	done(&resp, done_arg);
	return 0;
}

static void
encode(int i, const char *tracker, unsigned char **enc, size_t *len)
{
	struct mbt_metainfo *mi;
	char path[256];
	FILE *fp;

	snprintf(path, sizeof(path), "%s/f%d", tmpdir, i);
	fp = fopen(path, "wb");
	assert(fp);
	fprintf(fp, "torrent %d", i);
	fclose(fp);
	assert(mbt_metainfo_new(&mi) == 0);
	assert(mbt_metainfo_set_content(mi, path, 16384) == 0);
	assert(mbt_metainfo_add_tracker(mi, tracker) == 0);
	assert(mbt_metainfo_encode(mi, enc, len) == 0);
	mbt_metainfo_free(mi);
}

static void
add(struct mbt_session *s, int i, const char *tracker)
{
	unsigned char *enc;
	size_t len;

	encode(i, tracker, &enc, &len);
	assert(mbt_session_add_torrent_buf(s, enc, len, NULL, NULL) == 0);
	free(enc);
}

static void
run_until(int *counter, int want, double secs)
{
	double end = now() + secs;

	while (*counter < want && now() < end)
		mbt_ev_step(ev, 10);
}

/* File storage: each torrent seeds its own file in tmpdir. */
static struct mbt_session *
start(void)
{
	struct mbt_session *s;
	struct mbt_session_config cfg = {.ev = ev, .disable_utp = 1, .http = fake_http,
	    .default_data_dir = tmpdir, .announce_rate = RATE};
	assert(mbt_session_new(&s, &cfg) == 0);
	mbt_session_disable_dht(s);
	mbt_session_disable_lsd(s);
	return s;
}

/* A burst of adds goes out at the rate, and a new torrent goes before reannounces. */
static void
test_http_pace(void)
{
	struct mbt_session *s = start();

	for (int i = 0; i < N; i++)
		add(s, i, "http://t.invalid/announce");
	assert(calls <= 2);
	run_until(&calls, N, 10);
	assert(calls == N);
	/* N - 1 gaps of 1/RATE s each. */
	assert(last_call - first_call > (N - 1) * 0.9 / RATE);
	assert(min_gap > 0.5 / RATE);

	calls = 0;
	min_gap = 1e9;
	assert(mbt_session_reannounce(s, NULL) == 0);
	run_until(&calls, 5, 5);
	/* The new torrent must not wait behind about N reannounces. */
	int before = calls;
	started_at = 0;
	add(s, N, "http://t.invalid/announce");
	run_until(&calls, N + 1, 10);
	assert(started_at > 0 && started_at <= before + 1);
	assert(calls == N + 1);

	mbt_session_free(s);
}

/* A UDP tracker that answers connects and announces, and checks the source port. */
static void
udp_server_cb(struct mbt_ev_io *io, int fd, uint32_t events, void *v)
{
	uint8_t pkt[256], resp[20] = {0};
	struct sockaddr_in from;
	socklen_t fromlen = sizeof(from);
	uint16_t *want = v;
	ssize_t n;

	(void)io; (void)events;
	n = recvfrom(fd, pkt, sizeof(pkt), 0, (struct sockaddr *)&from, &fromlen);
	if (n < 16)
		return;
	if (ntohs(from.sin_port) != *want)
		udp_ports_differ++;
	if (n == 16 && udp_refuse) {
		udp_connects++;
		resp[3] = 3;
		memcpy(resp + 4, pkt + 12, 4);
		memcpy(resp + 8, "no", 2);
		sendto(fd, resp, 10, 0, (struct sockaddr *)&from, fromlen);
	} else if (n == 16) {
		udp_connects++;
		memcpy(resp + 4, pkt + 12, 4);
		memset(resp + 8, 0x42, 8);
		sendto(fd, resp, 16, 0, (struct sockaddr *)&from, fromlen);
	} else if (n == 98) {
		uint8_t id[8];

		memset(id, 0x42, 8);
		assert(memcmp(pkt, id, 8) == 0);
		udp_announces++;
		resp[3] = 1;
		memcpy(resp + 4, pkt + 12, 4);
		resp[10] = 3; /* interval 900 */
		resp[11] = 0x84;
		sendto(fd, resp, 20, 0, (struct sockaddr *)&from, fromlen);
	}
}

static void
test_udp_one_socket(void)
{
	struct sockaddr_in sa = {.sin_family = AF_INET};
	socklen_t salen = sizeof(sa);
	struct mbt_session *s = start();
	struct mbt_ev_io *io;
	char url[64];
	int fd;

	fd = socket(AF_INET, SOCK_DGRAM, 0);
	assert(fd >= 0);
	inet_pton(AF_INET, "127.0.0.1", &sa.sin_addr);
	assert(bind(fd, (struct sockaddr *)&sa, sizeof(sa)) == 0);
	assert(getsockname(fd, (struct sockaddr *)&sa, &salen) == 0);
	udp_port = mbt_session_listen_port(s);
	assert(mbt_ev_io_new(ev, &io, fd, MBT_EV_READ, udp_server_cb, &udp_port) == 0);
	snprintf(url, sizeof(url), "udp://127.0.0.1:%hu/announce", ntohs(sa.sin_port));

	for (int i = 0; i < 30; i++)
		add(s, 200 + i, url);
	run_until(&udp_announces, 30, 10);
	assert(udp_announces == 30);
	/* One source port, the session port, and one connect for all. */
	assert(udp_ports_differ == 0);
	assert(udp_connects == 1);

	mbt_session_free(s);
	mbt_ev_io_free(io);
	close(fd);
}

static void
done_cb(struct mbt_infohash *ih, int err, int interval, void *arg)
{
	(void)ih; (void)interval; (void)arg;
	if (err == 0)
		done_ok++;
	else
		done_err++;
}

/* Announces that wait on a connect fail with it, and the next one connects again. */
static void
test_udp_connect_error(void)
{
	struct sockaddr_in sa = {.sin_family = AF_INET};
	socklen_t salen = sizeof(sa);
	struct mbt_tracker_group *g;
	struct mbt_infohash *ih;
	struct mbt_ev_io *io;
	uint8_t pid[20] = {0};
	char url[64];
	int fd, total = 0;

	fd = socket(AF_INET, SOCK_DGRAM, 0);
	assert(fd >= 0);
	inet_pton(AF_INET, "127.0.0.1", &sa.sin_addr);
	assert(bind(fd, (struct sockaddr *)&sa, sizeof(sa)) == 0);
	assert(getsockname(fd, (struct sockaddr *)&sa, &salen) == 0);
	udp_port = 0;
	assert(mbt_ev_io_new(ev, &io, fd, MBT_EV_READ, udp_server_cb, &udp_port) == 0);
	snprintf(url, sizeof(url), "udp://127.0.0.1:%hu/announce", ntohs(sa.sin_port));
	assert(mbt_infohash_new(&ih, MBT_INFOHASH_SHA1,
	                        "0123456789abcdef0123456789abcdef01234567") == 0);
	assert(mbt_tracker_group_new(&g) == 0);

	struct mbt_tracker_request req = {.ev = ev, .ih = ih, .peer_id = pid,
	    .listen_port = 7000, .done_cb = done_cb, .group = g};
	udp_connects = udp_announces = done_ok = done_err = 0;
	udp_refuse = 1;
	for (int i = 0; i < 5; i++)
		assert(mbt_tracker_udp_announce(url, &req) == 0);
	run_until(&done_err, 5, 5);
	assert(done_err == 5 && done_ok == 0);
	assert(udp_connects == 1);

	udp_refuse = 0;
	assert(mbt_tracker_udp_announce(url, &req) == 0);
	run_until(&done_ok, 1, 5);
	assert(done_ok == 1 && udp_connects == 2);

	/* Free with announces in flight. */
	for (int i = 0; i < 3; i++)
		assert(mbt_tracker_udp_announce(url, &req) == 0);
	while (total++ < 3)
		mbt_ev_step(ev, 10);
	mbt_tracker_group_free(g);
	mbt_infohash_unref(ih);
	mbt_ev_io_free(io);
	close(fd);
}

int
main(void)
{
	char cmd[300];

	assert(mkdtemp(tmpdir));
	assert(mbt_ev_new_default(&ev) == 0);
	test_http_pace();
	test_udp_one_socket();
	test_udp_connect_error();
	mbt_ev_destroy(ev);
	snprintf(cmd, sizeof(cmd), "rm -rf %s", tmpdir);
	assert(system(cmd) == 0);
	puts("tracker_pace: ok");
	return 0;
}
