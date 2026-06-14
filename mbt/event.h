// SPDX-License-Identifier: ISC
#ifndef MBT_EVENT_H
#define MBT_EVENT_H

#include "export.h"

#include <stdint.h>

struct mbt_ev;
struct mbt_ev_io;
struct mbt_ev_timer;

enum mbt_ev_flags {
	MBT_EV_READ  = 1 << 0,
	MBT_EV_WRITE = 1 << 1,
};

typedef void (*mbt_ev_io_cb)(struct mbt_ev_io *io, int fd, uint32_t events, void *arg);
typedef void (*mbt_ev_timer_cb)(struct mbt_ev_timer *timer, void *arg);

struct mbt_ev_ops {
	int (*io_new)(struct mbt_ev *ev, struct mbt_ev_io **out, int fd, uint32_t events,
	              mbt_ev_io_cb cb, void *arg);
	int (*io_mod)(struct mbt_ev_io *io, uint32_t events);
	void (*io_free)(struct mbt_ev_io *io);

	int (*timer_new)(struct mbt_ev *ev, struct mbt_ev_timer **out, uint64_t usec,
	                 mbt_ev_timer_cb cb, void *arg);
	int (*timer_mod)(struct mbt_ev_timer *timer, uint64_t usec);
	void (*timer_free)(struct mbt_ev_timer *timer);

	int (*run)(struct mbt_ev *ev);
	void (*stop)(struct mbt_ev *ev);
	void (*destroy)(struct mbt_ev *ev);
};

struct mbt_ev {
	const struct mbt_ev_ops *ops;
};

/* sd-event backend */
#ifdef HAVE_SD_EVENT
MBT_API int mbt_ev_sd_new(struct mbt_ev **out);
struct sd_event;
MBT_API int mbt_ev_sd_new_from(struct mbt_ev **out, struct sd_event *loop);
#endif

/* libevent backend */
#ifdef HAVE_LIBEVENT
MBT_API int mbt_ev_libevent_new(struct mbt_ev **out);
struct event_base;
MBT_API int mbt_ev_libevent_new_from(struct mbt_ev **out, struct event_base *base);
#endif

/* Create event loop using the compiled-in backend */
static inline int
mbt_ev_new_default(struct mbt_ev **out)
{
#ifdef HAVE_SD_EVENT
	return mbt_ev_sd_new(out);
#elif defined(HAVE_LIBEVENT)
	return mbt_ev_libevent_new(out);
#else
	(void)out;
	return -1; /* no built-in backend; caller must provide their own */
#endif
}

/* Inline convenience wrappers */
static inline int
mbt_ev_io_new(struct mbt_ev *ev, struct mbt_ev_io **out, int fd, uint32_t events,
              mbt_ev_io_cb cb, void *arg)
{
	return ev->ops->io_new(ev, out, fd, events, cb, arg);
}

static inline int
mbt_ev_io_mod(struct mbt_ev_io *io, uint32_t events)
{
	struct mbt_ev *ev = *(struct mbt_ev **)io; /* first field is always ev pointer */
	return ev->ops->io_mod(io, events);
}

static inline void
mbt_ev_io_free(struct mbt_ev_io *io)
{
	struct mbt_ev *ev = *(struct mbt_ev **)io;
	ev->ops->io_free(io);
}

static inline int
mbt_ev_timer_new(struct mbt_ev *ev, struct mbt_ev_timer **out, uint64_t usec,
                 mbt_ev_timer_cb cb, void *arg)
{
	return ev->ops->timer_new(ev, out, usec, cb, arg);
}

static inline int
mbt_ev_timer_mod(struct mbt_ev_timer *timer, uint64_t usec)
{
	struct mbt_ev *ev = *(struct mbt_ev **)timer;
	return ev->ops->timer_mod(timer, usec);
}

static inline void
mbt_ev_timer_free(struct mbt_ev_timer *timer)
{
	struct mbt_ev *ev = *(struct mbt_ev **)timer;
	ev->ops->timer_free(timer);
}

static inline int
mbt_ev_run(struct mbt_ev *ev)
{
	return ev->ops->run(ev);
}

static inline void
mbt_ev_stop(struct mbt_ev *ev)
{
	ev->ops->stop(ev);
}

static inline void
mbt_ev_destroy(struct mbt_ev *ev)
{
	ev->ops->destroy(ev);
}

#endif
