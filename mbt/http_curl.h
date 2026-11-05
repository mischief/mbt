// SPDX-License-Identifier: ISC
#ifndef MBT_HTTP_CURL_H
#define MBT_HTTP_CURL_H

#include "export.h"
#include "tracker.h"

/* Non-blocking HTTP and HTTPS client for tracker announces, in builds with libcurl. */

struct mbt_ev;
struct mbt_http_curl;

MBT_API int mbt_http_curl_new(struct mbt_http_curl **out, struct mbt_ev *ev);

/* Takes a struct mbt_http_curl *. Pending requests finish as failures. */
MBT_API void mbt_http_curl_free(void *c);

/* An mbt_http_fn. arg is the struct mbt_http_curl *. */
MBT_API int mbt_http_curl_fn(struct mbt_http_request *req, mbt_http_done_fn done,
                             void *done_arg, void *arg);

#endif
