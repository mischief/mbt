// SPDX-License-Identifier: ISC
#ifndef MBT_PICKER_H
#define MBT_PICKER_H

struct mbt_torrent;

/* Returns piece index to request from this peer, or -1 if nothing to request */
typedef int (*mbt_picker_fn)(struct mbt_torrent *t, void *peer, void *ctx);

/* Built-in pickers */
int mbt_picker_sequential(struct mbt_torrent *t, void *peer, void *ctx);
int mbt_picker_random(struct mbt_torrent *t, void *peer, void *ctx);
int mbt_picker_rarest(struct mbt_torrent *t, void *peer, void *ctx);

#endif
