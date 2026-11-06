// SPDX-License-Identifier: ISC
/* HTTP client for tracker announces: curl_multi on the mbt_ev loop. */
#include <errno.h>
#include <limits.h>
#include <stdint.h>
#include <stdio.h>
#include <stdbool.h>
#include <stdlib.h>
#include <string.h>
#include <strings.h>
#include <sys/queue.h>

#include <curl/curl.h>

#include "mbt/event.h"
#include "mbt/http_curl.h"
#include "mbt/log.h"
#include "mbt/useful.h"
#include "banned.h"

#define HTTP_TIMEOUT_SEC 15L
#define HTTP_MAX_REDIRECTS 5L
#define HTTP_MAX_RESPONSE (8 * 1024 * 1024)
#define HTTP_ERROR_STATUS 400
#define HTTP_USER_AGENT "libmbt"
#define HTTP_MAX_HOST_CONNS 16L
#define HTTP_CONNECT_TIMEOUT_SEC 15L
#define HTTP_LOW_SPEED 1024L
#define HTTP_LOW_SPEED_SEC 30L
#define HTTP_OK 200
#define HTTP_PARTIAL 206
#define HTTP_HEADER_MAX 256

struct xfer {
	TAILQ_ENTRY(xfer) entries;
	struct mbt_http_curl *c;
	CURL *easy;
	char *data;
	size_t len;
	mbt_http_done_fn done;
	void *done_arg;

	/* A range transfer sets rdone. */
	mbt_http_data_fn rdata;
	mbt_http_range_done_fn rdone;
	void *cb_arg;
	uint64_t offset, length, total;
	uint64_t received;
	uint64_t first, last, size;  /* from Content-Range; size 0 = unknown */
	bool have_range;
	bool accepted;               /* the status and range are right */
	bool stopped;                /* the data callback said stop */
	unsigned retry_after;
};

struct sock {
	struct mbt_http_curl *c;
	struct mbt_ev_io *io;
};

struct mbt_http_curl {
	struct mbt_ev *ev;
	CURLM *multi;
	struct mbt_ev_timer *timer;
	TAILQ_HEAD(, xfer) xfers;
	bool closing;
	void *ca;
	size_t ca_len;
	long low_speed, low_speed_sec;
};

static size_t
write_cb(void *ptr, size_t size, size_t nmemb, void *arg)
{
	struct xfer *x = arg;
	size_t n = size * nmemb;
	char *data;

	if (n > HTTP_MAX_RESPONSE - x->len)
		return 0;
	if ((data = realloc(x->data, x->len + n)) == NULL)
		return 0;
	memcpy(data + x->len, ptr, n);
	x->data = data;
	x->len += n;
	return n;
}

/* Parse one header line. A new response clears what a redirect left. */
static size_t
header_cb(char *ptr, size_t size, size_t nmemb, void *arg)
{
	struct xfer *x = arg;
	size_t n = size * nmemb;
	char line[HTTP_HEADER_MAX];
	unsigned long long first, last;
	unsigned long secs;
	char total[32];

	if (n >= sizeof(line))
		return n;
	memcpy(line, ptr, n);
	line[n] = '\0';
	if (strncmp(line, "HTTP/", 5) == 0) {
		x->have_range = false;
		x->retry_after = 0;
	} else if (strncasecmp(line, "content-range:", 14) == 0 &&
	           sscanf(line + 14, " bytes %llu-%llu/%31s", &first, &last, total) == 3) {
		x->first = first;
		x->last = last;
		x->size = strtoull(total, NULL, 10);
		x->have_range = true;
	} else if (strncasecmp(line, "retry-after:", 12) == 0 &&
	           sscanf(line + 12, " %lu", &secs) == 1) {
		x->retry_after = secs > UINT_MAX ? UINT_MAX : (unsigned)secs;
	}
	return n;
}

/* The reply must hold the exact range. A 200 can only be the whole file. */
static bool
range_accepted(const struct xfer *x)
{
	long status = 0;

	curl_easy_getinfo(x->easy, CURLINFO_RESPONSE_CODE, &status);
	if (status == HTTP_OK)
		return x->offset == 0 && x->length == x->total;
	return status == HTTP_PARTIAL && x->have_range && x->first == x->offset &&
	       x->last == x->offset + x->length - 1 && (x->size == 0 || x->size == x->total);
}

static size_t
range_write_cb(void *ptr, size_t size, size_t nmemb, void *arg)
{
	struct xfer *x = arg;
	size_t n = size * nmemb;

	if (x->received == 0)
		x->accepted = range_accepted(x);
	if (!x->accepted) {
		long status = 0;

		curl_easy_getinfo(x->easy, CURLINFO_RESPONSE_CODE, &status);
		return status == HTTP_OK || status == HTTP_PARTIAL ? 0 : n;  /* keep an error status */
	}
	if (n > x->length - x->received) {
		x->stopped = true;
		return 0;
	}
	x->received += n;
	if (x->rdata(ptr, n, x->cb_arg) != 0) {
		x->stopped = true;
		return 0;
	}
	return n;
}

static void
xfer_cleanup(struct xfer *x)
{
	curl_multi_remove_handle(x->c->multi, x->easy);
	curl_easy_cleanup(x->easy);
	TAILQ_REMOVE(&x->c->xfers, x, entries);
}

static void
range_finish(struct xfer *x, CURLcode result)
{
	long status = 0;
	int rv;

	curl_easy_getinfo(x->easy, CURLINFO_RESPONSE_CODE, &status);
	if (x->stopped)
		rv = -ECANCELED;
	else if (result == CURLE_OPERATION_TIMEDOUT)
		rv = -ETIMEDOUT;
	else if (result == CURLE_OK && x->accepted && x->received == x->length)
		rv = HTTP_PARTIAL;
	else if (result == CURLE_OK && status != HTTP_OK && status != HTTP_PARTIAL)
		rv = (int)status;
	else
		rv = -EIO;
	xfer_cleanup(x);
	x->rdone(rv, x->retry_after, x->cb_arg);
	free(x);
}

/* Detach x, then report. done can start a new request. */
static void
xfer_finish(struct xfer *x, bool ok)
{
	struct mbt_http_curl *c = x->c;
	struct mbt_http_response resp = {0};
	long status = 0;

	curl_easy_getinfo(x->easy, CURLINFO_RESPONSE_CODE, &status);
	curl_multi_remove_handle(c->multi, x->easy);
	curl_easy_cleanup(x->easy);
	TAILQ_REMOVE(&c->xfers, x, entries);
	if (ok && status < HTTP_ERROR_STATUS) {
		resp.data = x->data;
		resp.len = x->len;
	} else {
		free(x->data);
	}
	x->done(&resp, x->done_arg);
	free(x);
}

static void
drain(struct mbt_http_curl *c)
{
	CURLMsg *msg;
	int left;

	while ((msg = curl_multi_info_read(c->multi, &left)) != NULL) {
		struct xfer *x = NULL;

		if (msg->msg != CURLMSG_DONE)
			continue;
		CURLcode result = msg->data.result;

		curl_easy_getinfo(msg->easy_handle, CURLINFO_PRIVATE, (char **)&x);
		if (result != CURLE_OK)
			mbt_log_warn(MBT_LOG_TRACKER, "http: %s", curl_easy_strerror(result));
		if (x->rdone)
			range_finish(x, result);
		else
			xfer_finish(x, result == CURLE_OK);
	}
}

static void
io_cb(struct mbt_ev_io *io, int fd, uint32_t events, void *arg)
{
	struct sock *s = arg;
	struct mbt_http_curl *c = s->c;
	int mask = 0, running;

	(void)io;
	if (events & MBT_EV_READ)
		mask |= CURL_CSELECT_IN;
	if (events & MBT_EV_WRITE)
		mask |= CURL_CSELECT_OUT;
	curl_multi_socket_action(c->multi, fd, mask, &running);
	drain(c);
}

static void
timer_fire(struct mbt_ev_timer *timer, void *arg)
{
	struct mbt_http_curl *c = arg;
	int running;

	(void)timer;
	curl_multi_socket_action(c->multi, CURL_SOCKET_TIMEOUT, 0, &running);
	drain(c);
}

/* curl asks for a timer. Negative means none. */
static int
timer_cb(CURLM *multi, long ms, void *arg)
{
	struct mbt_http_curl *c = arg;
	uint64_t usec;

	(void)multi;
	if (ms < 0) {
		if (c->timer != NULL) {
			mbt_ev_timer_free(c->timer);
			c->timer = NULL;
		}
		return 0;
	}
	usec = (uint64_t)ms * 1000;
	if (c->timer != NULL)
		return mbt_ev_timer_mod(c->timer, usec) < 0 ? -1 : 0;
	return mbt_ev_timer_new(c->ev, &c->timer, usec, timer_fire, c) < 0 ? -1 : 0;
}

/* curl asks to watch or drop a socket. */
static int
socket_cb(CURL *easy, curl_socket_t fd, int what, void *arg, void *sockp)
{
	struct mbt_http_curl *c = arg;
	struct sock *s = sockp;
	uint32_t events = 0;

	(void)easy;
	if (what == CURL_POLL_REMOVE) {
		if (s != NULL) {
			mbt_ev_io_free(s->io);
			free(s);
			curl_multi_assign(c->multi, fd, NULL);
		}
		return 0;
	}
	if (what == CURL_POLL_IN || what == CURL_POLL_INOUT)
		events |= MBT_EV_READ;
	if (what == CURL_POLL_OUT || what == CURL_POLL_INOUT)
		events |= MBT_EV_WRITE;
	if (s != NULL)
		return mbt_ev_io_mod(s->io, events) < 0 ? -1 : 0;
	if ((s = calloc(1, sizeof(*s))) == NULL)
		return -1;
	s->c = c;
	if (mbt_ev_io_new(c->ev, &s->io, fd, events, io_cb, s) < 0) {
		free(s);
		return -1;
	}
	curl_multi_assign(c->multi, fd, s);
	return 0;
}

int
mbt_http_curl_new(struct mbt_http_curl **out, struct mbt_ev *ev)
{
	ASSERT_RETURN(out, -EINVAL);
	ASSERT_RETURN(ev, -EINVAL);
	struct mbt_http_curl *c = calloc(1, sizeof(*c));

	if (c == NULL)
		return -ENOMEM;
	if (curl_global_init(CURL_GLOBAL_DEFAULT) != CURLE_OK) {
		free(c);
		return -EIO;
	}
	if ((c->multi = curl_multi_init()) == NULL) {
		curl_global_cleanup();
		free(c);
		return -ENOMEM;
	}
	c->ev = ev;
	TAILQ_INIT(&c->xfers);
	curl_multi_setopt(c->multi, CURLMOPT_SOCKETFUNCTION, socket_cb);
	curl_multi_setopt(c->multi, CURLMOPT_SOCKETDATA, c);
	curl_multi_setopt(c->multi, CURLMOPT_TIMERFUNCTION, timer_cb);
	curl_multi_setopt(c->multi, CURLMOPT_TIMERDATA, c);
	curl_multi_setopt(c->multi, CURLMOPT_MAX_HOST_CONNECTIONS, HTTP_MAX_HOST_CONNS);
	*out = c;
	return 0;
}

void
mbt_http_curl_free(void *arg)
{
	struct mbt_http_curl *c = arg;
	struct xfer *x;

	if (c == NULL)
		return;
	c->closing = true;
	while ((x = TAILQ_FIRST(&c->xfers)) != NULL) {
		if (x->rdone) {
			x->stopped = true;
			range_finish(x, CURLE_OK);
		} else {
			xfer_finish(x, false);
		}
	}
	curl_multi_cleanup(c->multi);
	if (c->timer != NULL)
		mbt_ev_timer_free(c->timer);
	curl_global_cleanup();
	free(c->ca);
	free(c);
}

int
mbt_http_curl_set_ca(struct mbt_http_curl *c, const void *pem, size_t len)
{
#if LIBCURL_VERSION_NUM >= 0x074d00
	void *copy = malloc(len);

	if (copy == NULL)
		return -ENOMEM;
	memcpy(copy, pem, len);
	free(c->ca);
	c->ca = copy;
	c->ca_len = len;
	return 0;
#else
	(void)c; (void)pem; (void)len;
	return -ENOTSUP;
#endif
}

void
mbt_http_curl_set_low_speed(struct mbt_http_curl *c, long bytes_per_sec, long sec)
{
	c->low_speed = bytes_per_sec;
	c->low_speed_sec = sec;
}

static void
xfer_set_ca(struct xfer *x)
{
#if LIBCURL_VERSION_NUM >= 0x074d00
	if (x->c->ca != NULL) {
		struct curl_blob blob = {x->c->ca, x->c->ca_len, CURL_BLOB_NOCOPY};

		curl_easy_setopt(x->easy, CURLOPT_CAINFO_BLOB, &blob);
	}
#else
	(void)x;
#endif
}

int
mbt_http_curl_range_fn(const struct mbt_http_range_request *req, mbt_http_data_fn data,
    mbt_http_range_done_fn done, void *cb_arg, void *arg, struct mbt_http_range **out)
{
	ASSERT_RETURN(req, -EINVAL);
	ASSERT_RETURN(req->length > 0, -EINVAL);
	ASSERT_RETURN(data, -EINVAL);
	ASSERT_RETURN(done, -EINVAL);
	ASSERT_RETURN(arg, -EINVAL);
	ASSERT_RETURN(out, -EINVAL);
	struct mbt_http_curl *c = arg;
	char range[64];
	struct xfer *x;

	if (c->closing)
		return -ECANCELED;
	if ((x = calloc(1, sizeof(*x))) == NULL)
		return -ENOMEM;
	if ((x->easy = curl_easy_init()) == NULL) {
		free(x);
		return -ENOMEM;
	}
	x->c = c;
	x->rdata = data;
	x->rdone = done;
	x->cb_arg = cb_arg;
	x->offset = req->offset;
	x->length = req->length;
	x->total = req->total;
	snprintf(range, sizeof(range), "%llu-%llu", (unsigned long long)req->offset,
	         (unsigned long long)(req->offset + req->length - 1));
	curl_easy_setopt(x->easy, CURLOPT_URL, req->url);
	curl_easy_setopt(x->easy, CURLOPT_RANGE, range);
	curl_easy_setopt(x->easy, CURLOPT_PRIVATE, x);
	curl_easy_setopt(x->easy, CURLOPT_WRITEFUNCTION, range_write_cb);
	curl_easy_setopt(x->easy, CURLOPT_WRITEDATA, x);
	curl_easy_setopt(x->easy, CURLOPT_HEADERFUNCTION, header_cb);
	curl_easy_setopt(x->easy, CURLOPT_HEADERDATA, x);
	curl_easy_setopt(x->easy, CURLOPT_CONNECTTIMEOUT, HTTP_CONNECT_TIMEOUT_SEC);
	curl_easy_setopt(x->easy, CURLOPT_LOW_SPEED_LIMIT, c->low_speed ? c->low_speed : HTTP_LOW_SPEED);
	curl_easy_setopt(x->easy, CURLOPT_LOW_SPEED_TIME,
	                 c->low_speed_sec ? c->low_speed_sec : HTTP_LOW_SPEED_SEC);
	curl_easy_setopt(x->easy, CURLOPT_FOLLOWLOCATION, 1L);
	curl_easy_setopt(x->easy, CURLOPT_MAXREDIRS, HTTP_MAX_REDIRECTS);
	curl_easy_setopt(x->easy, CURLOPT_NOSIGNAL, 1L);
	curl_easy_setopt(x->easy, CURLOPT_USERAGENT, HTTP_USER_AGENT);
	curl_easy_setopt(x->easy, CURLOPT_PROTOCOLS_STR, "http,https");
	curl_easy_setopt(x->easy, CURLOPT_REDIR_PROTOCOLS_STR, "http,https");
	xfer_set_ca(x);
	TAILQ_INSERT_TAIL(&c->xfers, x, entries);
	if (curl_multi_add_handle(c->multi, x->easy) != CURLM_OK) {
		TAILQ_REMOVE(&c->xfers, x, entries);
		curl_easy_cleanup(x->easy);
		free(x);
		return -EIO;
	}
	*out = (struct mbt_http_range *)x;
	return 0;
}

void
mbt_http_curl_range_cancel(struct mbt_http_range *r, void *arg)
{
	struct xfer *x = (struct xfer *)r;

	(void)arg;
	xfer_cleanup(x);
	free(x);
}

int
mbt_http_curl_fn(struct mbt_http_request *req, mbt_http_done_fn done, void *done_arg,
    void *arg)
{
	ASSERT_RETURN(req, -EINVAL);
	ASSERT_RETURN(done, -EINVAL);
	ASSERT_RETURN(arg, -EINVAL);
	struct mbt_http_curl *c = arg;
	struct xfer *x;

	if (c->closing)
		return -ECANCELED;
	if ((x = calloc(1, sizeof(*x))) == NULL)
		return -ENOMEM;
	if ((x->easy = curl_easy_init()) == NULL) {
		free(x);
		return -ENOMEM;
	}
	x->c = c;
	x->done = done;
	x->done_arg = done_arg;
	curl_easy_setopt(x->easy, CURLOPT_URL, req->url);
	curl_easy_setopt(x->easy, CURLOPT_PRIVATE, x);
	curl_easy_setopt(x->easy, CURLOPT_WRITEFUNCTION, write_cb);
	curl_easy_setopt(x->easy, CURLOPT_WRITEDATA, x);
	curl_easy_setopt(x->easy, CURLOPT_TIMEOUT, HTTP_TIMEOUT_SEC);
	curl_easy_setopt(x->easy, CURLOPT_FOLLOWLOCATION, 1L);
	curl_easy_setopt(x->easy, CURLOPT_MAXREDIRS, HTTP_MAX_REDIRECTS);
	curl_easy_setopt(x->easy, CURLOPT_NOSIGNAL, 1L);
	curl_easy_setopt(x->easy, CURLOPT_USERAGENT, HTTP_USER_AGENT);
#if LIBCURL_VERSION_NUM >= 0x075500
	curl_easy_setopt(x->easy, CURLOPT_PROTOCOLS_STR, "http,https");
	curl_easy_setopt(x->easy, CURLOPT_REDIR_PROTOCOLS_STR, "http,https");
#endif
	if (req->method != NULL && strcmp(req->method, "POST") == 0) {
		curl_easy_setopt(x->easy, CURLOPT_POSTFIELDSIZE_LARGE, (curl_off_t)req->body_len);
		curl_easy_setopt(x->easy, CURLOPT_COPYPOSTFIELDS, req->body != NULL ? req->body : "");
	}
	xfer_set_ca(x);
	if (curl_multi_add_handle(c->multi, x->easy) != CURLM_OK) {
		curl_easy_cleanup(x->easy);
		free(x);
		return -EIO;
	}
	TAILQ_INSERT_TAIL(&c->xfers, x, entries);
	return 0;
}
