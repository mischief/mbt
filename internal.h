// SPDX-License-Identifier: ISC
/* Internal definitions shared between library source files. Not installed. */
#ifndef MBT_INTERNAL_H
#define MBT_INTERNAL_H

/* BT message IDs (BEP 3) */
#define BT_CHOKE           0
#define BT_UNCHOKE         1
#define BT_INTERESTED      2
#define BT_NOT_INTERESTED  3
#define BT_HAVE            4
#define BT_BITFIELD        5
#define BT_REQUEST         6
#define BT_PIECE           7
#define BT_CANCEL          8
#define BT_PORT            9
#define BT_EXTENSION       20

/* BEP 6 Fast Extension */
#define BT_HAVE_ALL        14
#define BT_HAVE_NONE       15
#define BT_REJECT          16
#define BT_ALLOWED_FAST    17

/* Handshake reserved byte flags */
#define RESERVED_DHT       0x01  /* ext[7] bit 0 */
#define RESERVED_FAST      0x04  /* ext[7] bit 2 */
#define RESERVED_EXTENSION 0x10  /* ext[5] bit 4 */

/* Random number helpers — splitmix64 PRNG seeded once from getentropy */
#include <unistd.h>
#include <stdint.h>

static inline uint64_t mbt_splitmix64(void) {
	static uint64_t state;
	static int seeded;
	if (!seeded) {
		while (getentropy(&state, sizeof(state)) < 0)
			;
		seeded = 1;
	}
	uint64_t z = (state += 0x9e3779b97f4a7c15ULL);
	z = (z ^ (z >> 30)) * 0xbf58476d1ce4e5b9ULL;
	z = (z ^ (z >> 27)) * 0x94d049bb133111ebULL;
	return z ^ (z >> 31);
}

static inline uint32_t mbt_random32(void) {
	return (uint32_t)(mbt_splitmix64() >> 32);
}

static inline uint32_t mbt_random_uniform(uint32_t upper) {
	if (upper <= 1) return 0;
	/* rejection sampling to avoid modulo bias */
	uint32_t min = (uint32_t)(-(uint64_t)upper % upper);
	uint32_t r;
	do { r = mbt_random32(); } while (r < min);
	return r % upper;
}

/* Internal interfaces shared between session.c and torrent.c */
struct mbt_ratelimit;
struct mbt_session;
struct mbt_torrent;
struct mbt_ratelimit *mbt_session_get_upload_limit(struct mbt_session *s);
struct mbt_ratelimit *mbt_session_get_download_limit(struct mbt_session *s);
void mbt_torrent_set_session(struct mbt_torrent *t, struct mbt_session *s);
int mbt_session_max_pieces_per_peer(struct mbt_session *s);
int mbt_session_reqq(struct mbt_session *s);
int mbt_session_max_pipeline(struct mbt_session *s);

#endif
