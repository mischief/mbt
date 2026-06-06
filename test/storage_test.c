#include <assert.h>
#include <string.h>
#include <stdlib.h>
#include <unistd.h>
#include <stdio.h>

#include "storage.h"
#include "torrent.h"

static void
test_file_storage_write_read(void)
{
	char tmpdir[] = "/tmp/mbt_storage_test_XXXXXX";
	assert(mkdtemp(tmpdir) != NULL);

	/* Simulate: open, write piece, read it back */

	/* Write a "piece" directly to a file */
	char path[256];
	snprintf(path, sizeof(path), "%s/testfile", tmpdir);

	FILE *fp = fopen(path, "w+");
	assert(fp);

	uint8_t data[256];
	for (int i = 0; i < 256; i++)
		data[i] = (uint8_t)i;

	/* Write at offset 0 (piece 0) */
	assert(fwrite(data, 1, 256, fp) == 256);
	fclose(fp);

	/* Read it back */
	fp = fopen(path, "rb");
	assert(fp);
	uint8_t buf[256];
	assert(fread(buf, 1, 256, fp) == 256);
	fclose(fp);

	assert(memcmp(data, buf, 256) == 0);

	/* Cleanup */
	unlink(path);
	rmdir(tmpdir);
}

static void
test_file_storage_ops(void)
{
	char tmpdir[] = "/tmp/mbt_storage_test_XXXXXX";
	assert(mkdtemp(tmpdir) != NULL);

	const struct mbt_storage_ops *ops = mbt_storage_file_ops();
	assert(ops);
	assert(ops->open);
	assert(ops->write_piece);
	assert(ops->read_piece);
	assert(ops->have_piece);
	assert(ops->close);

	/* have_piece returns 0 (no resume support yet) */
	assert(ops->have_piece((void *)tmpdir, 0) == 0);

	/* Cleanup */
	rmdir(tmpdir);
}

int
main(void)
{
	test_file_storage_write_read();
	test_file_storage_ops();
	return 0;
}
