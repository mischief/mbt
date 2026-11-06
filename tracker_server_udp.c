// SPDX-License-Identifier: ISC
/*
 * tracker_server_udp.c — BEP 15 UDP tracker server
 *
 * Handles connect, announce, and scrape requests.
 * Stores peers per infohash in a simple hash table.
 */
#include <sys/socket.h>
#include <netinet/in.h>
#include <arpa/inet.h>
#include <stdlib.h>
#include <string.h>
#include <errno.h>
#include <stdint.h>
#include <sys/types.h>

#include "mbt/event.h"
#include "mbt/tracker.h"
#include "mbt/tracker_server.h"
#include "mbt/log.h"
#include "net.h"
#include "banned.h"

struct mbt_ev;
struct mbt_ev_io;
struct mbt_tracker_store;

/* BEP 15 protocol constants */
#define ACTION_CONNECT  0
#define ACTION_ANNOUNCE 1
#define ACTION_SCRAPE   2
#define ACTION_ERROR    3

#define CONNECT_MAGIC 0x41727101980ULL

/* Peer entry */
struct mbt_tracker_server {
	struct mbt_ev *ev;
	struct mbt_listener listener;
	uint16_t port;
	struct mbt_tracker_store *store;
	int owns_store;
	struct mbt_ev_timer *gc_timer;
};

/* --- Hash table helpers --- */

#include "tracker_store.h"


static void
tracker_recv(struct mbt_ev_io *io, int fd, uint32_t events, void *v)
{
	struct mbt_tracker_server *ts = v;
	uint8_t buf[2048];
	struct sockaddr_storage from;
	socklen_t fromlen = sizeof(from);
	(void)io; (void)events;

	ssize_t n = recvfrom(fd, buf, sizeof(buf), 0, (struct sockaddr *)&from, &fromlen);
	if (n < 16) return;

	uint32_t action, transaction_id;
	uint64_t connection_id;
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
		uint64_t cid = __builtin_bswap64(mbt_tracker_store_make_connection_id(ts->store, &from));
		memcpy(resp, &a, 4);
		memcpy(resp + 4, &tid, 4);
		memcpy(resp + 8, &cid, 8);
		resp_len = 16;

	} else if (action == ACTION_ANNOUNCE && n >= 98) {
		/* Announce request */
		if (!mbt_tracker_store_validate_connection_id(ts->store, &from, connection_id))
			return;

		uint8_t *ih = buf + 16;
		/* bytes 36-55: peer_id (ignored) */
		/* bytes 56-63: downloaded */
		/* bytes 64-71: left */
		uint64_t left;
		memcpy(&left, buf + 64, 8);
		left = __builtin_bswap64(left);
		/* bytes 72-79: uploaded */
		uint32_t event;
		memcpy(&event, buf + 80, 4);
		event = ntohl(event);
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

		struct sockaddr_storage peer_addr = from;
		if (peer_port) {
			if (peer_addr.ss_family == AF_INET6)
				((struct sockaddr_in6 *)&peer_addr)->sin6_port = peer_port;
			else
				((struct sockaddr_in *)&peer_addr)->sin_port = peer_port;
		}

		struct ts_torrent *t = mbt_tracker_store_get_or_create(ts->store, ih);
		if (!t) return;

		int is_seeder = (left == 0);
		if (event == MBT_TRACKER_EVENT_STOPPED)
			mbt_tracker_store_remove_peer(ts->store, t, &peer_addr);
		else
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

		/* Append peers — BEP 7: v6 clients get 18-byte entries */
		if (from.ss_family == AF_INET6) {
			int sent = mbt_tracker_store_get_peers_v6(t, &peer_addr, resp + resp_len, num_want);
			resp_len += sent * 18;
		} else {
			int sent = mbt_tracker_store_get_peers_v4(t, &peer_addr, resp + resp_len, num_want);
			resp_len += sent * 6;
		}

	} else if (action == ACTION_SCRAPE && n >= 36) {
		/* Scrape */
		if (!mbt_tracker_store_validate_connection_id(ts->store, &from, connection_id))
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
			uint32_t c = htonl(t ? mbt_tracker_store_torrent_downloaded(t) : 0);
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

static void
tracker_gc_cb(struct mbt_ev_timer *timer, void *arg)
{
	struct mbt_tracker_server *ts = arg;

	mbt_tracker_store_gc(ts->store);
	mbt_ev_timer_mod(timer, mbt_tracker_store_gc_usec(ts->store));
}

int
mbt_tracker_server_new(struct mbt_tracker_server **out, struct mbt_ev *ev, uint16_t port, struct mbt_tracker_store *store)
{
	struct mbt_tracker_server *ts = calloc(1, sizeof(*ts));
	if (!ts) return -ENOMEM;

	if (store) { ts->store = store; ts->owns_store = 0; } else { mbt_tracker_store_new(&ts->store); ts->owns_store = 1; }

	ts->ev = ev;
	ts->port = port;

	int rv = mbt_listen_udp(&ts->listener, ev, port, tracker_recv, ts);
	if (rv < 0) { free(ts); return rv; }
	ts->port = ts->listener.port;
	if ((rv = mbt_ev_timer_new(ev, &ts->gc_timer, mbt_tracker_store_gc_usec(ts->store),
	                           tracker_gc_cb, ts)) < 0) {
		mbt_listener_close(&ts->listener);
		free(ts);
		return rv;
	}

	mbt_log_info(MBT_LOG_SESSION, "tracker server listening on UDP port %hu", port);
	*out = ts;
	return 0;
}

uint16_t
mbt_tracker_server_port(struct mbt_tracker_server *ts)
{
	return ts ? ts->port : 0;
}

void
mbt_tracker_server_free(struct mbt_tracker_server *ts)
{
	if (!ts) return;
	if (ts->gc_timer)
		mbt_ev_timer_free(ts->gc_timer);
	mbt_listener_close(&ts->listener);
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
