// SPDX-License-Identifier: ISC
/* Bencode and metainfo parsing of truncated, large and hostile input. */
#include <assert.h>
#include <errno.h>
#include <sys/types.h>
#include <sha1.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "mbt/be.h"
#include "mbt/event.h"
#include "mbt/infohash.h"
#include "mbt/metainfo.h"
#include "mbt/session.h"
#include "mbt/storage.h"
#include "mbt/torrent.h"
#include "test_util.h"

#define BIG_STR (2 * 1000 * 1000)
#define MANY_PIECES 60000
#define HASH_LEN 20
#define LONG_COMPONENT 1000
#define TOO_MANY_COMPONENTS 8

/* Parse from an exact allocation, so ASan and valgrind see any over-read. */
static int
parse(const void *data, size_t len, struct mbt_be_value **out)
{
	unsigned char *in = malloc(len ? len : 1);
	int rv;

	assert(in);
	memcpy(in, data, len);
	*out = NULL;
	rv = mbt_be_value_parse(in, len, out);
	free(in);
	return rv;
}

static int
parse_mi(const void *data, size_t len, struct mbt_metainfo **out)
{
	unsigned char *in = malloc(len ? len : 1);
	int rv;

	assert(in);
	memcpy(in, data, len);
	*out = NULL;
	rv = mbt_metainfo_parse(out, in, len);
	free(in);
	return rv;
}

static void
assert_rejected(const char *s)
{
	struct mbt_be_value *be;

	assert(parse(s, strlen(s), &be) < 0);
	assert(be == NULL);
}

static void
test_truncated(void)
{
	static const char whole[] = "d1:ali1e2:xye1:bd1:c3:xyzee";
	struct mbt_be_value *be;

	assert_rejected("5:abc");
	assert_rejected("10:abc");
	assert_rejected("l");
	assert_rejected("d");
	assert_rejected("li1e");
	assert_rejected("l5:abc");
	assert_rejected("d1:ai1e");
	assert_rejected("d1:a5:abc");
	for (size_t n = 0; n + 1 < strlen(whole); n++) {
		assert(parse(whole, n, &be) < 0);
		assert(be == NULL);
	}
	assert(parse(whole, strlen(whole), &be) == (int)strlen(whole));
	mbt_be_unref(be);
}

static void
test_big_string(void)
{
	size_t len = BIG_STR + 16;
	char *s = malloc(len);
	struct mbt_be_value *be;
	int hdr, rv;

	assert(s);
	hdr = snprintf(s, len, "%d:", BIG_STR);
	memset(s + hdr, 'x', BIG_STR);
	rv = parse(s, (size_t)hdr + BIG_STR, &be);
	assert(rv == hdr + BIG_STR);
	assert(be->type == BE_STRING && be->string.len == BIG_STR);
	mbt_be_unref(be);
	free(s);
}

static void
test_value_span(void)
{
	static const char s[] = "d1:ai1e4:infod1:xi1eee";
	struct mbt_be_value *be, *info;

	assert(parse(s, strlen(s), &be) == (int)strlen(s));
	info = mbt_be_find(be, "info");
	assert(info);
	assert(info->src_len == strlen("d1:xi1ee"));
	assert(memcmp(s + info->src_off, "d1:xi1ee", info->src_len) == 0);
	mbt_be_unref(be);
}

static void
test_info_marker_in_comment(void)
{
	static const char info[] =
	    "d6:lengthi4e4:name1:a12:piece lengthi16384e6:pieces20:"
	    "aaaaaaaaaaaaaaaaaaaae";
	char s[256];
	struct mbt_metainfo *mi;
	const void *raw;
	size_t raw_len = 0;
	int n;

	n = snprintf(s, sizeof(s), "d7:comment11:4:infod1:xe4:info%se", info);
	assert(n > 0 && (size_t)n < sizeof(s));
	assert(parse_mi(s, (size_t)n, &mi) == 0);
	raw = mbt_metainfo_raw_info(mi, &raw_len);
	assert(raw_len == strlen(info));
	assert(memcmp(raw, info, raw_len) == 0);
	mbt_metainfo_free(mi);
}

/* The infohash covers the bytes as sent, also with keys out of order. */
static void
test_info_keys_unsorted(void)
{
	static const char info[] =
	    "d6:pieces20:aaaaaaaaaaaaaaaaaaaa6:lengthi4e12:piece lengthi16384e4:name1:ae";
	char s[256];
	struct mbt_metainfo *mi;
	const void *raw;
	size_t raw_len = 0;
	int n;

	n = snprintf(s, sizeof(s), "d4:info%se", info);
	assert(n > 0 && (size_t)n < sizeof(s));
	assert(parse_mi(s, (size_t)n, &mi) == 0);
	raw = mbt_metainfo_raw_info(mi, &raw_len);
	assert(raw_len == strlen(info));
	assert(memcmp(raw, info, raw_len) == 0);
	mbt_metainfo_free(mi);
}

static void
test_many_pieces(void)
{
	size_t plen = (size_t)MANY_PIECES * HASH_LEN;
	char *s = malloc(plen + 256);
	struct mbt_metainfo *mi;
	int n;

	assert(s);
	n = sprintf(s, "d4:infod6:lengthi%llue4:name1:a12:piece lengthi16384e6:pieces%zu:",
	    (unsigned long long)MANY_PIECES * 16384, plen);
	memset(s + n, 'h', plen);
	memcpy(s + n + plen, "ee", 2);
	assert(parse_mi(s, n + plen + 2, &mi) == 0);
	assert(mbt_metainfo_piece_count(mi) == MANY_PIECES);
	mbt_metainfo_free(mi);
	free(s);
}

static void
assert_bad_files(const char *files)
{
	char s[1024];
	struct mbt_metainfo *mi;
	int n;

	n = snprintf(s, sizeof(s), "d4:infod5:filesl%se4:name1:a12:piece lengthi16384e"
	    "6:pieces20:aaaaaaaaaaaaaaaaaaaaee", files);
	assert(n > 0 && (size_t)n < sizeof(s));
	assert(parse_mi(s, (size_t)n, &mi) == -EINVAL);
	assert(mi == NULL);
}

static void
test_bad_file_entries(void)
{
	assert_bad_files("d6:lengthi1e4:pathli1eee");
	assert_bad_files("d6:lengthi1e4:pathlleee");
	assert_bad_files("d6:length1:x4:pathl1:aee");
	assert_bad_files("d6:lengthi-1e4:pathl1:aee");
	assert_bad_files("d4:pathl1:aee");
	assert_bad_files("d6:lengthi1e4:path1:ae");
	assert_bad_files("d6:lengthi1ee");
}

/* A joined path over the buffer in the parser. */
static void
test_path_too_long(void)
{
	size_t cap = (LONG_COMPONENT + 8) * TOO_MANY_COMPONENTS + 256;
	char *s = malloc(cap);
	struct mbt_metainfo *mi;
	size_t n;

	assert(s);
	n = (size_t)sprintf(s, "d4:infod5:filesld6:lengthi1e4:pathl");
	for (int i = 0; i < TOO_MANY_COMPONENTS; i++) {
		n += (size_t)sprintf(s + n, "%d:", LONG_COMPONENT);
		memset(s + n, 'p', LONG_COMPONENT);
		n += LONG_COMPONENT;
	}
	n += (size_t)sprintf(s + n, "eee4:name1:a12:piece lengthi16384e"
	    "6:pieces20:aaaaaaaaaaaaaaaaaaaaee");
	assert(parse_mi(s, n, &mi) == -EINVAL);
	free(s);
}

static void
test_info_not_dict(void)
{
	static const char s[] = "d4:info3:abce";
	struct mbt_metainfo *mi;

	assert(parse_mi(s, strlen(s), &mi) == -EINVAL);
	assert(parse_mi("xxd4:infod1:xi1eee", 18, &mi) == -EINVAL);
}

static int
set_metadata(const char *info)
{
	static const uint8_t peer_id[20];
	struct mbt_infohash *ih = NULL;
	struct mbt_torrent *t = NULL;
	struct mbt_ev *ev = NULL;
	int rv;

	assert(mbt_ev_new_default(&ev) == 0);
	assert(mbt_infohash_new(&ih, MBT_INFOHASH_SHA1,
	    "ABABABABABABABABABABABABABABABABABABABAB") == 0);
	struct mbt_torrent_config cfg = {.ev = ev, .ih = ih, .peer_id = peer_id};
	assert(mbt_torrent_new(&t, &cfg) == 0);
	rv = mbt_torrent_set_metadata(t, info, strlen(info));
	assert(rv == 0 || !mbt_torrent_has_metadata(t));
	assert(rv == 0 || mbt_torrent_file_count(t) == 0);
	mbt_torrent_free(t);
	mbt_infohash_unref(ih);
	mbt_ev_destroy(ev);
	return rv;
}

#define HASHES "6:pieces20:aaaaaaaaaaaaaaaaaaaa"

static void
test_torrent_metadata(void)
{
	assert(set_metadata("d6:lengthi4e4:name1:a12:piece lengthi16384e" HASHES "e") == 0);
	/* no pieces for the length */
	assert(set_metadata("d6:lengthi99999e4:name1:a12:piece lengthi16384e" HASHES "e") == -EINVAL);
	/* more pieces than the length needs */
	assert(set_metadata("d6:lengthi4e4:name1:a12:piece lengthi16384e6:pieces40:"
	    "aaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaae") == -EINVAL);
	assert(set_metadata("d6:lengthi4e4:name1:a12:piece lengthi0e" HASHES "e") == -EINVAL);
	assert(set_metadata("d6:lengthi4e4:name1:a12:piece lengthi-1e" HASHES "e") == -EINVAL);
	assert(set_metadata("d6:lengthi-4e4:name1:a12:piece lengthi16384e" HASHES "e") == -EINVAL);
	assert(set_metadata("d6:lengthi4e4:name1:a12:piece lengthi16384e6:pieces7:aaaaaaae") == -EINVAL);
	assert(set_metadata("d5:filesl1:xe4:name1:a12:piece lengthi16384e" HASHES "e") == -EINVAL);
	assert(set_metadata("d5:filesld6:lengthi-1e4:pathl1:aeee4:name1:a"
	    "12:piece lengthi16384e" HASHES "e") == -EINVAL);
	assert(set_metadata("d5:filesld6:lengthi1e4:pathli1eeee4:name1:a"
	    "12:piece lengthi16384e" HASHES "e") == -EINVAL);
}

/* A string that looks like the pieces key does not move the hash table. */
static void
test_torrent_pieces_marker(void)
{
	static const char data[] = "abcd";
	uint8_t hash[HASH_LEN];
	char enc[256];
	size_t n = 0;
	SHA1_CTX ctx;
	struct mbt_ev *ev;
	struct mbt_session *s;
	struct mbt_torrent *t = NULL;

	SHA1Init(&ctx);
	SHA1Update(&ctx, (const uint8_t *)data, 4);
	SHA1Final(hash, &ctx);

	n += (size_t)sprintf(enc + n, "d4:infod1:a31:6:pieces20:bbbbbbbbbbbbbbbbbbbb"
	    "6:lengthi4e4:name1:a12:piece lengthi16384e6:pieces20:");
	memcpy(enc + n, hash, HASH_LEN);
	n += HASH_LEN;
	memcpy(enc + n, "ee", 2);
	n += 2;

	struct mbt_storage_memory *m = mbt_storage_memory_new();
	assert(mbt_storage_memory_put_file(m, 0, data, 4) == 0);
	assert(mbt_ev_new_default(&ev) == 0);
	struct mbt_session_config cfg = {
		.ev = ev, .default_storage = mbt_storage_memory_storage(m),
	};
	assert(mbt_session_new(&s, &cfg) == 0);
	assert(mbt_session_add_torrent_buf(s, enc, n, NULL, &t) == 0);
	test_wait_verified(ev, t);
	assert(mbt_torrent_complete(t));
	mbt_session_free(s);
	mbt_storage_memory_destroy(m);
	mbt_ev_destroy(ev);
}

int
main(void)
{
	test_truncated();
	test_big_string();
	test_value_span();
	test_info_marker_in_comment();
	test_info_keys_unsorted();
	test_many_pieces();
	test_bad_file_entries();
	test_path_too_long();
	test_info_not_dict();
	test_torrent_metadata();
	test_torrent_pieces_marker();
	fprintf(stderr, "bencode_test: PASS\n");
	return 0;
}
