#include <sys/param.h>
#include <sys/queue.h>
#include <arpa/inet.h>
#include <netinet/in.h>
#include <assert.h>
#include <stdint.h>
#include <stdlib.h>
#include <unistd.h>
#include <string.h>
#include <errno.h>
#include <err.h>
#include <fcntl.h>

#include <sha1.h>

#include "event.h"
#include "conn.h"
#include <hex.h>
#include "infohash.h"
#include <be.h>
#include "pack.h"
#include <remote.h>
#include <bt.h>
#include <node.h>
#include <tbmap.h>

enum {
	HANDSHAKE_SIZE = 20+4+4+20+20,

	METADATA_BLOCKSIZE = (1<<14),
};

static void
free_ucharp(unsigned char **p)
{
	if(*p)
		free(*p);
}

static int remote_read_handshake(struct remote *r);
static int remote_rx(struct remote *r);
static int remote_extension_tx(struct remote *r);
static int remote_send_interest(struct remote *r, int interested);

static void
remote_teardown_cb(struct mbt_ev_timer *timer, void *v)
{
	struct remote *r = v;

	(void)timer;

	warnx("%s: %s:%hu: removing peer", __func__, r->remote, r->port);

	torrent_remove_remote(r->t, r);
}

static void
remote_onerror(struct remote *r, int err)
{
	assert(err <= 0);

	if(!r->defer)
		mbt_ev_timer_new(r->ev, &r->defer, 0, remote_teardown_cb, r);
}

static void
remote_conn_readcb(struct mbt_conn *c, void *v)
{
	int rv;
	struct remote *r = v;

	warnx("%s %zu", __func__, mbt_conn_available(c));

	if(!r->shook){
		rv = remote_read_handshake(r);
		if(rv < 0){
			warnx("%s: %s:%hu: handshake: %s", __func__, r->remote, r->port, strerror(-rv));
			remote_onerror(r, rv);
			return;
		}

		/* 4-byte big-endian message length prefixed messages */
		mbt_conn_set_watermark(r->conn, 4);

		return;
	}

	rv = remote_rx(r);
	if(rv < 0){
		warnx("%s: %s:%hu: rx: %s", __func__, r->remote, r->port, strerror(-rv));
		remote_onerror(r, rv);
		return;
	}
}

static void
remote_conn_eventcb(struct mbt_conn *c, int err, void *v)
{
	struct remote *r = v;

	(void)c;

	warnx("%s: %s:%hu: conn error: %s", __func__, r->remote, r->port,
	      err == 0 ? "EOF" : strerror(-err));
	remote_onerror(r, err);
}

int
remote_new(struct torrent *t, struct remote **r, struct infohash *ih, struct mbt_ev *ev)
{
	struct remote *remote;

	remote = calloc(1, sizeof(*remote));
	if(!remote)
		return -ENOMEM;

	remote->ref = 1;

	remote->t = t;
	remote->ev = ev;
	remote->fd = -1;

	remote->ih = infohash_ref(ih);

	remote->hechoking = 1;
	remote->mechoking = 1;

	*r = remote;

	torrent_add_remote(t, remote);

	return 0;
}

struct remote*
remote_ref(struct remote *r)
{
	assert(r);
	assert(r->ref > 0);

	r->ref++;

	return r;
}

void
remote_unref(struct remote *r)
{
	if(!r)
		return;

	assert(r->ref > 0);

	if(--r->ref > 0)
		return;

	warnx("%s: %s:%hu destroying", __func__, r->remote, r->port);

	if(r->conn)
		mbt_conn_free(r->conn);
	if(r->connect_io)
		mbt_ev_io_free(r->connect_io);
	if(r->timer)
		mbt_ev_timer_free(r->timer);
	if(r->defer)
		mbt_ev_timer_free(r->defer);

	infohash_unrefp(&r->ih);

	tbmap_unref(r->bitmap);

	free(r->metadata);
	free(r);
}

void
remote_unrefp(struct remote **r)
{
	if(!*r)
		return;

	remote_unref(*r);
}

static ssize_t
remote_write(struct remote *r, const void *buf, size_t sz)
{
	warnx("%s: %s:%hu: send %lu", __func__, r->remote, r->port, sz);
	return mbt_conn_write(r->conn, buf, sz);
}

static ssize_t
remote_write_prefixed(struct remote *r, void *buf, size_t sz)
{
	int rv;
	unsigned char lprefix[4];

	warnx("%s: %s:%hu: send %lu", __func__, r->remote, r->port, sz);
	rv = tpack(lprefix, "l", sz);
	if(rv < 0)
		return rv;

	if(mbt_conn_write(r->conn, lprefix, rv) < 0)
		return -EIO;

	if(mbt_conn_write(r->conn, buf, sz) < 0)
		return -EIO;

	return 0;
}

static int
remote_send_handshake(struct remote *r)
{
	int rv = 0;
	ssize_t o = 0;
	unsigned char shit[HANDSHAKE_SIZE];
	unsigned char ext[8];
	uint8_t *nodeid;
	size_t nodeidlen;

	if((rv = tpack(shit, "b*", 19, 19, "BitTorrent protocol")) < 0)
		return rv;

	o += rv;

	memset(ext, 0, sizeof(ext));

	// BEP 10 Extension Protocol
	ext[5] |= 0x10;

	if((rv = tpack(shit+o, "*", 8, ext)) < 0)
		return rv;

	o += rv;

	if((rv = tpack(shit+o, "*", sizeof(r->ih->hash_sha1), r->ih->hash_sha1)) < 0)
		return rv;

	o += rv;

	nodeid = node_id(torrent_node(r->t), &nodeidlen);
	assert(nodeid && nodeidlen == 20);

	if((rv = tpack(shit+o, "*", nodeidlen, nodeid)) < 0)
		return rv;

	o += rv;

	if(remote_write(r, shit, o) < 0)
		return -EIO;

	return 0;
}

static int
remote_send_interest(struct remote *r, int interested)
{
	unsigned char shit[32];
	int rv;
	uint8_t v;

	if(interested)
		v = 2;
	else
		v = 3;

	if((rv = tpack(shit, "lb", 1, v)) < 0)
		return rv;

	if(remote_write(r, shit, rv) < 0)
		return -EIO;

	return 0;
}

static void
remote_timer_cb(struct mbt_ev_timer *timer, void *v)
{
	struct remote *r = v;

	mbt_ev_timer_mod(timer, 60*1000000ULL);

	remote_write(r, "\0\0\0\0", 4);
}

static int
remote_connected(struct remote *r)
{
	int rv;

	warnx("%s: connected to %s:%hu", __func__, r->remote, r->port);

	/* Create buffered connection from the connected fd */
	rv = mbt_conn_new(r->ev, &r->conn, r->fd, remote_conn_readcb, remote_conn_eventcb, r);
	if(rv < 0){
		warnx("%s: mbt_conn_new: %s", __func__, strerror(-rv));
		remote_onerror(r, rv);
		return rv;
	}

	/* Wait for handshake */
	mbt_conn_set_watermark(r->conn, HANDSHAKE_SIZE);

	rv = remote_send_handshake(r);
	if(rv < 0){
		warnx("%s: failed to send handshake: %s", __func__, strerror(-rv));
		remote_onerror(r, rv);
		return rv;
	}

	/* keepalive timer: first fire at 10s, then 60s */
	rv = mbt_ev_timer_new(r->ev, &r->timer, 10*1000000ULL, remote_timer_cb, r);
	if(rv < 0){
		warnx("%s: failed to setup timer: %s", __func__, strerror(-rv));
		remote_onerror(r, rv);
		return rv;
	}

	return 0;
}

static void
remote_connect_io_cb(struct mbt_ev_io *io, int fd, uint32_t events, void *v)
{
	socklen_t sasz;
	int rv, error;
	struct remote *r = v;

	(void)io;
	(void)fd;
	(void)events;

	sasz = sizeof(error);
	rv = getsockopt(r->fd, SOL_SOCKET, SO_ERROR, &error, &sasz);
	if(rv < 0){
		warn("%s: getsockopt", __func__);
		error = errno;
		goto err;
	}

	if(error != 0){
		warnx("%s: connection to %s:%hu failed: %s", __func__, r->remote, r->port, strerror(error));
		goto err;
	}

	/* Done with connect watcher */
	mbt_ev_io_free(r->connect_io);
	r->connect_io = NULL;

	remote_connected(r);
	return;
err:
	remote_onerror(r, -error);
}

int
remote_connect(struct remote *r, const char *da, uint16_t port)
{
	struct sockaddr_in sa;
	socklen_t sasz;
	int rv;

	warnx("connecting to %s:%hu", da, port);

	assert(strlcpy(r->remote, da, sizeof(r->remote)) <= sizeof(r->remote));
	r->port = port;

	sa.sin_family = AF_INET;

	rv = inet_pton(sa.sin_family, da, &sa.sin_addr);
	if(rv == 0)
		return -EINVAL;
	if(rv < 0)
		return -errno;

	sa.sin_port = htons(port);

	sasz = sizeof(sa);

	r->fd = socket(AF_INET, SOCK_STREAM|SOCK_NONBLOCK|SOCK_CLOEXEC, 0);
	if(r->fd < 0)
		return -errno;

	rv = connect(r->fd, (struct sockaddr*)&sa, sasz);
	if(rv < 0){
		if(errno == EINPROGRESS){
			rv = mbt_ev_io_new(r->ev, &r->connect_io, r->fd, MBT_EV_WRITE, remote_connect_io_cb, r);
			if(rv < 0)
				return rv;

			return 0;
		}

		return -errno;
	}

	return remote_connected(r);
}

static int
remote_send_utmetadata_request(struct remote *r, int piece)
{
	int rv;
	unsigned char shit[64];
	unsigned char *encoded;
	size_t belen;
	__attribute__((cleanup(be_value_freep))) struct be_value *v = NULL;

	rv = be_dict_new(&v);
	if(rv < 0)
		return rv;

	rv = be_dict_append_number(v, "msg_type", 0);
	if(rv < 0)
		return rv;

	rv = be_dict_append_number(v, "piece", piece);
	if(rv < 0)
		return rv;

	rv = be_value_encode(v, &encoded, &belen);
	if(rv < 0)
		return rv;

	rv = tpack(shit, "bb*", 20, r->bep9code, belen, encoded);
	free(encoded);
	if(rv < 0)
		return rv;

	assert(rv < (int)sizeof(shit));

	warnx("%s: requesting %d", __func__, piece);

	rv = remote_write_prefixed(r, shit, rv);
	if(rv < 0)
		return rv;

	return 0;
}

static int
remote_metadata_complete(struct remote *r)
{
	int rv;
	SHA1_CTX ctx;
	char sha1str[SHA1_DIGEST_STRING_LENGTH];
	__attribute__((cleanup(be_value_freep))) struct be_value *metadata = NULL;
	__attribute__((cleanup(infohash_unrefp))) struct infohash *ih = NULL;

	SHA1Init(&ctx);
	SHA1Update(&ctx, (uint8_t*) r->metadata, r->metadata_length);
	SHA1End(&ctx, sha1str);

	rv = infohash_new(&ih, INFOHASH_SHA1, sha1str);
	if(rv < 0)
		return rv;

	warnx("%s: metadata complete, hash : %s", __func__, sha1str);
	infohash_string(r->ih, sha1str, sizeof(sha1str));
	warnx("%s: original hash           : %s", __func__, sha1str);

	if(infohash_cmp(r->ih, ih) != 0){
		warnx("%s: infohash mismatch", __func__);
		return -EINVAL;
	}

	if((rv = be_value_parse(r->metadata, r->metadata_length, &metadata)) < 0){
		warnx("%s: completed metadata could not be decoded", __func__);
		return rv;
	}

	torrent_set_info(r->t, metadata);

	return 0;
}

static int
remote_request(struct remote *r, size_t piece, size_t block)
{
	int rv;
	unsigned char packed[1+4+4+4];

	rv = tpack(packed, "blll", 6, piece, block*BTBLOCKSIZE, BTBLOCKSIZE);
	assert(rv > 0);

	return remote_write_prefixed(r, packed, rv);
}

int
remote_request_blocks_for_piece(struct remote *r, size_t piece)
{
	int rv;
	size_t i, blockcnt;

	blockcnt = torrent_piece_length(r->t) / BTBLOCKSIZE;
	assert(blockcnt > 0);

	warnx("%s: requesting all blocks for piece %zu", __func__, piece);

	for(i = 0; i < blockcnt; i++){
		if((rv = remote_request(r, piece, i)) < 0)
			return rv;
	}

	return 0;
}

static int
remote_request_all_metadata(struct remote *r)
{
	int i, rv;

	for(i = 0; i < r->metadata_need_block; i++){
		rv = remote_send_utmetadata_request(r, i);
		if(rv < 0)
			return rv;
	}

	return 0;
}

static int
remote_utmetadata_rx(struct remote *r, size_t msglen)
{
	int rv;
	__attribute__((cleanup(be_value_freep))) struct be_value *be = NULL;
	struct be_value *tmp;
	ssize_t sz;
	size_t off;
	__attribute__((cleanup(free_ucharp))) unsigned char *buf = NULL;

	buf = malloc(msglen);
	if(!buf)
		return -ENOMEM;

	sz = mbt_conn_read(r->conn, buf, msglen);
	if(sz != (ssize_t)msglen){
		warnx("%s: short read", __func__);
		return -ENOMSG;
	}

	if((rv = be_value_parse(buf, sz, &be)) < 0){
		warnx("%s: be_value_parse: %s", __func__, strerror(-rv));
		return rv;
	}

	tmp = be_find(be, "msg_type");
	if(!tmp || tmp->type != BE_INTEGER)
		return -EINVAL;

	switch(tmp->integer){
	case 0:
		warnx("%s: request", __func__);
		return 0;
	case 1:
		warnx("%s: data", __func__);
		tmp = be_find(be, "piece");
		if(!tmp || tmp->type != BE_INTEGER)
			return -EINVAL;

		off = METADATA_BLOCKSIZE * tmp->integer;

		if(off >= r->metadata_length)
			return -EINVAL;

		sz = MIN(msglen - rv, METADATA_BLOCKSIZE);

		warnx("%s: copy %ld of metadata to %lu", __func__, sz, off);

		memcpy(r->metadata + off, buf + rv, sz);

		r->metadata_have_block++;
		if(r->metadata_have_block == r->metadata_need_block){
			return remote_metadata_complete(r);
		}

		return 0;
	case 2:
		warnx("%s: reject", __func__);
		return 0;
	}

	return -EINVAL;
}

static int
remote_extension_tx(struct remote *r)
{
	int rv;
	size_t sz;
	__attribute__((cleanup(be_value_freep))) struct be_value *be = NULL;
	struct be_value *m = NULL;
	unsigned char *encoded;
	unsigned char packed[64];

	if((rv = be_dict_new(&be)) < 0)
		return rv;

	if((rv = be_dict_new(&m)) < 0)
		return rv;

	if((rv = be_dict_append(be, "m", m)) < 0){
		free(m);
		return rv;
	}

	if((rv = be_dict_append_number(m, "ut_metadata", 3)) < 0)
		return rv;

	if((rv = be_value_encode(be, &encoded, &sz)) < 0)
		return rv;

	rv = tpack(packed, "bb*", 20, 0, sz, encoded);
	free(encoded);
	if(rv < 0)
		return rv;

	assert(rv <= (int)sizeof(packed));

	return remote_write_prefixed(r, packed, rv);
}

static int
remote_extension_rx(struct remote *r, size_t sz)
{
	uint8_t extcmd;
	int rv;
	__attribute__((cleanup(be_value_freep))) struct be_value *be = NULL;
	unsigned char buf[1024];
	ssize_t rd;
	struct be_value *tmp;

	if(sz < 1)
		return -ENOMSG;

	rd = mbt_conn_read(r->conn, &extcmd, 1);
	if (rd != 1)
		return -EIO;

	sz -= 1;

	warnx("%s: extcmd %hhu", __func__, extcmd);

	switch(extcmd){
	case 0:
		// handshake
		rd = mbt_conn_read(r->conn, buf, MIN(sizeof(buf), sz));
		if (rd <= 0)
			return -EIO;

		if((rv = be_value_parse(buf, rd, &be)) < 0){
			warnx("%s: be_value_parse: %s", __func__, strerror(-rv));
			return rv;
		}

		be_walk(be, be_pp, NULL);

		// BEP 9 Extension for Peers to Send Metadata Files
		tmp = be_find(be, "m");
		if(!tmp)
			return -ENOMSG;

		tmp = be_find(tmp, "ut_metadata");
		if(!tmp)
			return -ENOMSG;

		if(tmp->type != BE_INTEGER)
			return -EINVAL;

		r->bep9code = (uint8_t)tmp->integer;

		tmp = be_find(be, "metadata_size");
		if(!tmp)
			return -ENOMSG;

		if(tmp->type != BE_INTEGER)
			return -EINVAL;

		if(tmp->integer <= 0)
			return -EINVAL;

		r->metadata_length = (size_t) tmp->integer;

		r->metadata = malloc(r->metadata_length);
		if(!r->metadata)
			return -ENOMEM;

		r->metadata_need_block = (r->metadata_length + (METADATA_BLOCKSIZE-1)) / METADATA_BLOCKSIZE;

		rv = remote_extension_tx(r);
		if(rv < 0){
			warnx("%s: failed to send extension: %s", __func__, strerror(-rv));
			remote_onerror(r, rv);
			return rv;
		}

		/* Send interested after extension handshake */
		rv = remote_send_interest(r, 1);
		if(rv < 0){
			warnx("%s: failed to send interest: %s", __func__, strerror(-rv));
			remote_onerror(r, rv);
			return rv;
		}
		r->meinterest = 1;

		if(!r->hechoking && r->bep9code){
			rv = remote_request_all_metadata(r);
			if(rv < 0)
				return rv;
		}

		break;
	default:
		if(r->bep9code && extcmd == r->bep9code)
			return remote_utmetadata_rx(r, sz);

		return -EINVAL;
	}

	return 0;
}

static int
remote_read_handshake(struct remote *r)
{
	ssize_t sz;
	unsigned char frame[HANDSHAKE_SIZE];
	unsigned char tmp[20];
	char infohash[40+1];
	int rv, offset;
	uint32_t length;

	sz = mbt_conn_read(r->conn, frame, HANDSHAKE_SIZE);
	if(sz != HANDSHAKE_SIZE){
		warnx("%s: %s:%hu: short handshake: %ld", __func__, r->remote, r->port, sz);
		return -ENOMSG;
	}

	warnx("%s: %s:%hu: got handshake", __func__, r->remote, r->port);

	rv = tunpack(frame, "b*", &length, 19, tmp);
	assert(rv == 20);
	offset = rv;

	if(memcmp(tmp, "BitTorrent protocol", 19) != 0){
		warnx("%s: %s:%hu: wrong protocol '%.*s'", __func__, r->remote, r->port, 19, tmp);
		return -ENOMSG;
	}

	rv = tunpack(frame+offset, "*", sizeof(r->ext), r->ext);
	assert(rv == sizeof(r->ext));
	offset += rv;

	// check for BEP 10
	if(!(r->ext[5] & 0x10)){
		warnx("%s: %s:%hu: node lacks extension protocol", __func__, r->remote, r->port);
		return -ENOMSG;
	}

	rv = tunpack(frame+offset, "*", 20, tmp);
	assert(rv == 20);
	offset += rv;

	if(memcmp(tmp, r->ih->hash_sha1, sizeof(r->ih->hash_sha1)) != 0){
		mbintohex((unsigned char*)tmp, 20, infohash, sizeof(infohash));
		warnx("%s: %s:%hu: wrong infohash '%s'", __func__, r->remote, r->port, infohash);
		return -ENOMSG;
	}

	rv = tunpack(frame+offset, "*", 20, tmp);
	assert(rv == 20);
	offset += rv;

	// their node id
	memcpy(r->id, tmp, 20);

	r->shook = 1;

	return 0;
}

static int
remote_rx_piece(struct remote *r, size_t msglen)
{
	int rv;
	uint32_t piece, offset;
	unsigned char buf[4+4];
	unsigned char block[BTBLOCKSIZE];

	rv = mbt_conn_read(r->conn, buf, sizeof(buf));
	if(rv != sizeof(buf)){
		warnx("%s: mbt_conn_read rv %d", __func__, rv);
		return -EIO;
	}

	rv = tunpack(buf, "ll", &piece, &offset);
	if(rv != sizeof(buf)){
		warnx("%s: tunpack rv %d", __func__, rv);
		return -EIO;
	}

	msglen -= sizeof(buf);

	rv = mbt_conn_read(r->conn, block, sizeof(block));
	if(rv < 0)
		return rv;

	warnx("%s: rv %d msglen %zu", __func__, rv, msglen);

	if(rv != (int)msglen){
		warnx("%s: rv %d msglen %zu", __func__, rv, msglen);
		return -EIO;
	}

	rv = torrent_put_piece_block(r->t, piece, offset, block, rv);
	if(rv < 0){
		warnx("%s: torrent_put_piece_block: %d", __func__, rv);
		return -EIO;
	}

	return 0;
}

static int
remote_rx(struct remote *r)
{
	uint8_t cmd;
	ssize_t sz;
	int rv;
	uint32_t length;
	unsigned char lengthprefix[4];
	unsigned char bitmaptmp[4096];

	if(!r->msglen){
		sz = mbt_conn_read(r->conn, lengthprefix, sizeof(lengthprefix));
		if (sz != sizeof(lengthprefix))
			return -EIO;
		rv = tunpack(lengthprefix, "l", &length);
		if (rv <= 0)
			return -EIO;

		if(length > (1<<18)){
			warnx("%s: %s:%hu: bogus large frame size %u", __func__, r->remote, r->port, length);
			rv = -ENOMSG;
			goto err;
		}

		warnx("%s: %s:%hu: got %u", __func__, r->remote, r->port, length);

		r->msglen = length;

		/* keep alive */
		if(r->msglen == 0){
			warnx("%s: %s:%hu: keepalive", __func__, r->remote, r->port);
			return 0;
		}

		mbt_conn_set_watermark(r->conn, r->msglen);

		return 0;
	}

	sz = mbt_conn_read(r->conn, &cmd, 1);
	if (sz != 1)
		return -EIO;

	r->msglen -= 1;

	warnx("%s: %s:%hu: command %hhu", __func__, r->remote, r->port, cmd);

	switch(cmd){
	case 0:
		warnx("%s: %s:%hu: choke", __func__, r->remote, r->port);
		r->hechoking = 1;
		break;

	case 1:
		warnx("%s: %s:%hu: unchoke", __func__, r->remote, r->port);
		r->hechoking = 0;

		if(r->bep9code){
			rv = remote_request_all_metadata(r);
			if(rv < 0)
				return rv;
		}

		break;

	case 2:
		warnx("%s: %s:%hu: interest", __func__, r->remote, r->port);
		r->heinterest = 1;
		break;

	case 3:
		warnx("%s: %s:%hu: not interest", __func__, r->remote, r->port);
		r->heinterest = 0;
		break;

	case 4:
		warnx("%s: %s:%hu: have", __func__, r->remote, r->port);
		break;

	case 5:
		warnx("%s: %s:%hu: bitfield", __func__, r->remote, r->port);

		if (r->msglen >= sizeof(bitmaptmp)) {
			rv = -ENOMSG;
			goto err;
		}
		sz = mbt_conn_read(r->conn, bitmaptmp, MIN(r->msglen, sizeof(bitmaptmp)));
		if (sz != (ssize_t)r->msglen) {
			rv = -EIO;
			goto err;
		}

		if((rv = tbmap_fromarray(&r->bitmap, bitmaptmp, sz)) < 0)
			goto err;

		break;

	case 6:
		warnx("%s: %s:%hu: request", __func__, r->remote, r->port);
		break;

	case 7:
		warnx("%s: %s:%hu: piece", __func__, r->remote, r->port);

		rv = remote_rx_piece(r, r->msglen);
		if(rv < 0){
			warnx("%s: %s:%hu: read piece: %s", __func__, r->remote, r->port, strerror(-rv));
			goto err;
		}

		break;

	case 20:
		warnx("%s: %s:%hu: extension", __func__, r->remote, r->port);
		if((rv = remote_extension_rx(r, r->msglen)) < 0){
			warnx("%s: %s:%hu: extension error: %s", __func__, r->remote, r->port, strerror(-rv));
			goto err;
		}
		break;

	default:
		warnx("%s: %s:%hu: unknown message type %d", __func__, r->remote, r->port, cmd);
		rv = -ENOMSG;
		goto err;
	}

	r->msglen = 0;

	mbt_conn_set_watermark(r->conn, 4);

	return 0;

err:
	return rv;
}
