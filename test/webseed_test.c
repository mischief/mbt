// SPDX-License-Identifier: ISC
/* Web seeds, through libcurl to a forked HTTP server and through a fake hook. */
#include <arpa/inet.h>
#include <assert.h>
#include <errno.h>
#include <fcntl.h>
#include <netinet/in.h>
#include <signal.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/socket.h>
#include <sys/wait.h>
#include <unistd.h>

#include "mbt/event.h"
#include "mbt/http_curl.h"
#include "mbt/infohash.h"
#include "mbt/metainfo.h"
#include "mbt/session.h"
#include "mbt/storage.h"
#include "mbt/torrent.h"
#include "test_util.h"

#define PIECE_LEN 16384
#define NUM_PIECES 4
#define TOTAL_LEN (PIECE_LEN * NUM_PIECES - 100)
#define SMALL_LEN 6000
#define WAIT_STEPS 800           /* 8 seconds at 10 ms */
#define NAME "data bin"
#define SMALL_NAME "small"
#define SERVER_LIFE_SEC 110
#define STALL_SEC 4

static char tmpdir[] = "/tmp/mbt_ws_XXXXXX";
static uint8_t data[TOTAL_LEN];
static unsigned char *enc, *small_enc;
static size_t enc_len, small_len;
static struct mbt_ev *ev;
static uint16_t http_port;
static pid_t server;

/* --- HTTP server --- */

static void
send_all(int fd, const void *buf, size_t len)
{
	const char *p = buf;

	while (len > 0) {
		ssize_t n = send(fd, p, len, MSG_NOSIGNAL);

		if (n <= 0)
			return;
		p += n;
		len -= (size_t)n;
	}
}

static void
send_str(int fd, const char *s)
{
	send_all(fd, s, strlen(s));
}

static void
serve_one(int fd)
{
	char req[2048], hdr[256], path[256];
	unsigned long long first = 0, last = TOTAL_LEN - 1;
	size_t got = 0;
	const char *range;
	const uint8_t *body = data;
	uint8_t wrong[TOTAL_LEN];
	size_t total = TOTAL_LEN;

	while (got < sizeof(req) - 1) {
		ssize_t n = recv(fd, req + got, sizeof(req) - 1 - got, 0);

		if (n <= 0)
			return;
		got += (size_t)n;
		req[got] = '\0';
		if (strstr(req, "\r\n\r\n"))
			break;
	}
	if (sscanf(req, "GET %255s", path) != 1)
		return;
	if ((range = strstr(req, "Range: bytes=")) != NULL)
		sscanf(range, "Range: bytes=%llu-%llu", &first, &last);
	if (strncmp(path, "/small", 6) == 0)
		total = SMALL_LEN;

	if (strcmp(path, "/good/data%20bin") != 0 && strcmp(path, "/wrong") != 0 &&
	    strcmp(path, "/short") != 0 && strcmp(path, "/stall") != 0 &&
	    strcmp(path, "/full") != 0 && strcmp(path, "/small/whole") != 0 &&
	    strcmp(path, "/busy") != 0) {
		send_str(fd, "HTTP/1.1 404 Not Found\r\nContent-Length: 0\r\nConnection: close\r\n\r\n");
		return;
	}
	if (strcmp(path, "/busy") == 0) {
		send_str(fd, "HTTP/1.1 503 Busy\r\nRetry-After: 60\r\nContent-Length: 0\r\nConnection: close\r\n\r\n");
		return;
	}
	if (strcmp(path, "/wrong") == 0) {
		memcpy(wrong, data, sizeof(wrong));
		wrong[first + 3] ^= 0xff;
		body = wrong;
	}
	if (strcmp(path, "/full") == 0 || strcmp(path, "/small/whole") == 0) {
		snprintf(hdr, sizeof(hdr), "HTTP/1.1 200 OK\r\nContent-Length: %zu\r\n"
		         "Connection: close\r\n\r\n", total);
		send_all(fd, hdr, strlen(hdr));
		send_all(fd, body, total);
		return;
	}
	snprintf(hdr, sizeof(hdr), "HTTP/1.1 206 Partial\r\nContent-Length: %llu\r\n"
	         "Content-Range: bytes %llu-%llu/%zu\r\nConnection: close\r\n\r\n",
	         last - first + 1, first, last, total);
	send_all(fd, hdr, strlen(hdr));
	if (strcmp(path, "/short") == 0) {
		send_all(fd, body + first, 100);
		return;
	}
	if (strcmp(path, "/stall") == 0) {
		send_all(fd, body + first, 100);
		sleep(STALL_SEC);
		return;
	}
	send_all(fd, body + first, (size_t)(last - first + 1));
}

static void
start_server(void)
{
	struct sockaddr_in sa = {.sin_family = AF_INET};
	socklen_t len = sizeof(sa);
	int ls = socket(AF_INET, SOCK_STREAM, 0);

	assert(ls >= 0);
	sa.sin_addr.s_addr = htonl(INADDR_LOOPBACK);
	assert(bind(ls, (struct sockaddr *)&sa, sizeof(sa)) == 0);
	assert(listen(ls, 16) == 0);
	assert(getsockname(ls, (struct sockaddr *)&sa, &len) == 0);
	http_port = ntohs(sa.sin_port);
	if ((server = fork()) == 0) {
		/* Do not hold the test harness's pipes. */
		int null = open("/dev/null", O_WRONLY);

		dup2(null, STDOUT_FILENO);
		dup2(null, STDERR_FILENO);
		alarm(SERVER_LIFE_SEC);
		signal(SIGCHLD, SIG_IGN);
		for (;;) {
			int fd = accept(ls, NULL, NULL);

			if (fd < 0)
				_exit(0);
			if (fork() == 0) {
				serve_one(fd);
				_exit(0);
			}
			close(fd);
		}
	}
	assert(server > 0);
	close(ls);
}

/* --- Helpers --- */

static void
encode_file(const char *name, size_t len, unsigned char **out, size_t *out_len)
{
	struct mbt_metainfo *mi;
	char path[300];
	FILE *fp;

	snprintf(path, sizeof(path), "%s/%s", tmpdir, name);
	fp = fopen(path, "wb");
	assert(fp);
	assert(fwrite(data, 1, len, fp) == len);
	fclose(fp);
	assert(mbt_metainfo_new(&mi) == 0);
	assert(mbt_metainfo_set_content(mi, path, PIECE_LEN) == 0);
	assert(mbt_metainfo_encode(mi, out, out_len) == 0);
	mbt_metainfo_free(mi);
}

static void
url(char *buf, size_t n, const char *path)
{
	snprintf(buf, n, "http://127.0.0.1:%hu%s", http_port, path);
}

struct run {
	struct mbt_session *s;
	struct mbt_torrent *t;
	struct mbt_storage_memory *m;
	struct mbt_http_curl *http;
};

static const struct mbt_webseed_policy now_policy = {.mode = MBT_WEBSEED_FALLBACK};
static const struct mbt_webseed_policy one_piece = {.mode = MBT_WEBSEED_FALLBACK,
    .run_min = PIECE_LEN, .run_max = PIECE_LEN};

static void
run_start(struct run *r, const unsigned char *buf, size_t len,
          const struct mbt_webseed_policy *policy)
{
	struct mbt_session_config cfg = {.ev = ev, .disable_utp = 1, .webseed = *policy};

	memset(r, 0, sizeof(*r));
	r->m = mbt_storage_memory_new();
	assert(r->m);
	assert(mbt_http_curl_new(&r->http, ev) == 0);
	mbt_http_curl_set_low_speed(r->http, 1024, 1);
	cfg.default_storage = mbt_storage_memory_storage(r->m);
	cfg.http = mbt_http_curl_fn;
	cfg.http_arg = r->http;
	cfg.http_free = mbt_http_curl_free;
	cfg.http_range = mbt_http_curl_range_fn;
	cfg.http_range_cancel = mbt_http_curl_range_cancel;
	cfg.http_range_arg = r->http;
	assert(mbt_session_new(&r->s, &cfg) == 0);
	assert(mbt_session_add_torrent_buf(r->s, buf, len, NULL, &r->t) == 0);
}

static void
run_end(struct run *r)
{
	mbt_session_free(r->s);
	mbt_storage_memory_destroy(r->m);
}

static int
wait_complete(struct mbt_torrent *t, int steps)
{
	for (int i = 0; i < steps; i++) {
		if (mbt_torrent_complete(t))
			return 1;
		mbt_ev_step(ev, 10);
	}
	return mbt_torrent_complete(t);
}

static void
add_url(struct run *r, const char *path)
{
	char u[128];

	url(u, sizeof(u), path);
	assert(mbt_torrent_add_webseed(r->t, u) == 0);
}

/* --- Tests through libcurl --- */

static void
test_download(const struct mbt_webseed_policy *policy)
{
	struct run r;

	run_start(&r, enc, enc_len, policy);
	/* The trailing slash adds the name, with its space escaped. */
	add_url(&r, "/good/");
	add_url(&r, "/good/");
	assert(wait_complete(r.t, WAIT_STEPS));
	assert(memcmp(mbt_storage_memory_data(r.m), data, TOTAL_LEN) == 0);
	assert(mbt_torrent_webseed_bytes(r.t) == TOTAL_LEN);
	run_end(&r);
}

/* A URL that fails must not fail the torrent, and another URL finishes it. */
static void
test_bad_then_good(const char *bad)
{
	struct run r;

	run_start(&r, enc, enc_len, &one_piece);
	add_url(&r, bad);
	for (int i = 0; i < 100; i++)
		mbt_ev_step(ev, 10);
	assert(mbt_torrent_have_count(r.t) == 0);
	assert(mbt_torrent_error(r.t) == 0);
	add_url(&r, "/good/");
	assert(wait_complete(r.t, WAIT_STEPS));
	assert(memcmp(mbt_storage_memory_data(r.m), data, TOTAL_LEN) == 0);
	run_end(&r);
}

static void
test_small_whole_file(void)
{
	struct run r;

	run_start(&r, small_enc, small_len, &now_policy);
	add_url(&r, "/small/whole");
	assert(wait_complete(r.t, WAIT_STEPS));
	assert(memcmp(mbt_storage_memory_data(r.m), data, SMALL_LEN) == 0);
	run_end(&r);

	/* A run of the whole file accepts a 200. */
	run_start(&r, enc, enc_len, &now_policy);
	add_url(&r, "/full");
	assert(wait_complete(r.t, WAIT_STEPS));
	assert(memcmp(mbt_storage_memory_data(r.m), data, TOTAL_LEN) == 0);
	run_end(&r);
}

/* --- Tests through a fake hook --- */

#define FAKE_MAX 8

struct fake {
	mbt_http_data_fn data;
	mbt_http_range_done_fn done;
	void *arg;
	uint64_t offset, length;
	int active;
};

static struct fake fakes[FAKE_MAX];
static int fake_active, fake_peak, fake_cancels;

static int
fake_range(const struct mbt_http_range_request *req, mbt_http_data_fn fdata,
           mbt_http_range_done_fn done, void *cb_arg, void *hook_arg,
           struct mbt_http_range **out)
{
	(void)hook_arg;
	for (int i = 0; i < FAKE_MAX; i++) {
		struct fake *f = &fakes[i];

		if (f->active)
			continue;
		*f = (struct fake){fdata, done, cb_arg, req->offset, req->length, 1};
		*out = (struct mbt_http_range *)f;
		if (++fake_active > fake_peak)
			fake_peak = fake_active;
		return 0;
	}
	return -ENOMEM;
}

static void
fake_cancel(struct mbt_http_range *r, void *hook_arg)
{
	(void)hook_arg;
	((struct fake *)r)->active = 0;
	fake_active--;
	fake_cancels++;
}

static void
fake_finish(struct fake *f, int status)
{
	f->active = 0;
	fake_active--;
	if (status == 206 && f->data(data + f->offset, f->length, f->arg) != 0)
		status = -ECANCELED;
	f->done(status, 0, f->arg);
}

static struct mbt_session *
fake_session(int max, struct mbt_storage_memory *m, const struct mbt_webseed_policy *policy)
{
	struct mbt_session_config cfg = {
		.ev = ev, .disable_utp = 1, .webseed = *policy,
		.default_storage = mbt_storage_memory_storage(m),
		.http_range = fake_range, .http_range_cancel = fake_cancel,
		.webseed_max_requests = max,
	};
	struct mbt_session *s;

	assert(mbt_session_new(&s, &cfg) == 0);
	return s;
}

static void
steps(int n)
{
	for (int i = 0; i < n; i++)
		mbt_ev_step(ev, 10);
}

static void
fake_reset(void)
{
	memset(fakes, 0, sizeof(fakes));
	fake_active = fake_peak = fake_cancels = 0;
}

static void
test_cap(void)
{
	struct mbt_storage_memory *m = mbt_storage_memory_new();
	struct mbt_session *s = fake_session(1, m, &now_policy);
	struct mbt_torrent *t;

	fake_reset();
	assert(mbt_session_add_torrent_buf(s, enc, enc_len, NULL, &t) == 0);
	assert(mbt_torrent_add_webseed(t, "http://a/x") == 0);
	assert(mbt_torrent_add_webseed(t, "http://b/x") == 0);
	assert(mbt_torrent_add_webseed(t, "http://c/x") == 0);
	steps(40);
	assert(fake_peak == 1);
	assert(mbt_torrent_state(t) == MBT_TORRENT_DOWNLOADING);

	/* Session free with a request in flight cancels it. */
	mbt_session_free(s);
	assert(fake_cancels == 1 && fake_active == 0);
	mbt_storage_memory_destroy(m);
}

static void
test_policy_off(void)
{
	struct mbt_storage_memory *m = mbt_storage_memory_new();
	struct mbt_session *s = fake_session(0, m, &now_policy);
	struct mbt_add_torrent_cfg acfg = {.webseed = {.mode = MBT_WEBSEED_OFF}};
	struct mbt_torrent *t;

	fake_reset();
	assert(mbt_session_add_torrent_buf(s, enc, enc_len, &acfg, &t) == 0);
	assert(mbt_torrent_add_webseed(t, "http://a/x") == 0);
	assert(mbt_torrent_add_webseed(t, "ftp://a/x") == -EINVAL);
	steps(40);
	assert(fake_peak == 0);
	mbt_session_free(s);
	mbt_storage_memory_destroy(m);
}

static void
test_no_hook(void)
{
	struct mbt_storage_memory *m = mbt_storage_memory_new();
	struct mbt_session_config cfg = {.ev = ev, .disable_utp = 1, .webseed = now_policy,
	    .default_storage = mbt_storage_memory_storage(m)};
	struct mbt_session *s;
	struct mbt_torrent *t;

	assert(mbt_session_new(&s, &cfg) == 0);
	assert(mbt_session_add_torrent_buf(s, enc, enc_len, NULL, &t) == 0);
	assert(mbt_torrent_add_webseed(t, "http://a/x") == 0);
	steps(40);
	assert(mbt_torrent_webseed_bytes(t) == 0);
	mbt_session_free(s);
	mbt_storage_memory_destroy(m);
}

/* A peer cannot take a piece a web seed holds, until the web seed fails. */
static int piece_hits[NUM_PIECES];

static void
count_piece(struct mbt_torrent *t, uint32_t piece, void *arg)
{
	(void)t; (void)arg;
	piece_hits[piece]++;
}

static void
test_peer_wins(void)
{
	struct mbt_storage_memory *sm = mbt_storage_memory_new();
	struct mbt_storage_memory *lm = mbt_storage_memory_new();
	struct mbt_session_config cfg = {.ev = ev, .disable_utp = 1,
	    .default_storage = mbt_storage_memory_storage(sm)};
	struct mbt_session *seed, *leech = fake_session(0, lm, &one_piece);
	struct mbt_torrent *st, *lt;
	struct mbt_torrent_callbacks cb = {.on_piece_complete = count_piece};

	fake_reset();
	memset(piece_hits, 0, sizeof(piece_hits));
	assert(mbt_session_new(&seed, &cfg) == 0);
	assert(mbt_session_add_torrent_buf(seed, enc, enc_len, NULL, &st) == 0);
	assert(mbt_storage_memory_put_file(sm, 0, data, TOTAL_LEN) == 0);
	assert(mbt_session_add_torrent_buf(leech, enc, enc_len, NULL, &lt) == 0);
	mbt_torrent_set_callbacks(lt, &cb, NULL);
	assert(mbt_torrent_add_webseed(lt, "http://a/x") == 0);
	test_wait_verified(ev, lt);
	steps(20);
	assert(fake_active == 1);

	/* The web seed finishes piece 0, then holds piece 1. */
	fake_finish(&fakes[0], 206);
	steps(5);
	assert(mbt_torrent_have_count(lt) == 1 && fake_active == 1);
	test_wait_verified(ev, st);
	assert(mbt_session_add_peer(leech, mbt_torrent_infohash(lt), "127.0.0.1",
	                            mbt_session_listen_port(seed), MBT_SOURCE_MANUAL, 0) == 0);
	for (int i = 0; i < WAIT_STEPS && mbt_torrent_have_count(lt) < NUM_PIECES - 1; i++)
		mbt_ev_step(ev, 10);
	assert(mbt_torrent_have_count(lt) == NUM_PIECES - 1);
	assert(mbt_torrent_has_piece(lt, 1) == 0);
	for (int i = 0; i < FAKE_MAX; i++)
		if (fakes[i].active) {
			fake_finish(&fakes[i], -EIO);
			break;
		}
	assert(wait_complete(lt, WAIT_STEPS));
	for (int i = 0; i < NUM_PIECES; i++)
		assert(piece_hits[i] == 1);
	assert(fake_active == 0);
	assert(mbt_torrent_webseed_bytes(lt) == PIECE_LEN);
	mbt_session_free(leech);
	mbt_session_free(seed);
	mbt_storage_memory_destroy(lm);
	mbt_storage_memory_destroy(sm);
}

/* A run stops at a piece that another source holds. */
static void
test_lazy_claim(void)
{
	struct mbt_storage_memory *m = mbt_storage_memory_new();
	struct mbt_session *s = fake_session(0, m, &now_policy);
	struct mbt_torrent *t;
	struct mbt_torrent_callbacks cb = {.on_piece_complete = count_piece};
	struct fake *a = NULL, *b = NULL;

	fake_reset();
	memset(piece_hits, 0, sizeof(piece_hits));
	assert(mbt_session_add_torrent_buf(s, enc, enc_len, NULL, &t) == 0);
	mbt_torrent_set_callbacks(t, &cb, NULL);
	assert(mbt_torrent_add_webseed(t, "http://a/x") == 0);
	assert(mbt_torrent_add_webseed(t, "http://b/x") == 0);
	test_wait_verified(ev, t);
	steps(10);
	assert(fake_active == 2);
	for (int i = 0; i < FAKE_MAX; i++) {
		if (fakes[i].active && fakes[i].offset == 0)
			a = &fakes[i];
		else if (fakes[i].active)
			b = &fakes[i];
	}
	assert(a && b);
	assert(a->length == TOTAL_LEN);
	assert(b->offset == PIECE_LEN && b->length == TOTAL_LEN - PIECE_LEN);

	/* a gets all of piece 0. Piece 1 belongs to b, so a stops there. */
	assert(a->data(data, PIECE_LEN, a->arg) == 0);
	assert(a->data(data + PIECE_LEN, 1, a->arg) != 0);
	a->active = 0;
	fake_active--;
	a->done(-ECANCELED, 0, a->arg);
	steps(5);
	assert(mbt_torrent_have_count(t) == 1);

	for (int i = 0; i < 50 && !mbt_torrent_complete(t); i++) {
		for (int j = 0; j < FAKE_MAX; j++)
			if (fakes[j].active)
				fake_finish(&fakes[j], 206);
		steps(5);
	}
	assert(mbt_torrent_complete(t));
	assert(memcmp(mbt_storage_memory_data(m), data, TOTAL_LEN) == 0);
	for (int i = 0; i < NUM_PIECES; i++)
		assert(piece_hits[i] == 1);
	mbt_session_free(s);
	mbt_storage_memory_destroy(m);
}

int
main(void)
{
	char cmd[300];

	signal(SIGPIPE, SIG_IGN);
	assert(mkdtemp(tmpdir));
	for (size_t i = 0; i < sizeof(data); i++)
		data[i] = (uint8_t)(i * 31 + i / 251);
	encode_file(NAME, TOTAL_LEN, &enc, &enc_len);
	encode_file(SMALL_NAME, SMALL_LEN, &small_enc, &small_len);
	start_server();
	assert(mbt_ev_new_default(&ev) == 0);

	test_download(&now_policy);
	test_download(&one_piece);
	test_bad_then_good("/missing");
	test_bad_then_good("/wrong");
	test_bad_then_good("/short");
	test_bad_then_good("/stall");
	test_bad_then_good("/full");
	test_bad_then_good("/busy");
	test_small_whole_file();
	test_cap();
	test_policy_off();
	test_no_hook();
	test_peer_wins();
	test_lazy_claim();

	mbt_ev_destroy(ev);
	kill(server, SIGTERM);
	waitpid(server, NULL, 0);
	free(enc);
	free(small_enc);
	snprintf(cmd, sizeof(cmd), "rm -rf %s", tmpdir);
	assert(system(cmd) == 0);
	puts("webseed: ok");
	return 0;
}
