#include <net/ethernet.h>
#include <sys/param.h>
#include <sys/types.h>
#include <sys/socket.h>
#include <netinet/in.h>
#include <unistd.h>
#include <fcntl.h>
#include <errno.h>
#include <stdlib.h>
#include <stdio.h>
#include <string.h>
#include <assert.h>
#include <err.h>

#include <systemd/sd-event.h>

#include <hex.h>
#include "infohash.h"
#include <be.h>
#include <bt.h>
#include <remote.h>
#include <node.h>

struct node {
	uint16_t port;
	int fd;

	uint8_t id[20];

	struct torrent_tree torrents;

	sd_event *ev;
};

int
node_new(struct node **node, uint16_t port, sd_event *ev)
{
	struct node *p;
	int rv, fd = -1, y = 1;
	struct sockaddr_in sa;
	unsigned char rnd[10];
	char id[20+1];

	p = calloc(1, sizeof(*p));
	if(!p)
		return -ENOMEM;

	*p = (struct node){
		.port = port,
	};

	fd = socket(AF_INET, SOCK_STREAM, 0);
	if(fd < 0){
		rv = -errno;
		goto err;
	}

	if((rv = fcntl(fd, F_SETFL, O_NONBLOCK)) < 0){
		rv = -errno;
		goto err;
	}

	p->fd = fd;

	if(setsockopt(fd, SOL_SOCKET, SO_REUSEADDR, &y, sizeof(y)) < 0){
		rv = -errno;
		goto err;
	}

	sa = (struct sockaddr_in){
		.sin_family = AF_INET,
		.sin_addr = (struct in_addr){ .s_addr = htonl(INADDR_ANY) },
		.sin_port = htons(port),
	};

	if(bind(fd, (struct sockaddr*)&sa, sizeof(sa)) < 0){
		rv = -errno;
		goto err;
	}

	arc4random_buf(rnd, sizeof(rnd));

	mbintohex(rnd, sizeof(rnd), id, sizeof(id));

	memcpy(p->id, id, sizeof(p->id));

	memcpy(p->id, "-NO0000-", 8);

	RB_INIT(&p->torrents);

	p->ev = sd_event_ref(ev);

	*node = p;

	return 0;

err:
	if(fd > 0)
		close(fd);
	free(p);
	return rv;
}

void
node_freep(struct node **node)
{
	struct node *n;
	struct torrent *t, *tt;

	if(!*node)
		return;

	n = *node;

	close(n->fd);

	RB_FOREACH_SAFE(t, torrent_tree, &n->torrents, tt){
		torrent_unref(t);
	}

	sd_event_unref(n->ev);

	free(n);
}

int
node_send_remote(struct node *p, struct remote *r, unsigned char *buf, size_t sz)
{

	return 0;
}

struct torrent*
node_find_torrent(struct node*, struct infohash*)
{
	return NULL;
}

void
node_add_torrent(struct node *n, struct torrent *t)
{
	RB_INSERT(torrent_tree, &n->torrents, t);
}

/*
int
node_rx(sd_event_source *s, int fd, uint32_t revents, void *userdata)
{
	int rv;
	unsigned char frame[ETH_DATA_LEN];
	char hex[ETH_DATA_LEN*2+1];
	struct sockaddr_storage sa;
	socklen_t sasz;
	struct utp_frame *utp;

	return 0;
}
*/

uint8_t*
node_id(struct node *n, size_t *len)
{
	*len = sizeof(n->id);
	return n->id;
}

sd_event*
node_event_ref(struct node *n)
{
	return sd_event_ref(n->ev);
}

