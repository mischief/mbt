// SPDX-License-Identifier: ISC
/*
 * Self-dial avoidance. A session is handed its own address (as a tracker,
 * DHT or PEX would hand it its external one). The first dial reaches
 * ourselves -- loopback hairpins trivially -- and is rejected on peer id;
 * the address must then be remembered so later attempts are skipped, and
 * the peer list must not churn on it. Run for TCP and for uTP.
 */
#include <assert.h>
#include <signal.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "mbt/event.h"
#include "mbt/infohash.h"
#include "mbt/log.h"
#include "mbt/peer_source.h"
#include "mbt/peerdb.h"
#include "mbt/session.h"
#include "mbt/storage.h"
#include "mbt/torrent.h"

static struct mbt_ev *ev;
static int self_rejects;      /* "self-connection rejected" lines */
static int skipped;           /* "not dialing ...: it is us" lines */
static int dials;             /* "connecting to 127.0.0.1:<port>" lines */
static char port_str[16];

static void
log_fn(enum mbt_log_level level, enum mbt_log_component comp,
       const char *fmt, va_list ap, void *arg)
{
	char buf[512];
	(void)level; (void)comp; (void)arg;
	vsnprintf(buf, sizeof(buf), fmt, ap);
	if (strstr(buf, "self-connection rejected") ||
	    strstr(buf, "learned own address"))
		self_rejects++;
	if (strstr(buf, "it is us"))
		skipped++;
	if (strstr(buf, "connecting to 127.0.0.1:") && strstr(buf, port_str))
		dials++;
	if (getenv("SELF_DIAL_DEBUG"))
		fprintf(stderr, "%s\n", buf);
}

/* Step the loop for about usec. Stepping rather than run/stop: with
 * sd-event a stop is permanent, and this test runs the loop several times. */
static void
run_for(uint64_t usec)
{
	for (uint64_t done = 0; done < usec; done += 10000)
		mbt_ev_step(ev, 10);
}

static void
one(int disable_tcp, int disable_utp, const char *name)
{
	static const char ih_hex[] = "0123456789abcdef0123456789abcdef01234567";
	struct mbt_storage_memory *mem = mbt_storage_memory_new();
	struct mbt_session_config cfg = {
		.ev = ev,
		.default_storage = mbt_storage_memory_storage(mem),
		.listen_port = 0,
		.disable_tcp = disable_tcp,
		.disable_utp = disable_utp,
		.timers = { .maintain_interval = 1 },
	};
	struct mbt_session *s;
	struct mbt_infohash *ih;
	char magnet[128];

	printf("test_self_dial_%s\n", name); fflush(stdout);
	self_rejects = skipped = dials = 0;
	assert(mbt_session_new(&s, &cfg) == 0);
	uint16_t port = mbt_session_listen_port(s);
	snprintf(port_str, sizeof(port_str), ":%hu", port);
	snprintf(magnet, sizeof(magnet), "magnet:?xt=urn:btih:%s", ih_hex);
	assert(mbt_session_add_magnet(s, magnet, NULL) == 0);
	assert(mbt_infohash_new(&ih, MBT_INFOHASH_SHA1, ih_hex) == 0);

	/* first dial: reaches ourselves, rejected, address learned */
	mbt_session_add_peer(s, ih, "127.0.0.1", port, MBT_SOURCE_TRACKER, 0);
	run_for(3000000);
	assert(self_rejects > 0 && "self connection was not detected");

	/* every later attempt is skipped before dialing */
	int before = dials;
	for (int i = 0; i < 3; i++)
		mbt_session_add_peer(s, ih, "127.0.0.1", port, MBT_SOURCE_TRACKER, 0);
	run_for(2500000);   /* a few maintenance rounds too */
	fprintf(stderr, "self_dial %s: rejects %d, skipped %d, dials %d (then %d)\n",
	        name, self_rejects, skipped, before, dials);
	assert(skipped >= 3 && "later self dials were not skipped");
	assert(dials == before && "dialed ourselves again");

	struct mbt_torrent *t = mbt_session_torrent_first(s);
	assert(mbt_torrent_peer_count(t) == 0);
	struct mbt_peer_entry *pe = mbt_peerdb_find(mbt_torrent_peerdb(t), "127.0.0.1", port);
	assert(!pe || pe->state == MBT_PEERDB_BANNED);

	mbt_infohash_unref(ih);
	mbt_session_free(s);
	mbt_storage_memory_destroy(mem);
	printf("PASS self_dial_%s\n", name);
}

int
main(void)
{
	signal(SIGPIPE, SIG_IGN);
	mbt_log_set(log_fn, NULL);
	mbt_log_set_debug(MBT_LOG_SESSION);
	mbt_ev_new_default(&ev);
	one(0, 1, "tcp");
	one(1, 0, "utp");
	mbt_ev_destroy(ev);
	return 0;
}
