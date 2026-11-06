// SPDX-License-Identifier: ISC
/* Connections past the HTTP tracker cap wait. The server must not close them. */
#include <arpa/inet.h>
#include <assert.h>
#include <netinet/in.h>
#include <stdio.h>
#include <string.h>
#include <sys/socket.h>
#include <time.h>
#include <unistd.h>

#include "mbt/event.h"
#include "mbt/tracker_server.h"

#define NCONN 400                /* above HTTP_MAX_CONNS */

int
main(void)
{
	struct mbt_ev *ev;
	struct mbt_tracker_http *th;
	struct sockaddr_in sa = {.sin_family = AF_INET,
	    .sin_addr.s_addr = htonl(INADDR_LOOPBACK)};
	int fd[NCONN], done = 0, got[NCONN] = {0};
	char req[256], resp[512];
	time_t end;

	assert(mbt_ev_new_default(&ev) == 0);
	assert(mbt_tracker_http_new(&th, ev, 0, NULL) == 0);
	sa.sin_port = htons(mbt_tracker_http_port(th));

	/* All connect, and the server accepts what it will, before any write. */
	for (int i = 0; i < NCONN; i++) {
		fd[i] = socket(AF_INET, SOCK_STREAM, 0);
		assert(fd[i] >= 0);
		assert(connect(fd[i], (struct sockaddr *)&sa, sizeof(sa)) == 0);
	}
	for (int i = 0; i < 2000; i++)
		mbt_ev_step(ev, 0);

	for (int i = 0; i < NCONN; i++) {
		int n = snprintf(req, sizeof(req), "GET /announce?info_hash=aaaaaaaaaaaaaaaaaa%02x"
		                 "&port=%d&left=1 HTTP/1.0\r\n\r\n", i % 256, 1000 + i);

		assert(send(fd[i], req, n, MSG_NOSIGNAL) == n);
	}
	end = time(NULL) + 10;
	while (done < NCONN && time(NULL) < end) {
		mbt_ev_step(ev, 10);
		for (int i = 0; i < NCONN; i++) {
			ssize_t n;

			if (got[i])
				continue;
			n = recv(fd[i], resp, sizeof(resp), MSG_DONTWAIT);
			if (n > 0) {
				assert(strncmp(resp, "HTTP/1.0 200", 12) == 0);
				got[i] = 1;
				done++;
			} else if (n == 0) {
				assert(!"closed with no reply");
			}
		}
	}
	assert(done == NCONN);
	assert(mbt_tracker_http_num_peers(th) == NCONN);
	for (int i = 0; i < NCONN; i++)
		close(fd[i]);
	mbt_tracker_http_free(th);
	mbt_ev_destroy(ev);
	puts("tracker_burst: ok");
	return 0;
}
