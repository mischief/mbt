// SPDX-License-Identifier: ISC
/*
 * metainfo.c — .torrent file parsing, creation, and serialization
 */
#define _GNU_SOURCE
#include <sys/stat.h>
#include <dirent.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>
#include <fcntl.h>
#include <errno.h>

#include <sha1.h>

#include "mbt/metainfo.h"
#include "mbt/infohash.h"
#include "mbt/be.h"
#include "mbt/hex.h"

#define MAX_TRACKERS 16
#define MAX_FILES 4096
#define DEFAULT_PIECE_LEN 262144

struct mbt_metainfo_file {
	char *path;
	uint64_t length;
};

struct mbt_metainfo {
	char *name;
	char *comment;
	uint32_t piece_length;
	uint64_t total_length;
	uint8_t *pieces;        /* concatenated SHA1 hashes */
	uint32_t piece_count;
	int is_private;

	struct mbt_metainfo_file *files;
	int file_count;

	char *trackers[MAX_TRACKERS];
	int tracker_count;

	/* Raw info dict (for infohash + BEP 9) */
	uint8_t *raw_info;
	size_t raw_info_len;

	struct mbt_infohash *ih;
	char infohash_hex[41];
};

/* --- Lifecycle --- */

int
mbt_metainfo_new(struct mbt_metainfo **out)
{
	struct mbt_metainfo *mi = calloc(1, sizeof(*mi));
	if (!mi) return -ENOMEM;
	mi->piece_length = DEFAULT_PIECE_LEN;
	*out = mi;
	return 0;
}

void
mbt_metainfo_free(struct mbt_metainfo *mi)
{
	if (!mi) return;
	free(mi->name);
	free(mi->comment);
	free(mi->pieces);
	free(mi->raw_info);
	for (int i = 0; i < mi->file_count; i++)
		free(mi->files[i].path);
	free(mi->files);
	for (int i = 0; i < mi->tracker_count; i++)
		free(mi->trackers[i]);
	if (mi->ih) mbt_infohash_unref(mi->ih);
	free(mi);
}

/* --- Parse --- */

static void
compute_infohash(struct mbt_metainfo *mi)
{
	if (!mi->raw_info || mi->raw_info_len == 0) return;
	SHA1_CTX ctx;
	uint8_t digest[20];
	SHA1Init(&ctx);
	SHA1Update(&ctx, mi->raw_info, mi->raw_info_len);
	SHA1Final(digest, &ctx);
	mbt_hex_encode(digest, 20, mi->infohash_hex, sizeof(mi->infohash_hex));
	if (mi->ih) mbt_infohash_unref(mi->ih);
	mbt_infohash_new(&mi->ih, MBT_INFOHASH_SHA1, mi->infohash_hex);
}

int
mbt_metainfo_parse(struct mbt_metainfo **out, const void *data, size_t len)
{
	struct mbt_metainfo *mi;
	int rv = mbt_metainfo_new(&mi);
	if (rv < 0) return rv;

	const unsigned char *buf = data;

	/* Find raw info dict */
	unsigned char *p = memmem(buf, len, "4:infod", 7);
	if (!p) p = memmem(buf, len, "4:info", 6);
	if (!p) { mbt_metainfo_free(mi); return -EINVAL; }

	unsigned char *info_start = p + 6;
	int depth = 0;
	unsigned char *i = info_start;
	const unsigned char *end = buf + len;
	while (i < end) {
		if (*i == 'd' || *i == 'l') { depth++; i++; }
		else if (*i == 'e') { depth--; i++; if (depth == 0) break; }
		else if (*i == 'i') { i = memchr(i, 'e', end - i); if (i) i++; else break; }
		else if (*i >= '0' && *i <= '9') {
			unsigned char *colon = memchr(i, ':', end - i);
			if (!colon) break;
			size_t slen = strtoul((char *)i, NULL, 10);
			i = colon + 1 + slen;
		} else { break; }
	}
	size_t info_len = (size_t)(i - info_start);
	if (info_len == 0) { mbt_metainfo_free(mi); return -EINVAL; }

	mi->raw_info = malloc(info_len);
	if (!mi->raw_info) { mbt_metainfo_free(mi); return -ENOMEM; }
	memcpy(mi->raw_info, info_start, info_len);
	mi->raw_info_len = info_len;
	compute_infohash(mi);

	/* Parse bencode */
	struct mbt_be_value *be = NULL;
	rv = mbt_be_value_parse(buf, len, &be);
	if (rv <= 0) { mbt_metainfo_free(mi); return -EINVAL; }

	struct mbt_be_value *info = mbt_be_find(be, "info");
	if (!info) { mbt_be_value_free(be); mbt_metainfo_free(mi); return -EINVAL; }

	/* Name */
	struct mbt_be_value *v = mbt_be_find(info, "name");
	if (v && v->type == BE_STRING)
		mi->name = strndup(v->string.iov_base, v->string.iov_len);

	/* Piece length */
	v = mbt_be_find(info, "piece length");
	if (v && v->type == BE_INTEGER)
		mi->piece_length = (uint32_t)v->integer;

	/* Pieces */
	v = mbt_be_find(info, "pieces");
	if (v && v->type == BE_STRING) {
		mi->piece_count = v->string.iov_len / 20;
		mi->pieces = malloc(v->string.iov_len);
		if (mi->pieces)
			memcpy(mi->pieces, v->string.iov_base, v->string.iov_len);
	}

	/* Single file or multi-file */
	v = mbt_be_find(info, "length");
	if (v && v->type == BE_INTEGER) {
		mi->total_length = (uint64_t)v->integer;
		mi->files = calloc(1, sizeof(*mi->files));
		mi->file_count = 1;
		mi->files[0].path = mi->name ? strdup(mi->name) : strdup("unknown");
		mi->files[0].length = mi->total_length;
	} else {
		struct mbt_be_value *files = mbt_be_find(info, "files");
		if (files && files->type == BE_LIST) {
			struct mbt_be_value *f;
			int count = 0;
			STAILQ_FOREACH(f, &files->listhead, next) count++;
			mi->files = calloc(count, sizeof(*mi->files));
			mi->file_count = count;
			int idx = 0;
			STAILQ_FOREACH(f, &files->listhead, next) {
				struct mbt_be_value *fl = mbt_be_find(f, "length");
				struct mbt_be_value *fp = mbt_be_find(f, "path");
				if (fl) mi->files[idx].length = (uint64_t)fl->integer;
				mi->total_length += mi->files[idx].length;
				if (fp && fp->type == BE_LIST) {
					/* Join path components */
					char path[4096] = {0};
					struct mbt_be_value *comp;
					STAILQ_FOREACH(comp, &fp->listhead, next) {
						if (path[0]) strcat(path, "/");
						strncat(path, comp->string.iov_base, comp->string.iov_len);
					}
					mi->files[idx].path = strdup(path);
				}
				idx++;
			}
		}
	}

	/* Private */
	v = mbt_be_find(info, "private");
	if (v && v->type == BE_INTEGER)
		mi->is_private = (int)v->integer;

	/* Trackers */
	struct mbt_be_value *al = mbt_be_find(be, "announce-list");
	if (al && al->type == BE_LIST) {
		struct mbt_be_value *tier;
		STAILQ_FOREACH(tier, &al->listhead, next) {
			if (tier->type != BE_LIST) continue;
			struct mbt_be_value *url;
			STAILQ_FOREACH(url, &tier->listhead, next) {
				if (url->type == BE_STRING && mi->tracker_count < MAX_TRACKERS)
					mi->trackers[mi->tracker_count++] = strndup(url->string.iov_base, url->string.iov_len);
			}
		}
	}
	if (mi->tracker_count == 0) {
		struct mbt_be_value *ann = mbt_be_find(be, "announce");
		if (ann && ann->type == BE_STRING)
			mi->trackers[mi->tracker_count++] = strndup(ann->string.iov_base, ann->string.iov_len);
	}

	/* Comment */
	v = mbt_be_find(be, "comment");
	if (v && v->type == BE_STRING)
		mi->comment = strndup(v->string.iov_base, v->string.iov_len);

	mbt_be_value_free(be);
	*out = mi;
	return 0;
}

/* --- Builder --- */

static int
collect_files(const char *base, const char *rel, struct mbt_metainfo *mi)
{
	char fullpath[4096];
	struct stat st;

	snprintf(fullpath, sizeof(fullpath), "%s/%s", base, rel);
	if (stat(fullpath, &st) < 0) return -errno;

	if (S_ISREG(st.st_mode)) {
		mi->files = realloc(mi->files, (mi->file_count + 1) * sizeof(*mi->files));
		mi->files[mi->file_count].path = strdup(rel);
		mi->files[mi->file_count].length = st.st_size;
		mi->total_length += st.st_size;
		mi->file_count++;
		return 0;
	}

	if (S_ISDIR(st.st_mode)) {
		DIR *d = opendir(fullpath);
		if (!d) return -errno;
		struct dirent *ent;
		while ((ent = readdir(d))) {
			if (ent->d_name[0] == '.') continue;
			char subrel[4096];
			if (rel[0])
				snprintf(subrel, sizeof(subrel), "%s/%s", rel, ent->d_name);
			else
				snprintf(subrel, sizeof(subrel), "%s", ent->d_name);
			collect_files(base, subrel, mi);
		}
		closedir(d);
	}
	return 0;
}

int
mbt_metainfo_set_content(struct mbt_metainfo *mi, const char *path, uint32_t piece_length)
{
	struct stat st;
	if (stat(path, &st) < 0) return -errno;

	if (piece_length > 0) {
		/* Validate: must be power of 2, 16KB–16MB */
		if (piece_length < 16384) piece_length = 16384;
		if (piece_length > 16 * 1024 * 1024) piece_length = 16 * 1024 * 1024;
		/* Round up to next power of 2 */
		uint32_t p = 1;
		while (p < piece_length) p <<= 1;
		mi->piece_length = p;
	} else {
		/* Auto-select based on total size (after scanning files) */
	}
	int single = S_ISREG(st.st_mode);

	const char *name = strrchr(path, '/');
	name = name ? name + 1 : path;
	free(mi->name);
	mi->name = strdup(name);

	/* Collect files */
	mi->total_length = 0;
	if (single) {
		mi->files = calloc(1, sizeof(*mi->files));
		mi->files[0].path = strdup(name);
		mi->files[0].length = st.st_size;
		mi->total_length = st.st_size;
		mi->file_count = 1;
	} else {
		collect_files(path, "", mi);
	}

	/* Auto-select piece length if not specified */
	if (piece_length == 0) {
		if (mi->total_length < 256 * 1024)         mi->piece_length = 16384;
		else if (mi->total_length < 4 * 1024 * 1024)   mi->piece_length = 65536;
		else if (mi->total_length < 64 * 1024 * 1024)  mi->piece_length = 262144;
		else if (mi->total_length < 512 * 1024 * 1024) mi->piece_length = 524288;
		else if (mi->total_length < (uint64_t)2 * 1024 * 1024 * 1024) mi->piece_length = 1048576;
		else mi->piece_length = 2097152;
	}

	/* Compute piece hashes */
	mi->piece_count = (mi->total_length + mi->piece_length - 1) / mi->piece_length;
	mi->pieces = malloc(mi->piece_count * 20);
	uint8_t *piecebuf = malloc(mi->piece_length);
	if (!mi->pieces || !piecebuf) { free(piecebuf); return -ENOMEM; }

	int piece_idx = 0, piece_off = 0;
	SHA1_CTX ctx;
	SHA1Init(&ctx);

	for (int i = 0; i < mi->file_count; i++) {
		char fullpath[4096];
		if (single)
			snprintf(fullpath, sizeof(fullpath), "%s", path);
		else
			snprintf(fullpath, sizeof(fullpath), "%s/%s", path, mi->files[i].path);

		int fd = open(fullpath, O_RDONLY);
		if (fd < 0) { free(piecebuf); return -errno; }

		int64_t remain = mi->files[i].length;
		while (remain > 0) {
			int want = mi->piece_length - piece_off;
			if (want > remain) want = remain;
			ssize_t n = read(fd, piecebuf + piece_off, want);
			if (n <= 0) { close(fd); free(piecebuf); return -EIO; }
			SHA1Update(&ctx, piecebuf + piece_off, n);
			piece_off += n;
			remain -= n;

			if ((uint32_t)piece_off == mi->piece_length) {
				SHA1Final(mi->pieces + piece_idx * 20, &ctx);
				piece_idx++;
				piece_off = 0;
				SHA1Init(&ctx);
			}
		}
		close(fd);
	}
	if (piece_off > 0) {
		SHA1Final(mi->pieces + piece_idx * 20, &ctx);
		piece_idx++;
	}

	free(piecebuf);
	return 0;
}

int mbt_metainfo_add_tracker(struct mbt_metainfo *mi, const char *url)
{
	if (mi->tracker_count >= MAX_TRACKERS) return -ENOSPC;
	mi->trackers[mi->tracker_count++] = strdup(url);
	return 0;
}

void mbt_metainfo_set_comment(struct mbt_metainfo *mi, const char *comment)
{
	free(mi->comment);
	mi->comment = comment ? strdup(comment) : NULL;
}

void mbt_metainfo_set_private(struct mbt_metainfo *mi, int priv)
{
	mi->is_private = priv;
}

/* --- Accessors --- */

const char *mbt_metainfo_name(struct mbt_metainfo *mi) { return mi->name; }
struct mbt_infohash *mbt_metainfo_infohash(struct mbt_metainfo *mi) { return mi->ih; }
uint32_t mbt_metainfo_piece_count(struct mbt_metainfo *mi) { return mi->piece_count; }
uint32_t mbt_metainfo_piece_length(struct mbt_metainfo *mi) { return mi->piece_length; }
uint64_t mbt_metainfo_total_length(struct mbt_metainfo *mi) { return mi->total_length; }
int mbt_metainfo_file_count(struct mbt_metainfo *mi) { return mi->file_count; }
const char *mbt_metainfo_file_path(struct mbt_metainfo *mi, int idx) { return (idx < mi->file_count) ? mi->files[idx].path : NULL; }
uint64_t mbt_metainfo_file_length(struct mbt_metainfo *mi, int idx) { return (idx < mi->file_count) ? mi->files[idx].length : 0; }
int mbt_metainfo_tracker_count(struct mbt_metainfo *mi) { return mi->tracker_count; }
const char *mbt_metainfo_tracker(struct mbt_metainfo *mi, int idx) { return (idx < mi->tracker_count) ? mi->trackers[idx] : NULL; }
int mbt_metainfo_is_private(struct mbt_metainfo *mi) { return mi->is_private; }
const char *mbt_metainfo_comment(struct mbt_metainfo *mi) { return mi->comment; }
const void *mbt_metainfo_raw_info(struct mbt_metainfo *mi, size_t *len) { if (len) *len = mi->raw_info_len; return mi->raw_info; }
const char *mbt_metainfo_infohash_hex(struct mbt_metainfo *mi) { return mi->infohash_hex; }

/* --- Encode --- */

int
mbt_metainfo_encode(struct mbt_metainfo *mi, unsigned char **out, size_t *len)
{
	struct mbt_be_value *root = NULL, *info = NULL;
	mbt_be_dict_new(&root);

	/* Trackers */
	if (mi->tracker_count > 0)
		mbt_be_dict_append_string(root, "announce", mi->trackers[0], strlen(mi->trackers[0]));
	if (mi->tracker_count > 1) {
		struct mbt_be_value *al, *tier;
		mbt_be_list_new(&al);
		mbt_be_list_new(&tier);
		for (int i = 0; i < mi->tracker_count; i++) {
			struct mbt_be_value *url;
			mbt_be_string_new(&url, mi->trackers[i], strlen(mi->trackers[i]));
			mbt_be_list_append(tier, url);
		}
		mbt_be_list_append(al, tier);
		mbt_be_dict_append(root, "announce-list", al);
	}

	if (mi->comment)
		mbt_be_dict_append_string(root, "comment", mi->comment, strlen(mi->comment));

	/* Info dict */
	mbt_be_dict_new(&info);
	if (mi->file_count == 1) {
		mbt_be_dict_append_number(info, "length", mi->total_length);
	} else {
		struct mbt_be_value *flist;
		mbt_be_list_new(&flist);
		for (int i = 0; i < mi->file_count; i++) {
			struct mbt_be_value *fd;
			mbt_be_dict_new(&fd);
			mbt_be_dict_append_number(fd, "length", mi->files[i].length);
			struct mbt_be_value *plist;
			mbt_be_list_new(&plist);
			char *p = strdup(mi->files[i].path);
			char *tok, *saveptr;
			for (tok = strtok_r(p, "/", &saveptr); tok; tok = strtok_r(NULL, "/", &saveptr)) {
				struct mbt_be_value *comp;
				mbt_be_string_new(&comp, tok, strlen(tok));
				mbt_be_list_append(plist, comp);
			}
			free(p);
			mbt_be_dict_append(fd, "path", plist);
			mbt_be_list_append(flist, fd);
		}
		mbt_be_dict_append(info, "files", flist);
	}

	mbt_be_dict_append_string(info, "name", mi->name, strlen(mi->name));
	mbt_be_dict_append_number(info, "piece length", mi->piece_length);
	mbt_be_dict_append_string(info, "pieces", mi->pieces, mi->piece_count * 20);

	if (mi->is_private)
		mbt_be_dict_append_number(info, "private", 1);

	/* Encode info dict separately to compute infohash (before append sets key) */
	if (!mi->raw_info) {
		unsigned char *info_enc;
		size_t info_enc_len;
		if (mbt_be_value_encode(info, &info_enc, &info_enc_len) == 0) {
			mi->raw_info = info_enc;
			mi->raw_info_len = info_enc_len;
			compute_infohash(mi);
		}
	}

	mbt_be_dict_append(root, "info", info);

	int rv = mbt_be_value_encode(root, out, len);
	mbt_be_value_free(root);
	return rv;
}
