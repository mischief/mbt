// SPDX-License-Identifier: ISC
#include <sys/stat.h>
#include <fcntl.h>
#include <unistd.h>
#include <dirent.h>
#include <stdlib.h>
#include <string.h>
#include <stdio.h>
#include <errno.h>
#include <time.h>
#include <stdint.h>

#include "mbt/state.h"
#include "mbt/session.h"
#include "mbt/torrent.h"
#include "internal.h"
#include "mbt/torrent.h"
#include "mbt/infohash.h"
#include "mbt/be.h"
#include "banned.h"

struct mbt_torrent;

#define MAX_SAVED_URLS 32
#define INFO_PREFIX "d4:info"

struct mbt_state {
	char *dir;
};

int
mbt_state_open(struct mbt_state **out, const char *dir)
{
	struct mbt_state *st = calloc(1, sizeof(*st));
	if (!st) return -ENOMEM;

	st->dir = strdup(dir);
	if (!st->dir) { free(st); return -ENOMEM; }

	if (mkdir(dir, 0755) < 0 && errno != EEXIST) {
		int e = errno;
		free(st->dir);
		free(st);
		return -e;
	}

	*out = st;
	return 0;
}

void
mbt_state_close(struct mbt_state *st)
{
	if (!st) return;
	free(st->dir);
	free(st);
}

/* Replace dst with data through tmp, durably when sync is set. */
static int
write_file(const char *dir, const char *tmp, const char *dst, const void *data, size_t len,
    int sync)
{
	FILE *fp = fopen(tmp, "wb");
	int rv = 0;

	if (!fp)
		return -errno;
	if (fwrite(data, 1, len, fp) != len || fflush(fp) != 0)
		rv = -EIO;
	if (rv == 0 && sync && fsync(fileno(fp)) < 0)
		rv = -errno;
	if (fclose(fp) != 0 && rv == 0)
		rv = -EIO;
	if (rv < 0) {
		unlink(tmp);
		return rv;
	}
	if (rename(tmp, dst) < 0)
		return -errno;
	if (sync) {
		int fd = open(dir, O_RDONLY | O_DIRECTORY);

		if (fd < 0)
			return -errno;
		if (fsync(fd) < 0)
			rv = -errno;
		close(fd);
	}
	return rv;
}

static int
state_save(void *ctx, struct mbt_torrent *t, int sync)
{
	struct mbt_state *st = ctx;

	struct mbt_infohash *ih = mbt_torrent_infohash(t);
	if (!ih) return 0;

	char hex[MBT_MBT_INFOHASH_SHA1HEXLEN + 1];
	mbt_infohash_string(ih, hex, sizeof(hex));

	/* Write <hex>.torrent once: the file as added, or for a magnet its info dict. */
	size_t meta_len = 0, src_len = 0;
	const void *meta = mbt_torrent_metadata(t, &meta_len);
	const void *src = torrent_source(t, &src_len);
	char tmp_t[512], dst_t[512];

	snprintf(tmp_t, sizeof(tmp_t), "%s/%s.torrent.tmp", st->dir, hex);
	snprintf(dst_t, sizeof(dst_t), "%s/%s.torrent", st->dir, hex);
	if (src != NULL) {
		int rv = write_file(st->dir, tmp_t, dst_t, src, src_len, sync);

		if (rv < 0) return rv;
		torrent_source_clear(t);
	} else if (meta && meta_len > 0 && access(dst_t, F_OK) != 0) {
		size_t len = sizeof(INFO_PREFIX) - 1 + meta_len + 1;
		char *buf = malloc(len);
		int rv;

		if (!buf) return -ENOMEM;
		memcpy(buf, INFO_PREFIX, sizeof(INFO_PREFIX) - 1);
		memcpy(buf + sizeof(INFO_PREFIX) - 1, meta, meta_len);
		buf[len - 1] = 'e';
		rv = write_file(st->dir, tmp_t, dst_t, buf, len, sync);
		free(buf);
		if (rv < 0) return rv;
	}

	/* Write <hex>.resume: data_dir, added, trackers, webseeds */
	const char *data_dir = mbt_torrent_data_dir(t);
	if (!data_dir) return 0;

	_cleanup_be_ struct mbt_be_value *resume = NULL;
	if (mbt_be_dict_new(&resume) < 0) return -ENOMEM;
	mbt_be_dict_append_string(resume, "data_dir", data_dir, strlen(data_dir));
	mbt_be_dict_append_number(resume, "added", (int64_t)time(NULL));

	_cleanup_be_ struct mbt_be_value *tl = NULL, *wl = NULL;
	struct mbt_session *s = torrent_session(t);

	if (mbt_be_list_new(&tl) < 0 || mbt_be_list_new(&wl) < 0) return -ENOMEM;
	for (int i = 0; i < mbt_session_torrent_tracker_count(s, t); i++) {
		const char *url = session_saved_tracker(s, t, i);
		_cleanup_be_ struct mbt_be_value *v = NULL;

		if (!url) continue;
		if (mbt_be_string_new(&v, url, strlen(url)) < 0) return -ENOMEM;
		mbt_be_list_append(tl, v);
	}
	for (int i = 0; i < mbt_torrent_webseed_count(t); i++) {
		const char *url = mbt_torrent_webseed(t, i);
		_cleanup_be_ struct mbt_be_value *v = NULL;

		if (mbt_be_string_new(&v, url, strlen(url)) < 0) return -ENOMEM;
		mbt_be_list_append(wl, v);
	}
	if (mbt_be_dict_append(resume, "trackers", tl) < 0) return -ENOMEM;
	if (mbt_be_dict_append(resume, "webseeds", wl) < 0) return -ENOMEM;

	unsigned char *enc = NULL;
	size_t enc_len = 0;
	if (mbt_be_value_encode(resume, &enc, &enc_len) < 0) return -ENOMEM;

	char tmp[512], dst[512];
	snprintf(tmp, sizeof(tmp), "%s/%s.resume.tmp", st->dir, hex);
	snprintf(dst, sizeof(dst), "%s/%s.resume", st->dir, hex);
	int rv = write_file(st->dir, tmp, dst, enc, enc_len, sync);
	free(enc);
	return rv;
}

static int
state_save_impl(void *ctx, struct mbt_torrent *t)
{
	return state_save(ctx, t, 0);
}

static int
state_save_sync_impl(void *ctx, struct mbt_torrent *t)
{
	return state_save(ctx, t, 1);
}

static int
state_remove_impl(void *ctx, struct mbt_torrent *t)
{
	struct mbt_state *st = ctx;

	struct mbt_infohash *ih = mbt_torrent_infohash(t);
	if (!ih) return 0;

	char hex[MBT_MBT_INFOHASH_SHA1HEXLEN + 1];
	mbt_infohash_string(ih, hex, sizeof(hex));

	char path[512];
	snprintf(path, sizeof(path), "%s/%s.torrent", st->dir, hex);
	remove(path);
	snprintf(path, sizeof(path), "%s/%s.resume", st->dir, hex);
	remove(path);
	return 0;
}

/* Point out[] at the strings of the list named key. */
static int
url_list(struct mbt_be_value *resume, const char *key, const char **out)
{
	struct mbt_be_value *list = resume ? mbt_be_find(resume, key) : NULL;
	struct mbt_be_value *v;
	int n = 0;

	if (!list || list->type != BE_LIST)
		return 0;
	TAILQ_FOREACH(v, &list->listhead, next)
		if (v->type == BE_STRING && n < MAX_SAVED_URLS)
			out[n++] = (const char *)v->string.buf;
	return n;
}

static int
state_enumerate_impl(void *ctx,
                     int (*cb)(const struct mbt_state_entry *e, void *arg),
                     void *arg)
{
	struct mbt_state *st = ctx;

	DIR *d = opendir(st->dir);
	if (!d) return -errno;

	struct dirent *de;
	int rv = 0;
	while ((de = readdir(d)) != NULL) {
		size_t nlen = strlen(de->d_name);
		if (nlen < 8 || strcmp(de->d_name + nlen - 8, ".torrent") != 0)
			continue;

		/* Read .torrent */
		char tpath[512];
		snprintf(tpath, sizeof(tpath), "%s/%s", st->dir, de->d_name);
		FILE *fp = fopen(tpath, "rb");
		if (!fp) continue;
		fseek(fp, 0, SEEK_END);
		long tlen = ftell(fp);
		rewind(fp);
		if (tlen <= 0) { fclose(fp); continue; }
		void *tbuf = malloc(tlen);
		if (!tbuf) { fclose(fp); rv = -ENOMEM; break; }
		size_t got = fread(tbuf, 1, (size_t)tlen, fp);
		fclose(fp);
		if (got != (size_t)tlen) { free(tbuf); continue; }

		/* Read paired .resume */
		char rpath[512];
		snprintf(rpath, sizeof(rpath), "%s/%.*s.resume",
		         st->dir, (int)(nlen - 8), de->d_name);
		const char *data_dir = NULL;
		int64_t added = 0;
		_cleanup_be_ struct mbt_be_value *resume = NULL;
		FILE *rfp = fopen(rpath, "rb");
		if (rfp) {
			fseek(rfp, 0, SEEK_END);
			long rlen = ftell(rfp);
			rewind(rfp);
			if (rlen > 0) {
				void *rbuf = malloc(rlen);
				if (rbuf) {
					if (fread(rbuf, 1, (size_t)rlen, rfp) == (size_t)rlen)
						mbt_be_value_parse(rbuf, rlen, &resume);
					free(rbuf);
				}
			}
			fclose(rfp);
		}
		if (resume) {
			struct mbt_be_value *v = mbt_be_find(resume, "data_dir");
			if (v && v->type == BE_STRING)
				data_dir = (const char *)v->string.buf;
			v = mbt_be_find(resume, "added");
			if (v && v->type == BE_INTEGER)
				added = v->integer;
		}

		const char *trackers[MAX_SAVED_URLS], *webseeds[MAX_SAVED_URLS];
		struct mbt_state_entry entry = {
			.metadata      = tbuf,
			.metadata_len  = (size_t)tlen,
			.data_dir      = data_dir,
			.added         = added,
			.trackers      = trackers,
			.tracker_count = url_list(resume, "trackers", trackers),
			.webseeds      = webseeds,
			.webseed_count = url_list(resume, "webseeds", webseeds),
		};
		rv = cb(&entry, arg);
		free(tbuf);
		if (rv < 0) break;
		rv = 0;
	}

	closedir(d);
	return rv;
}

static void
state_close_impl(void *ctx)
{
	mbt_state_close(ctx);
}

static const struct mbt_state_ops file_state_ops = {
	.enumerate = state_enumerate_impl,
	.save      = state_save_impl,
	.save_sync = state_save_sync_impl,
	.remove    = state_remove_impl,
	.close     = state_close_impl,
};

const struct mbt_state_ops *
mbt_state_file_ops(void)
{
	return &file_state_ops;
}
