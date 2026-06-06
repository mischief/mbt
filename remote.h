/*
#include <sys/queue.h>
#include <netinet/in.h>
#include <stdint.h>

#include "event.h"
#include "conn.h"
*/

struct remote;

LIST_HEAD(remote_head, remote);

struct remote {
	int ref;
	LIST_ENTRY(remote) next;

	struct torrent *t;

	char remote[INET6_ADDRSTRLEN];
	uint16_t port;
	int fd;

	unsigned char ext[8];
	unsigned char id[20];

	struct mbt_conn *conn;
	struct mbt_ev_io *connect_io;
	struct mbt_ev_timer *timer;
	struct mbt_ev_timer *defer;

	struct mbt_ev *ev;

	/* expected next message size */
	size_t msglen;
	int active_cmd;

	struct mbt_infohash *ih;

	/* completed bittorrent v1 handshake? */
	int shook;

	int hechoking, mechoking;
	int heinterest, meinterest;

	/* if nonzero, extended message type for BEP 9 metadata messages */
	uint8_t bep9code;

	/* bitfield copy from peer */
	struct mbt_bitmap *bitmap;

	/* metadata copy from peer */
	unsigned char *metadata;
	size_t metadata_length;
	int metadata_need_block;
	int metadata_have_block;
};

int remote_new(struct torrent *t, struct remote **r, struct mbt_infohash *ih, struct mbt_ev *ev);
struct remote *remote_ref(struct remote *r);
void remote_unref(struct remote *r);
void remote_unrefp(struct remote **r);
int remote_connect(struct remote *r, const char *da, uint16_t port);

int remote_request_blocks_for_piece(struct remote *r, size_t piece);
