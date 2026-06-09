// SPDX-License-Identifier: ISC
/*
 * mbt-torrent: create, inspect, and convert .torrent files
 *
 * Usage:
 *   mbt-torrent create [-p piece_kib] [-t tracker] [-o output] <file|dir>
 *   mbt-torrent show <file.torrent>
 *   mbt-torrent magnet <file.torrent>
 */
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>
#include <err.h>
#include <fcntl.h>

#include "mbt/metainfo.h"
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

	struct mbt_metainfo *mi;
	if (mbt_metainfo_parse(&mi, buf, len) < 0) errx(1, "parse failed");
	free(buf);

	printf("Name:       %s\n", mbt_metainfo_name(mi));
	printf("Hash:       %s\n", mbt_metainfo_infohash_hex(mi));
	printf("Piece size: %u (%u KiB)\n", mbt_metainfo_piece_length(mi), mbt_metainfo_piece_length(mi) / 1024);
	printf("Pieces:     %u\n", mbt_metainfo_piece_count(mi));
	printf("Size:       %lu bytes\n", (unsigned long)mbt_metainfo_total_length(mi));

	if (mbt_metainfo_file_count(mi) > 1) {
		printf("Files:      %d\n", mbt_metainfo_file_count(mi));
		for (int i = 0; i < mbt_metainfo_file_count(mi); i++)
			printf("  %s (%lu)\n", mbt_metainfo_file_path(mi, i),
			       (unsigned long)mbt_metainfo_file_length(mi, i));
	}

	for (int i = 0; i < mbt_metainfo_tracker_count(mi); i++)
		printf("Tracker:    %s\n", mbt_metainfo_tracker(mi, i));

	if (mbt_metainfo_comment(mi))
		printf("Comment:    %s\n", mbt_metainfo_comment(mi));
	if (mbt_metainfo_is_private(mi))
		printf("Private:    yes\n");

	mbt_metainfo_free(mi);
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

	struct mbt_metainfo *mi;
	if (mbt_metainfo_parse(&mi, buf, len) < 0) errx(1, "parse failed");
	free(buf);

	printf("magnet:?xt=urn:btih:%s", mbt_metainfo_infohash_hex(mi));
	if (mbt_metainfo_name(mi))
		printf("&dn=%s", mbt_metainfo_name(mi));
	for (int i = 0; i < mbt_metainfo_tracker_count(mi); i++)
		printf("&tr=%s", mbt_metainfo_tracker(mi, i));
	printf("\n");

	mbt_metainfo_free(mi);
	return 0;
}

/* --- create --- */

static int
cmd_create(int argc, char **argv)
{
	const char *output = NULL;
	const char *tracker = NULL;
	int piece_kib = 256;
	int ch;

	optind = 1;
	while ((ch = getopt(argc, argv, "o:t:p:")) != -1) {
		switch (ch) {
		case 'o': output = optarg; break;
		case 't': tracker = optarg; break;
		case 'p': piece_kib = atoi(optarg); break;
		default:
			fprintf(stderr, "create: [-o out.torrent] [-t tracker] [-p piece_kib] <path>\n");
			return 1;
		}
	}
	argc -= optind; argv += optind;
	if (argc < 1) { fprintf(stderr, "create: need a file or directory\n"); return 1; }

	struct mbt_metainfo *mi;
	mbt_metainfo_new(&mi);

	int rv = mbt_metainfo_set_content(mi, argv[0], piece_kib * 1024);
	if (rv < 0) errx(1, "set_content: %s", strerror(-rv));

	if (tracker)
		mbt_metainfo_add_tracker(mi, tracker);

	unsigned char *encoded;
	size_t enc_len;
	rv = mbt_metainfo_encode(mi, &encoded, &enc_len);
	if (rv < 0) errx(1, "encode failed");

	const char *outpath = output ? output : "out.torrent";
	FILE *fp = fopen(outpath, "wb");
	if (!fp) err(1, "%s", outpath);
	fwrite(encoded, 1, enc_len, fp);
	fclose(fp);

	fprintf(stderr, "Created %s: %s (%lu bytes, %u pieces)\n",
	        outpath, mbt_metainfo_name(mi),
	        (unsigned long)mbt_metainfo_total_length(mi),
	        mbt_metainfo_piece_count(mi));

	free(encoded);
	mbt_metainfo_free(mi);
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
	int ch;
	while ((ch = getopt(argc, argv, "h")) != -1) {
		switch (ch) {
		case 'h':
		default:
			usage();
		}
	}
	argc -= optind;
	argv += optind;

	if (argc < 1) usage();

	if (strcmp(argv[0], "create") == 0)
		return cmd_create(argc, argv);
	else if (strcmp(argv[0], "show") == 0) {
		if (argc < 2) usage();
		return cmd_show(argv[1]);
	} else if (strcmp(argv[0], "magnet") == 0) {
		if (argc < 2) usage();
		return cmd_magnet(argv[1]);
	} else {
		usage();
	}
	return 1;
}
