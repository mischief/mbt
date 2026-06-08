#ifndef MBT_TRACKER_SERVER_H
#define MBT_TRACKER_SERVER_H

#include <stdint.h>

struct mbt_ev;

/* --- UDP tracker server (BEP 15) --- */
struct mbt_tracker_server;

int mbt_tracker_server_new(struct mbt_tracker_server **out, struct mbt_ev *ev, uint16_t port);
void mbt_tracker_server_free(struct mbt_tracker_server *ts);
int mbt_tracker_server_num_torrents(struct mbt_tracker_server *ts);
int mbt_tracker_server_num_peers(struct mbt_tracker_server *ts);

/* --- HTTP tracker server (BEP 3) --- */
struct mbt_tracker_http;
struct mbt_tracker_store;

int mbt_tracker_store_new(struct mbt_tracker_store **out);
void mbt_tracker_store_free(struct mbt_tracker_store *s);
int mbt_tracker_store_num_torrents(struct mbt_tracker_store *s);
int mbt_tracker_store_num_peers(struct mbt_tracker_store *s);

int mbt_tracker_http_new(struct mbt_tracker_http **out, struct mbt_ev *ev, uint16_t port);
void mbt_tracker_http_free(struct mbt_tracker_http *th);
int mbt_tracker_http_num_torrents(struct mbt_tracker_http *th);
int mbt_tracker_http_num_peers(struct mbt_tracker_http *th);

#endif
