// SPDX-License-Identifier: ISC
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <err.h>

#include "mbt/be.h"

int
main(int argc, char *argv[])
{
	int rv;
	struct mbt_be_value *be;

	(void)argc;
	(void)argv;

	unsigned char *buf;
	size_t len;
	FILE *fp;

	buf = calloc(1, 1024*1024);
	if(!buf)
		err(1, "calloc");

	fp = fopen(argv[1], "rb");
	if(!fp)
		err(1, "fopen");

	len = fread(buf, 1, 1024*1024, fp);
	if(len <= 0)
		err(1, "fread");

	rv = mbt_be_value_parse(buf, len, &be);
	if(rv < 0)
		errx(1, "mbt_be_value_parse: %s", strerror(-rv));

	mbt_be_walk(be, mbt_be_pp, NULL);

	mbt_be_unref(be);
	free(buf);
	fclose(fp);

	return 0;
}
