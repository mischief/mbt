// SPDX-License-Identifier: ISC
#ifndef MBT_RESOLVE_H
#define MBT_RESOLVE_H

#include "export.h"
#include <sys/socket.h>
#include <stdint.h>

struct mbt_ev;
struct mbt_resolve;

/* Callback: addr=NULL + err<0 on failure */
typedef void (*mbt_resolve_cb)(struct sockaddr_storage *addr, socklen_t len,
                               int err, void *arg);

/* Start async resolution. out may be NULL if you don't need to cancel. */
MBT_API int mbt_resolve(struct mbt_ev *ev, const char *host, uint16_t port,
                         mbt_resolve_cb cb, void *arg, struct mbt_resolve **out);

/* Cancel a pending resolution (safe to call with NULL). */
MBT_API void mbt_resolve_cancel(struct mbt_resolve *r);

#endif
