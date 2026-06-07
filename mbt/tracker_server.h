#ifndef MBT_TRACKER_SERVER_H
#define MBT_TRACKER_SERVER_H

#include <stdint.h>

struct mbt_ev;
struct mbt_tracker_server;

/* Create a UDP tracker server bound to a port */
int mbt_tracker_server_new(struct mbt_tracker_server **out, struct mbt_ev *ev, uint16_t port);
void mbt_tracker_server_free(struct mbt_tracker_server *ts);

/* Stats */
int mbt_tracker_server_num_torrents(struct mbt_tracker_server *ts);
int mbt_tracker_server_num_peers(struct mbt_tracker_server *ts);

#endif
