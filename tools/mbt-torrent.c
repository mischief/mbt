/*
 * mbt-torrent: create, inspect, and convert .torrent files
 *
 * Usage:
 *   mbt-torrent create [-p piece_size] [-t tracker] [-o output] <file|dir>
 *   mbt-torrent show <file.torrent>
 *   mbt-torrent magnet <file.torrent>
 */
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>
#include <err.h>
#include <sys/stat.h>
#include <dirent.h>
#include <errno.h>
#include <fcntl.h>

#include <sha1.h>

#include "mbt/be.h"
#include "mbt/hex.h"
#include "mbt/infohash.h"

/* --- show --- */

static int
cmd_show(const char *path)
{
	FILE *fp = fopen(path, "rb");
	if (!fp) err(1, "%s", path);

	unsigned char *buf = malloc(4 * 1024 * 1024);
	size_t len = fread(buf, 1, 4 * 1024 * 1024, fp);
	fclose(fp);

	struct mbt_be_value *be = NULL;
	int rv = mbt_be_value_parse(buf, len, &be);
	if (rv <= 0) errx(1, "parse failed");

	struct mbt_be_value *info = mbt_be_find(be, "info");
	if (!info) errx(1, "no info dict");

	struct mbt_be_value *name = mbt_be_find(info, "name");
	struct mbt_be_value *plen = mbt_be_find(info, "piece length");
	struct mbt_be_value *pieces = mbt_be_find(info, "pieces");
	struct mbt_be_value *length = mbt_be_find(info, "length");
	struct mbt_be_value *files = mbt_be_find(info, "files");
	struct mbt_be_value *announce = mbt_be_find(be, "announce");

	/* Compute infohash */
	unsigned char *info_start = memmem(buf, len, "4:infod", 7);
	if (!info_start) info_start = memmem(buf, len, "4:info", 6);
	char hexhash[41] = {0};
	if (info_start) {
		info_start += 6;
		struct mbt_be_value *tmp = NULL;
		int ilen = mbt_be_value_parse(info_start, len - (info_start - buf), &tmp);
		if (ilen > 0) {
			SHA1_CTX ctx;
			uint8_t digest[20];
			SHA1Init(&ctx);
			SHA1Update(&ctx, info_start, ilen);
			SHA1Final(digest, &ctx);
			mbt_hex_encode(digest, 20, hexhash, sizeof(hexhash));
		}
		mbt_be_value_free(tmp);
	}

	printf("Name:       %.*s\n", (int)(name ? name->string.iov_len : 0),
	       name ? (char *)name->string.iov_base : "?");
	printf("Hash:       %s\n", hexhash);
	if (plen) printf("Piece size: %lld (%lld KiB)\n", (long long)plen->integer, (long long)plen->integer / 1024);
	if (pieces) printf("Pieces:     %zu\n", pieces->string.iov_len / 20);

	if (length) {
		printf("Size:       %lld bytes\n", (long long)length->integer);
	} else if (files) {
		int64_t total = 0;
		int nfiles = 0;
		struct mbt_be_value *f;
		STAILQ_FOREACH(f, &files->listhead, next) {
			struct mbt_be_value *fl = mbt_be_find(f, "length");
			if (fl) total += (long long)fl->integer;
			nfiles++;
		}
		printf("Files:      %d\n", nfiles);
		printf("Total size: %lld bytes\n", (long long)total);
		STAILQ_FOREACH(f, &files->listhead, next) {
			struct mbt_be_value *fl = mbt_be_find(f, "length");
			struct mbt_be_value *fp2 = mbt_be_find(f, "path");
			if (fp2) {
				struct mbt_be_value *comp;
				printf("  ");
				STAILQ_FOREACH(comp, &fp2->listhead, next) {
					printf("%.*s/", (int)comp->string.iov_len, (char *)comp->string.iov_base);
				}
				if (fl) printf(" (%lld)", (long long)fl->integer);
				printf("\n");
			}
		}
	}

	if (announce)
		printf("Announce:   %.*s\n", (int)announce->string.iov_len, (char *)announce->string.iov_base);

	/* announce-list */
	struct mbt_be_value *al = mbt_be_find(be, "announce-list");
	if (al) {
		struct mbt_be_value *tier;
		STAILQ_FOREACH(tier, &al->listhead, next) {
			struct mbt_be_value *url;
			STAILQ_FOREACH(url, &tier->listhead, next) {
				printf("Tracker:    %.*s\n", (int)url->string.iov_len, (char *)url->string.iov_base);
			}
		}
	}

	mbt_be_value_free(be);
	free(buf);
	return 0;
}

/* --- magnet --- */

static int
cmd_magnet(const char *path)
{
	FILE *fp = fopen(path, "rb");
	if (!fp) err(1, "%s", path);

	unsigned char *buf = malloc(4 * 1024 * 1024);
	size_t len = fread(buf, 1, 4 * 1024 * 1024, fp);
	fclose(fp);

	struct mbt_be_value *be = NULL;
	int rv = mbt_be_value_parse(buf, len, &be);
	if (rv <= 0) errx(1, "parse failed");

	struct mbt_be_value *info = mbt_be_find(be, "info");
	if (!info) errx(1, "no info dict");
	struct mbt_be_value *name = mbt_be_find(info, "name");

	/* Compute infohash */
	unsigned char *info_start = memmem(buf, len, "4:infod", 7);
	if (!info_start) info_start = memmem(buf, len, "4:info", 6);
	if (!info_start) errx(1, "cannot find info dict");
	info_start += 6;

	struct mbt_be_value *tmp = NULL;
	int ilen = mbt_be_value_parse(info_start, len - (info_start - buf), &tmp);
	if (ilen <= 0) errx(1, "cannot parse info dict");
	mbt_be_value_free(tmp);

	SHA1_CTX ctx;
	uint8_t digest[20];
	SHA1Init(&ctx);
	SHA1Update(&ctx, info_start, ilen);
	SHA1Final(digest, &ctx);

	char hex[41];
	mbt_hex_encode(digest, 20, hex, sizeof(hex));

	printf("magnet:?xt=urn:btih:%s", hex);
	if (name)
		printf("&dn=%.*s", (int)name->string.iov_len, (char *)name->string.iov_base);

	/* Add trackers */
	struct mbt_be_value *announce = mbt_be_find(be, "announce");
	if (announce)
		printf("&tr=%.*s", (int)announce->string.iov_len, (char *)announce->string.iov_base);

	struct mbt_be_value *al = mbt_be_find(be, "announce-list");
	if (al) {
		struct mbt_be_value *tier;
		STAILQ_FOREACH(tier, &al->listhead, next) {
			struct mbt_be_value *url;
			STAILQ_FOREACH(url, &tier->listhead, next) {
				printf("&tr=%.*s", (int)url->string.iov_len, (char *)url->string.iov_base);
			}
		}
	}
	printf("\n");

	mbt_be_value_free(be);
	free(buf);
	return 0;
}

/* --- create --- */

struct file_entry {
	char *path;      /* relative path */
	int64_t size;
};

static int
collect_files(const char *base, const char *rel, struct file_entry **files, int *count)
{
	char fullpath[4096];
	struct stat st;

	snprintf(fullpath, sizeof(fullpath), "%s/%s", base, rel);
	if (stat(fullpath, &st) < 0) return -errno;

	if (S_ISREG(st.st_mode)) {
		*files = realloc(*files, (*count + 1) * sizeof(**files));
		(*files)[*count].path = strdup(rel);
		(*files)[*count].size = st.st_size;
		(*count)++;
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
			collect_files(base, subrel, files, count);
		}
		closedir(d);
	}
	return 0;
}

static int
cmd_create(int argc, char **argv)
{
	const char *output = NULL;
	const char *tracker = NULL;
	int piece_size = 256 * 1024;
	int ch;

	optind = 1;
	while ((ch = getopt(argc, argv, "o:t:p:")) != -1) {
		switch (ch) {
		case 'o': output = optarg; break;
		case 't': tracker = optarg; break;
		case 'p': piece_size = atoi(optarg) * 1024; break;
		default:
			fprintf(stderr, "create: [-o out.torrent] [-t tracker] [-p piece_kib] <path>\n");
			return 1;
		}
	}
	argc -= optind; argv += optind;
	if (argc < 1) { fprintf(stderr, "create: need a file or directory\n"); return 1; }

	const char *input = argv[0];
	struct stat st;
	if (stat(input, &st) < 0) err(1, "%s", input);

	int single = S_ISREG(st.st_mode);
	const char *name = strrchr(input, '/');
	name = name ? name + 1 : input;

	/* Collect files */
	struct file_entry *files = NULL;
	int nfiles = 0;
	if (single) {
		files = malloc(sizeof(*files));
		files[0].path = strdup(name);
		files[0].size = st.st_size;
		nfiles = 1;
	} else {
		collect_files(input, "", &files, &nfiles);
	}

	/* Compute total size and pieces hash */
	int64_t total = 0;
	for (int i = 0; i < nfiles; i++) total += files[i].size;
	int npieces = (total + piece_size - 1) / piece_size;

	unsigned char *pieces_hash = malloc(npieces * 20);
	unsigned char *piecebuf = malloc(piece_size);
	int piece_idx = 0;
	int piece_off = 0;
	SHA1_CTX ctx;
	SHA1Init(&ctx);

	for (int i = 0; i < nfiles; i++) {
		char fullpath[4096];
		if (single)
			snprintf(fullpath, sizeof(fullpath), "%s", input);
		else
			snprintf(fullpath, sizeof(fullpath), "%s/%s", input, files[i].path);

		int fd = open(fullpath, O_RDONLY);
		if (fd < 0) err(1, "%s", fullpath);

		int64_t remain = files[i].size;
		while (remain > 0) {
			int want = piece_size - piece_off;
			if (want > remain) want = remain;
			ssize_t n = read(fd, piecebuf + piece_off, want);
			if (n <= 0) err(1, "read %s", fullpath);
			SHA1Update(&ctx, piecebuf + piece_off, n);
			piece_off += n;
			remain -= n;

			if (piece_off == piece_size) {
				SHA1Final(pieces_hash + piece_idx * 20, &ctx);
				piece_idx++;
				piece_off = 0;
				SHA1Init(&ctx);
			}
		}
		close(fd);
	}
	/* Final partial piece */
	if (piece_off > 0) {
		SHA1Final(pieces_hash + piece_idx * 20, &ctx);
		piece_idx++;
	}

	/* Build bencode */
	struct mbt_be_value *root = NULL, *info = NULL;
	mbt_be_dict_new(&root);
	mbt_be_dict_new(&info);

	if (tracker) {
		mbt_be_dict_append_string(root, "announce", tracker, strlen(tracker));
	}

	mbt_be_dict_append(root, "info", info);
	mbt_be_dict_append_number(info, "piece length", piece_size);

	/* pieces (binary string) */
	mbt_be_dict_append_string(info, "pieces", pieces_hash, piece_idx * 20);

	/* name */
	mbt_be_dict_append_string(info, "name", name, strlen(name));

	if (single) {
		mbt_be_dict_append_number(info, "length", total);
	} else {
		/* files list */
		struct mbt_be_value *flist;
		mbt_be_list_new(&flist);
		for (int i = 0; i < nfiles; i++) {
			struct mbt_be_value *fdict = NULL;
			mbt_be_dict_new(&fdict);
			mbt_be_dict_append_number(fdict, "length", files[i].size);

			/* path list */
			struct mbt_be_value *plist;
			mbt_be_list_new(&plist);

			char *p = files[i].path;
			char *tok, *saveptr;
			for (tok = strtok_r(p, "/", &saveptr); tok; tok = strtok_r(NULL, "/", &saveptr)) {
				struct mbt_be_value *comp;
				mbt_be_string_new(&comp, tok, strlen(tok));
				mbt_be_list_append(plist, comp);
			}
			mbt_be_dict_append(fdict, "path", plist);
			mbt_be_list_append(flist, fdict);
		}
		mbt_be_dict_append(info, "files", flist);
	}

	/* Encode and write */
	unsigned char *encoded;
	size_t enc_len;
	if (mbt_be_value_encode(root, &encoded, &enc_len) < 0)
		errx(1, "encode failed");

	/* If tracker, patch it in (our API doesn't have string append yet) */
	/* For now just write without tracker — TODO */

	const char *outpath = output ? output : "out.torrent";
	FILE *fp = fopen(outpath, "wb");
	if (!fp) err(1, "%s", outpath);
	fwrite(encoded, 1, enc_len, fp);
	fclose(fp);

	/* Print summary */
	fprintf(stderr, "Created %s: %s (%lld bytes, %d pieces)\n",
	        outpath, name, (long long)total, piece_idx);

	free(encoded);
	free(piecebuf);
	/* Don't free pieces_hash/name — owned by bencode tree */
	free(files);
	return 0;
}

/* --- main --- */

static void
usage(void)
{
	fprintf(stderr, "usage: mbt-torrent <command> [args]\n");
	fprintf(stderr, "  create [-o out.torrent] [-t tracker] [-p piece_kib] <file|dir>\n");
	fprintf(stderr, "  show <file.torrent>\n");
	fprintf(stderr, "  magnet <file.torrent>\n");
	exit(1);
}

int
main(int argc, char *argv[])
{
	if (argc < 2) usage();

	if (strcmp(argv[1], "create") == 0)
		return cmd_create(argc - 1, argv + 1);
	else if (strcmp(argv[1], "show") == 0) {
		if (argc < 3) usage();
		return cmd_show(argv[2]);
	} else if (strcmp(argv[1], "magnet") == 0) {
		if (argc < 3) usage();
		return cmd_magnet(argv[2]);
	} else {
		usage();
	}
	return 1;
}
