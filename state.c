// SPDX-License-Identifier: ISC
#include <sys/stat.h>
#include <dirent.h>
#include <stdlib.h>
#include <string.h>
#include <stdio.h>
#include <errno.h>
#include <time.h>
#include <stdint.h>

#include "mbt/state.h"
#include "mbt/torrent.h"
#include "mbt/infohash.h"
#include "mbt/be.h"

struct mbt_torrent;

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

static int
state_save_impl(void *ctx, struct mbt_torrent *t)
{
	struct mbt_state *st = ctx;

	struct mbt_infohash *ih = mbt_torrent_infohash(t);
	if (!ih) return 0;

	char hex[MBT_MBT_INFOHASH_SHA1HEXLEN + 1];
	mbt_infohash_string(ih, hex, sizeof(hex));

	/* Write <hex>.torrent if we have metadata */
	size_t meta_len = 0;
	const void *meta = mbt_torrent_metadata(t, &meta_len);
	if (meta && meta_len > 0) {
		char tmp[512], dst[512];
		snprintf(tmp, sizeof(tmp), "%s/%s.torrent.tmp", st->dir, hex);
		snprintf(dst, sizeof(dst), "%s/%s.torrent", st->dir, hex);
		FILE *fp = fopen(tmp, "wb");
		if (!fp) return -errno;
		fwrite(meta, 1, meta_len, fp);
		fclose(fp);
		if (rename(tmp, dst) < 0) return -errno;
	}

	/* Write <hex>.resume (bencode dict: data_dir, added) */
	const char *data_dir = mbt_torrent_data_dir(t);
	if (!data_dir) return 0;

	_cleanup_be_ struct mbt_be_value *resume = NULL;
	if (mbt_be_dict_new(&resume) < 0) return -ENOMEM;
	mbt_be_dict_append_string(resume, "data_dir", data_dir, strlen(data_dir));
	mbt_be_dict_append_number(resume, "added", (int64_t)time(NULL));

	unsigned char *enc = NULL;
	size_t enc_len = 0;
	if (mbt_be_value_encode(resume, &enc, &enc_len) < 0) return -ENOMEM;

	char tmp[512], dst[512];
	snprintf(tmp, sizeof(tmp), "%s/%s.resume.tmp", st->dir, hex);
	snprintf(dst, sizeof(dst), "%s/%s.resume", st->dir, hex);
	FILE *fp = fopen(tmp, "wb");
	if (!fp) { free(enc); return -errno; }
	fwrite(enc, 1, enc_len, fp);
	fclose(fp);
	free(enc);

	if (rename(tmp, dst) < 0) return -errno;
	return 0;
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
		fread(tbuf, 1, tlen, fp);
		fclose(fp);

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
					fread(rbuf, 1, rlen, rfp);
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

		struct mbt_state_entry entry = {
			.metadata     = tbuf,
			.metadata_len = (size_t)tlen,
			.data_dir     = data_dir,
			.added        = added,
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
	.remove    = state_remove_impl,
	.close     = state_close_impl,
};

const struct mbt_state_ops *
mbt_state_file_ops(void)
{
	return &file_state_ops;
}
