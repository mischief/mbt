/*
 * test/metainfo_test.c — unit test for mbt_metainfo API
 */
#include <assert.h>
#include <stdio.h>
#include <string.h>
#include <stdlib.h>
#include <unistd.h>
#include <fcntl.h>

#include "mbt/metainfo.h"
#include "mbt/infohash.h"

static void
test_create_single_file(void)
{
	/* Create a temp file */
	char path[] = "/tmp/mbt_mi_test_XXXXXX";
	int fd = mkstemp(path);
	assert(fd >= 0);
	uint8_t data[65536];
	for (int i = 0; i < (int)sizeof(data); i++) data[i] = i & 0xff;
	write(fd, data, sizeof(data));
	close(fd);

	struct mbt_metainfo *mi;
	assert(mbt_metainfo_new(&mi) == 0);
	assert(mbt_metainfo_set_content(mi, path, 16384) == 0);

	assert(mbt_metainfo_piece_count(mi) == 4);
	assert(mbt_metainfo_piece_length(mi) == 16384);
	assert(mbt_metainfo_total_length(mi) == 65536);
	assert(mbt_metainfo_file_count(mi) == 1);
	assert(mbt_metainfo_name(mi) != NULL);

	/* Encode */
	unsigned char *buf; size_t len;
	assert(mbt_metainfo_encode(mi, &buf, &len) == 0);
	assert(len > 0);
	assert(mbt_metainfo_infohash(mi) != NULL);
	assert(strlen(mbt_metainfo_infohash_hex(mi)) == 40);

	/* Round-trip: parse the encoded output */
	struct mbt_metainfo *mi2;
	assert(mbt_metainfo_parse(&mi2, buf, len) == 0);
	assert(mbt_metainfo_piece_count(mi2) == 4);
	assert(mbt_metainfo_piece_length(mi2) == 16384);
	assert(mbt_metainfo_total_length(mi2) == 65536);
	assert(mbt_metainfo_file_count(mi2) == 1);
	assert(strcmp(mbt_metainfo_name(mi2), mbt_metainfo_name(mi)) == 0);

	free(buf);
	mbt_metainfo_free(mi);
	mbt_metainfo_free(mi2);
	unlink(path);
}

static void
test_trackers(void)
{
	struct mbt_metainfo *mi;
	mbt_metainfo_new(&mi);

	mbt_metainfo_add_tracker(mi, "udp://tracker1:6969");
	mbt_metainfo_add_tracker(mi, "http://tracker2/announce");
	assert(mbt_metainfo_tracker_count(mi) == 2);
	assert(strcmp(mbt_metainfo_tracker(mi, 0), "udp://tracker1:6969") == 0);
	assert(strcmp(mbt_metainfo_tracker(mi, 1), "http://tracker2/announce") == 0);

	mbt_metainfo_free(mi);
}

static void
test_private(void)
{
	struct mbt_metainfo *mi;
	mbt_metainfo_new(&mi);
	assert(mbt_metainfo_is_private(mi) == 0);
	mbt_metainfo_set_private(mi, 1);
	assert(mbt_metainfo_is_private(mi) == 1);
	mbt_metainfo_free(mi);
}

static void
test_auto_piece_length(void)
{
	/* Create a small file, pass piece_length=0 → auto-select */
	char path[] = "/tmp/mbt_mi_auto_XXXXXX";
	int fd = mkstemp(path);
	uint8_t data[1024];
	memset(data, 'A', sizeof(data));
	write(fd, data, sizeof(data));
	close(fd);

	struct mbt_metainfo *mi;
	mbt_metainfo_new(&mi);
	mbt_metainfo_set_content(mi, path, 0);
	/* 1KB file → should auto-select 16KB pieces */
	assert(mbt_metainfo_piece_length(mi) == 16384);
	assert(mbt_metainfo_piece_count(mi) == 1);

	mbt_metainfo_free(mi);
	unlink(path);
}

static void
test_parse_real_torrent(void)
{
	/* Parse ubuntu torrent from repo test corpus */
	FILE *fp = fopen("../fuzz/corpus/ubuntu-24.04.2-desktop-amd64.iso.torrent", "rb");
	if (!fp) fp = fopen("fuzz/corpus/ubuntu-24.04.2-desktop-amd64.iso.torrent", "rb");
	if (!fp) return; /* skip if not found (e.g. out-of-tree build) */

	unsigned char *buf = malloc(4 * 1024 * 1024);
	size_t len = fread(buf, 1, 4 * 1024 * 1024, fp);
	fclose(fp);

	struct mbt_metainfo *mi;
	assert(mbt_metainfo_parse(&mi, buf, len) == 0);
	assert(strcmp(mbt_metainfo_name(mi), "ubuntu-24.04.2-desktop-amd64.iso") == 0);
	assert(mbt_metainfo_piece_length(mi) == 262144);
	assert(mbt_metainfo_tracker_count(mi) >= 1);
	assert(strlen(mbt_metainfo_infohash_hex(mi)) == 40);

	free(buf);
	mbt_metainfo_free(mi);
}

int main(void)
{
	test_create_single_file();
	test_trackers();
	test_private();
	test_auto_piece_length();
	test_parse_real_torrent();
	fprintf(stderr, "metainfo_test: PASS\n");
	return 0;
}
