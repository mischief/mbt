#include "picker.h"

int
mbt_picker_sequential(struct mbt_torrent *t, void *peer, void *ctx)
{
	(void)t; (void)peer; (void)ctx;
	return -1; /* TODO */
}

int
mbt_picker_random(struct mbt_torrent *t, void *peer, void *ctx)
{
	(void)t; (void)peer; (void)ctx;
	return -1; /* TODO */
}

int
mbt_picker_rarest(struct mbt_torrent *t, void *peer, void *ctx)
{
	(void)t; (void)peer; (void)ctx;
	return -1; /* TODO */
}
