// SPDX-License-Identifier: ISC
/*
 * BEP 19 web seeds. A web seed is not a peer: it takes whole pieces, one at a
 * time, and gives them to the torrent's commit path. Each torrent owns a list
 * of them and one tick timer that decides when they may run.
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

struct mbt_webseed {
	SLIST_ENTRY(mbt_webseed) entry;
	struct mbt_torrent *t;
	struct mbt_webseed_set *set;
	char *base;                 /* URL as given */
	char *url;                  /* URL of the file */
	int dropped;
	uint64_t not_before;        /* monotonic ms */
	uint32_t backoff_ms;
	int piece;                  /* -1 when idle */
	size_t received;
	size_t plen;
	uint8_t *buf;
	struct mbt_http_range *req;
};

struct mbt_webseed_set {
	struct mbt_torrent *t;
	SLIST_HEAD(, mbt_webseed) list;
	int count;
	struct mbt_ev *ev;
	struct mbt_ev_timer *timer;
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

static int
webseed_data(const void *data, size_t len, void *arg)
{
	struct mbt_webseed *w = arg;

	if (len > w->plen - w->received)
		return 1;
	memcpy(w->buf + w->received, data, len);
	w->received += len;
	w->set->bytes += len;
	torrent_webseed_consume(w->t, len);
	return 0;
}

static void
webseed_done(int status, unsigned retry_after, void *arg)
{
	struct mbt_webseed *w = arg;
	uint32_t piece = (uint32_t)w->piece;
	int rv;

	w->req = NULL;
	session_webseed_release(torrent_session(w->t));
	if (status == HTTP_PARTIAL && w->received == w->plen) {
		w->piece = -1;
		rv = torrent_webseed_commit(w->t, piece, w->buf, w->plen);
		if (rv == TORRENT_COMMIT_DONE)
			return;         /* the set may be gone */
		if (rv == TORRENT_COMMIT_BAD_HASH) {
			torrent_webseed_release(w->t, piece);
			torrent_webseed_drive(w->t);
			webseed_drop(w, "hash mismatch");
		} else if (rv == TORRENT_COMMIT_OK) {
			w->backoff_ms = 0;
			webseed_start(w);
		}
		return;
	}
	webseed_release(w);
	torrent_webseed_drive(w->t);
	if (status == 404 || status == 410) {
		webseed_drop(w, "file not found");
	} else {
		mbt_log_info(MBT_LOG_TORRENT, "web seed %s: status %d", w->base, status);
		webseed_back_off(w, retry_after);
	}
}

static void
webseed_start(struct mbt_webseed *w)
{
	struct mbt_torrent *t = w->t;
	struct mbt_session *s = torrent_session(t);
	mbt_http_range_fn fn;
	mbt_http_range_cancel_fn cancel;
	void *arg;
	int piece, rv;

	if (w->dropped || w->req || now_ms() < w->not_before)
		return;
	if (!session_webseed_hook(s, &fn, &cancel, &arg) || torrent_webseed_throttled(t))
		return;
	if (!w->url && (w->url = webseed_url(w->base, mbt_torrent_name(t))) == NULL) {
		webseed_drop(w, "bad URL");
		return;
	}
	if (!w->buf && (w->buf = malloc(mbt_torrent_piece_length(t))) == NULL)
		return;
	if (session_webseed_acquire(s) < 0)
		return;
	if ((piece = torrent_webseed_pick(t, &w->plen)) < 0) {
		session_webseed_release(s);
		return;
	}
	w->piece = piece;
	w->received = 0;

	struct mbt_http_range_request req = {
		.url = w->url,
		.offset = (uint64_t)piece * mbt_torrent_piece_length(t),
		.length = w->plen,
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

uint64_t
webseed_set_bytes(const struct mbt_webseed_set *set)
{
	return set ? set->bytes : 0;
}
