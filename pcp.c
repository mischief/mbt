// SPDX-License-Identifier: ISC
/*
 * pcp.c — PCP client (RFC 6887) for NAT port mapping
 *
 * Sends MAP requests to the gateway, handles retransmit + renewal.
 * Supports IPv4 and IPv6. Single mapping per instance.
 */
#define _GNU_SOURCE
#include <sys/socket.h>
#include <netinet/in.h>
#include <arpa/inet.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>
#include <errno.h>
#include <stdio.h>
#include <net/if.h>

#include "mbt/pcp.h"
#include "mbt/event.h"
#include "mbt/log.h"
#include "internal.h"

#define PCP_PORT 5351
#define PCP_VERSION 2
#define PCP_OPCODE_MAP 1
#define PCP_RESULT_SUCCESS 0

struct mbt_pcp {
	struct mbt_ev *ev;
	int fd;
	struct mbt_ev_io *io;
	struct mbt_ev_timer *timer;
	struct sockaddr_storage gateway;

	/* Mapping state */
	uint8_t protocol;
	uint16_t internal_port;
	uint32_t requested_lifetime;
	uint8_t nonce[12];
	uint8_t client_addr[16]; /* IPv4-mapped or IPv6 */
	int is_ipv6;

	/* Assigned */
	char external_addr[INET6_ADDRSTRLEN];
	uint16_t external_port;
	uint32_t assigned_lifetime;

	/* Retransmit state */
	uint64_t rt_usec; /* current retransmit timeout */
	int awaiting_response;

	mbt_pcp_cb cb;
	void *cb_arg;
};

/* Encode client address as IPv4-mapped IPv6 (::ffff:a.b.c.d) */
static void
encode_client_addr(struct mbt_pcp *p)
{
	memset(p->client_addr, 0, 16);
	if (p->is_ipv6) {
		struct sockaddr_in6 local;
		socklen_t len = sizeof(local);
		getsockname(p->fd, (struct sockaddr *)&local, &len);
		memcpy(p->client_addr, &local.sin6_addr, 16);
	} else {
		struct sockaddr_in local;
		socklen_t len = sizeof(local);
		getsockname(p->fd, (struct sockaddr *)&local, &len);
		/* IPv4-mapped: ::ffff:x.x.x.x */
		p->client_addr[10] = 0xff;
		p->client_addr[11] = 0xff;
		memcpy(p->client_addr + 12, &local.sin_addr, 4);
	}
}

static int
send_map_request(struct mbt_pcp *p)
{
	uint8_t pkt[60];
	memset(pkt, 0, sizeof(pkt));

	/* Common header (24 bytes) */
	pkt[0] = PCP_VERSION;
	pkt[1] = PCP_OPCODE_MAP; /* R=0, Opcode=1 */
	/* pkt[2..3] reserved */
	uint32_t lt = htonl(p->requested_lifetime);
	memcpy(pkt + 4, &lt, 4);
	memcpy(pkt + 8, p->client_addr, 16);

	/* MAP opcode data (36 bytes at offset 24) */
	memcpy(pkt + 24, p->nonce, 12);          /* Mapping Nonce */
	pkt[36] = p->protocol;                     /* Protocol */
	/* pkt[37..39] reserved */
	uint16_t iport = htons(p->internal_port);
	memcpy(pkt + 40, &iport, 2);              /* Internal Port */
	uint16_t eport = htons(p->external_port);  /* Suggested External Port */
	memcpy(pkt + 42, &eport, 2);
	/* pkt[44..59] Suggested External IP (zeros = any) */
	if (p->external_addr[0]) {
		uint8_t addr[16] = {0};
		if (inet_pton(AF_INET, p->external_addr, addr + 12) > 0) {
			addr[10] = 0xff; addr[11] = 0xff;
			memcpy(pkt + 44, addr, 16);
		} else {
			inet_pton(AF_INET6, p->external_addr, addr);
			memcpy(pkt + 44, addr, 16);
		}
	}

	ssize_t rv = sendto(p->fd, pkt, 60, 0,
	                    (struct sockaddr *)&p->gateway,
	                    p->is_ipv6 ? sizeof(struct sockaddr_in6) : sizeof(struct sockaddr_in));
	return rv < 0 ? -errno : 0;
}

static void
pcp_recv(struct mbt_ev_io *io, int fd, uint32_t events, void *v)
{
	struct mbt_pcp *p = v;
	uint8_t pkt[256];
	(void)io; (void)events;

	ssize_t n = recv(fd, pkt, sizeof(pkt), 0);
	if (n < 60) return;

	/* Validate response */
	if (pkt[0] != PCP_VERSION) return;
	if (!(pkt[1] & 0x80)) return; /* R bit must be 1 */
	if ((pkt[1] & 0x7f) != PCP_OPCODE_MAP) return;

	uint8_t result = pkt[3];
	uint32_t lifetime;
	memcpy(&lifetime, pkt + 4, 4);
	lifetime = ntohl(lifetime);

	/* Validate nonce */
	if (memcmp(pkt + 24, p->nonce, 12) != 0) return;

	if (result != PCP_RESULT_SUCCESS) {
		mbt_log_warn(MBT_LOG_SESSION, "PCP MAP failed: result=%d", result);
		return;
	}

	/* Extract assigned external port and address */
	uint16_t aport;
	memcpy(&aport, pkt + 42, 2);
	p->external_port = ntohs(aport);
	p->assigned_lifetime = lifetime;

	/* Decode external IP (offset 44, 128 bits) */
	uint8_t *eip = pkt + 44;
	if (eip[0] == 0 && eip[1] == 0 && eip[10] == 0xff && eip[11] == 0xff) {
		/* IPv4-mapped */
		inet_ntop(AF_INET, eip + 12, p->external_addr, sizeof(p->external_addr));
	} else {
		inet_ntop(AF_INET6, eip, p->external_addr, sizeof(p->external_addr));
	}

	p->awaiting_response = 0;

	mbt_log_info(MBT_LOG_SESSION, "PCP: mapped %s:%hu (lifetime %us)",
	             p->external_addr, p->external_port, lifetime);

	if (p->cb)
		p->cb(p->external_addr, p->external_port, lifetime, p->cb_arg);

	/* Schedule renewal at 1/2 to 5/8 of lifetime */
	uint64_t renew_usec = ((uint64_t)lifetime * 1000000ULL) / 2 +
	                       (mbt_random_uniform(lifetime * 1000000ULL / 8));
	mbt_ev_timer_mod(p->timer, renew_usec);
}

static void
pcp_timer_cb(struct mbt_ev_timer *timer, void *v)
{
	struct mbt_pcp *p = v;

	send_map_request(p);

	if (p->awaiting_response) {
		/* Retransmit: exponential backoff */
		p->rt_usec = p->rt_usec * 2;
		if (p->rt_usec > 1024 * 1000000ULL)
			p->rt_usec = 1024 * 1000000ULL;
	} else {
		/* First send or renewal */
		p->rt_usec = 3 * 1000000ULL; /* IRT = 3s */
		p->awaiting_response = 1;
	}

	/* Add jitter ±10% */
	uint64_t jitter = p->rt_usec / 10;
	uint64_t next = p->rt_usec + mbt_random_uniform(jitter * 2) - jitter;
	mbt_ev_timer_mod(timer, next);
}

/* --- Public API --- */

int
mbt_pcp_new(struct mbt_pcp **out, struct mbt_ev *ev, const char *gateway)
{
	struct mbt_pcp *p = calloc(1, sizeof(*p));
	if (!p) return -ENOMEM;
	p->ev = ev;

	/* Determine gateway address */
	if (gateway && inet_pton(AF_INET6, gateway, &((struct sockaddr_in6 *)&p->gateway)->sin6_addr) > 0) {
		p->is_ipv6 = 1;
		p->gateway.ss_family = AF_INET6;
		((struct sockaddr_in6 *)&p->gateway)->sin6_port = htons(PCP_PORT);
	} else if (gateway && inet_pton(AF_INET, gateway, &((struct sockaddr_in *)&p->gateway)->sin_addr) > 0) {
		p->gateway.ss_family = AF_INET;
		((struct sockaddr_in *)&p->gateway)->sin_port = htons(PCP_PORT);
	} else {
		/* Default: try to use default gateway (common: x.x.x.1) */
		/* Fallback: use 192.168.0.1 */
		p->gateway.ss_family = AF_INET;
		((struct sockaddr_in *)&p->gateway)->sin_port = htons(PCP_PORT);
		inet_pton(AF_INET, "192.168.0.1", &((struct sockaddr_in *)&p->gateway)->sin_addr);
	}

	int af = p->is_ipv6 ? AF_INET6 : AF_INET;
	p->fd = socket(af, SOCK_DGRAM | SOCK_NONBLOCK | SOCK_CLOEXEC, 0);
	if (p->fd < 0) { free(p); return -errno; }

	/* Connect to gateway so getsockname works */
	connect(p->fd, (struct sockaddr *)&p->gateway,
	        p->is_ipv6 ? sizeof(struct sockaddr_in6) : sizeof(struct sockaddr_in));
	encode_client_addr(p);

	int rv = mbt_ev_io_new(ev, &p->io, p->fd, MBT_EV_READ, pcp_recv, p);
	if (rv < 0) { close(p->fd); free(p); return rv; }

	/* Generate random nonce */
	getentropy(p->nonce, 12);

	*out = p;
	return 0;
}

void
mbt_pcp_free(struct mbt_pcp *p)
{
	if (!p) return;
	/* Send unmap (lifetime=0) */
	if (p->external_port) {
		p->requested_lifetime = 0;
		send_map_request(p);
	}
	if (p->timer) mbt_ev_timer_free(p->timer);
	if (p->io) mbt_ev_io_free(p->io);
	if (p->fd >= 0) close(p->fd);
	free(p);
}

int
mbt_pcp_map(struct mbt_pcp *p, uint8_t protocol, uint16_t internal_port,
            uint32_t lifetime, mbt_pcp_cb cb, void *arg)
{
	p->protocol = protocol;
	p->internal_port = internal_port;
	p->requested_lifetime = lifetime;
	p->cb = cb;
	p->cb_arg = arg;
	p->awaiting_response = 0;

	/* Start immediately */
	if (!p->timer)
		mbt_ev_timer_new(p->ev, &p->timer, 0, pcp_timer_cb, p);
	else
		mbt_ev_timer_mod(p->timer, 0);

	return 0;
}

int
mbt_pcp_unmap(struct mbt_pcp *p, uint8_t protocol, uint16_t internal_port)
{
	p->protocol = protocol;
	p->internal_port = internal_port;
	p->requested_lifetime = 0;
	send_map_request(p);
	p->external_port = 0;
	p->external_addr[0] = '\0';
	return 0;
}

const char *mbt_pcp_external_addr(struct mbt_pcp *p) { return p->external_addr[0] ? p->external_addr : NULL; }
uint16_t mbt_pcp_external_port(struct mbt_pcp *p) { return p->external_port; }
