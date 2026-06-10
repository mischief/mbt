// SPDX-License-Identifier: ISC
/*
 * mbt-pcp: standalone PCP port mapping tool
 * Usage: mbt-pcp [-g gateway] [-l lifetime] [-t tcp|udp] -p port
 */
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>
#include <err.h>

#include "mbt/event.h"
#include "mbt/pcp.h"
#include "mbt/log.h"

static struct mbt_ev *ev;
static int got_mapping;

static void
on_mapped(const char *addr, uint16_t port, uint32_t lifetime, void *arg)
{
	(void)arg;
	printf("mapped: %s:%hu (lifetime %us)\n", addr, port, lifetime);
	got_mapping = 1;
	mbt_ev_stop(ev);
}

static void
timeout_cb(struct mbt_ev_timer *t, void *a)
{
	(void)t; (void)a;
	fprintf(stderr, "timeout: no PCP response\n");
	mbt_ev_stop(ev);
}

int
main(int argc, char *argv[])
{
	int ch;
	const char *gateway = NULL;
	int lifetime = 120;
	int port = 0;
	int protocol = 6; /* TCP */

	while ((ch = getopt(argc, argv, "hg:l:p:t:d")) != -1) {
		switch (ch) {
		case 'g': gateway = optarg; break;
		case 'l': lifetime = atoi(optarg); break;
		case 'p': port = atoi(optarg); break;
		case 't':
			if (strcmp(optarg, "udp") == 0) protocol = 17;
			else if (strcmp(optarg, "tcp") == 0) protocol = 6;
			else errx(1, "protocol must be tcp or udp");
			break;
		case 'd':
			mbt_log_set(mbt_log_stderr, NULL);
			mbt_log_set_debug(MBT_LOG_ALL);
			break;
		case 'h':
		default:
			fprintf(stderr, "usage: mbt-pcp [-g gateway] [-l lifetime] [-t tcp|udp] [-d] -p port\n");
			return ch == 'h' ? 0 : 1;
		}
	}

	if (port == 0) {
		fprintf(stderr, "usage: mbt-pcp [-g gateway] [-l lifetime] [-t tcp|udp] [-d] -p port\n");
		return 1;
	}

	mbt_ev_new_default(&ev);

	struct mbt_pcp *pcp;
	int rv = mbt_pcp_new(&pcp, ev, gateway);
	if (rv < 0)
		errx(1, "mbt_pcp_new: %s", strerror(-rv));

	rv = mbt_pcp_map(pcp, protocol, port, lifetime, on_mapped, NULL);
	if (rv < 0)
		errx(1, "mbt_pcp_map: %s", strerror(-rv));

	struct mbt_ev_timer *to;
	mbt_ev_timer_new(ev, &to, 10 * 1000000ULL, timeout_cb, NULL);

	mbt_ev_run(ev);

	if (got_mapping) {
		printf("external: %s:%hu\n", mbt_pcp_external_addr(pcp), mbt_pcp_external_port(pcp));
	}

	mbt_ev_timer_free(to);
	mbt_pcp_free(pcp);
	mbt_ev_destroy(ev);
	return got_mapping ? 0 : 1;
}
