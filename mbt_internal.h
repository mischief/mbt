// SPDX-License-Identifier: ISC
#ifndef MBT_SESSION_INTERNAL_H
#define MBT_SESSION_INTERNAL_H

/* Internal interfaces shared between session.c and torrent.c */

struct mbt_ratelimit;
struct mbt_session;

struct mbt_ratelimit *mbt_session_get_upload_limit(struct mbt_session *s);
struct mbt_ratelimit *mbt_session_get_download_limit(struct mbt_session *s);

#endif
