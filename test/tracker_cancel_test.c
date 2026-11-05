// SPDX-License-Identifier: ISC
/* A UDP tracker reply that arrives after its torrent or session has gone. */
#include <arpa/inet.h>
#include <assert.h>
#include <endian.h>
#include <netinet/in.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/socket.h>
#include <time.h>
#include <unistd.h>

#include "mbt/event.h"
#include "mbt/metainfo.h"
#include "mbt/session.h"
#include "mbt/storage.h"
#include "mbt/torrent.h"
#include "test_util.h"

#define WAIT_SEC 20
#define PIECE_LEN 16384
#define CONNECT_LEN 16
#define ANNOUNCE_LEN 98
#define REPLY_LEN 26
#define ACTION_ANNOUNCE 1
#define AFTER_STEPS 20

enum after { AFTER_REMOVE, AFTER_SESSION_FREE };

static struct mbt_ev *ev;

static void
recv_request(int fd, uint8_t *buf, size_t want, struct sockaddr_in *from)
{
	time_t end = time(NULL) + WAIT_SEC;

	for (;;) {
		socklen_t len = sizeof(*from);
		ssize_t n = recvfrom(fd, buf, want, MSG_DONTWAIT, (struct sockaddr *)from, &len);

		if (n == (ssize_t)want)
			return;
		assert(time(NULL) < end);
		test_ev_step(ev, 10);
	}
}

static void
run(enum after after)
{
	struct sockaddr_in sin = {.sin_family = AF_INET}, from;
	socklen_t len = sizeof(sin);
	struct mbt_torrent *t = NULL;
	struct mbt_metainfo *mi;
	struct mbt_session *s;
	unsigned char *enc;
	size_t enc_len;
	uint8_t buf[ANNOUNCE_LEN];
	char dir[] = "/tmp/mbt_cancel_XXXXXX", path[64], url[64], cmd[64];
	int fd;
	FILE *fp;

	assert(mkdtemp(dir));
	sin.sin_addr.s_addr = htonl(INADDR_LOOPBACK);
	assert((fd = socket(AF_INET, SOCK_DGRAM, 0)) >= 0);
	assert(bind(fd, (struct sockaddr *)&sin, sizeof(sin)) == 0);
	assert(getsockname(fd, (struct sockaddr *)&sin, &len) == 0);
	snprintf(url, sizeof(url), "udp://127.0.0.1:%hu/announce", ntohs(sin.sin_port));

	snprintf(path, sizeof(path), "%s/x.bin", dir);
	assert((fp = fopen(path, "wb")) != NULL);
	assert(fwrite("abcd", 1, 4, fp) == 4);
	fclose(fp);
	assert(mbt_metainfo_new(&mi) == 0);
	assert(mbt_metainfo_set_content(mi, path, PIECE_LEN) == 0);
	assert(mbt_metainfo_add_tracker(mi, url) == 0);
	assert(mbt_metainfo_encode(mi, &enc, &enc_len) == 0);
	mbt_metainfo_free(mi);

	struct mbt_storage *st = mbt_storage_file_new(dir);
	struct mbt_session_config cfg = {.ev = ev, .default_storage = st};
	assert(mbt_session_new(&s, &cfg) == 0);
	mbt_storage_unref(st);
	assert(mbt_session_add_torrent_buf(s, enc, enc_len, NULL, &t) == 0);

	recv_request(fd, buf, CONNECT_LEN, &from);
	uint8_t connect_reply[16] = {0};
	memcpy(connect_reply + 4, buf + 12, 4);
	assert(sendto(fd, connect_reply, sizeof(connect_reply), 0,
	    (struct sockaddr *)&from, sizeof(from)) == sizeof(connect_reply));

	recv_request(fd, buf, ANNOUNCE_LEN, &from);
	if (after == AFTER_REMOVE) {
		assert(mbt_session_remove_torrent(s, mbt_torrent_infohash(t)) == 0);
	} else {
		mbt_session_free(s);
		s = NULL;
	}

	uint8_t reply[REPLY_LEN] = {0};
	uint32_t action = htobe32(ACTION_ANNOUNCE);
	memcpy(reply, &action, 4);
	memcpy(reply + 4, buf + 12, 4);
	reply[20] = 127;
	reply[23] = 1;
	reply[24] = 0x1a;
	reply[25] = 0xe1;
	assert(sendto(fd, reply, sizeof(reply), 0, (struct sockaddr *)&from,
	    sizeof(from)) == sizeof(reply));
	for (int i = 0; i < AFTER_STEPS; i++)
		test_ev_step(ev, 1);

	if (s != NULL)
		mbt_session_free(s);
	close(fd);
	free(enc);
	snprintf(cmd, sizeof(cmd), "rm -rf %s", dir);
	assert(system(cmd) == 0);
}

int
main(void)
{
	assert(mbt_ev_new_default(&ev) == 0);
	run(AFTER_REMOVE);
	run(AFTER_SESSION_FREE);
	mbt_ev_destroy(ev);
	fprintf(stderr, "tracker_cancel_test: PASS\n");
	return 0;
}
