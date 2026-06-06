#ifndef BT_H
#define BT_H

#include <sys/queue.h>
#include <sys/tree.h>

enum {
	BTBLOCKSIZE = 1<<14,
};

struct node;
struct torrent;
struct remote;

int torrent_new(struct node *node, struct infohash *ih, struct torrent **out);
struct torrent *torrent_ref(struct torrent*);
void torrent_unref(struct torrent*);
void torrent_unrefp(struct torrent**);

int torrent_cmp(struct torrent *a, struct torrent *b);

struct node *torrent_node(struct torrent*);

void torrent_add_remote(struct torrent *t, struct remote *r);
void torrent_remove_remote(struct torrent *t, struct remote *r);

void torrent_set_info(struct torrent *t, struct be_value *info);

size_t torrent_piece_length(struct torrent *t);
int torrent_put_piece_block(struct torrent *t, size_t piece_index, size_t offset, unsigned char *buf, size_t sz);

RB_HEAD(torrent_tree, torrent);
RB_PROTOTYPE(torrent_tree, torrent, entry, torrent_cmp)

int tpack(unsigned char *out, char *v, ...);
int tunpack(unsigned char *in, char *v, ...);

#endif

