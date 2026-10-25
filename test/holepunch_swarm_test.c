// SPDX-License-Identifier: ISC
/*
 * BEP 55 end to end over real loopback uTP, three sessions:
 *
 *   R  relay, seeder, open: accepts anything.
 *   A  leecher behind a simulated stateful firewall.
 *   C  leecher behind a simulated stateful firewall.
 *
 * The firewall is a pair of uTP hooks on A's and C's contexts: every
 * destination a session sends to is remembered (tx hook), and an inbound
 * datagram is dropped unless its source is one of those (rx hook). That is
 * the endpoint-dependent filtering of pf "keep state" or a NAT's filter.
 *
 * A and C both connect out to R, so R can relay. R's PEX tells A about C.
 * A dials C directly; C's firewall drops the SYN, the connect fails, and A
 * sends a rendezvous to R. R sends connect to both, both send SYNs, each
 * firewall now has state for the other, and the connection forms.
 *
 * C's maintenance timer is pushed out so it never dials A from its own PEX
 * list: otherwise C's direct SYN would pass A's firewall (A already sent to
 * C) and the two would connect without BEP 55, proving nothing. The only
 * way C sends a SYN to A is the holepunch connect from R, which the test
 * checks for.
 */
#include <assert.h>
#include <signal.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <arpa/inet.h>
#include <netinet/in.h>
#include <sha1.h>

#include "mbt/be.h"
#include "mbt/event.h"
#include "mbt/hex.h"
#include "mbt/log.h"
#include "mbt/peer.h"
#include "mbt/session.h"
#include "mbt/storage.h"
#include "mbt/torrent.h"
#include "mbt/utp.h"

#define PIECE_LEN  16384
#define NUM_PIECES 128
#define TOTAL_LEN  (PIECE_LEN * NUM_PIECES)
#define ST_SYN     4
#define MAX_SEEN   64

struct firewall {
	const char *name;
	struct sockaddr_in seen[MAX_SEEN];   /* endpoints we have sent to */
	int nseen;
	int dropped;                        /* inbound datagrams refused */
	uint16_t watch_port;                /* count SYNs sent to this port */
	int syns_to_watch;
};

static struct mbt_ev *ev;

static int
same_ep(const struct sockaddr_in *a, const struct sockaddr *b)
{
	const struct sockaddr_in *bi = (const struct sockaddr_in *)b;
	return b->sa_family == AF_INET && a->sin_port == bi->sin_port &&
	       a->sin_addr.s_addr == bi->sin_addr.s_addr;
}

static int
fw_tx(struct mbt_utp_ctx *ctx, const void *buf, size_t len,
      const struct sockaddr *dst, socklen_t dstlen, void *arg)
{
	struct firewall *fw = arg;
	(void)ctx; (void)dstlen;
	if (dst->sa_family != AF_INET)
		return 1;
	const struct sockaddr_in *d = (const struct sockaddr_in *)dst;
	if (len >= 20 && (((const uint8_t *)buf)[0] >> 4) == ST_SYN &&
	    ntohs(d->sin_port) == fw->watch_port)
		fw->syns_to_watch++;
	for (int i = 0; i < fw->nseen; i++)
		if (same_ep(&fw->seen[i], dst))
			return 1;
	if (fw->nseen < MAX_SEEN)
		fw->seen[fw->nseen++] = *d;
	return 1;
}

static int
fw_rx(struct mbt_utp_ctx *ctx, const void *buf, size_t len,
      const struct sockaddr *src, socklen_t srclen, void *arg)
{
	struct firewall *fw = arg;
	(void)ctx; (void)buf; (void)len; (void)srclen;
	for (int i = 0; i < fw->nseen; i++)
		if (same_ep(&fw->seen[i], src))
			return 1;
	fw->dropped++;
	return 0;
}

/* Is torrent t connected to a peer on 127.0.0.1:port? */
static int
has_peer(struct mbt_torrent *t, uint16_t port)
{
	for (int i = 0; i < mbt_torrent_peer_count(t); i++) {
		struct mbt_peer *p = mbt_torrent_peer_at(t, i);
		if (p && mbt_peer_state(p) == MBT_PEER_ACTIVE &&
		    strcmp(mbt_peer_addr(p), "127.0.0.1") == 0 &&
		    (mbt_peer_port(p) == port || mbt_peer_conn_port(p) == port))
			return 1;
	}
	return 0;
}

struct check {
	struct mbt_torrent *ta, *tc;
	uint16_t pa, pc;
	int ticks;
	int done;
};

static void
check_cb(struct mbt_ev_timer *t, void *arg)
{
	struct check *c = arg;
	if ((has_peer(c->ta, c->pc) && has_peer(c->tc, c->pa)) || ++c->ticks > 300) {
		c->done = 1;
		mbt_ev_stop(ev);
		return;
	}
	mbt_ev_timer_mod(t, 100000);
}

static uint8_t data[TOTAL_LEN];

/* Build a single-file torrent over `data`; returns the encoded metainfo and
 * the hex infohash. */
static unsigned char *
make_torrent(size_t *len, char hex[41])
{
	static uint8_t hashes[NUM_PIECES * 20];
	for (int i = 0; i < TOTAL_LEN; i++)
		data[i] = (uint8_t)(i * 97 + 31);
	for (int i = 0; i < NUM_PIECES; i++) {
		SHA1_CTX ctx;
		SHA1Init(&ctx);
		SHA1Update(&ctx, data + i * PIECE_LEN, PIECE_LEN);
		SHA1Final(hashes + i * 20, &ctx);
	}
	_cleanup_be_ struct mbt_be_value *root = NULL, *info = NULL, *tmp = NULL;
	mbt_be_dict_new(&root);
	mbt_be_dict_new(&info);
	mbt_be_dict_append_number(info, "length", TOTAL_LEN);
	mbt_be_dict_append_string(info, "name", "hp.bin", 6);
	mbt_be_dict_append_number(info, "piece length", PIECE_LEN);
	mbt_be_dict_append_string(info, "pieces", hashes, sizeof(hashes));
	mbt_be_dict_append(root, "info", info);
	unsigned char *enc;
	mbt_be_value_encode(root, &enc, len);

	unsigned char *is = memmem(enc, *len, "4:infod", 7);
	assert(is);
	is += 6;
	int ilen = mbt_be_value_parse(is, *len - (size_t)(is - enc), &tmp);
	SHA1_CTX h;
	uint8_t digest[20];
	SHA1Init(&h);
	SHA1Update(&h, is, (size_t)ilen);
	SHA1Final(digest, &h);
	mbt_hex_encode(digest, 20, hex, 41);
	return enc;
}

static struct mbt_session *
leecher(struct mbt_storage_memory *mem, int maintain, const char *hex,
        uint16_t relay_port, struct firewall *fw)
{
	struct mbt_session_config cfg = {
		.ev = ev,
		.default_storage = mbt_storage_memory_storage(mem),
		.listen_port = 0,
		.disable_tcp = 1,
		.max_peers = 10,
		.timers = { .pex_interval = 1, .maintain_interval = maintain },
	};
	struct mbt_session *s;
	char magnet[256];
	assert(mbt_session_new(&s, &cfg) == 0);
	mbt_utp_ctx_set_tx_hook(mbt_session_utp_ctx(s), fw_tx, fw);
	mbt_utp_ctx_set_rx_hook(mbt_session_utp_ctx(s), fw_rx, fw);
	snprintf(magnet, sizeof(magnet),
	         "magnet:?xt=urn:btih:%s&x.pe=127.0.0.1:%hu", hex, relay_port);
	assert(mbt_session_add_magnet(s, magnet, NULL) == 0);
	return s;
}

int
main(int argc, char **argv)
{
	signal(SIGPIPE, SIG_IGN);
	if (argc > 1 && strcmp(argv[1], "-d") == 0) {
		mbt_log_set(mbt_log_stderr, NULL);
		mbt_log_set_debug(MBT_LOG_SESSION | MBT_LOG_TORRENT);
	}

	size_t enc_len;
	char hex[41];
	unsigned char *enc = make_torrent(&enc_len, hex);
	mbt_ev_new_default(&ev);

	/* R: open seeder, slow enough that A and C stay leechers */
	struct mbt_storage_memory *rmem = mbt_storage_memory_new();
	mbt_storage_memory_put_file(rmem, 0, data, TOTAL_LEN);
	struct mbt_session_config rcfg = {
		.ev = ev,
		.default_storage = mbt_storage_memory_storage(rmem),
		.listen_port = 0,
		.disable_tcp = 1,
		.max_peers = 10,
		.timers = { .pex_interval = 1, .maintain_interval = 1 },
		.upload_rate = 32 * 1024,
	};
	struct mbt_session *r;
	assert(mbt_session_new(&r, &rcfg) == 0);
	assert(mbt_session_add_torrent_buf(r, enc, enc_len, NULL) == 0);
	uint16_t rport = mbt_session_listen_port(r);

	struct firewall fwa = { .name = "A" }, fwc = { .name = "C" };
	struct mbt_storage_memory *amem = mbt_storage_memory_new();
	struct mbt_storage_memory *cmem = mbt_storage_memory_new();
	/* C first, with dialing from its peer list effectively off */
	struct mbt_session *c = leecher(cmem, 3600, hex, rport, &fwc);
	struct mbt_session *a = leecher(amem, 1, hex, rport, &fwa);
	uint16_t pa = mbt_session_listen_port(a), pc = mbt_session_listen_port(c);
	fwc.watch_port = pa;   /* C's SYNs to A can only come from a holepunch */
	fprintf(stderr, "holepunch_swarm: R=%hu A=%hu C=%hu\n", rport, pa, pc);

	struct check chk = {
		.ta = mbt_session_torrent_first(a),
		.tc = mbt_session_torrent_first(c),
		.pa = pa, .pc = pc,
	};
	struct mbt_ev_timer *tick;
	mbt_ev_timer_new(ev, &tick, 100000, check_cb, &chk);
	mbt_ev_run(ev);
	mbt_ev_timer_free(tick);

	int a_has_c = has_peer(chk.ta, pc), c_has_a = has_peer(chk.tc, pa);
	fprintf(stderr, "holepunch_swarm: after %d.%ds A->C %d, C->A %d, "
	        "C dropped %d, A dropped %d, C SYNs to A %d\n",
	        chk.ticks / 10, chk.ticks % 10, a_has_c, c_has_a,
	        fwc.dropped, fwa.dropped, fwc.syns_to_watch);

	assert(fwc.dropped > 0 && "A's direct SYN should have hit C's firewall");
	assert(fwc.syns_to_watch > 0 && "C never sent a SYN to A: no holepunch connect");
	assert(a_has_c && c_has_a && "A and C did not connect through the holepunch");

	mbt_session_free(a);
	mbt_session_free(c);
	mbt_session_free(r);
	mbt_storage_memory_destroy(amem);
	mbt_storage_memory_destroy(cmem);
	mbt_storage_memory_destroy(rmem);
	mbt_ev_destroy(ev);
	free(enc);
	fprintf(stderr, "holepunch_swarm: PASS\n");
	return 0;
}
