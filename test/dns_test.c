// SPDX-License-Identifier: ISC
/*
 * test/dns_test.c — verify mbt_resolve API
 */
#include <assert.h>
#include <stdio.h>
#include <string.h>
#include <netinet/in.h>
#include <arpa/inet.h>
#include <sys/socket.h>

#include "mbt/event.h"
#include "mbt/resolve.h"

struct mbt_ev_timer;

static struct mbt_ev *ev;
static int ok_count;
static int err_count;
static int total;
static int expected;

static void
check_done(void)
{
	total++;
	if (total >= expected)
		mbt_ev_stop(ev);
}

static void
on_resolve(struct sockaddr_storage *addr, socklen_t len, int err, void *arg)
{
	const char *name = arg;
	if (err < 0) {
		fprintf(stderr, "  %s: error %d (%s)\n", name, -err, strerror(-err));
		err_count++;
	} else {
		char buf[INET6_ADDRSTRLEN];
		if (addr->ss_family == AF_INET)
			inet_ntop(AF_INET, &((struct sockaddr_in *)addr)->sin_addr, buf, sizeof(buf));
		else
			inet_ntop(AF_INET6, &((struct sockaddr_in6 *)addr)->sin6_addr, buf, sizeof(buf));
		fprintf(stderr, "  %s: %s\n", name, buf);
		ok_count++;
	}
	check_done();
}

static void
on_timeout(struct mbt_ev_timer *t, void *arg)
{
	(void)t; (void)arg;
	fprintf(stderr, "  TIMEOUT (total=%d expected=%d)\n", total, expected);
	mbt_ev_stop(ev);
}

int main(void)
{
	mbt_ev_new_default(&ev);

	/* Test 1: resolve localhost */
	fprintf(stderr, "dns_test: resolve localhost + nonexistent + real hosts\n");
	total = 0; ok_count = 0; err_count = 0;
	expected = 5;

	mbt_resolve(ev, "localhost", 8080, on_resolve, (void*)"localhost", NULL);
	mbt_resolve(ev, "nonexistent.invalid.test", 80, on_resolve, (void*)"nonexistent", NULL);
	mbt_resolve(ev, "openbsd.org", 443, on_resolve, (void*)"openbsd.org", NULL);
	mbt_resolve(ev, "kernel.org", 443, on_resolve, (void*)"kernel.org", NULL);
	mbt_resolve(ev, "freebsd.org", 443, on_resolve, (void*)"freebsd.org", NULL);

	struct mbt_ev_timer *to;
	mbt_ev_timer_new(ev, &to, 5 * 1000000ULL, on_timeout, NULL);
	mbt_ev_run(ev);

	fprintf(stderr, "dns_test: ok=%d err=%d total=%d\n", ok_count, err_count, total);

	/* localhost must always work */
	assert(total >= 2); /* at least localhost + nonexistent completed */
	assert(ok_count >= 1); /* localhost resolved */

	/* nonexistent should fail */
	assert(err_count >= 1);

	mbt_ev_timer_free(to);
	mbt_ev_destroy(ev);

	fprintf(stderr, "dns_test: PASS\n");
	return 0;
}
