#include <assert.h>
#include <string.h>
#include <stdint.h>

#include "hex.h"

static void
test_bintohex(void)
{
	unsigned char bin[] = { 0xDE, 0xAD, 0xBE, 0xEF };
	char hex[9];
	int rv;

	rv = mbintohex(bin, 4, hex, sizeof(hex));
	assert(rv == 0);
	assert(strcmp(hex, "DEADBEEF") == 0);
}

static void
test_hextobin(void)
{
	unsigned char bin[4];
	int rv;

	rv = mhextobin(bin, sizeof(bin), "DEADBEEF");
	assert(rv == 4);
	assert(bin[0] == 0xDE);
	assert(bin[1] == 0xAD);
	assert(bin[2] == 0xBE);
	assert(bin[3] == 0xEF);

	/* lowercase */
	rv = mhextobin(bin, sizeof(bin), "cafebabe");
	assert(rv == 4);
	assert(bin[0] == 0xCA);
	assert(bin[1] == 0xFE);
	assert(bin[2] == 0xBA);
	assert(bin[3] == 0xBE);
}

static void
test_roundtrip(void)
{
	unsigned char orig[] = { 0x61, 0x1F, 0x70, 0x89, 0x9D, 0x4E, 0x1D, 0x6A,
	                         0x9C, 0x39, 0xCF, 0xC9, 0x25, 0xF1, 0x03, 0xDF,
	                         0xEF, 0x63, 0x03, 0x28 };
	char hex[41];
	unsigned char back[20];

	mbintohex(orig, 20, hex, sizeof(hex));
	assert(strlen(hex) == 40);

	mhextobin(back, sizeof(back), hex);
	assert(memcmp(orig, back, 20) == 0);
}

int
main(void)
{
	test_bintohex();
	test_hextobin();
	test_roundtrip();
	return 0;
}
