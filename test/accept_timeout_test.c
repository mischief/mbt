// SPDX-License-Identifier: ISC
/* The session must drop an accepted peer that never sends a BT handshake. */
#include <arpa/inet.h>
#include <assert.h>
#include <fcntl.h>
#include <netinet/in.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/socket.h>
#include <unistd.h>

#include "mbt/conn.h"
#include "mbt/event.h"
#include "mbt/infohash.h"
#include "mbt/mse.h"
#include "mbt/session.h"

#define HASH "abababababababababababababababababababab"
#define WAIT_STEPS 500           /* 5 seconds at 10 ms */

static struct mbt_ev *ev;
static struct mbt_session *sess;
static uint8_t hash_bytes[20];

static int
dial(void)
{
	struct sockaddr_in sa = {.sin_family = AF_INET};
	int fd = socket(AF_INET, SOCK_STREAM, 0);

	assert(fd >= 0);
	sa.sin_port = htons(mbt_session_listen_port(sess));
	sa.sin_addr.s_addr = htonl(INADDR_LOOPBACK);
	assert(connect(fd, (struct sockaddr *)&sa, sizeof(sa)) == 0);
	assert(fcntl(fd, F_SETFL, O_NONBLOCK) == 0);
	return fd;
}

static int
closed_by_peer(int fd)
{
	char c;

	return recv(fd, &c, 1, 0) == 0;
}

/* A TCP peer that connects and stays silent. */
static void
test_silent(void)
{
	int fd = dial();
	int closed = 0;

	for (int i = 0; i < 20; i++)
		mbt_ev_step(ev, 10);
	assert(!closed_by_peer(fd));
	for (int i = 0; i < WAIT_STEPS && !closed; i++) {
		mbt_ev_step(ev, 10);
		closed = closed_by_peer(fd);
	}
	assert(closed);
	close(fd);
}

static void
on_read(struct mbt_conn *c, void *arg)
{
	(void)c; (void)arg;
}

static void
on_event(struct mbt_conn *c, int err, void *arg)
{
	int *closed = arg;

	(void)c; (void)err;
	*closed = 1;
}

/* A peer that ends the MSE handshake and sends no BT handshake. */
static void
test_mse_silent(void)
{
	struct mbt_conn *inner, *c;
	int closed = 0;

	assert(mbt_conn_new(ev, &inner, dial(), NULL, NULL, NULL) == 0);
	assert(mbt_mse_client(ev, inner, hash_bytes, MBT_MSE_RC4, &c) == 0);
	mbt_conn_set_callbacks(c, on_read, on_event, &closed);

	for (int i = 0; i < WAIT_STEPS && !closed; i++) {
		mbt_ev_step(ev, 10);
		if (mbt_mse_selected(c))
			break;
	}
	assert(mbt_mse_selected(c) == MBT_MSE_RC4);
	for (int i = 0; i < WAIT_STEPS && !closed; i++)
		mbt_ev_step(ev, 10);
	assert(closed);
	mbt_conn_free(c);
	mbt_ev_step(ev, 1);
}

/* The session must release a socket that is still in the handshake wait. */
static void
test_free_while_pending(void)
{
	int fd = dial();

	for (int i = 0; i < 5; i++)
		mbt_ev_step(ev, 10);
	mbt_session_free(sess);
	sess = NULL;
	close(fd);
}

int
main(void)
{
	struct mbt_infohash *ih;
	struct mbt_session_config cfg = {.disable_utp = 1, .timers = {.accept_timeout = 1}};

	for (int i = 0; i < 20; i++)
		hash_bytes[i] = 0xab;
	assert(mbt_ev_new_default(&ev) == 0);
	cfg.ev = ev;
	assert(mbt_session_new(&sess, &cfg) == 0);
	assert(mbt_infohash_new(&ih, MBT_INFOHASH_SHA1, HASH) == 0);
	assert(mbt_session_add_infohash(sess, ih, NULL, NULL) == 0);
	mbt_infohash_unref(ih);
	test_silent();
	test_mse_silent();
	test_free_while_pending();
	mbt_ev_destroy(ev);
	puts("accept_timeout: ok");
	return 0;
}
