// SPDX-License-Identifier: ISC
#ifndef MBT_HTTP_CURL_H
#define MBT_HTTP_CURL_H

#include "export.h"
#include "tracker.h"
#include "webseed.h"

/* Non-blocking HTTP and HTTPS client for tracker announces, in builds with libcurl. */

struct mbt_ev;
struct mbt_http_curl;

MBT_API int mbt_http_curl_new(struct mbt_http_curl **out, struct mbt_ev *ev);

/* Takes a struct mbt_http_curl *. Pending requests finish as failures. */
MBT_API void mbt_http_curl_free(void *c);

/* Use this PEM data as the CA list, in place of files. Returns -ENOTSUP on an old libcurl. */
MBT_API int mbt_http_curl_set_ca(struct mbt_http_curl *c, const void *pem, size_t len);

/* A range transfer fails below bytes_per_sec for sec seconds. Default: 1024 for 30. */
MBT_API void mbt_http_curl_set_low_speed(struct mbt_http_curl *c, long bytes_per_sec, long sec);

/* An mbt_http_range_fn and its cancel. arg is the struct mbt_http_curl *. */
MBT_API int mbt_http_curl_range_fn(const struct mbt_http_range_request *req,
                                   mbt_http_data_fn data, mbt_http_range_done_fn done,
                                   void *cb_arg, void *arg, struct mbt_http_range **out);
MBT_API void mbt_http_curl_range_cancel(struct mbt_http_range *r, void *arg);

/* An mbt_http_fn. arg is the struct mbt_http_curl *. */
MBT_API int mbt_http_curl_fn(struct mbt_http_request *req, mbt_http_done_fn done,
                             void *done_arg, void *arg);

#endif
