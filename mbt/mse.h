// SPDX-License-Identifier: ISC
#ifndef MBT_MSE_H
#define MBT_MSE_H

/*
 * Message Stream Encryption, also called Protocol Encryption (MSE/PE).
 * This hides the BitTorrent handshake from naive traffic shaping. RC4 and a
 * 768-bit DH group give no real confidentiality: do not rely on them for it.
 *
 * MSE wraps an mbt_conn and is an mbt_conn. The peer layer speaks the normal
 * BitTorrent handshake on top. Upper writes before the handshake ends go out
 * as the initial payload, and the read callback does not fire until it ends.
 * A failed handshake reaches the event callback with -errno.
 */

#include "export.h"

#include <stddef.h>
#include <stdint.h>

#include "conn.h"

struct mbt_ev;

/* crypto_provide and crypto_select bits. */
enum mbt_mse_method {
	MBT_MSE_PLAINTEXT = 0x01, /* hide the handshake only */
	MBT_MSE_RC4       = 0x02, /* encrypt the whole stream */
};

/*
 * Find the torrent for an incoming handshake. hash is SHA1("req2" + infohash).
 * Write the infohash to skey and return 0, or return a negative -errno.
 */
typedef int (*mbt_mse_skey_lookup)(const uint8_t hash[20], uint8_t skey[20], void *arg);

/*
 * Wrap a connected outgoing conn. provide is a mask of mbt_mse_method.
 * On success *out owns inner, and sends the first packet. On failure the
 * caller still owns inner.
 */
MBT_API int mbt_mse_client(struct mbt_ev *ev, struct mbt_conn *inner,
                           const uint8_t infohash[20], unsigned provide,
                           struct mbt_conn **out);

/*
 * Wrap an accepted conn. seed holds bytes already read from inner, for
 * example the 68 bytes that failed the plaintext handshake check. RC4 is
 * selected when both sides offer it. Ownership is as for mbt_mse_client.
 */
MBT_API int mbt_mse_server(struct mbt_ev *ev, struct mbt_conn *inner,
                           const void *seed, size_t seedlen,
                           mbt_mse_skey_lookup lookup, void *arg,
                           unsigned provide, struct mbt_conn **out);

/* The selected method, or 0 if the handshake is not done or c is not MSE. */
MBT_API unsigned mbt_mse_selected(struct mbt_conn *c);

/* 1 if c is an MSE conn whose handshake did not finish. */
MBT_API int mbt_mse_incomplete(struct mbt_conn *c);

#endif
