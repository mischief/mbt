// SPDX-License-Identifier: ISC
#ifndef MBT_NET_H
#define MBT_NET_H

#include <stdint.h>
#include <sys/socket.h>
#include <stddef.h>

struct mbt_ev;
struct mbt_ev_io;

typedef void (*mbt_ev_io_cb)(struct mbt_ev_io *, int fd, uint32_t events, void *);

/*
 * Dual-stack listener pair. Creates both v4 and v6 sockets on the same port.
 * Both share the same callback. Either fd may be -1 if binding fails for
 * that address family (e.g., no v6 on the system).
 */
struct mbt_listener {
	int fd4, fd6;
	struct mbt_ev_io *io4, *io6;
	uint16_t port; /* actual bound port (from getsockname after bind) */
};

/* TCP listener pair (listen + accept) */
int mbt_listen_tcp(struct mbt_listener *l, struct mbt_ev *ev, uint16_t port,
                   mbt_ev_io_cb cb, void *arg);

/* UDP listener pair (recvfrom) */
int mbt_listen_udp(struct mbt_listener *l, struct mbt_ev *ev, uint16_t port,
                   mbt_ev_io_cb cb, void *arg);

/* Close and free both sockets */
void mbt_listener_close(struct mbt_listener *l);

/* Outbound TCP connect — picks AF from address string. Returns fd or -errno. */
int mbt_connect_tcp(const char *addr, uint16_t port);

/* Send UDP to a sockaddr (picks fd4 or fd6 based on sa_family) */
int mbt_sendto(int fd4, int fd6, const void *buf, size_t len,
               const struct sockaddr *sa, socklen_t salen);

/* Parse "addr:port" or "[addr]:port" into sockaddr_storage. Returns 0 or -1. */
int mbt_parse_endpoint(const char *str, uint16_t default_port,
                       struct sockaddr_storage *out, socklen_t *outlen);

/* Format sockaddr_storage as "ip:port" string. Returns buf. */
const char *mbt_sockaddr_str(const struct sockaddr_storage *sa,
                             char *buf, size_t len);

/* Format sockaddr_storage into separate addr + port. */
void mbt_sockaddr_split(const struct sockaddr_storage *sa,
                        char *addr, size_t addrlen, uint16_t *port);

#endif
