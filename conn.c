// SPDX-License-Identifier: ISC
#include "mbt/conn.h"

void
mbt_conn_set_callbacks(struct mbt_conn *c, mbt_conn_read_cb readcb,
                       mbt_conn_event_cb eventcb, void *arg)
{
	c->ops->set_callbacks(c, readcb, eventcb, arg);
}

int
mbt_conn_peer_addr(struct mbt_conn *c, char *buf, size_t buflen, uint16_t *port)
{
	return c->ops->peer_addr(c, buf, buflen, port);
}
