#include <assert.h>
#include <string.h>
#include <stdint.h>

#include "mbt/pack.h"

static void
test_pack_byte(void)
{
	unsigned char buf[4];
	int rv;

	rv = mbt_pack(buf, "b", 0x42);
	assert(rv == 1);
	assert(buf[0] == 0x42);

	rv = mbt_pack(buf, "bb", 0xAA, 0xBB);
	assert(rv == 2);
	assert(buf[0] == 0xAA);
	assert(buf[1] == 0xBB);
}

static void
test_pack_uint32(void)
{
	unsigned char buf[8];
	int rv;

	rv = mbt_pack(buf, "l", (uint32_t)0x01020304);
	assert(rv == 4);
	assert(buf[0] == 0x01);
	assert(buf[1] == 0x02);
	assert(buf[2] == 0x03);
	assert(buf[3] == 0x04);

	rv = mbt_pack(buf, "ll", (uint32_t)1, (uint32_t)2);
	assert(rv == 8);
}

static void
test_pack_raw(void)
{
	unsigned char buf[16];
	int rv;

	rv = mbt_pack(buf, "*", (size_t)4, "abcd");
	assert(rv == 4);
	assert(memcmp(buf, "abcd", 4) == 0);
}

static void
test_pack_mixed(void)
{
	unsigned char buf[32];
	int rv;

	/* BT interested message: length=1, type=2 */
	rv = mbt_pack(buf, "lb", (uint32_t)1, 2);
	assert(rv == 5);
	assert(buf[0] == 0 && buf[1] == 0 && buf[2] == 0 && buf[3] == 1);
	assert(buf[4] == 2);
}

static void
test_unpack_byte(void)
{
	unsigned char buf[] = { 0x13, 0xFF };
	unsigned char a, b;
	int rv;

	rv = mbt_unpack(buf, "bb", &a, &b);
	assert(rv == 2);
	assert(a == 0x13);
	assert(b == 0xFF);
}

static void
test_unpack_uint32(void)
{
	unsigned char buf[] = { 0x00, 0x01, 0x00, 0x00 };
	uint32_t v;
	int rv;

	rv = mbt_unpack(buf, "l", &v);
	assert(rv == 4);
	assert(v == 65536);
}

static void
test_unpack_raw(void)
{
	unsigned char buf[] = { 'h', 'e', 'l', 'l', 'o' };
	unsigned char out[5] = {0};
	int rv;

	rv = mbt_unpack(buf, "*", 5, out);
	assert(rv == 5);
	assert(memcmp(out, "hello", 5) == 0);
}

static void
test_roundtrip(void)
{
	unsigned char buf[32];
	uint32_t piece = 42, offset = 16384, length = 16384;
	uint32_t p2, o2, l2;
	int rv;

	rv = mbt_pack(buf, "blll", 6, piece, offset, length);
	assert(rv == 13);

	unsigned char cmd;
	rv = mbt_unpack(buf, "blll", &cmd, &p2, &o2, &l2);
	assert(rv == 13);
	assert(cmd == 6);
	assert(p2 == 42);
	assert(o2 == 16384);
	assert(l2 == 16384);
}

int
main(void)
{
	test_pack_byte();
	test_pack_uint32();
	test_pack_raw();
	test_pack_mixed();
	test_unpack_byte();
	test_unpack_uint32();
	test_unpack_raw();
	test_roundtrip();
	return 0;
}
