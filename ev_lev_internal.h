// SPDX-License-Identifier: ISC
#ifndef EV_LEV_INTERNAL_H
#define EV_LEV_INTERNAL_H

#include <event2/event.h>
#include "mbt/event.h"

struct ev_lev {
	struct mbt_ev base;
	struct event_base *base_ev;
	int owned;
};

#endif
