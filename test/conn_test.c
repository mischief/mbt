// SPDX-License-Identifier: ISC
#include <assert.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>
#include <sys/socket.h>
#include <netinet/in.h>
#include <arpa/inet.h>

#include "mbt/event.h"
#include "mbt/conn.h"

static struct mbt_ev *ev;
static int read_count;
static int event_count;
static int last_event_err;
static char read_buf[65536];
static size_t read_total;

static void
on_read(struct mbt_conn *c, void *arg)
{
	(void)arg;
	char buf[64];
	ssize_t n = mbt_conn_read(c, buf, sizeof(buf));
	if (n <= 0) return;
	memcpy(read_buf + read_total, buf, n);
	read_total += n;
	read_count++;
}

static void
on_event(struct mbt_conn *c, int err, void *arg)
{
	(void)c;
	(void)arg;
	last_event_err = err;
	event_count++;
	mbt_ev_stop(ev);
}

/* Test: create conn from socketpair, write, read back */
static void
test_basic_rw(void)
{
	int sv[2];
	int rv;

	mbt_ev_new_default(&ev); printf("test_basic_rw\n");
	fflush(stdout);

	rv = socketpair(AF_UNIX, SOCK_STREAM, 0, sv);
	assert(rv == 0);

	read_count = 0;
	read_total = 0;
	event_count = 0;
	memset(read_buf, 0, sizeof(read_buf));

	struct mbt_conn *conn = NULL;
	rv = mbt_conn_new(ev, &conn, sv[0], on_read, on_event, NULL);
	assert(rv == 0);
	assert(conn != NULL);

	/* Write from the other end */
	write(sv[1], "hello", 5);
	close(sv[1]); /* will cause EOF after read */

	mbt_ev_run(ev);

	assert(read_count >= 1);
	assert(read_total == 5);
	assert(memcmp(read_buf, "hello", 5) == 0);
	assert(event_count == 1);
	assert(last_event_err == 0); /* EOF */

	mbt_conn_free(conn);
	mbt_ev_destroy(ev);
}

/* Test: watermark - don't fire read until N bytes available */
static void
test_watermark(void)
{
	int sv[2];
	int rv;

	mbt_ev_new_default(&ev); printf("test_watermark\n");

	rv = socketpair(AF_UNIX, SOCK_STREAM, 0, sv);
	assert(rv == 0);

	read_count = 0;
	read_total = 0;
	event_count = 0;
	memset(read_buf, 0, sizeof(read_buf));

	struct mbt_conn *conn = NULL;
	rv = mbt_conn_new(ev, &conn, sv[0], on_read, on_event, NULL);
	assert(rv == 0);

	mbt_conn_set_watermark(conn, 10);

	/* Write 5 bytes - should not trigger read */
	write(sv[1], "hello", 5);
	/* Write 5 more - now we have 10 */
	write(sv[1], "world", 5);
	close(sv[1]);

	mbt_ev_run(ev);

	assert(read_total == 10);
	assert(memcmp(read_buf, "helloworld", 10) == 0);

	mbt_conn_free(conn);
	mbt_ev_destroy(ev);
}

/* Test: conn write then read back from other end (needs loop to flush) */
static int write_test_done;
static void
on_write_reader(struct mbt_conn *c, void *arg)
{
	char buf[16] = {0};
	ssize_t r = mbt_conn_read(c, buf, sizeof(buf));
	if (r <= 0) return;
	assert(r == 8);
	assert(memcmp(buf, "testdata", 8) == 0);
	write_test_done = 1;
	mbt_ev_stop(ev);
}

static void
test_write(void)
{
	int sv[2];
	int rv;

	mbt_ev_new_default(&ev); printf("test_write\n");
	fflush(stdout);

	rv = socketpair(AF_UNIX, SOCK_STREAM, 0, sv);
	assert(rv == 0);

	write_test_done = 0;

	struct mbt_conn *writer = NULL;
	rv = mbt_conn_new(ev, &writer, sv[0], NULL, on_event, NULL);
	assert(rv == 0);

	struct mbt_conn *reader = NULL;
	rv = mbt_conn_new(ev, &reader, sv[1], on_write_reader, on_event, NULL);
	assert(rv == 0);

	ssize_t n = mbt_conn_write(writer, "testdata", 8);
	assert(n == 8);

	mbt_ev_run(ev);
	assert(write_test_done == 1);

	mbt_conn_free(writer);
	mbt_conn_free(reader);
	mbt_ev_destroy(ev);
}

/* Test: available reports buffered bytes */
static void
test_available(void)
{
	int sv[2];
	int rv;

	mbt_ev_new_default(&ev); printf("test_available\n");

	rv = socketpair(AF_UNIX, SOCK_STREAM, 0, sv);
	assert(rv == 0);

	read_count = 0;
	read_total = 0;
	event_count = 0;

	struct mbt_conn *conn = NULL;
	rv = mbt_conn_new(ev, &conn, sv[0], NULL, on_event, NULL);
	assert(rv == 0);

	/* Set high watermark so read callback won't fire */
	mbt_conn_set_watermark(conn, 100);

	write(sv[1], "abc", 3);
	close(sv[1]);

	mbt_ev_run(ev);

	/* After EOF event, data should still be available */
	size_t avail = mbt_conn_available(conn);
	assert(avail == 3);

	char buf[4] = {0};
	ssize_t n = mbt_conn_read(conn, buf, 3);
	assert(n == 3);
	assert(memcmp(buf, "abc", 3) == 0);

	mbt_conn_free(conn);
	mbt_ev_destroy(ev);
}

/* Test: set_callbacks changes handlers mid-stream */
static int second_read_fired;
static void
on_read2(struct mbt_conn *c, void *arg)
{
	(void)c;
	(void)arg;
	second_read_fired = 1;
	mbt_ev_stop(ev);
}

static void
test_set_callbacks(void)
{
	int sv[2];
	int rv;

	mbt_ev_new_default(&ev); printf("test_set_callbacks\n");

	rv = socketpair(AF_UNIX, SOCK_STREAM, 0, sv);
	assert(rv == 0);

	second_read_fired = 0;

	struct mbt_conn *conn = NULL;
	rv = mbt_conn_new(ev, &conn, sv[0], on_read, on_event, NULL);
	assert(rv == 0);

	mbt_conn_set_callbacks(conn, on_read2, on_event, NULL);

	write(sv[1], "x", 1);
	close(sv[1]);

	mbt_ev_run(ev);

	assert(second_read_fired == 1);

	mbt_conn_free(conn);
	mbt_ev_destroy(ev);
}

/* Test: large write/read */
static void
on_large_read(struct mbt_conn *c, void *arg)
{
	char buf[4096];
	ssize_t n = mbt_conn_read(c, buf, sizeof(buf));
	if (n > 0) {
		memcpy(read_buf + read_total, buf, n);
		read_total += n;
	}
	if (read_total >= 16384)
		mbt_ev_stop(ev);
}

static void
on_large_event(struct mbt_conn *c, int err, void *arg)
{
	/* Drain remaining data on EOF */
	char buf[4096];
	ssize_t n;
	while ((n = mbt_conn_read(c, buf, sizeof(buf))) > 0) {
		memcpy(read_buf + read_total, buf, n);
		read_total += n;
	}
	mbt_ev_stop(ev);
}

static void
test_large_transfer(void)
{
	int sv[2];
	int rv;

	mbt_ev_new_default(&ev); printf("test_large_transfer\n");
	fflush(stdout);

	rv = socketpair(AF_UNIX, SOCK_STREAM, 0, sv);
	assert(rv == 0);

	read_count = 0;
	read_total = 0;
	event_count = 0;

	struct mbt_conn *conn = NULL;
	rv = mbt_conn_new(ev, &conn, sv[0], on_large_read, on_large_event, NULL);
	assert(rv == 0);

	/* Write 16KB from the other end */
	char *big = malloc(16384);
	assert(big);
	memset(big, 'A', 16384);

	size_t written = 0;
	while (written < 16384) {
		ssize_t n = write(sv[1], big + written, 16384 - written);
		if (n <= 0) break;
		written += n;
	}
	close(sv[1]);

	mbt_ev_run(ev);

	assert(read_total == 16384);

	free(big);
	mbt_conn_free(conn);
	mbt_ev_destroy(ev);
}

int
main(void)
{
	test_basic_rw();
	printf("PASS basic_rw\n"); fflush(stdout);
	test_watermark();
	printf("PASS watermark\n"); fflush(stdout);
	test_write();
	printf("PASS write\n"); fflush(stdout);
	test_available();
	printf("PASS available\n"); fflush(stdout);
	test_set_callbacks();
	printf("PASS set_callbacks\n"); fflush(stdout);
	test_large_transfer();
	printf("PASS large_transfer\n"); fflush(stdout);

	printf("all conn tests passed\n");
	return 0;
}
