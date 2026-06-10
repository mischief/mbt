// SPDX-License-Identifier: ISC
#ifndef MBT_TIMERS_H
#define MBT_TIMERS_H

/* Timer configuration — all values in seconds, 0 = use default */
struct mbt_timers {
	int choke_interval;      /* default 10 */
	int pex_interval;        /* default 60 */
	int request_timeout;     /* default 15 */
	int keepalive_interval;  /* default 120 */
	int snub_timeout;        /* default 60 — no data from peer = snubbed */
	int dht_refresh;         /* default 900 (15 min) */
	int lsd_interval;        /* default 300 (5 min) */
	int tracker_interval;    /* default 1800 (30 min) */
};

#endif
