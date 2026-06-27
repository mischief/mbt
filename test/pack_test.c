// SPDX-License-Identifier: ISC
#include <assert.h>
#include <string.h>
#include <stdint.h>

#include "mbt/pack.h"

static void
test_pack_byte(void)
{
	unsigned char buf[4];
	struct mbt_packer pk;

	mbt_pack_init(&pk, buf, sizeof(buf));
	mbt_pack_u8(&pk, 0x42);
	assert(mbt_pack_end(&pk) == 1);
	assert(buf[0] == 0x42);

	mbt_pack_init(&pk, buf, sizeof(buf));
	mbt_pack_u8(&pk, 0xAA);
	mbt_pack_u8(&pk, 0xBB);
	assert(mbt_pack_end(&pk) == 2);
	assert(buf[0] == 0xAA);
	assert(buf[1] == 0xBB);
}

static void
test_pack_uint32(void)
{
	unsigned char buf[8];
	struct mbt_packer pk;

	mbt_pack_init(&pk, buf, sizeof(buf));
	mbt_pack_u32(&pk, 0x01020304);
	assert(mbt_pack_end(&pk) == 4);
	assert(buf[0] == 0x01);
	assert(buf[1] == 0x02);
	assert(buf[2] == 0x03);
	assert(buf[3] == 0x04);

	mbt_pack_init(&pk, buf, sizeof(buf));
	mbt_pack_u32(&pk, 1);
	mbt_pack_u32(&pk, 2);
	assert(mbt_pack_end(&pk) == 8);
}

static void
test_pack_raw(void)
{
	unsigned char buf[16];
	struct mbt_packer pk;

	mbt_pack_init(&pk, buf, sizeof(buf));
	mbt_pack_mem(&pk, "abcd", 4);
	assert(mbt_pack_end(&pk) == 4);
	assert(memcmp(buf, "abcd", 4) == 0);
}

static void
test_pack_mixed(void)
{
	unsigned char buf[32];
	struct mbt_packer pk;

	/* BT interested message: length=1, type=2 */
	mbt_pack_init(&pk, buf, sizeof(buf));
	mbt_pack_u32(&pk, 1);
	mbt_pack_u8(&pk, 2);
	assert(mbt_pack_end(&pk) == 5);
	assert(buf[0] == 0 && buf[1] == 0 && buf[2] == 0 && buf[3] == 1);
	assert(buf[4] == 2);
}

static void
test_unpack_byte(void)
{
	unsigned char buf[] = { 0x13, 0xFF };
	struct mbt_unpacker pk;

	mbt_unpack_init(&pk, buf, sizeof(buf));
	uint8_t a = mbt_unpack_u8(&pk);
	uint8_t b = mbt_unpack_u8(&pk);
	assert(mbt_unpack_end(&pk) == 2);
	assert(a == 0x13);
	assert(b == 0xFF);
}

static void
test_unpack_uint32(void)
{
	unsigned char buf[] = { 0x00, 0x01, 0x00, 0x00 };
	struct mbt_unpacker pk;

	mbt_unpack_init(&pk, buf, sizeof(buf));
	uint32_t v = mbt_unpack_u32(&pk);
	assert(mbt_unpack_end(&pk) == 4);
	assert(v == 65536);
}

static void
test_unpack_raw(void)
{
	unsigned char buf[] = { 'h', 'e', 'l', 'l', 'o' };
	unsigned char out[5] = {0};
	struct mbt_unpacker pk;

	mbt_unpack_init(&pk, buf, sizeof(buf));
	mbt_unpack_mem(&pk, out, 5);
	assert(mbt_unpack_end(&pk) == 5);
	assert(memcmp(out, "hello", 5) == 0);
}

static void
test_roundtrip(void)
{
	unsigned char buf[13];
	uint32_t piece = 42, offset = 16384, length = 16384;

	struct mbt_packer pk;
	mbt_pack_init(&pk, buf, sizeof(buf));
	mbt_pack_u8(&pk, 6);
	mbt_pack_u32(&pk, piece);
	mbt_pack_u32(&pk, offset);
	mbt_pack_u32(&pk, length);
	assert(mbt_pack_end(&pk) == 13);

	struct mbt_unpacker upk;
	mbt_unpack_init(&upk, buf, sizeof(buf));
	uint8_t cmd = mbt_unpack_u8(&upk);
	uint32_t p2 = mbt_unpack_u32(&upk);
	uint32_t o2 = mbt_unpack_u32(&upk);
	uint32_t l2 = mbt_unpack_u32(&upk);
	assert(mbt_unpack_end(&upk) == 13);
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
	fprintf(stderr, "pack_test: PASS\n");
	return 0;
}
