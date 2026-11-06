// SPDX-License-Identifier: ISC
/* A choked peer that sends a request gets a reject, not a piece. */
#include <arpa/inet.h>
#include <assert.h>
#include <fcntl.h>
#include <netinet/in.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/socket.h>
#include <unistd.h>

#include "mbt/event.h"
#include "mbt/infohash.h"
#include "mbt/metainfo.h"
#include "mbt/session.h"
#include "mbt/storage.h"
#include "mbt/torrent.h"
#include "test_util.h"

#define PIECE_LEN 16384
#define BT_CHOKE 0
#define BT_UNCHOKE 1
#define BT_PIECE 7
#define BT_REJECT 16
#define FAST_BIT 0x04
#define WAIT_STEPS 500           /* 5 seconds at 10 ms */

static uint8_t data[PIECE_LEN];
static struct mbt_ev *ev;
static int choked, rejected, pieces;
static uint8_t rx[1 << 16];
static size_t rx_len;

/* Read what the session sent, and count messages. */
static void
pump(int fd)
{
	ssize_t n;

	mbt_ev_step(ev, 10);
	while ((n = recv(fd, rx + rx_len, sizeof(rx) - rx_len, MSG_DONTWAIT)) > 0)
		rx_len += (size_t)n;
	while (rx_len >= 4) {
		uint32_t len = (uint32_t)rx[0] << 24 | rx[1] << 16 | rx[2] << 8 | rx[3];

		if (rx_len < 4 + (size_t)len)
			return;
		if (len > 0) {
			if (rx[4] == BT_CHOKE)
				choked = 1;
			else if (rx[4] == BT_UNCHOKE)
				choked = 0;
			else if (rx[4] == BT_REJECT)
				rejected++;
			else if (rx[4] == BT_PIECE)
				pieces++;
		}
		memmove(rx, rx + 4 + len, rx_len - 4 - len);
		rx_len -= 4 + len;
	}
}

int
main(void)
{
	char path[] = "/tmp/mbt_choke_XXXXXX";
	struct mbt_storage_memory *m = mbt_storage_memory_new();
	struct mbt_session_config cfg = {.disable_utp = 1, .timers = {.choke_interval = 1},
	    .default_storage = mbt_storage_memory_storage(m)};
	struct mbt_session *s;
	struct mbt_torrent *t;
	struct mbt_metainfo *mi;
	struct sockaddr_in sa = {.sin_family = AF_INET};
	unsigned char *enc;
	size_t enc_len;
	uint8_t hs[68] = {19}, peer_hs[68];
	static const uint8_t request[17] = {0, 0, 0, 13, 6, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0x40, 0};
	int fd = mkstemp(path), got = 0;

	assert(fd >= 0 && m);
	memset(data, 7, sizeof(data));
	assert(write(fd, data, sizeof(data)) == (ssize_t)sizeof(data));
	close(fd);
	assert(mbt_metainfo_new(&mi) == 0);
	assert(mbt_metainfo_set_content(mi, path, PIECE_LEN) == 0);
	assert(mbt_metainfo_encode(mi, &enc, &enc_len) == 0);
	memcpy(hs + 28, mbt_metainfo_infohash(mi)->hash_sha1, 20);
	mbt_metainfo_free(mi);
	unlink(path);

	assert(mbt_ev_new_default(&ev) == 0);
	cfg.ev = ev;
	assert(mbt_session_new(&s, &cfg) == 0);
	assert(mbt_session_add_torrent_buf(s, enc, enc_len, NULL, &t) == 0);
	assert(mbt_storage_memory_put_file(m, 0, data, sizeof(data)) == 0);
	test_wait_verified(ev, t);

	memcpy(hs + 1, "BitTorrent protocol", 19);
	hs[27] = FAST_BIT;
	memset(hs + 48, 'x', 20);
	sa.sin_port = htons(mbt_session_listen_port(s));
	sa.sin_addr.s_addr = htonl(INADDR_LOOPBACK);
	fd = socket(AF_INET, SOCK_STREAM, 0);
	assert(fd >= 0 && connect(fd, (struct sockaddr *)&sa, sizeof(sa)) == 0);
	assert(fcntl(fd, F_SETFL, O_NONBLOCK) == 0);
	assert(send(fd, hs, sizeof(hs), 0) == (ssize_t)sizeof(hs));
	for (int i = 0; i < WAIT_STEPS && got < (int)sizeof(peer_hs); i++) {
		ssize_t n = recv(fd, peer_hs + got, sizeof(peer_hs) - (size_t)got, MSG_DONTWAIT);

		if (n > 0)
			got += (int)n;
		mbt_ev_step(ev, 10);
	}
	assert(got == (int)sizeof(peer_hs));
	/* BEP 6: a fast peer opens with its piece state. */
	static const uint8_t have_none[5] = {0, 0, 0, 1, 15};

	assert(send(fd, have_none, sizeof(have_none), 0) == (ssize_t)sizeof(have_none));

	/* Not interested, so the choke round chokes this peer. */
	for (int i = 0; i < WAIT_STEPS && !choked; i++)
		pump(fd);
	assert(choked);
	assert(send(fd, request, sizeof(request), 0) == (ssize_t)sizeof(request));
	for (int i = 0; i < WAIT_STEPS && !rejected; i++)
		pump(fd);
	assert(rejected == 1);
	assert(pieces == 0);
	assert(mbt_torrent_bytes_uploaded(t) == 0);

	close(fd);
	mbt_session_free(s);
	mbt_storage_memory_destroy(m);
	mbt_ev_destroy(ev);
	free(enc);
	puts("choke_request: ok");
	return 0;
}
