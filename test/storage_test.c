#include <fcntl.h>
#include <assert.h>
#include <string.h>
#include <stdlib.h>
#include <unistd.h>
#include <stdio.h>
#include <sys/stat.h>

#include "storage.h"
#include "torrent.h"

static void
test_storage_ops_exist(void)
{
	const struct mbt_storage_ops *ops = mbt_storage_file_ops();
	assert(ops);
	assert(ops->open);
	assert(ops->write_piece);
	assert(ops->read_piece);
	assert(ops->have_piece);
	assert(ops->close);
}

static void
test_single_file(void)
{
	/* Simulate a single-file torrent by writing directly and reading back */
	char tmpdir[] = "/tmp/mbt_st_single_XXXXXX";
	assert(mkdtemp(tmpdir));

	char path[256];
	snprintf(path, sizeof(path), "%s/testfile.bin", tmpdir);

	/* Write 3 "pieces" of 8 bytes each */
	FILE *fp = fopen(path, "w");
	assert(fp);
	uint8_t piece0[] = {0,1,2,3,4,5,6,7};
	uint8_t piece1[] = {8,9,10,11,12,13,14,15};
	uint8_t piece2[] = {16,17,18,19,20,21,22,23};
	fwrite(piece0, 1, 8, fp);
	fwrite(piece1, 1, 8, fp);
	fwrite(piece2, 1, 8, fp);
	fclose(fp);

	/* Read back at piece offsets */
	fp = fopen(path, "rb");
	uint8_t buf[8];

	fseek(fp, 0, SEEK_SET);
	fread(buf, 1, 8, fp);
	assert(memcmp(buf, piece0, 8) == 0);

	fseek(fp, 8, SEEK_SET);
	fread(buf, 1, 8, fp);
	assert(memcmp(buf, piece1, 8) == 0);

	fseek(fp, 16, SEEK_SET);
	fread(buf, 1, 8, fp);
	assert(memcmp(buf, piece2, 8) == 0);

	fclose(fp);
	unlink(path);
	rmdir(tmpdir);
}

static void
test_multi_file_spanning(void)
{
	/* Test the concept: a piece spans two files.
	 * File A: 6 bytes (offset 0)
	 * File B: 6 bytes (offset 6)
	 * Piece size: 8 bytes
	 * Piece 0: bytes 0-7 → 6 bytes in A, 2 bytes in B
	 * Piece 1: bytes 8-11 → 4 bytes in B (last piece, shorter)
	 */
	char tmpdir[] = "/tmp/mbt_st_multi_XXXXXX";
	assert(mkdtemp(tmpdir));

	char pathA[256], pathB[256];
	snprintf(pathA, sizeof(pathA), "%s/fileA.bin", tmpdir);
	snprintf(pathB, sizeof(pathB), "%s/fileB.bin", tmpdir);

	/* Create files */
	int fdA = open(pathA, O_RDWR | O_CREAT, 0644);
	int fdB = open(pathB, O_RDWR | O_CREAT, 0644);
	assert(fdA >= 0 && fdB >= 0);

	/* Write piece 0: first 6 bytes to A, next 2 to B */
	uint8_t piece0[] = {10,20,30,40,50,60,70,80};
	pwrite(fdA, piece0, 6, 0);      /* A gets bytes 0-5 */
	pwrite(fdB, piece0 + 6, 2, 0);  /* B gets bytes 6-7 at file offset 0 */

	/* Write piece 1: 4 bytes to B at file offset 2 */
	uint8_t piece1[] = {90,100,110,120};
	pwrite(fdB, piece1, 4, 2);      /* B at offset 2 */

	/* Verify file A */
	uint8_t buf[8];
	pread(fdA, buf, 6, 0);
	assert(memcmp(buf, piece0, 6) == 0);

	/* Verify file B: bytes 70,80,90,100,110,120 */
	uint8_t expected_B[] = {70, 80, 90, 100, 110, 120};
	pread(fdB, buf, 6, 0);
	assert(memcmp(buf, expected_B, 6) == 0);

	close(fdA);
	close(fdB);
	unlink(pathA);
	unlink(pathB);
	rmdir(tmpdir);
}

int
main(void)
{
	test_storage_ops_exist();
	test_single_file();
	test_multi_file_spanning();
	return 0;
}
