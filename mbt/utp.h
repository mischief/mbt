// SPDX-License-Identifier: ISC
#ifndef MBT_UTP_H
#define MBT_UTP_H

#include "export.h"
#include "conn.h"

#include <stdint.h>
#include <stddef.h>
#include <netinet/in.h>

struct mbt_ev;
struct mbt_utp_ctx;

/*
 * Callback invoked when an incoming uTP connection has completed the handshake.
 * The caller takes ownership of `conn`.
 */
typedef void (*mbt_utp_accept_cb)(struct mbt_utp_ctx *ctx,
                                   struct mbt_conn *conn,
                                   const struct sockaddr *addr, socklen_t addrlen,
                                   void *arg);

/*
 * Create a uTP multiplexer.  Binds a UDP socket on `port` (same port as the
 * TCP listener so peers can reach us on both transports with one port number).
 * `port` 0 picks an ephemeral port; use mbt_utp_ctx_port() to retrieve it.
 *
 * If the session also enables DHT on the same port, the session is responsible
 * for demultiplexing incoming UDP datagrams: packets whose first byte is 'd'
 * go to DHT, everything else goes to mbt_utp_recv().
 */
MBT_API int mbt_utp_ctx_new(struct mbt_utp_ctx **out, struct mbt_ev *ev,
                              uint16_t port,
                              mbt_utp_accept_cb accept_cb, void *accept_arg);

MBT_API void mbt_utp_ctx_free(struct mbt_utp_ctx *ctx);

/* UDP port the ctx is bound to */
MBT_API uint16_t mbt_utp_ctx_port(struct mbt_utp_ctx *ctx);

/* UDP fd — session can hand this to DHT when DHT is enabled on same port */
MBT_API int mbt_utp_ctx_fd(struct mbt_utp_ctx *ctx);

/*
 * Feed a received UDP datagram into the multiplexer.
 * Called by the session's UDP read handler after filtering out DHT packets.
 */
MBT_API int mbt_utp_recv(struct mbt_utp_ctx *ctx,
                          const void *buf, size_t len,
                          const struct sockaddr *src, socklen_t srclen);

/*
 * Initiate an outbound uTP connection to `addr`.
 * Returns an mbt_conn* in connecting state; eventcb fires with err=0 on
 * success or err<0 on failure (same contract as TCP mbt_conn).
 */
MBT_API int mbt_utp_connect(struct mbt_utp_ctx *ctx,
                              struct mbt_conn **out,
                              const struct sockaddr *addr, socklen_t addrlen,
                              mbt_conn_read_cb readcb,
                              mbt_conn_event_cb eventcb,
                              void *arg);

#endif
