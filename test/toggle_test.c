// SPDX-License-Identifier: ISC
/*
 * test/toggle_test.c — enable/disable DHT and LSD without torrents
 */
#include <assert.h>
#include <stdio.h>

#include "mbt/event.h"
#include "mbt/session.h"

int main(void)
{
	int rv;
	struct mbt_ev *ev;
	struct mbt_session *s;

#ifdef HAVE_SD_EVENT
	rv = mbt_ev_sd_new(&ev);
#elif defined(HAVE_LIBEVENT)
	rv = mbt_ev_libevent_new(&ev);
#endif
	assert(rv == 0);

	struct mbt_session_config cfg = { .ev = ev, .listen_port = 0 };
	rv = mbt_session_new(&s, &cfg);
	assert(rv == 0);

	/* Enable/disable DHT */
	rv = mbt_session_enable_dht(s);
	assert(rv == 0);
	/* Enable again — idempotent */
	rv = mbt_session_enable_dht(s);
	assert(rv == 0);
	mbt_session_disable_dht(s);
	/* Disable again — safe */
	mbt_session_disable_dht(s);
	/* Re-enable after disable */
	rv = mbt_session_enable_dht(s);
	assert(rv == 0);
	mbt_session_disable_dht(s);

	/* Enable/disable LSD */
	rv = mbt_session_enable_lsd(s);
	assert(rv == 0);
	rv = mbt_session_enable_lsd(s);
	assert(rv == 0);
	mbt_session_disable_lsd(s);
	mbt_session_disable_lsd(s);
	rv = mbt_session_enable_lsd(s);
	assert(rv == 0);
	mbt_session_disable_lsd(s);

	fprintf(stderr, "toggle_test: PASS\n");

	mbt_session_free(s);
	mbt_ev_destroy(ev);
	return 0;
}
