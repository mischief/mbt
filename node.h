/*
#include <sys/queue.h>
#include <stdint.h>

#include "event.h"
#include "bt_legacy.h"
#include <remote.h>
*/

struct node;

int node_new(struct node **p, uint16_t port, struct mbt_ev *ev);
void node_freep(struct node **p);
int node_send_remote(struct node *p, struct remote *r, unsigned char *buf, size_t sz);

struct torrent *node_find_torrent(struct node*, struct mbt_infohash*);
void node_add_torrent(struct node*, struct torrent*);

uint8_t *node_id(struct node *n, size_t *len);

struct mbt_ev *node_event(struct node *n);
