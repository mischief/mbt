// SPDX-License-Identifier: ISC
/* A failed announce retries with backoff, and a good tracker sets the pace. */
#include <assert.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>
#include <unistd.h>

#include "mbt/event.h"
#include "mbt/log.h"
#include "mbt/metainfo.h"
#include "mbt/session.h"
#include "mbt/storage.h"
#include "mbt/tracker.h"

#define REPLY_900 "d8:intervali900e5:peers0:e"
#define REPLY_5 "d8:intervali5e5:peers0:e"

static struct mbt_ev *ev;
static struct mbt_storage_memory *mem;
static char tmpdir[] = "/tmp/mbt_retry_XXXXXX";

/* The reply to each call to tracker a, then to all later calls. */
static const char *script[8];
static int calls_a, calls_b, started_a;

static int
fake_http(struct mbt_http_request *req, mbt_http_done_fn done, void *done_arg, void *arg)
{
	struct mbt_http_response resp = {0};
	const char *reply = REPLY_900;

	(void)arg;
	if (strstr(req->url, "//a.invalid/")) {
		int i = calls_a < 7 ? calls_a : 7;

		reply = script[i];
		calls_a++;
		started_a += strstr(req->url, "event=started") != NULL;
	} else {
		calls_b++;
	}
	if (reply) {
		resp.data = strdup(reply);
		resp.len = strlen(reply);
	}
	done(&resp, done_arg);
	return 0;
}

static void
encode(const char *name, int two_trackers, unsigned char **enc, size_t *len)
{
	struct mbt_metainfo *mi;
	char path[256];
	FILE *fp;

	snprintf(path, sizeof(path), "%s/%s", tmpdir, name);
	fp = fopen(path, "wb");
	assert(fp);
	fputs(name, fp);
	fclose(fp);
	assert(mbt_metainfo_new(&mi) == 0);
	assert(mbt_metainfo_set_content(mi, path, 16384) == 0);
	assert(mbt_metainfo_add_tracker(mi, "http://a.invalid/announce") == 0);
	if (two_trackers)
		assert(mbt_metainfo_add_tracker(mi, "http://b.invalid/announce") == 0);
	assert(mbt_metainfo_encode(mi, enc, len) == 0);
	mbt_metainfo_free(mi);
}

static struct mbt_session *
start(const char *name, int two_trackers)
{
	struct mbt_session *s;
	unsigned char *enc;
	size_t len;

	mem = mbt_storage_memory_new();
	assert(mem);

	struct mbt_session_config cfg = {.ev = ev, .disable_utp = 1, .http = fake_http,
	    .default_storage = mbt_storage_memory_storage(mem),
	    .timers = {.announce_retry = 1}};

	calls_a = calls_b = started_a = 0;
	assert(mbt_session_new(&s, &cfg) == 0);
	mbt_session_disable_dht(s);
	mbt_session_disable_lsd(s);
	encode(name, two_trackers, &enc, &len);
	assert(mbt_session_add_torrent_buf(s, enc, len, NULL, NULL) == 0);
	free(enc);
	return s;
}

/* Run for secs, or until tracker a has seen want calls. */
static void
run(int secs, int want)
{
	time_t end = time(NULL) + secs;

	while (time(NULL) < end && (want == 0 || calls_a < want))
		mbt_ev_step(ev, 10);
}

/* Fail twice, then succeed. A retry repeats event=started. */
static void
test_first_announce(void)
{
	struct mbt_session *s;

	memset(script, 0, sizeof(script));
	script[2] = script[3] = REPLY_900;
	s = start("first", 0);
	run(10, 3);
	assert(calls_a == 3);
	assert(started_a == 3);
	/* A success ends the retries. */
	run(2, 0);
	assert(calls_a == 3);
	mbt_session_free(s);
	mbt_storage_memory_destroy(mem);
}

/* An announce that fails after a success must not end the announces. */
static void
test_later_failure(void)
{
	struct mbt_session *s;

	memset(script, 0, sizeof(script));
	script[0] = REPLY_5;
	script[2] = script[3] = REPLY_900;
	s = start("later", 0);
	run(15, 3);
	assert(calls_a == 3);
	assert(started_a == 1);
	mbt_session_free(s);
	mbt_storage_memory_destroy(mem);
}

/* One dead tracker: the good one sets the time of the next round. */
static void
test_dead_tracker(void)
{
	struct mbt_session *s;

	memset(script, 0, sizeof(script));
	s = start("dead", 1);
	run(3, 0);
	assert(calls_a == 1);
	assert(calls_b == 1);
	mbt_session_free(s);
	mbt_storage_memory_destroy(mem);
}

int
main(void)
{
	char cmd[300];

	assert(mkdtemp(tmpdir));
	assert(mbt_ev_new_default(&ev) == 0);
	test_first_announce();
	test_later_failure();
	test_dead_tracker();
	mbt_ev_destroy(ev);
	snprintf(cmd, sizeof(cmd), "rm -rf %s", tmpdir);
	assert(system(cmd) == 0);
	puts("tracker_retry: ok");
	return 0;
}
