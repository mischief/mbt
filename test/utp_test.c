// SPDX-License-Identifier: ISC
/*
 * Basic uTP test: two uTP contexts on localhost exchange data.
 * No torrent or peer logic — just the transport layer.
 *
 * Test topology:
 *   server ctx — binds port 0, accept_cb stores the incoming conn
 *   client ctx — binds port 0, mbt_utp_connect to server port
 *
 * Flow:
 *   1. client sends "hello utp"
 *   2. server receives it, echoes it back
 *   3. client receives echo, stops the loop
 */

#include <assert.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>
#include <netinet/in.h>
#include <arpa/inet.h>

#include "mbt/event.h"
#include "mbt/utp.h"
#include "mbt/conn.h"

#define PAYLOAD "hello utp"
#define PAYLOAD_LEN 9

static struct mbt_ev *ev;

/* ---- server side ---- */

static struct mbt_conn *server_conn;
static int server_received;

static void
server_readcb(struct mbt_conn *c, void *arg)
{
	(void)arg;
	char buf[64];
	ssize_t n = mbt_conn_read(c, buf, sizeof(buf));
	if (n <= 0) return;
	server_received += (int)n;
	mbt_conn_write(c, buf, n);
}

static void
server_eventcb(struct mbt_conn *c, int err, void *arg)
{
	(void)c; (void)arg;
	if (err < 0) {
		fprintf(stderr, "server conn error: %d\n", err);
		mbt_ev_stop(ev);
	}
}

static void
server_accept(struct mbt_utp_ctx *ctx, struct mbt_conn *conn,
              const struct sockaddr *addr, socklen_t addrlen, void *arg)
{
	(void)ctx; (void)addr; (void)addrlen; (void)arg;
	server_conn = conn;
	mbt_conn_set_callbacks(conn, server_readcb, server_eventcb, NULL);
}

/* ---- client side ---- */

static int client_received;

static void
client_readcb(struct mbt_conn *c, void *arg)
{
	(void)arg;
	char buf[64];
	ssize_t n = mbt_conn_read(c, buf, sizeof(buf));
	if (n <= 0) return;
	client_received += (int)n;
	if (client_received >= PAYLOAD_LEN)
		mbt_ev_stop(ev);
}

static void
client_eventcb(struct mbt_conn *c, int err, void *arg)
{
	(void)arg;
	if (err < 0) {
		mbt_ev_stop(ev);
		return;
	}
	mbt_conn_write(c, PAYLOAD, PAYLOAD_LEN);
}

/* ---- test ---- */

static void
test_basic(void)
{
	printf("test_basic\n");
	fflush(stdout);

	int rv = mbt_ev_new_default(&ev);
	assert(rv == 0);

	struct mbt_utp_ctx *server_ctx = NULL;
	struct mbt_utp_ctx *client_ctx = NULL;

	rv = mbt_utp_ctx_new(&server_ctx, ev, 0, server_accept, NULL);
	assert(rv == 0);
	uint16_t server_port = mbt_utp_ctx_port(server_ctx);
	assert(server_port != 0);
	printf("  server bound on port %u\n", server_port);

	rv = mbt_utp_ctx_new(&client_ctx, ev, 0, NULL, NULL);
	assert(rv == 0);

	/* connect client → server via IPv4 loopback */
	struct sockaddr_in dst = {
		.sin_family = AF_INET,
		.sin_port   = htons(server_port),
		.sin_addr   = { .s_addr = htonl(INADDR_LOOPBACK) },
	};

	struct mbt_conn *client_conn = NULL;
	rv = mbt_utp_connect(client_ctx, &client_conn,
	                     (struct sockaddr *)&dst, sizeof(dst),
	                     client_readcb, client_eventcb, NULL);
	assert(rv == 0);
	assert(client_conn != NULL);

	/* run until client receives the echo (or timeout) */
	mbt_ev_run(ev);

	assert(server_received == PAYLOAD_LEN);
	assert(client_received == PAYLOAD_LEN);

	mbt_conn_free(client_conn);
	if (server_conn)
		mbt_conn_free(server_conn);

	mbt_utp_ctx_free(client_ctx);
	mbt_utp_ctx_free(server_ctx);

	mbt_ev_destroy(ev);

	printf("PASS basic\n");
	fflush(stdout);
}

int
main(void)
{
	test_basic();
	printf("all utp tests passed\n");
	return 0;
}
