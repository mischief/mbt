// SPDX-License-Identifier: ISC
/*
 * BEP 19 web seeds. A web seed is not a peer. It streams a run of pieces in one
 * range request. It claims a piece only as its bytes arrive, so peers can take
 * the rest. Data callbacks run inside libcurl, so they only copy bytes: a timer
 * commits full pieces, because a commit can stop other transfers.
 */
#include <ctype.h>
#include <errno.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/queue.h>
#include <time.h>

#include "mbt/event.h"
#include "mbt/log.h"
#include "mbt/torrent.h"
#include "mbt/webseed.h"
#include "internal.h"
#include "banned.h"

#define MAX_WEBSEEDS 16
#define TICK_USEC 250000
#define RATE_WINDOW_MS 1000
#define BACKOFF_MIN_MS 5000
#define BACKOFF_MAX_MS 300000
#define HTTP_PARTIAL 206
#define URL_MAX 4096
#define RUN_MIN (4 * 1024 * 1024ULL)
#define RUN_MAX (64 * 1024 * 1024ULL)
#define RUN_DIVISOR 20           /* BEP 19: about 5% of the file */
#define READY_MAX 8

/* A full piece that waits for the commit timer. */
struct ready_piece {
	STAILQ_ENTRY(ready_piece) entry;
	uint32_t piece;
	size_t len;
	uint8_t *buf;
};

struct mbt_webseed {
	SLIST_ENTRY(mbt_webseed) entry;
	struct mbt_torrent *t;
	struct mbt_webseed_set *set;
	char *base;                 /* URL as given */
	char *url;                  /* URL of the file */
	int dropped;
	uint64_t not_before;        /* monotonic ms */
	uint32_t backoff_ms;
	int piece;                  /* claimed and in progress, or -1 */
	uint32_t next;              /* next piece of the run */
	uint32_t run_end;           /* first piece after the run */
	int stopped;                /* the data callback ended the run */
	size_t received;
	size_t plen;
	uint8_t *buf;
	struct mbt_http_range *req;
	STAILQ_HEAD(, ready_piece) ready;
	int ready_count;
};

struct mbt_webseed_set {
	struct mbt_torrent *t;
	SLIST_HEAD(, mbt_webseed) list;
	int count;
	struct mbt_ev *ev;
	struct mbt_ev_timer *timer;
	struct mbt_ev_timer *flush;
	struct mbt_webseed_policy policy;
	int armed;                  /* metadata and verify are done */
	int sources_empty;
	uint64_t no_peer_since;
	uint64_t low_since;
	uint64_t window_start;
	uint64_t window_bytes;
	uint64_t bytes;
};

static void webseed_start(struct mbt_webseed *w);
static int set_open(struct mbt_webseed_set *set, uint64_t now);

static uint64_t
now_ms(void)
{
	struct timespec ts;

	clock_gettime(CLOCK_MONOTONIC, &ts);
	return (uint64_t)ts.tv_sec * 1000 + (uint64_t)ts.tv_nsec / 1000000;
}

/* BEP 19: a URL that ends in a slash needs the torrent name. */
static char *
webseed_url(const char *base, const char *name)
{
	static const char hex[] = "0123456789ABCDEF";
	size_t blen = strlen(base);
	size_t len = blen + strlen(name) * 3 + 1;
	char *url, *p;

	if (blen == 0 || base[blen - 1] != '/')
		return strdup(base);
	if (len > URL_MAX || (url = malloc(len)) == NULL)
		return NULL;
	memcpy(url, base, blen);
	p = url + blen;
	for (const unsigned char *s = (const unsigned char *)name; *s; s++) {
		if (isalnum(*s) || strchr("-._~", *s)) {
			*p++ = (char)*s;
		} else {
			*p++ = '%';
			*p++ = hex[*s >> 4];
			*p++ = hex[*s & 15];
		}
	}
	*p = '\0';
	return url;
}

static void
webseed_drop(struct mbt_webseed *w, const char *why)
{
	mbt_log_warn(MBT_LOG_TORRENT, "web seed %s dropped: %s", w->base, why);
	w->dropped = 1;
}

static void
webseed_back_off(struct mbt_webseed *w, unsigned retry_after)
{
	uint64_t wait;

	w->backoff_ms = w->backoff_ms ? w->backoff_ms * 2 : BACKOFF_MIN_MS;
	if (w->backoff_ms > BACKOFF_MAX_MS)
		w->backoff_ms = BACKOFF_MAX_MS;
	wait = w->backoff_ms;
	if ((uint64_t)retry_after * 1000 > wait)
		wait = (uint64_t)retry_after * 1000;
	w->not_before = now_ms() + wait;
}

/* Give the piece back, if the request still holds one. */
static void
webseed_release(struct mbt_webseed *w)
{
	if (w->piece >= 0)
		torrent_webseed_release(w->t, (uint32_t)w->piece);
	w->piece = -1;
}

static void set_flush_cb(struct mbt_ev_timer *timer, void *arg);

static void
set_schedule_flush(struct mbt_webseed_set *set)
{
	if (set->flush)
		mbt_ev_timer_mod(set->flush, 0);
	else if (mbt_ev_timer_new(set->ev, &set->flush, 0, set_flush_cb, set) < 0)
		set->flush = NULL;
}

/* Make the next piece of the run current. Returns 0, or -1 to end the run. */
static int
webseed_next_piece(struct mbt_webseed *w)
{
	if (w->next >= w->run_end || torrent_webseed_claim(w->t, w->next, &w->plen) < 0)
		return -1;
	if (!w->buf && (w->buf = malloc(mbt_torrent_piece_length(w->t))) == NULL) {
		torrent_webseed_release(w->t, w->next);
		return -1;
	}
	w->piece = (int)w->next++;
	w->received = 0;
	return 0;
}

static int
webseed_queue_piece(struct mbt_webseed *w)
{
	struct ready_piece *r;

	if (w->ready_count >= READY_MAX || (r = malloc(sizeof(*r))) == NULL)
		return -1;
	r->piece = (uint32_t)w->piece;
	r->len = w->plen;
	r->buf = w->buf;
	STAILQ_INSERT_TAIL(&w->ready, r, entry);
	w->ready_count++;
	w->buf = NULL;
	w->piece = -1;
	set_schedule_flush(w->set);
	return 0;
}

static int
webseed_data(const void *data, size_t len, void *arg)
{
	struct mbt_webseed *w = arg;
	const uint8_t *p = data;

	while (len > 0) {
		size_t n;

		if (w->piece < 0 && webseed_next_piece(w) < 0) {
			w->stopped = 1;
			return 1;
		}
		n = w->plen - w->received;
		if (n > len)
			n = len;
		memcpy(w->buf + w->received, p, n);
		w->received += n;
		w->set->bytes += n;
		torrent_webseed_consume(w->t, n);
		p += n;
		len -= n;
		if (w->received == w->plen && webseed_queue_piece(w) < 0) {
			w->stopped = 1;
			return 1;
		}
	}
	return 0;
}

static void
webseed_done(int status, unsigned retry_after, void *arg)
{
	struct mbt_webseed *w = arg;

	w->req = NULL;
	session_webseed_release(torrent_session(w->t));
	webseed_release(w);
	if (status == HTTP_PARTIAL || w->stopped) {
		w->stopped = 0;
		set_schedule_flush(w->set);
		return;
	}
	torrent_webseed_drive(w->t);
	if (status == 404 || status == 410) {
		webseed_drop(w, "file not found");
	} else {
		mbt_log_info(MBT_LOG_TORRENT, "web seed %s: status %d", w->base, status);
		webseed_back_off(w, retry_after);
	}
	set_schedule_flush(w->set);
}

/* Bytes for one request: BEP 19 suggests a share of the file. */
static uint32_t
webseed_run_pieces(struct mbt_webseed *w)
{
	const struct mbt_webseed_policy *p = &w->set->policy;
	uint64_t min = p->run_min ? p->run_min : RUN_MIN;
	uint64_t max = p->run_max ? p->run_max : RUN_MAX;
	uint64_t run = torrent_total_length(w->t) / RUN_DIVISOR;
	uint32_t plen = mbt_torrent_piece_length(w->t);

	if (run < min)
		run = min;
	if (run > max)
		run = max;
	run /= plen;
	if (run == 0 || torrent_webseed_limited(w->t))
		return 1;
	return run > UINT32_MAX ? UINT32_MAX : (uint32_t)run;
}

static void
webseed_start(struct mbt_webseed *w)
{
	struct mbt_torrent *t = w->t;
	struct mbt_session *s = torrent_session(t);
	mbt_http_range_fn fn;
	mbt_http_range_cancel_fn cancel;
	void *arg;
	uint64_t length;
	uint32_t first;
	int piece, rv;

	if (w->dropped || w->req || w->ready_count || now_ms() < w->not_before)
		return;
	if (!session_webseed_hook(s, &fn, &cancel, &arg) || torrent_webseed_throttled(t))
		return;
	if (!w->url && (w->url = webseed_url(w->base, mbt_torrent_name(t))) == NULL) {
		webseed_drop(w, "bad URL");
		return;
	}
	if (session_webseed_acquire(s) < 0)
		return;
	if ((piece = torrent_webseed_pick(t)) < 0) {
		session_webseed_release(s);
		return;
	}
	first = (uint32_t)piece;
	w->next = first;
	w->run_end = torrent_webseed_run_end(t, first, webseed_run_pieces(w), &length);
	w->stopped = 0;
	if (webseed_next_piece(w) < 0) {
		session_webseed_release(s);
		return;
	}

	struct mbt_http_range_request req = {
		.url = w->url,
		.offset = (uint64_t)first * mbt_torrent_piece_length(t),
		.length = length,
		.total = torrent_total_length(t),
	};

	rv = fn(&req, webseed_data, webseed_done, w, arg, &w->req);
	if (rv < 0) {
		w->req = NULL;
		session_webseed_release(s);
		webseed_release(w);
		webseed_back_off(w, 0);
	}
}

static void
webseed_stop(struct mbt_webseed *w)
{
	struct mbt_session *s = torrent_session(w->t);
	struct ready_piece *r;
	mbt_http_range_fn fn;
	mbt_http_range_cancel_fn cancel;
	void *arg;

	if (w->req) {
		if (session_webseed_hook(s, &fn, &cancel, &arg) && cancel)
			cancel(w->req, arg);
		w->req = NULL;
		session_webseed_release(s);
	}
	webseed_release(w);
	while ((r = STAILQ_FIRST(&w->ready)) != NULL) {
		STAILQ_REMOVE_HEAD(&w->ready, entry);
		torrent_webseed_release(w->t, r->piece);
		free(r->buf);
		free(r);
	}
	w->ready_count = 0;
}

/* Commit queued pieces. Returns 1 if the set can be gone. */
static int
webseed_commit_ready(struct mbt_webseed *w)
{
	struct ready_piece *r;

	while ((r = STAILQ_FIRST(&w->ready)) != NULL) {
		int rv;

		STAILQ_REMOVE_HEAD(&w->ready, entry);
		w->ready_count--;
		rv = torrent_webseed_commit(w->t, r->piece, r->buf, r->len);
		if (rv == TORRENT_COMMIT_DONE || rv < 0) {
			free(r->buf);
			free(r);
			return 1;
		}
		if (rv == TORRENT_COMMIT_BAD_HASH) {
			torrent_webseed_release(w->t, r->piece);
			free(r->buf);
			free(r);
			webseed_drop(w, "hash mismatch");
			webseed_stop(w);
			torrent_webseed_drive(w->t);
			return 0;
		}
		w->backoff_ms = 0;
		if (!w->buf)
			w->buf = r->buf;
		else
			free(r->buf);
		free(r);
	}
	return 0;
}

static void
set_flush_cb(struct mbt_ev_timer *timer, void *arg)
{
	struct mbt_webseed_set *set = arg;
	struct mbt_webseed *w;

	(void)timer;
	SLIST_FOREACH(w, &set->list, entry)
		if (webseed_commit_ready(w))
			return;
	if (!torrent_webseed_wanted(set->t) || !set->armed)
		return;
	if (set_open(set, now_ms()))
		SLIST_FOREACH(w, &set->list, entry)
			webseed_start(w);
}

/* Should web seeds take new pieces now? */
static int
set_open(struct mbt_webseed_set *set, uint64_t now)
{
	const struct mbt_webseed_policy *p = &set->policy;
	struct mbt_torrent *t = set->t;
	uint64_t have = mbt_torrent_bytes_downloaded(t);

	if (p->mode == MBT_WEBSEED_OFF)
		return 0;
	if (p->mode == MBT_WEBSEED_ALWAYS)
		return 1;
	if (mbt_torrent_peer_count(t) > 0)
		set->no_peer_since = now;
	else if (set->sources_empty || session_torrent_no_sources(torrent_session(t), t) ||
	         now - set->no_peer_since >= p->no_peer_delay_ms)
		return 1;

	if (now - set->window_start >= RATE_WINDOW_MS) {
		uint64_t got = have > set->window_bytes ? have - set->window_bytes : 0;

		if (got * 1000 >= p->low_rate * (now - set->window_start))
			set->low_since = now;
		set->window_start = now;
		set->window_bytes = have;
	}
	return now - set->low_since >= p->low_rate_delay_ms;
}

static void
set_tick(struct mbt_ev_timer *timer, void *arg)
{
	struct mbt_webseed_set *set = arg;
	struct mbt_webseed *w;
	uint64_t now = now_ms();

	if (!torrent_webseed_wanted(set->t)) {
		mbt_ev_timer_mod(timer, TICK_USEC);
		return;
	}
	if (!set->armed) {
		set->armed = 1;
		set->no_peer_since = set->low_since = set->window_start = now;
		set->window_bytes = mbt_torrent_bytes_downloaded(set->t);
	}
	if (set_open(set, now))
		SLIST_FOREACH(w, &set->list, entry)
			webseed_start(w);
	mbt_ev_timer_mod(timer, TICK_USEC);
}

int
webseed_set_new(struct mbt_webseed_set **out, struct mbt_torrent *t, struct mbt_ev *ev,
                const struct mbt_webseed_policy *policy)
{
	struct mbt_webseed_set *set = calloc(1, sizeof(*set));

	if (!set)
		return -ENOMEM;
	set->t = t;
	set->ev = ev;
	set->policy = *policy;
	SLIST_INIT(&set->list);
	*out = set;
	return 0;
}

int
webseed_set_add(struct mbt_webseed_set *set, const char *url)
{
	struct mbt_webseed *w;
	int rv;

	SLIST_FOREACH(w, &set->list, entry)
		if (strcmp(w->base, url) == 0)
			return 0;
	if (set->count >= MAX_WEBSEEDS)
		return -ENOSPC;
	if ((w = calloc(1, sizeof(*w))) == NULL)
		return -ENOMEM;
	if ((w->base = strdup(url)) == NULL) {
		free(w);
		return -ENOMEM;
	}
	if (!set->timer && (rv = mbt_ev_timer_new(set->ev, &set->timer, 0, set_tick, set)) < 0) {
		set->timer = NULL;
		free(w->base);
		free(w);
		return rv;
	}
	webseed_set_kick(set);
	w->t = set->t;
	w->set = set;
	w->piece = -1;
	STAILQ_INIT(&w->ready);
	SLIST_INSERT_HEAD(&set->list, w, entry);
	set->count++;
	return 0;
}

void
webseed_set_stop(struct mbt_webseed_set *set)
{
	struct mbt_webseed *w;

	if (!set)
		return;
	SLIST_FOREACH(w, &set->list, entry)
		webseed_stop(w);
	if (set->timer)
		mbt_ev_timer_free(set->timer);
	set->timer = NULL;
	if (set->flush)
		mbt_ev_timer_free(set->flush);
	set->flush = NULL;
}

void
webseed_set_free(struct mbt_webseed_set *set)
{
	struct mbt_webseed *w;

	if (!set)
		return;
	webseed_set_stop(set);
	while ((w = SLIST_FIRST(&set->list)) != NULL) {
		SLIST_REMOVE_HEAD(&set->list, entry);
		free(w->base);
		free(w->url);
		free(w->buf);
		free(w);
	}
	free(set);
}

void
webseed_set_sources_empty(struct mbt_webseed_set *set)
{
	if (set)
		set->sources_empty = 1;
}

/* A small file must not wait for the next tick. */
void
webseed_set_kick(struct mbt_webseed_set *set)
{
	if (set && set->timer)
		mbt_ev_timer_mod(set->timer, 0);
}

int
webseed_set_busy(const struct mbt_webseed_set *set)
{
	const struct mbt_webseed *w;

	if (!set)
		return 0;
	SLIST_FOREACH(w, &set->list, entry)
		if (w->req)
			return 1;
	return 0;
}

uint64_t
webseed_set_bytes(const struct mbt_webseed_set *set)
{
	return set ? set->bytes : 0;
}
