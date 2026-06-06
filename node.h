/*
#include <sys/queue.h>
#include <stdint.h>
#include <systemd/sd-event.h>

#include <bt.h>
#include <remote.h>
*/

struct node;

int node_new(struct node **p, uint16_t port, sd_event *ev);
void node_freep(struct node **p);
int node_send_remote(struct node *p, struct remote *r, unsigned char *buf, size_t sz);

struct torrent *node_find_torrent(struct node*, struct infohash*);
void node_add_torrent(struct node*, struct torrent*);

uint8_t *node_id(struct node *n, size_t *len);

sd_event *node_event_ref(struct node *n);

