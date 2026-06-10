// SPDX-License-Identifier: ISC
/*
 * test/pcp_test.c — PCP client test with stub server
 */
#include <assert.h>
#include <stdio.h>
#include <string.h>
#include <stdlib.h>
#include <sys/socket.h>
#include <netinet/in.h>
#include <arpa/inet.h>
#include <unistd.h>

#include "mbt/event.h"
#include "mbt/pcp.h"

static struct mbt_ev *ev;
static int mapping_received;
static int server_request_count;
static char got_addr[64];
static uint16_t got_port;

/* Stub PCP server: validates request format, responds with success */
static void
stub_server_cb(struct mbt_ev_io *io, int fd, uint32_t events, void *v)
{
	(void)io; (void)events; (void)v;
	uint8_t pkt[256];
	struct sockaddr_storage from;
	socklen_t fromlen = sizeof(from);

	ssize_t n = recvfrom(fd, pkt, sizeof(pkt), 0, (struct sockaddr *)&from, &fromlen);
	if (n < 60) return;

	/* Validate request */
	assert(pkt[0] == 2);           /* Version 2 */
	assert((pkt[1] & 0x80) == 0);  /* R=0 (request) */
	assert((pkt[1] & 0x7f) == 1);  /* Opcode=MAP */
	assert(pkt[36] == 6 || pkt[36] == 17); /* TCP or UDP */

	uint16_t iport;
	memcpy(&iport, pkt + 40, 2);
	iport = ntohs(iport);
	assert(iport > 0);

	/* Build response */
	uint8_t resp[60];
	memcpy(resp, pkt, 60); /* Copy request as base */

	resp[1] |= 0x80; /* R=1 (response) */
	resp[3] = 0;      /* Result = SUCCESS */

	server_request_count++;

	/* Lifetime = 2s (short for refresh testing) */
	uint32_t lt = htonl(2);
	memcpy(resp + 4, &lt, 4);

	/* Epoch time */
	uint32_t epoch = htonl(12345);
	memcpy(resp + 8, &epoch, 4);
	memset(resp + 12, 0, 12); /* reserved */

	/* Assigned external port = internal port + 1000 */
	uint16_t eport = htons(iport + 1000);
	memcpy(resp + 42, &eport, 2);

	/* Assigned external IP = 203.0.113.42 (IPv4-mapped) */
	memset(resp + 44, 0, 16);
	resp[54] = 0xff; resp[55] = 0xff;
	uint8_t ext_ip[4]; inet_pton(AF_INET, "203.0.113.42", ext_ip);
	memcpy(resp + 56, ext_ip, 4);

	sendto(fd, resp, 60, 0, (struct sockaddr *)&from, fromlen);
}

static void
on_mapped(const char *addr, uint16_t port, uint32_t lifetime, void *arg)
{
	(void)arg;
	mapping_received++;
	snprintf(got_addr, sizeof(got_addr), "%s", addr);
	got_port = port;
	fprintf(stderr, "  mapped (#%d): %s:%hu (lifetime %u)\n", mapping_received, addr, port, lifetime);
	/* Stop after second mapping (proves refresh worked) */
	if (mapping_received >= 2)
		mbt_ev_stop(ev);
}

static void timeout_cb(struct mbt_ev_timer *t, void *a) {
	(void)t; (void)a;
	fprintf(stderr, "  TIMEOUT: mapping_received=%d server_requests=%d\n", mapping_received, server_request_count);
	mbt_ev_stop(ev);
}

int main(void)
{
	int rv;
	mbt_ev_new_default(&ev);

	/* Start stub PCP server on localhost:5351 */
	int sfd = socket(AF_INET, SOCK_DGRAM | SOCK_NONBLOCK | SOCK_CLOEXEC, 0);
	assert(sfd >= 0);
	struct sockaddr_in sa = { .sin_family = AF_INET, .sin_port = htons(5351) };
	inet_pton(AF_INET, "127.0.0.1", &sa.sin_addr);
	int yes = 1; setsockopt(sfd, SOL_SOCKET, SO_REUSEADDR, &yes, sizeof(yes));
	rv = bind(sfd, (struct sockaddr *)&sa, sizeof(sa));
	if (rv < 0) { perror("bind 5351"); return 77; } /* skip if port busy */

	struct mbt_ev_io *sio;
	mbt_ev_io_new(ev, &sio, sfd, MBT_EV_READ, stub_server_cb, NULL);

	/* Create PCP client pointing at localhost */
	struct mbt_pcp *pcp;
	rv = mbt_pcp_new(&pcp, ev, "127.0.0.1");
	assert(rv == 0);

	/* Request TCP mapping for port 6881 */
	rv = mbt_pcp_map(pcp, 6 /* TCP */, 6881, 2, on_mapped, NULL);
	assert(rv == 0);

	/* Timeout */
	struct mbt_ev_timer *to;
	mbt_ev_timer_new(ev, &to, 5 * 1000000ULL, timeout_cb, NULL);

	mbt_ev_run(ev);

	assert(mapping_received >= 2);
	assert(server_request_count >= 2);
	assert(strcmp(got_addr, "203.0.113.42") == 0);
	assert(got_port == 7881); /* 6881 + 1000 */

	fprintf(stderr, "pcp_test: PASS (refresh verified, %d requests)\n", server_request_count);

	mbt_ev_timer_free(to);
	mbt_pcp_free(pcp);
	mbt_ev_io_free(sio);
	close(sfd);
	mbt_ev_destroy(ev);
	return 0;
}
