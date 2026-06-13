// SPDX-License-Identifier: ISC
#ifndef EV_SD_INTERNAL_H
#define EV_SD_INTERNAL_H

#include <systemd/sd-event.h>
#include "mbt/event.h"

struct ev_sd {
	struct mbt_ev base;
	sd_event *loop;
	int owned;
};

#endif
