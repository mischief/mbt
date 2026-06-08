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
struct mbt_tracker_server {
	struct mbt_ev *ev;
	int fd;
	struct mbt_ev_io *io;
	uint16_t port;
	uint64_t connection_id;
	struct mbt_tracker_store *store;
	int owns_store;
};

/* --- Hash table helpers --- */

/* Forward declarations from tracker_store.c */
struct ts_torrent;
struct ts_torrent *mbt_tracker_store_get_or_create(struct mbt_tracker_store *s, const uint8_t *ih);
struct ts_torrent *mbt_tracker_store_find(struct mbt_tracker_store *s, const uint8_t *ih);
void mbt_tracker_store_add_peer(struct mbt_tracker_store *s, struct ts_torrent *t, const struct sockaddr_in *addr, int is_seeder);
int mbt_tracker_store_get_peers(struct ts_torrent *t, const struct sockaddr_in *exclude, uint8_t *compact_out, int max_peers);
int mbt_tracker_store_torrent_seeders(struct ts_torrent *t);
int mbt_tracker_store_torrent_leechers(struct ts_torrent *t);
int mbt_tracker_store_torrent_peer_count(struct ts_torrent *t);


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
		/* bytes 72-79: uploaded */
		/* bytes 80-83: event */
		/* bytes 84-87: IP (0 = use source) */
		/* bytes 88-91: key */
		/* bytes 92-95: num_want */
		int32_t num_want;
		memcpy(&num_want, buf + 92, 4);
		num_want = (int32_t)ntohl(num_want);
		if (num_want <= 0 || num_want > 50) num_want = 50;
		/* bytes 96-97: port */
		uint16_t peer_port;
		memcpy(&peer_port, buf + 96, 2);

		struct sockaddr_in peer_addr = from;
		if (peer_port) peer_addr.sin_port = peer_port; /* already network order */

		struct ts_torrent *t = mbt_tracker_store_get_or_create(ts->store, ih);
		if (!t) return;

		int is_seeder = (left == 0);
		mbt_tracker_store_add_peer(ts->store, t, &peer_addr, is_seeder);

		/* Build response */
		uint32_t a = htonl(ACTION_ANNOUNCE);
		uint32_t tid = htonl(transaction_id);
		uint32_t interval = htonl(120);
		uint32_t leechers = htonl(mbt_tracker_store_torrent_leechers(t));
		uint32_t seeders = htonl(mbt_tracker_store_torrent_seeders(t));
		memcpy(resp, &a, 4);
		memcpy(resp + 4, &tid, 4);
		memcpy(resp + 8, &interval, 4);
		memcpy(resp + 12, &leechers, 4);
		memcpy(resp + 16, &seeders, 4);
		resp_len = 20;

		/* Append peers (compact: 6 bytes each) */
		int sent = mbt_tracker_store_get_peers(t, &peer_addr, resp + resp_len, num_want);
		resp_len += sent * 6;

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
			struct ts_torrent *t = mbt_tracker_store_find(ts->store, ih);
			uint32_t s = htonl(t ? mbt_tracker_store_torrent_seeders(t) : 0);
			uint32_t c = htonl(t ? mbt_tracker_store_torrent_peer_count(t) : 0);
			uint32_t l = htonl(t ? mbt_tracker_store_torrent_leechers(t) : 0);
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
mbt_tracker_server_new(struct mbt_tracker_server **out, struct mbt_ev *ev, uint16_t port, struct mbt_tracker_store *store)
{
	struct mbt_tracker_server *ts = calloc(1, sizeof(*ts));
	if (!ts) return -ENOMEM;

	if (store) { ts->store = store; ts->owns_store = 0; } else { mbt_tracker_store_new(&ts->store); ts->owns_store = 1; }

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
	if (ts->store && ts->owns_store) mbt_tracker_store_free(ts->store);
	free(ts);
}

int
mbt_tracker_server_num_torrents(struct mbt_tracker_server *ts)
{
	return mbt_tracker_store_num_torrents(ts->store);
}

int
mbt_tracker_server_num_peers(struct mbt_tracker_server *ts)
{
	return mbt_tracker_store_num_peers(ts->store);
}
