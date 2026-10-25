// SPDX-License-Identifier: ISC
#ifndef MBT_UDP_MUX_H
#define MBT_UDP_MUX_H

/*
 * Internal UDP demultiplexer. uTP (BEP 29) and the DHT (BEP 5) both run on UDP
 * on the same port; real clients share ONE socket and demux datagrams by their
 * first byte rather than binding two sockets to one port (which collide). This
 * owns the dual-stack mbt_listener and dispatches each datagram to the matching
 * registered handler. Not part of the public API.
 *
 * Demux: a uTP packet's first byte is (type<<4)|version with version==1 and
 * type in 0..4, i.e. 0x01/0x11/0x21/0x31/0x41. A DHT KRPC message is a bencoded
 * dict starting with 'd' (0x64). These never collide.
 */

#include <stdint.h>
#include <sys/socket.h>
#include <stddef.h>

/*
 * Largest datagram the mux receives whole: one Ethernet MTU, which covers uTP
 * and DHT. A larger datagram is truncated, so it is dropped. The uTP receive
 * path sizes its payload limit from this (UTP_MAX_RECV_PAYLOAD in utp.c); it is
 * a receive-side limit, unrelated to the payload size we send (UTP_MSS).
 */
#define UDP_RECV_MAX 1500

struct mbt_ev;
struct mbt_udp_mux;

/* Handler: return 1 if the datagram was consumed, 0 to let another handler
 * try. Registered per protocol. */
typedef int (*mbt_udp_handler)(const void *buf, size_t len,
                               const struct sockaddr *src, socklen_t srclen,
                               void *arg);

/* Create a mux bound to a dual-stack UDP socket on port (0 = ephemeral). */
int  mbt_udp_mux_new(struct mbt_udp_mux **out, struct mbt_ev *ev, uint16_t port);

/* Register a protocol handler. utp_h handles uTP datagrams, dht_h handles
 * bencoded (DHT) datagrams; either may be NULL. */
void mbt_udp_mux_set_utp(struct mbt_udp_mux *m, mbt_udp_handler h, void *arg);
void mbt_udp_mux_set_dht(struct mbt_udp_mux *m, mbt_udp_handler h, void *arg);

/* Send a datagram. Picks the v4/v6 socket from dst family. 0 on success. */
int  mbt_udp_mux_sendto(struct mbt_udp_mux *m, const void *buf, size_t len,
                        const struct sockaddr *dst, socklen_t dstlen);

/* Actual bound port. */
uint16_t mbt_udp_mux_port(struct mbt_udp_mux *m);

/* Free the mux and close its sockets. Consumers must already be torn down. */
void mbt_udp_mux_free(struct mbt_udp_mux *m);

#endif
