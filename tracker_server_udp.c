/*
 * tracker_server_udp.c — BEP 15 UDP tracker server
 *
 * Handles connect, announce, and scrape requests.
 * Stores peers per infohash in a simple hash table.
 */
#define _GNU_SOURCE
#include <sys/socket.h>
#include <netinet/in.h>
#include <arpa/inet.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>
#include <errno.h>
#include <time.h>

#include "mbt/tracker_server.h"
#include "mbt/event.h"
#include "mbt/log.h"

/* BEP 15 protocol constants */
#define ACTION_CONNECT  0
#define ACTION_ANNOUNCE 1
#define ACTION_SCRAPE   2
#define ACTION_ERROR    3

#define CONNECT_MAGIC 0x41727101980ULL

/* Peer entry */
struct ts_peer {
	struct sockaddr_in addr;
	time_t last_seen;
};

/* Per-torrent peer list */
#define MAX_PEERS_PER_TORRENT 200
struct ts_torrent {
	uint8_t infohash[20];
	struct ts_peer peers[MAX_PEERS_PER_TORRENT];
	int peer_count;
	int seeders;
	int leechers;
	struct ts_torrent *next;
};

/* Tracker server */
#define TORRENT_BUCKETS 256
struct mbt_tracker_server {
	struct mbt_ev *ev;
	int fd;
	struct mbt_ev_io *io;
	uint16_t port;
	uint64_t connection_id; /* simple: same for all clients */
	struct ts_torrent *torrents[TORRENT_BUCKETS];
	int num_torrents;
	int num_peers;
};

/* --- Hash table helpers --- */

static unsigned
hash_infohash(const uint8_t *ih)
{
	unsigned h = 0;
	for (int i = 0; i < 20; i++)
		h = h * 31 + ih[i];
	return h % TORRENT_BUCKETS;
}

static struct ts_torrent *
find_torrent(struct mbt_tracker_server *ts, const uint8_t *ih)
{
	unsigned bucket = hash_infohash(ih);
	struct ts_torrent *t = ts->torrents[bucket];
	while (t) {
		if (memcmp(t->infohash, ih, 20) == 0)
			return t;
		t = t->next;
	}
	return NULL;
}

static struct ts_torrent *
get_or_create_torrent(struct mbt_tracker_server *ts, const uint8_t *ih)
{
	struct ts_torrent *t = find_torrent(ts, ih);
	if (t) return t;

	t = calloc(1, sizeof(*t));
	if (!t) return NULL;
	memcpy(t->infohash, ih, 20);

	unsigned bucket = hash_infohash(ih);
	t->next = ts->torrents[bucket];
	ts->torrents[bucket] = t;
	ts->num_torrents++;
	return t;
}

static void
torrent_add_peer(struct mbt_tracker_server *ts, struct ts_torrent *t,
                 const struct sockaddr_in *addr, int is_seeder)
{
	/* Update existing */
	for (int i = 0; i < t->peer_count; i++) {
		if (t->peers[i].addr.sin_addr.s_addr == addr->sin_addr.s_addr &&
		    t->peers[i].addr.sin_port == addr->sin_port) {
			t->peers[i].last_seen = time(NULL);
			return;
		}
	}

	/* Add new */
	if (t->peer_count >= MAX_PEERS_PER_TORRENT)
		return;

	t->peers[t->peer_count].addr = *addr;
	t->peers[t->peer_count].last_seen = time(NULL);
	t->peer_count++;
	ts->num_peers++;

	if (is_seeder) t->seeders++;
	else t->leechers++;
}

/* --- UDP protocol handling --- */

static void
tracker_recv(struct mbt_ev_io *io, int fd, uint32_t events, void *v)
{
	struct mbt_tracker_server *ts = v;
	uint8_t buf[2048];
	struct sockaddr_in from;
	socklen_t fromlen = sizeof(from);
	(void)io; (void)events;

	ssize_t n = recvfrom(fd, buf, sizeof(buf), 0, (struct sockaddr *)&from, &fromlen);
	if (n < 16) return;

	uint64_t connection_id;
	uint32_t action, transaction_id;
	memcpy(&connection_id, buf, 8);
	memcpy(&action, buf + 8, 4);
	memcpy(&transaction_id, buf + 12, 4);
	connection_id = __builtin_bswap64(connection_id);
	action = ntohl(action);
	transaction_id = ntohl(transaction_id);

	uint8_t resp[1500];
	ssize_t resp_len = 0;

	if (action == ACTION_CONNECT) {
		/* Connect request: return connection_id */
		uint32_t a = htonl(ACTION_CONNECT);
		uint32_t tid = htonl(transaction_id);
		uint64_t cid = __builtin_bswap64(ts->connection_id);
		memcpy(resp, &a, 4);
		memcpy(resp + 4, &tid, 4);
		memcpy(resp + 8, &cid, 8);
		resp_len = 16;

	} else if (action == ACTION_ANNOUNCE && n >= 98) {
		/* Announce request */
		if (connection_id != ts->connection_id)
			return;

		uint8_t *ih = buf + 16;
		/* bytes 36-55: peer_id (ignored) */
		/* bytes 56-63: downloaded */
		/* bytes 64-71: left */
		uint64_t left;
		memcpy(&left, buf + 64, 8);
		left = __builtin_bswap64(left);
		/* bytes 72-75: event */
		/* bytes 76-79: IP (0 = use source) */
		/* bytes 80-83: key */
		/* bytes 84-87: num_want */
		int32_t num_want;
		memcpy(&num_want, buf + 84, 4);
		num_want = (int32_t)ntohl(num_want);
		if (num_want <= 0 || num_want > 50) num_want = 50;
		/* bytes 88-89: port */
		uint16_t peer_port;
		memcpy(&peer_port, buf + 88, 2);

		struct sockaddr_in peer_addr = from;
		if (peer_port) peer_addr.sin_port = peer_port; /* already network order */

		struct ts_torrent *t = get_or_create_torrent(ts, ih);
		if (!t) return;

		int is_seeder = (left == 0);
		torrent_add_peer(ts, t, &peer_addr, is_seeder);

		/* Build response */
		uint32_t a = htonl(ACTION_ANNOUNCE);
		uint32_t tid = htonl(transaction_id);
		uint32_t interval = htonl(120);
		uint32_t leechers = htonl(t->leechers);
		uint32_t seeders = htonl(t->seeders);
		memcpy(resp, &a, 4);
		memcpy(resp + 4, &tid, 4);
		memcpy(resp + 8, &interval, 4);
		memcpy(resp + 12, &leechers, 4);
		memcpy(resp + 16, &seeders, 4);
		resp_len = 20;

		/* Append peers (compact: 6 bytes each) */
		int sent = 0;
		for (int i = 0; i < t->peer_count && sent < num_want; i++) {
			/* Don't return the requester to themselves */
			if (t->peers[i].addr.sin_addr.s_addr == from.sin_addr.s_addr &&
			    t->peers[i].addr.sin_port == peer_addr.sin_port)
				continue;
			memcpy(resp + resp_len, &t->peers[i].addr.sin_addr, 4);
			memcpy(resp + resp_len + 4, &t->peers[i].addr.sin_port, 2);
			resp_len += 6;
			sent++;
		}

	} else if (action == ACTION_SCRAPE && n >= 36) {
		/* Scrape */
		if (connection_id != ts->connection_id)
			return;

		uint32_t a = htonl(ACTION_SCRAPE);
		uint32_t tid = htonl(transaction_id);
		memcpy(resp, &a, 4);
		memcpy(resp + 4, &tid, 4);
		resp_len = 8;

		int num_hashes = (n - 16) / 20;
		for (int i = 0; i < num_hashes && resp_len + 12 <= (ssize_t)sizeof(resp); i++) {
			uint8_t *ih = buf + 16 + i * 20;
			struct ts_torrent *t = find_torrent(ts, ih);
			uint32_t s = htonl(t ? t->seeders : 0);
			uint32_t c = htonl(t ? t->peer_count : 0);
			uint32_t l = htonl(t ? t->leechers : 0);
			memcpy(resp + resp_len, &s, 4);
			memcpy(resp + resp_len + 4, &c, 4);
			memcpy(resp + resp_len + 8, &l, 4);
			resp_len += 12;
		}
	}

	if (resp_len > 0)
		sendto(fd, resp, resp_len, 0, (struct sockaddr *)&from, fromlen);
}

/* --- Public API --- */

int
mbt_tracker_server_new(struct mbt_tracker_server **out, struct mbt_ev *ev, uint16_t port)
{
	struct mbt_tracker_server *ts = calloc(1, sizeof(*ts));
	if (!ts) return -ENOMEM;

	ts->ev = ev;
	ts->port = port;
	ts->connection_id = 0xDEADBEEF01020304ULL; /* fixed for simplicity */

	ts->fd = socket(AF_INET, SOCK_DGRAM | SOCK_NONBLOCK | SOCK_CLOEXEC, 0);
	if (ts->fd < 0) { free(ts); return -errno; }

	struct sockaddr_in sa = {
		.sin_family = AF_INET,
		.sin_port = htons(port),
		.sin_addr.s_addr = INADDR_ANY,
	};
	if (bind(ts->fd, (struct sockaddr *)&sa, sizeof(sa)) < 0) {
		int e = errno; close(ts->fd); free(ts); return -e;
	}

	int rv = mbt_ev_io_new(ev, &ts->io, ts->fd, MBT_EV_READ, tracker_recv, ts);
	if (rv < 0) { close(ts->fd); free(ts); return rv; }

	mbt_log_info(MBT_LOG_SESSION, "tracker server listening on UDP port %hu", port);
	*out = ts;
	return 0;
}

void
mbt_tracker_server_free(struct mbt_tracker_server *ts)
{
	if (!ts) return;
	if (ts->io) mbt_ev_io_free(ts->io);
	if (ts->fd >= 0) close(ts->fd);
	for (int i = 0; i < TORRENT_BUCKETS; i++) {
		struct ts_torrent *t = ts->torrents[i];
		while (t) {
			struct ts_torrent *next = t->next;
			free(t);
			t = next;
		}
	}
	free(ts);
}

int
mbt_tracker_server_num_torrents(struct mbt_tracker_server *ts)
{
	return ts->num_torrents;
}

int
mbt_tracker_server_num_peers(struct mbt_tracker_server *ts)
{
	return ts->num_peers;
}
