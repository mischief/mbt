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

/* UDP fd -- session can hand this to DHT when DHT is enabled on same port */
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

/*
 * Install an RX hook on the ctx.  The hook is called for every datagram passed
 * to mbt_utp_recv(), before it is processed.  If the hook returns 0 the
 * datagram is dropped; any other return value lets it through.  Used by tests
 * to inject loss/reorder on the receive path while running over real UDP.
 */
MBT_API void mbt_utp_ctx_set_rx_hook(struct mbt_utp_ctx *ctx,
    int (*hook)(struct mbt_utp_ctx *ctx, const void *buf, size_t len,
                const struct sockaddr *src, socklen_t srclen, void *arg),
    void *arg);

/*
 * Set the in-flight window (in packets) for sockets created on this ctx. 0
 * means library default. The value is clamped to [UTP_WINDOW_MIN,
 * UTP_WINDOW_MAX] and rounded up to a power of two per socket. The window sizes
 * the send/recv queues, SACK width and advertised window, so it is the single
 * memory-vs-throughput knob. Set before sockets are created.
 */
MBT_API void mbt_utp_ctx_set_window(struct mbt_utp_ctx *ctx, uint32_t pkts);

#endif
