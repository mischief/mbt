// SPDX-License-Identifier: ISC
/* Announces report the external address and port that PCP gives. */
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
#include "mbt/storage.h"
#include "mbt/tracker.h"

#define HASH "0123456789abcdef0123456789abcdef01234567"

static struct mbt_ev *ev;
static char last_url[2048];
static int http_calls, udp_done, pcp_calls;
static uint8_t udp_ip[4];

static int
fake_http(struct mbt_http_request *req, mbt_http_done_fn done, void *done_arg, void *arg)
{
	static const char reply[] = "d8:intervali900e5:peers0:e";
	struct mbt_http_response resp = {0};

	(void)arg;
	snprintf(last_url, sizeof(last_url), "%s", req->url);
	http_calls++;
	resp.data = strdup(reply);
	resp.len = sizeof(reply) - 1;
	done(&resp, done_arg);
	return 0;
}

static void
http_announce(struct mbt_infohash *ih, const char *ip)
{
	uint8_t pid[20] = {0};
	struct mbt_tracker_request req = {
		.ev = ev, .ih = ih, .peer_id = pid, .listen_port = 7000, .ip = ip,
	};

	last_url[0] = '\0';
	assert(mbt_tracker_http_announce("http://t.invalid/announce", &req, fake_http, NULL) == 0);
}

static void
test_http(struct mbt_infohash *ih)
{
	http_announce(ih, "203.0.113.5");
	assert(strstr(last_url, "&ip=203.0.113.5&"));
	http_announce(ih, "2001:db8::1");
	assert(strstr(last_url, "&ip=2001:db8::1&"));
	http_announce(ih, NULL);
	assert(!strstr(last_url, "ip="));
	http_announce(ih, "1.2.3.4&left=0");
	assert(!strstr(last_url, "ip="));
}

/* A UDP tracker that records the IP field of the announce. */
static void
udp_server_cb(struct mbt_ev_io *io, int fd, uint32_t events, void *v)
{
	uint8_t pkt[256], resp[20] = {0};
	struct sockaddr_storage from;
	socklen_t fromlen = sizeof(from);
	ssize_t n;

	(void)io; (void)events; (void)v;
	n = recvfrom(fd, pkt, sizeof(pkt), 0, (struct sockaddr *)&from, &fromlen);
	if (n == 16) {
		memcpy(resp + 4, pkt + 12, 4);
		memset(resp + 8, 0x42, 8);
		sendto(fd, resp, 16, 0, (struct sockaddr *)&from, fromlen);
	} else if (n == 98) {
		memcpy(udp_ip, pkt + 84, 4);
		resp[3] = 1;
		memcpy(resp + 4, pkt + 12, 4);
		resp[10] = 3; /* interval 900 */
		resp[11] = 0x84;
		sendto(fd, resp, 20, 0, (struct sockaddr *)&from, fromlen);
	}
}

static void
udp_done_cb(struct mbt_infohash *ih, int err, int interval, void *arg)
{
	(void)ih; (void)interval; (void)arg;
	assert(err == 0);
	udp_done++;
}

static void
udp_announce(struct mbt_infohash *ih, uint16_t port, const char *ip)
{
	uint8_t pid[20] = {0};
	char url[64];
	struct mbt_tracker_request req = {
		.ev = ev, .ih = ih, .peer_id = pid, .listen_port = 7000, .ip = ip,
		.done_cb = udp_done_cb,
	};
	time_t end = time(NULL) + 5;
	int want = udp_done + 1;

	memset(udp_ip, 0xff, sizeof(udp_ip));
	snprintf(url, sizeof(url), "udp://127.0.0.1:%hu/announce", port);
	assert(mbt_tracker_udp_announce(url, &req) == 0);
	while (udp_done < want && time(NULL) < end)
		mbt_ev_step(ev, 10);
	assert(udp_done == want);
}

static void
test_udp(struct mbt_infohash *ih)
{
	static const uint8_t want[4] = {203, 0, 113, 5}, zero[4] = {0};
	struct sockaddr_in sa = {.sin_family = AF_INET};
	socklen_t len = sizeof(sa);
	struct mbt_ev_io *io;
	int fd;

	fd = socket(AF_INET, SOCK_DGRAM, 0);
	assert(fd >= 0);
	inet_pton(AF_INET, "127.0.0.1", &sa.sin_addr);
	assert(bind(fd, (struct sockaddr *)&sa, sizeof(sa)) == 0);
	assert(getsockname(fd, (struct sockaddr *)&sa, &len) == 0);
	assert(mbt_ev_io_new(ev, &io, fd, MBT_EV_READ, udp_server_cb, NULL) == 0);

	udp_announce(ih, ntohs(sa.sin_port), "203.0.113.5");
	assert(memcmp(udp_ip, want, 4) == 0);
	/* IPv6 does not fit the field. */
	udp_announce(ih, ntohs(sa.sin_port), "2001:db8::1");
	assert(memcmp(udp_ip, zero, 4) == 0);
	udp_announce(ih, ntohs(sa.sin_port), NULL);
	assert(memcmp(udp_ip, zero, 4) == 0);

	mbt_ev_io_free(io);
	close(fd);
}

/* A PCP server that maps internal port p to 203.0.113.42:p+1000. */
static void
pcp_server_cb(struct mbt_ev_io *io, int fd, uint32_t events, void *v)
{
	uint8_t pkt[256];
	struct sockaddr_storage from;
	socklen_t fromlen = sizeof(from);
	uint16_t iport, eport;
	uint32_t lifetime = htonl(2);

	(void)io; (void)events; (void)v;
	if (recvfrom(fd, pkt, sizeof(pkt), 0, (struct sockaddr *)&from, &fromlen) < 60)
		return;
	pkt[1] |= 0x80;
	pkt[3] = 0;
	memcpy(pkt + 4, &lifetime, 4);
	memset(pkt + 8, 0, 16);
	memcpy(&iport, pkt + 40, 2);
	eport = htons(ntohs(iport) + 1000);
	memcpy(pkt + 42, &eport, 2);
	memset(pkt + 44, 0, 16);
	pkt[54] = pkt[55] = 0xff;
	inet_pton(AF_INET, "203.0.113.42", pkt + 56);
	sendto(fd, pkt, 60, 0, (struct sockaddr *)&from, fromlen);
	pcp_calls++;
}

/* After the map, the session announces again with the external address. */
static void
test_session(void)
{
	char tmpdir[] = "/tmp/mbt_tracker_ip_XXXXXX", path[300], want[64];
	struct sockaddr_in sa = {.sin_family = AF_INET, .sin_port = htons(5351)};
	struct mbt_storage_memory *mem;
	struct mbt_metainfo *mi;
	struct mbt_session *s;
	struct mbt_ev_io *io;
	unsigned char *enc;
	size_t len;
	FILE *fp;
	int fd, yes = 1;

	fd = socket(AF_INET, SOCK_DGRAM, 0);
	assert(fd >= 0);
	setsockopt(fd, SOL_SOCKET, SO_REUSEADDR, &yes, sizeof(yes));
	inet_pton(AF_INET, "127.0.0.1", &sa.sin_addr);
	if (bind(fd, (struct sockaddr *)&sa, sizeof(sa)) < 0) {
		puts("tracker_ip: PCP port busy, session part skipped");
		close(fd);
		return;
	}
	assert(mbt_ev_io_new(ev, &io, fd, MBT_EV_READ, pcp_server_cb, NULL) == 0);

	assert(mkdtemp(tmpdir));
	snprintf(path, sizeof(path), "%s/data", tmpdir);
	fp = fopen(path, "wb");
	assert(fp);
	fputs("data", fp);
	fclose(fp);
	assert(mbt_metainfo_new(&mi) == 0);
	assert(mbt_metainfo_set_content(mi, path, 16384) == 0);
	assert(mbt_metainfo_add_tracker(mi, "http://t.invalid/announce") == 0);
	assert(mbt_metainfo_encode(mi, &enc, &len) == 0);
	mbt_metainfo_free(mi);

	mem = mbt_storage_memory_new();
	assert(mem);
	struct mbt_session_config cfg = {.ev = ev, .disable_utp = 1, .http = fake_http,
	    .default_storage = mbt_storage_memory_storage(mem),
	    .enable_pcp = 1, .pcp_gateway = "127.0.0.1"};
	assert(mbt_session_new(&s, &cfg) == 0);
	mbt_session_disable_dht(s);
	mbt_session_disable_lsd(s);
	snprintf(want, sizeof(want), "port=%hu&",
	         (unsigned short)(mbt_session_listen_port(s) + 1000));

	http_calls = 0;
	assert(mbt_session_add_torrent_buf(s, enc, len, NULL, NULL) == 0);
	free(enc);
	time_t end = time(NULL) + 5;
	while (time(NULL) < end && !strstr(last_url, "&ip=203.0.113.42&"))
		mbt_ev_step(ev, 10);
	assert(strstr(last_url, "&ip=203.0.113.42&"));
	assert(strstr(last_url, want));

	/* A refresh with the same mapping does not announce again. */
	int calls = http_calls, maps = pcp_calls;
	end = time(NULL) + 4;
	while (time(NULL) < end && pcp_calls == maps)
		mbt_ev_step(ev, 10);
	assert(pcp_calls > maps);
	for (int i = 0; i < 50; i++)
		mbt_ev_step(ev, 10);
	assert(http_calls == calls);

	mbt_session_free(s);
	mbt_storage_memory_destroy(mem);
	mbt_ev_io_free(io);
	close(fd);
	snprintf(path, sizeof(path), "rm -rf %s", tmpdir);
	assert(system(path) == 0);
}

int
main(void)
{
	struct mbt_infohash *ih;

	assert(mbt_ev_new_default(&ev) == 0);
	assert(mbt_infohash_new(&ih, MBT_INFOHASH_SHA1, HASH) == 0);
	test_http(ih);
	test_udp(ih);
	mbt_infohash_unref(ih);
	test_session();
	mbt_ev_destroy(ev);
	puts("tracker_ip: ok");
	return 0;
}
