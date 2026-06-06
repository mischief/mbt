#include <sys/param.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <err.h>
#include <assert.h>

#include <be.h>

int
main(int argc, char *argv[])
{
	int rv;
	struct be_value *be;

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

	rv = be_value_parse(buf, len, &be);
	if(rv < 0)
		errx(1, "be_value_parse: %s", strerror(-rv));

	be_walk(be, be_pp, NULL);

	be_value_free(be);
	free(buf);
	fclose(fp);

	return 0;
}
