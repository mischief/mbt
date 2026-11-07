// SPDX-License-Identifier: ISC
/*
 * LSD announces rotate through all torrents, one packet per tick, and
 * the rotation survives a list that shrinks below its cursor. Nodes with
 * the same torrents in the same order start at different places.
 */
#include <sys/queue.h>
#include <assert.h>
#include <fcntl.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>
#include <unistd.h>

#include "mbt/event.h"
#include "mbt/infohash.h"
#include "mbt/lsd.h"
#include "mbt/session.h"
#include "mbt/storage.h"

#define N 200
#define KEEP 10

static struct mbt_ev *ev;
static struct mbt_lsd *lsd;
static uint16_t port;
static char hashes[N][41];
static int seen[N], packets, max_len, burst, first_idx = -1;
static double last_packet, min_gap = 1e9;

static double
now(void)
{
	struct timespec ts;

	clock_gettime(CLOCK_MONOTONIC, &ts);
	return ts.tv_sec + ts.tv_nsec / 1e9;
}

static void
lsd_io_cb(struct mbt_ev_io *io, int fd, uint32_t events, void *v)
{
	struct mbt_lsd_frame *frame = NULL;
	struct mbt_lsd_frame_info *info = NULL;
	struct mbt_infohash *ih;
	char hex[41];
	double t = now();

	(void)io; (void)fd; (void)events; (void)v;
	if (mbt_lsd_recv(lsd, &frame) < 0)
		return;
	int len = frame->len;
	int rv = mbt_lsd_frame_parse(frame, &info);
	mbt_lsd_frame_freep(&frame);
	if (rv < 0)
		return;
	/* Only the session under test, and one copy of each packet. */
	if (info->port != port || !info->cookie || strchr(info->remote, ':')) {
		mbt_lsd_frame_info_freep(&info);
		return;
	}
	packets++;
	if (len > max_len)
		max_len = len;
	if (last_packet > 0 && t - last_packet < min_gap)
		min_gap = t - last_packet;
	last_packet = t;
	STAILQ_FOREACH(ih, &info->infohashes, next) {
		mbt_infohash_string(ih, hex, sizeof(hex));
		for (int i = 0; i < N; i++) {
			if (strcmp(hex, hashes[i]) != 0)
				continue;
			seen[i]++;
			if (first_idx < 0)
				first_idx = i;
		}
	}
	mbt_lsd_frame_info_freep(&info);
}

static void
run(double secs)
{
	double end = now() + secs;

	while (now() < end)
		mbt_ev_step(ev, 10);
}

static int
count_seen(int from, int to)
{
	int c = 0;

	for (int i = from; i < to; i++)
		c += seen[i] > 0;
	return c;
}

static struct mbt_session *
start(struct mbt_storage_memory *mem, struct mbt_infohash **ih)
{
	struct mbt_session *s;
	struct mbt_session_config cfg = {.ev = ev, .disable_utp = 1,
	    .default_storage = mbt_storage_memory_storage(mem),
	    .timers = {.lsd_interval = 5}};

	assert(mbt_session_new(&s, &cfg) == 0);
	mbt_session_disable_dht(s);
	assert(mbt_session_enable_lsd(s) == 0);
	port = mbt_session_listen_port(s);
	for (int i = 0; i < N; i++)
		assert(mbt_session_add_infohash(s, ih[i], NULL, NULL) == 0);
	return s;
}

/* Where the first packet of a new node starts in the list. */
static int
first_start(struct mbt_storage_memory *mem, struct mbt_infohash **ih)
{
	struct mbt_session *s = start(mem, ih);
	double end = now() + 3;

	first_idx = -1;
	while (first_idx < 0 && now() < end)
		mbt_ev_step(ev, 10);
	assert(first_idx >= 0);
	mbt_session_free(s);
	return first_idx;
}

int
main(void)
{
	struct mbt_storage_memory *mem;
	struct mbt_session *s;
	struct mbt_ev_io *io;
	struct mbt_infohash *ih[N];

	assert(mbt_ev_new_default(&ev) == 0);
	if (mbt_lsd_init(&lsd, O_NONBLOCK) < 0) {
		puts("lsd_rotate: no multicast, skipped");
		return 77;
	}
	assert(mbt_ev_io_new(ev, &io, lsd->fd, MBT_EV_READ, lsd_io_cb, NULL) == 0);

	mem = mbt_storage_memory_new();
	assert(mem);
	srand((unsigned)time(NULL) ^ (unsigned)getpid());
	for (int i = 0; i < N; i++) {
		snprintf(hashes[i], sizeof(hashes[i]), "%08x%08x%08x%08x%08x",
		         (unsigned)rand(), (unsigned)rand(), (unsigned)rand(),
		         (unsigned)rand(), (unsigned)i);
		assert(mbt_infohash_new(&ih[i], MBT_INFOHASH_SHA1, hashes[i]) == 0);
		mbt_infohash_string(ih[i], hashes[i], sizeof(hashes[i]));
	}

	/* Four nodes, same list: all four at one start is 1 in 8 million. */
	int a = first_start(mem, ih), same = 1;
	for (int k = 0; k < 3; k++)
		same += first_start(mem, ih) == a;
	assert(same < 4);

	memset(seen, 0, sizeof(seen));
	packets = 0;
	max_len = 0;
	last_packet = 0;
	min_gap = 1e9;
	s = start(mem, ih);

	/* A burst of adds shares one packet, not one packet per torrent. */
	run(1.6);
	burst = packets;
	assert(burst == 1);

	/* Every new torrent gets a turn, 26 or so per packet. */
	run(8.5);
	assert(count_seen(0, N) == N);
	/*
	 * More torrents than five packets hold: the first ones are due again
	 * before the last ones, so only the cursor gets every torrent a turn.
	 */
	memset(seen, 0, sizeof(seen));
	run(9.0);
	assert(count_seen(0, N) == N);
	assert(max_len <= MBT_LSD_MAX_PACKET);
	assert(min_gap > 0.8);

	/* Shrink the list below the cursor, then all that stay get a turn. */
	for (int i = KEEP; i < N; i++)
		assert(mbt_session_remove_torrent(s, ih[i]) == 0);
	memset(seen, 0, sizeof(seen));
	run(6.0);
	assert(count_seen(0, KEEP) == KEEP);
	assert(count_seen(KEEP, N) == 0);

	printf("lsd_rotate: ok, %d packets, largest %d bytes\n", packets, max_len);
	mbt_session_free(s);
	mbt_storage_memory_destroy(mem);
	for (int i = 0; i < N; i++)
		mbt_infohash_unref(ih[i]);
	mbt_ev_io_free(io);
	mbt_lsd_freep(&lsd);
	mbt_ev_destroy(ev);
	return 0;
}
