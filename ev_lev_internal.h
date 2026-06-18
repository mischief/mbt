// SPDX-License-Identifier: ISC
#ifndef EV_LEV_INTERNAL_H
#define EV_LEV_INTERNAL_H

#ifdef HAVE_LIBEVENT1
#include <event.h>
#else
#include <event2/event.h>
#endif
#include "mbt/event.h"

struct ev_lev {
	struct mbt_ev base;
	struct event_base *base_ev;
	int owned;
};

#endif
