// SPDX-License-Identifier: ISC
/* Scrape over HTTP (BEP 48) and UDP (BEP 15) reports one swarm the same way. */
#include <arpa/inet.h>
#include <assert.h>
#include <netinet/in.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/socket.h>
#include <unistd.h>

#include "mbt/be.h"
#include "mbt/event.h"
#include "mbt/tracker_server.h"

#define STEPS 200                /* 2 seconds at 10 ms */
#define HASH_A "%00%AB%AB%AB%AB%AB%AB%AB%AB%AB%AB%AB%AB%AB%AB%AB%AB%AB%AB%AB"
#define HASH_B "%01%CD%CD%CD%CD%CD%CD%CD%CD%CD%CD%CD%CD%CD%CD%CD%CD%CD%CD%CD"

static struct mbt_ev *ev;
static struct mbt_tracker_http *th;
static struct mbt_tracker_server *us;
static struct mbt_tracker_store *store;
static char reply[8192];

/* Send a request line. Returns the HTTP body, with the head left in reply. */
static char *
http(const char *line, size_t *body_len)
{
	struct sockaddr_in sa = {.sin_family = AF_INET,
	    .sin_port = htons(mbt_tracker_http_port(th)),
	    .sin_addr.s_addr = htonl(INADDR_LOOPBACK)};
	char req[1024];
	int fd = socket(AF_INET, SOCK_STREAM, 0);
	size_t got = 0;

	assert(fd >= 0);
	assert(connect(fd, (struct sockaddr *)&sa, sizeof(sa)) == 0);
	snprintf(req, sizeof(req), "GET %s HTTP/1.0\r\n\r\n", line);
	assert(send(fd, req, strlen(req), 0) == (ssize_t)strlen(req));
	for (int i = 0; i < STEPS; i++) {
		ssize_t n;

		mbt_ev_step(ev, 10);
		n = recv(fd, reply + got, sizeof(reply) - 1 - got, MSG_DONTWAIT);
		if (n == 0)
			break;
		if (n > 0)
			got += (size_t)n;
	}
	close(fd);
	reply[got] = '\0';
	char *body = strstr(reply, "\r\n\r\n");

	assert(body);
	body += 4;
	*body_len = got - (size_t)(body - reply);
	return body;
}

static void
announce(const char *hash, int port, int left)
{
	char line[256];
	size_t len;

	snprintf(line, sizeof(line), "/announce?info_hash=%s&port=%d&left=%d", hash, port, left);
	http(line, &len);
}

static long long
field(struct mbt_be_value *d, const char *key)
{
	struct mbt_be_value *v = mbt_be_find(d, key);

	assert(v && v->type == BE_INTEGER);
	return v->integer;
}

static struct mbt_be_value *
scrape(const char *query)
{
	struct mbt_be_value *v;
	size_t len;
	char *body = http(query, &len);

	assert(strncmp(reply, "HTTP/1.0 200", 12) == 0);
	assert(mbt_be_value_parse((unsigned char *)body, len, &v) >= 0);
	return v;
}

static struct mbt_be_value *
swarm(struct mbt_be_value *resp, uint8_t first)
{
	struct mbt_be_value *files = mbt_be_find(resp, "files"), *v;

	assert(files && files->type == BE_DICTIONARY);
	TAILQ_FOREACH(v, &files->listhead, next)
		if (v->key_len == 20 && (uint8_t)v->key[0] == first)
			return v;
	return NULL;
}

static int
count_swarms(struct mbt_be_value *resp)
{
	struct mbt_be_value *v;
	int n = 0;

	TAILQ_FOREACH(v, &mbt_be_find(resp, "files")->listhead, next)
		n++;
	return n;
}

static void
check_failure(const char *query, const char *reason)
{
	struct mbt_be_value *v = scrape(query);

	assert(mbt_be_streq(mbt_be_find(v, "failure reason"), reason));
	mbt_be_unref(v);
}

static void
test_http(void)
{
	struct mbt_be_value *v, *s;
	size_t len;

	v = scrape("/scrape?info_hash=" HASH_A "&info_hash=" HASH_A "&info_hash=" HASH_B);
	assert(count_swarms(v) == 2);
	assert((s = swarm(v, 0x00)) != NULL);
	assert(field(s, "complete") == 2);
	assert(field(s, "incomplete") == 1);
	assert(field(s, "downloaded") == 1);
	assert((s = swarm(v, 0x01)) != NULL);
	assert(field(s, "complete") == 0);
	assert(field(s, "incomplete") == 0);
	assert(field(s, "downloaded") == 0);
	mbt_be_unref(v);
	/* The scrape must not create the swarm that it asked about. */
	assert(mbt_tracker_store_num_torrents(store) == 1);

	check_failure("/scrape", "missing info_hash");
	check_failure("/scrape?port=1", "missing info_hash");
	check_failure("/scrape?info_hash=short", "invalid info_hash");
	check_failure("/scrape?info_hash=%zz" HASH_A, "invalid info_hash");
	check_failure("/scrape?info_hash=" HASH_A "x", "invalid info_hash");

	http("/scraped?info_hash=" HASH_A, &len);
	assert(strncmp(reply, "HTTP/1.0 404", 12) == 0);
}

static void
udp_exchange(int fd, const void *req, size_t len, void *out, size_t want)
{
	assert(send(fd, req, len, 0) == (ssize_t)len);
	for (int i = 0; i < STEPS; i++) {
		mbt_ev_step(ev, 10);
		if (recv(fd, out, want, MSG_DONTWAIT) == (ssize_t)want)
			return;
	}
	assert(0);
}

static void
test_udp(void)
{
	struct sockaddr_in sa = {.sin_family = AF_INET,
	    .sin_port = htons(mbt_tracker_server_port(us)),
	    .sin_addr.s_addr = htonl(INADDR_LOOPBACK)};
	int fd = socket(AF_INET, SOCK_DGRAM, 0);
	uint8_t req[36] = {0}, resp[20];
	uint32_t v;

	assert(fd >= 0);
	assert(connect(fd, (struct sockaddr *)&sa, sizeof(sa)) == 0);
	/* BEP 15 protocol_id, then action 0. */
	memcpy(req, "\x00\x00\x04\x17\x27\x10\x19\x80", 8);
	udp_exchange(fd, req, 16, resp, 16);
	memcpy(req, resp + 8, 8);
	memset(req + 8, 0, 8);
	req[11] = 2;
	req[16] = 0x00;
	memset(req + 17, 0xab, 19);
	udp_exchange(fd, req, 36, resp, 20);
	memcpy(&v, resp + 8, 4);
	assert(ntohl(v) == 2);   /* seeders */
	memcpy(&v, resp + 12, 4);
	assert(ntohl(v) == 1);   /* completed */
	memcpy(&v, resp + 16, 4);
	assert(ntohl(v) == 1);   /* leechers */
	close(fd);
}

int
main(void)
{
	assert(mbt_ev_new_default(&ev) == 0);
	assert(mbt_tracker_store_new(&store) == 0);
	assert(mbt_tracker_http_new(&th, ev, 0, store) == 0);
	assert(mbt_tracker_server_new(&us, ev, 0, store) == 0);

	announce(HASH_A, 6000, 5);    /* a leecher that finishes */
	announce(HASH_A, 6000, 0);
	announce(HASH_A, 6000, 0);    /* a repeat counts once */
	announce(HASH_A, 6001, 0);    /* seeder from the start */
	announce(HASH_A, 6002, 9);    /* leecher */

	test_http();
	test_udp();

	mbt_tracker_server_free(us);
	mbt_tracker_http_free(th);
	mbt_tracker_store_free(store);
	mbt_ev_destroy(ev);
	puts("tracker_scrape: ok");
	return 0;
}
