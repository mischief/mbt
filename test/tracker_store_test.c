// SPDX-License-Identifier: ISC
/* Store limits and counts, through the UDP server. */
#include <arpa/inet.h>
#include <assert.h>
#include <netinet/in.h>
#include <stdio.h>
#include <string.h>
#include <sys/socket.h>
#include <unistd.h>

#include "mbt/event.h"
#include "mbt/tracker_server.h"

#define MAX_SWARMS 4200          /* above the default limit */
#define NUM_SWARMS (MAX_SWARMS + 20)
#define PEERS_PER_TORRENT 200
#define PEERS_TRIED 250
#define INTERVAL 777
#define ANNOUNCE_LEN 98
#define ACTION_ANNOUNCE 1

static struct mbt_ev *ev;
static int sock;
static uint8_t cid[8];

/* Returns 1 with a reply in resp, or 0 with no reply. */
static int
exchange(const void *req, size_t len, uint8_t *resp, size_t want)
{
	assert(send(sock, req, len, 0) == (ssize_t)len);
	for (int i = 0; i < 200; i++) {
		ssize_t n;

		mbt_ev_step(ev, 1);
		n = recv(sock, resp, want, MSG_DONTWAIT);
		if (n > 0)
			return n >= 16;
	}
	return 0;
}

static int
announce(int swarm, uint16_t port, uint64_t left, uint32_t *interval)
{
	uint8_t req[ANNOUNCE_LEN] = {0}, resp[64];
	uint32_t action = htonl(ACTION_ANNOUNCE);
	uint16_t nport = htons(port);
	uint32_t s = htonl((uint32_t)swarm), v;

	memcpy(req, cid, 8);
	memcpy(req + 8, &action, 4);
	memcpy(req + 16, &s, 4);
	for (int i = 0; i < 8; i++)
		req[64 + i] = (uint8_t)(left >> (56 - i * 8));
	memcpy(req + 96, &nport, 2);
	if (!exchange(req, sizeof(req), resp, sizeof(resp)))
		return 0;
	if (interval) {
		memcpy(&v, resp + 8, 4);
		*interval = ntohl(v);
	}
	return 1;
}

static void
stats_are(struct mbt_tracker_store *st, uint32_t torrents, uint32_t seeders, uint32_t leechers)
{
	struct mbt_tracker_stats s;

	mbt_tracker_store_stats(st, &s);
	assert(s.torrents == torrents);
	assert(s.seeders == seeders);
	assert(s.leechers == leechers);
	assert(s.peers == seeders + leechers);
}

int
main(void)
{
	struct mbt_tracker_store *st;
	struct mbt_tracker_server *ts;
	uint8_t req[16] = {0}, resp[16];
	uint32_t interval = 0;
	int answered = 0;

	assert(mbt_ev_new_default(&ev) == 0);
	assert(mbt_tracker_store_new(&st) == 0);
	mbt_tracker_store_set_max_torrents(st, MAX_SWARMS);
	mbt_tracker_store_set_interval(st, INTERVAL);
	assert(mbt_tracker_server_new(&ts, ev, 0, st) == 0);

	struct sockaddr_in sa = {.sin_family = AF_INET, .sin_port = htons(mbt_tracker_server_port(ts)),
	    .sin_addr.s_addr = htonl(INADDR_LOOPBACK)};

	sock = socket(AF_INET, SOCK_DGRAM, 0);
	assert(sock >= 0);
	assert(connect(sock, (struct sockaddr *)&sa, sizeof(sa)) == 0);
	memcpy(req, "\x00\x00\x04\x17\x27\x10\x19\x80", 8);
	assert(exchange(req, 16, resp, 16));
	memcpy(cid, resp + 8, 8);

	/* The interval comes from the store, and the limit holds. */
	assert(announce(0, 6000, 1, &interval));
	assert(interval == INTERVAL);
	for (int i = 1; i < NUM_SWARMS; i++)
		answered += announce(i, 6000, 1, NULL);
	assert(answered == MAX_SWARMS - 1);
	stats_are(st, MAX_SWARMS, 0, MAX_SWARMS);

	/* A leecher that finishes moves between the running totals. */
	assert(announce(0, 6000, 0, NULL));
	stats_are(st, MAX_SWARMS, 1, MAX_SWARMS - 1);

	/* A swarm grows to its peer cap and stops there. */
	for (int i = 1; i < PEERS_TRIED; i++)
		assert(announce(1, (uint16_t)(6000 + i), 1, NULL));
	stats_are(st, MAX_SWARMS, 1, MAX_SWARMS - 1 + PEERS_PER_TORRENT - 1);

	/* Silent peers go, and the counts reach zero. */
	mbt_tracker_store_set_expire(st, 1);
	sleep(3);
	mbt_tracker_store_gc(st);
	stats_are(st, 0, 0, 0);

	close(sock);
	mbt_tracker_server_free(ts);
	mbt_tracker_store_free(st);
	mbt_ev_destroy(ev);
	puts("tracker_store: ok");
	return 0;
}
