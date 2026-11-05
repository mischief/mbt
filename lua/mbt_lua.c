// SPDX-License-Identifier: ISC
/*
 * require("mbt"): run BitTorrent sessions from a Lua script. Sleeps and
 * torrent waits run in coroutines that the mbt event loop resumes while
 * mbt.run drives it. See lua/README.md.
 */
#include <sys/queue.h>
#include <errno.h>
#include <signal.h>
#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include <lauxlib.h>
#include <lua.h>

#include "mbt/event.h"
#include "mbt/infohash.h"
#include "mbt/log.h"
#include "mbt/magnet.h"
#include "mbt/metainfo.h"
#include "mbt/picker.h"
#include "mbt/session.h"
#include "mbt/storage.h"
#include "mbt/torrent.h"

#define SESSION_META "mbt.session"
#define STORAGE_META "mbt.storage"

#define TORRENT_FAILED "torrent failed"

/* User values of a session. */
#define UV_ON_COMPLETE 1
#define UV_ON_ERROR 2
#define TORRENT_META "mbt.torrent"
#define CTX_META "mbt.ctx"
#define CTX_KEY "mbt.context"
#define TASKS_KEY "mbt.tasks"
#define SESSIONS_KEY "mbt.sessions"


struct task;
struct lwaiter;
struct ldone;
struct lsleep;

/*
 * One per Lua state that loads the module, kept in its registry: the event
 * loop its sessions share and the state's main thread. Lua code never runs
 * under a libmbt call. Completions queue here, and the wake timer delivers
 * them from the top of the loop, so a callback can close its own session.
 */
struct cctx {
	struct mbt_ev *ev;
	lua_State *L;
	int in_loop; /* mbt.run or mbt.step under way */
	struct mbt_ev_timer *wake;
	bool wake_armed;
	TAILQ_HEAD(, task) tasks; /* tasks still running */
	TAILQ_HEAD(, lwaiter) ready;
	TAILQ_HEAD(, ldone) done;
	TAILQ_HEAD(, lsleep) sleeps;
};

static struct cctx *
ctx_of(lua_State *L)
{
	struct cctx *c;

	lua_getfield(L, LUA_REGISTRYINDEX, CTX_KEY);
	c = lua_touserdata(L, -1);
	lua_pop(L, 1);
	return c;
}

/* libmbt returns -errno. Push nil, the message and the errno. */
static int
fail(lua_State *L, int rv)
{
	int e = rv < 0 ? -rv : EINVAL;

	lua_pushnil(L);
	lua_pushstring(L, strerror(e));
	lua_pushinteger(L, e);
	return 3;
}

/* ------------------------------------------------------------------ */
/* Tasks: coroutines this module starts                                */
/* ------------------------------------------------------------------ */

struct task {
	TAILQ_ENTRY(task) entries;
	struct cctx *c;
	lua_State *co;
	int ref;
	bool keep; /* the starter reads the result, then frees it */
	bool done;
	bool ok;
	char *err;
};

static void
report(const char *what, const char *msg)
{
	fprintf(stderr, "mbt: %s: %s\n", what, msg != NULL ? msg : "error");
}

static struct task *
task_of(struct cctx *c, lua_State *co)
{
	lua_State *mainL = c->L;
	struct task *t;

	lua_getfield(mainL, LUA_REGISTRYINDEX, TASKS_KEY);
	lua_pushthread(co);
	lua_xmove(co, mainL, 1);
	lua_rawget(mainL, -2);
	t = lua_touserdata(mainL, -1);
	lua_pop(mainL, 2);
	return t;
}

static void
task_forget(struct task *t)
{
	lua_State *mainL = t->c->L;

	lua_getfield(mainL, LUA_REGISTRYINDEX, TASKS_KEY);
	lua_pushthread(t->co);
	lua_xmove(t->co, mainL, 1);
	lua_pushnil(mainL);
	lua_rawset(mainL, -3);
	lua_pop(mainL, 1);
	luaL_unref(mainL, LUA_REGISTRYINDEX, t->ref);
	TAILQ_REMOVE(&t->c->tasks, t, entries);
}

/* Resume co with the nargs values on its stack. */
static void
co_resume(struct cctx *c, lua_State *co, int nargs)
{
	struct task *t = task_of(c, co);
	int nres = 0, rc = lua_resume(co, c->L, nargs, &nres);

	if (rc == LUA_YIELD)
		return;
	if (t == NULL) {
		if (rc != LUA_OK)
			report("coroutine", lua_tostring(co, -1));
		return;
	}
	t->done = true;
	t->ok = rc == LUA_OK;
	if (!t->ok) {
		const char *m = lua_tostring(co, -1);

		t->err = strdup(m != NULL ? m : "error");
		if (!t->keep)
			report("task", t->err);
	}
	task_forget(t);
	if (t->keep)
		return;
	free(t->err);
	free(t);
}

/* Start fn (below nargs arguments on L) in a new coroutine. */
static struct task *
task_start(struct cctx *c, lua_State *L, int nargs, bool keep)
{
	lua_State *mainL = c->L;
	struct task *t = calloc(1, sizeof(*t));

	if (t == NULL) {
		lua_pop(L, nargs + 1);
		return NULL;
	}
	t->c = c;
	t->keep = keep;
	t->co = lua_newthread(mainL);
	t->ref = luaL_ref(mainL, LUA_REGISTRYINDEX);
	lua_xmove(L, t->co, nargs + 1);
	lua_getfield(mainL, LUA_REGISTRYINDEX, TASKS_KEY);
	lua_pushthread(t->co);
	lua_xmove(t->co, mainL, 1);
	lua_pushlightuserdata(mainL, t);
	lua_rawset(mainL, -3);
	lua_pop(mainL, 1);
	TAILQ_INSERT_TAIL(&c->tasks, t, entries);
	co_resume(c, t->co, nargs);
	return t;
}

/* A coroutine parked until a callback resumes it. */
struct wait {
	struct cctx *c;
	lua_State *co;
	int ref;
};

static int
wait_begin(struct cctx *c, lua_State *L, struct wait *w)
{
	if (L == c->L || !lua_isyieldable(L))
		return -1;
	w->c = c;
	w->co = L;
	lua_pushthread(L);
	lua_xmove(L, c->L, 1);
	w->ref = luaL_ref(c->L, LUA_REGISTRYINDEX);
	return 0;
}

static void
wait_end(struct wait *w, int nargs)
{
	struct cctx *c = w->c;
	lua_State *co = w->co;

	luaL_unref(c->L, LUA_REGISTRYINDEX, w->ref);
	w->co = NULL;
	co_resume(c, co, nargs);
}

/* Continuation: return what the resumer pushed above the first ctx slots. */
static int
wait_k(lua_State *L, int status, lua_KContext ctx)
{
	(void)status;
	return lua_gettop(L) - (int)ctx;
}

/* ------------------------------------------------------------------ */
/* Deferred delivery                                                   */
/* ------------------------------------------------------------------ */

struct lsession;

/* A t:wait call. On c->ready, err says how it ended; NULL means complete. */
struct lwaiter {
	TAILQ_ENTRY(lwaiter) entries;
	struct wait w;
	struct mbt_infohash *ih;
	const char *err;
};

/* A completed or failed torrent, for on_complete or on_error. */
struct ldone {
	TAILQ_ENTRY(ldone) entries;
	struct lsession *ls;
	struct mbt_infohash *ih;
	int err;
};

/* Sessions and torrents hold their own references to st. */
struct lstorage {
	struct mbt_storage *st;
	char *dir;
};

/*
 * on_complete and on_error live in the userdata's user values, so a callback
 * that captures the session forms a cycle the collector can free.
 * SESSIONS_KEY maps each lsession to its userdata, weakly, for delivery.
 */
struct lsession {
	struct cctx *c;
	struct mbt_session *s;
	char *dir;
	TAILQ_HEAD(, lwaiter) waiters;
};

struct ltorrent {
	struct mbt_infohash *ih;
};

static void torrent_push(lua_State *L, int session_idx, struct mbt_infohash *ih);

static void
deliver_waiter(struct cctx *c, struct lwaiter *w)
{
	lua_State *co = w->w.co;
	int n = 1;

	TAILQ_REMOVE(&c->ready, w, entries);
	if (w->err == NULL) {
		lua_pushboolean(co, 1);
	} else {
		lua_pushnil(co);
		lua_pushstring(co, w->err);
		n = 2;
	}
	mbt_infohash_unref(w->ih);
	wait_end(&w->w, n);
	free(w);
}

static void
deliver_done(struct cctx *c, struct ldone *d)
{
	lua_State *L = c->L;

	TAILQ_REMOVE(&c->done, d, entries);
	lua_getfield(L, LUA_REGISTRYINDEX, SESSIONS_KEY);
	if (lua_rawgetp(L, -1, d->ls) == LUA_TUSERDATA) {
		lua_getiuservalue(L, -1, d->err != 0 ? UV_ON_ERROR : UV_ON_COMPLETE);
		if (lua_isfunction(L, -1)) {
			int nargs = 1;

			torrent_push(L, -2, d->ih);
			if (d->err != 0) {
				lua_pushstring(L, strerror(-d->err));
				lua_pushinteger(L, -d->err);
				nargs = 3;
			}
			(void)task_start(c, L, nargs, false);
		} else {
			lua_pop(L, 1);
		}
	}
	lua_pop(L, 2);
	mbt_infohash_unref(d->ih);
	free(d);
}

static void
wake_cb(struct mbt_ev_timer *timer, void *arg)
{
	struct cctx *c = arg;
	struct lwaiter *w;
	struct ldone *d;

	(void)timer;
	c->wake_armed = false;
	for (;;) {
		if ((w = TAILQ_FIRST(&c->ready)) != NULL)
			deliver_waiter(c, w);
		else if ((d = TAILQ_FIRST(&c->done)) != NULL)
			deliver_done(c, d);
		else
			break;
	}
}

static void
wake(struct cctx *c)
{
	int rv;

	if (c->wake_armed)
		return;
	if (c->wake == NULL)
		rv = mbt_ev_timer_new(c->ev, &c->wake, 0, wake_cb, c);
	else
		rv = mbt_ev_timer_mod(c->wake, 0);
	if (rv < 0)
		report("wake", strerror(-rv));
	else
		c->wake_armed = true;
}

/* Move the waiters on ih (all of them for ih NULL) to the ready list. */
static void
waiters_ready(struct lsession *ls, struct mbt_infohash *ih, const char *err)
{
	struct cctx *c = ls->c;
	struct lwaiter *w, *next;

	for (w = TAILQ_FIRST(&ls->waiters); w != NULL; w = next) {
		next = TAILQ_NEXT(w, entries);
		if (ih != NULL && mbt_infohash_cmp(w->ih, ih) != 0)
			continue;
		TAILQ_REMOVE(&ls->waiters, w, entries);
		w->err = err;
		TAILQ_INSERT_TAIL(&c->ready, w, entries);
		wake(c);
	}
}

static void
done_queue(struct lsession *ls, struct mbt_infohash *ih, int err)
{
	struct ldone *d;

	if ((d = calloc(1, sizeof(*d))) == NULL)
		return;
	d->ls = ls;
	d->ih = mbt_infohash_ref(ih);
	d->err = err;
	TAILQ_INSERT_TAIL(&ls->c->done, d, entries);
	wake(ls->c);
}

/* Runs inside libmbt: queue only. */
static void
session_complete_cb(struct mbt_session *s, struct mbt_torrent *t, void *arg)
{
	struct lsession *ls = arg;
	struct mbt_infohash *ih = mbt_torrent_infohash(t);

	(void)s;
	waiters_ready(ls, ih, NULL);
	done_queue(ls, ih, 0);
}

/* Runs inside libmbt: queue only. */
static void
session_error_cb(struct mbt_session *s, struct mbt_torrent *t, int err, void *arg)
{
	struct lsession *ls = arg;
	struct mbt_infohash *ih = mbt_torrent_infohash(t);

	(void)s;
	waiters_ready(ls, ih, TORRENT_FAILED);
	done_queue(ls, ih, err);
}

/* Drop queued completions for ls, on ih only if ih is not NULL. */
static void
done_forget(struct lsession *ls, struct mbt_infohash *ih)
{
	struct cctx *c = ls->c;
	struct ldone *d, *next;

	for (d = TAILQ_FIRST(&c->done); d != NULL; d = next) {
		next = TAILQ_NEXT(d, entries);
		if (d->ls != ls || (ih != NULL && mbt_infohash_cmp(d->ih, ih) != 0))
			continue;
		TAILQ_REMOVE(&c->done, d, entries);
		mbt_infohash_unref(d->ih);
		free(d);
	}
}

/* ------------------------------------------------------------------ */
/* Storage                                                             */
/* ------------------------------------------------------------------ */

static struct lstorage *
check_storage(lua_State *L, int idx)
{
	struct lstorage *lst = luaL_checkudata(L, idx, STORAGE_META);

	if (lst->st == NULL)
		luaL_error(L, "mbt: storage is closed");
	return lst;
}

/* mbt.storage.file(dir): one directory, for any number of torrents. */
static int
l_storage_file(lua_State *L)
{
	const char *dir = luaL_checkstring(L, 1);
	struct lstorage *lst = lua_newuserdatauv(L, sizeof(*lst), 0);

	memset(lst, 0, sizeof(*lst));
	luaL_setmetatable(L, STORAGE_META);
	if ((lst->dir = strdup(dir)) == NULL ||
	    (lst->st = mbt_storage_file_new(dir)) == NULL)
		return fail(L, -ENOMEM);
	return 1;
}

static int
l_storage_dir(lua_State *L)
{
	lua_pushstring(L, check_storage(L, 1)->dir);
	return 1;
}

static int
l_storage_gc(lua_State *L)
{
	struct lstorage *lst = luaL_checkudata(L, 1, STORAGE_META);

	if (lst->st != NULL)
		mbt_storage_unref(lst->st);
	lst->st = NULL;
	free(lst->dir);
	lst->dir = NULL;
	return 0;
}

/* ------------------------------------------------------------------ */
/* Session                                                             */
/* ------------------------------------------------------------------ */

static struct lsession *
check_session(lua_State *L, int idx)
{
	struct lsession *ls = luaL_checkudata(L, idx, SESSION_META);

	if (ls->s == NULL)
		luaL_error(L, "mbt: session is closed");
	return ls;
}

/*
 * Free the session. With resume set, its waiters get an error. The state
 * close path clears it: their coroutines cannot run any more, and only a
 * closing state collects a session with waiters.
 */
static void
session_close(struct lsession *ls, bool resume)
{
	struct lwaiter *w;

	if (ls->s == NULL)
		return;
	mbt_session_free(ls->s);
	ls->s = NULL;
	done_forget(ls, NULL);
	if (resume) {
		waiters_ready(ls, NULL, "session closed");
	} else {
		while ((w = TAILQ_FIRST(&ls->waiters)) != NULL) {
			TAILQ_REMOVE(&ls->waiters, w, entries);
			mbt_infohash_unref(w->ih);
			free(w);
		}
	}
	free(ls->dir);
	ls->dir = NULL;
}

static int
l_session_gc(lua_State *L)
{
	session_close(luaL_checkudata(L, 1, SESSION_META), false);
	return 0;
}

static int
l_session_close(lua_State *L)
{
	session_close(luaL_checkudata(L, 1, SESSION_META), true);
	return 0;
}

static lua_Integer
opt_int(lua_State *L, int idx, const char *name, lua_Integer def)
{
	lua_Integer v = def;

	lua_getfield(L, idx, name);
	if (!lua_isnil(L, -1)) {
		if (!lua_isinteger(L, -1))
			luaL_error(L, "mbt: %s must be an integer", name);
		if ((v = lua_tointeger(L, -1)) < 0)
			luaL_error(L, "mbt: %s must not be negative", name);
	}
	lua_pop(L, 1);
	return v;
}

static bool
opt_bool(lua_State *L, int idx, const char *name, bool def)
{
	bool v;

	lua_getfield(L, idx, name);
	v = lua_isnil(L, -1) ? def : lua_toboolean(L, -1);
	lua_pop(L, 1);
	return v;
}

static mbt_picker_fn
opt_picker(lua_State *L, int idx)
{
	static const char *const names[] = {"rarest", "random", "sequential", NULL};
	static const mbt_picker_fn fns[] = {
	    mbt_picker_rarest, mbt_picker_random, mbt_picker_sequential};
	mbt_picker_fn fn;

	lua_getfield(L, idx, "picker");
	fn = fns[luaL_checkoption(L, -1, "rarest", names)];
	lua_pop(L, 1);
	return fn;
}

static void
opt_timers(lua_State *L, int idx, struct mbt_timers *t)
{
	lua_getfield(L, idx, "timers");
	if (lua_istable(L, -1)) {
		int o = lua_gettop(L);

		t->maintain_interval = (int)opt_int(L, o, "maintain_interval", 0);
		t->choke_interval = (int)opt_int(L, o, "choke_interval", 0);
		t->pex_interval = (int)opt_int(L, o, "pex_interval", 0);
		t->request_timeout = (int)opt_int(L, o, "request_timeout", 0);
		t->keepalive_interval = (int)opt_int(L, o, "keepalive_interval", 0);
		t->snub_timeout = (int)opt_int(L, o, "snub_timeout", 0);
		t->dht_refresh = (int)opt_int(L, o, "dht_refresh", 0);
		t->lsd_interval = (int)opt_int(L, o, "lsd_interval", 0);
		t->tracker_interval = (int)opt_int(L, o, "tracker_interval", 0);
		t->connect_timeout = (int)opt_int(L, o, "connect_timeout", 0);
	}
	lua_pop(L, 1);
}

/* mbt.session{...}: see README.md for the options. */
static int
l_session(lua_State *L)
{
	struct cctx *c = ctx_of(L);
	struct mbt_session_config cfg = {.ev = c->ev};
	struct mbt_session_callbacks cb = {
		.on_torrent_complete = session_complete_cb,
		.on_torrent_error = session_error_cb,
	};
	struct lstorage *lst = NULL;
	struct lsession *ls;
	const char *dir;
	bool lsd, dht;
	int rv;

	if (lua_isnoneornil(L, 1)) {
		lua_settop(L, 0);
		lua_newtable(L);
	}
	luaL_checktype(L, 1, LUA_TTABLE);
	lua_getfield(L, 1, "storage");
	if (!lua_isnil(L, -1))
		lst = check_storage(L, -1);
	lua_getfield(L, 1, "dir");
	if (lst != NULL && !lua_isnil(L, -1))
		return luaL_error(L, "mbt.session: use dir or storage, not both");
	dir = lst != NULL ? lst->dir : luaL_optstring(L, -1, ".");
	cfg.listen_port = (uint16_t)opt_int(L, 1, "port", 0);
	cfg.disable_tcp = !opt_bool(L, 1, "tcp", true);
	cfg.disable_utp = !opt_bool(L, 1, "utp", true);
	cfg.max_peers = (int)opt_int(L, 1, "max_peers", 0);
	cfg.max_pieces_per_peer = (int)opt_int(L, 1, "max_pieces_per_peer", 0);
	cfg.min_peers_before_download =
	    (int)opt_int(L, 1, "min_peers_before_download", 0);
	cfg.reqq = (int)opt_int(L, 1, "reqq", 0);
	cfg.max_pipeline = (int)opt_int(L, 1, "max_pipeline", 0);
	cfg.upload_rate = (uint64_t)opt_int(L, 1, "upload_rate", 0);
	cfg.download_rate = (uint64_t)opt_int(L, 1, "download_rate", 0);
	cfg.utp_window_pkts = (uint32_t)opt_int(L, 1, "utp_window_pkts", 0);
	cfg.mbt_lsd_announce = opt_bool(L, 1, "lsd", false);
	cfg.enable_pcp = opt_bool(L, 1, "pcp", false);
	cfg.picker = opt_picker(L, 1);
	opt_timers(L, 1, &cfg.timers);
	lsd = cfg.mbt_lsd_announce != 0;
	dht = opt_bool(L, 1, "dht", false);

	ls = lua_newuserdatauv(L, sizeof(*ls), 2);
	memset(ls, 0, sizeof(*ls));
	ls->c = c;
	TAILQ_INIT(&ls->waiters);
	luaL_setmetatable(L, SESSION_META);
	lua_getfield(L, 1, "on_complete");
	lua_setiuservalue(L, -2, UV_ON_COMPLETE);
	lua_getfield(L, 1, "on_error");
	lua_setiuservalue(L, -2, UV_ON_ERROR);

	if ((ls->dir = strdup(dir)) == NULL)
		return luaL_error(L, "mbt.session: out of memory");
	cfg.default_data_dir = ls->dir;
	cfg.default_storage = lst != NULL ? mbt_storage_ref(lst->st) :
	    mbt_storage_file_new(ls->dir);
	if (cfg.default_storage == NULL) {
		free(ls->dir);
		ls->dir = NULL;
		return fail(L, -ENOMEM);
	}
	rv = mbt_session_new(&ls->s, &cfg);
	mbt_storage_unref(cfg.default_storage);
	if (rv < 0) {
		ls->s = NULL;
		free(ls->dir);
		ls->dir = NULL;
		return fail(L, rv);
	}
	cb.arg = ls;
	mbt_session_set_callbacks(ls->s, &cb);
	if (lsd && (rv = mbt_session_enable_lsd(ls->s)) < 0) {
		session_close(ls, false);
		return fail(L, rv);
	}
	if (dht && (rv = mbt_session_enable_dht(ls->s)) < 0) {
		session_close(ls, false);
		return fail(L, rv);
	}
	lua_getfield(L, LUA_REGISTRYINDEX, SESSIONS_KEY);
	lua_pushvalue(L, -2);
	lua_rawsetp(L, -2, ls);
	lua_pop(L, 1);
	return 1;
}

/*
 * The options of an add call. Their values stay on the stack. A dir option
 * makes a storage that the caller must drop in add_done.
 */
static int
add_opts(lua_State *L, int idx, struct mbt_add_torrent_cfg *cfg,
    struct mbt_storage **own)
{
	struct lstorage *lst = NULL;
	const char *dir;

	*own = NULL;
	if (lua_isnoneornil(L, idx))
		return 0;
	luaL_checktype(L, idx, LUA_TTABLE);
	lua_getfield(L, idx, "storage");
	if (!lua_isnil(L, -1))
		lst = check_storage(L, -1);
	lua_getfield(L, idx, "dir");
	dir = luaL_optstring(L, -1, NULL);
	if (lst != NULL && dir != NULL)
		return luaL_error(L, "mbt: use dir or storage, not both");
	if (lst != NULL) {
		cfg->storage = lst->st;
		cfg->data_dir = lst->dir;
	} else if (dir != NULL) {
		if ((*own = mbt_storage_file_new(dir)) == NULL)
			return -ENOMEM;
		cfg->storage = *own;
		cfg->data_dir = dir;
	}
	return 0;
}

/* Finish an add. The session is at index 1. */
static int
add_done(lua_State *L, struct mbt_storage *own, int rv, struct mbt_torrent *t)
{
	if (own != NULL)
		mbt_storage_unref(own);
	if (rv < 0)
		return fail(L, rv);
	torrent_push(L, 1, mbt_torrent_infohash(t));
	return 1;
}

static int
l_add_torrent_buf(lua_State *L)
{
	struct lsession *ls = check_session(L, 1);
	size_t len;
	const char *data = luaL_checklstring(L, 2, &len);
	struct mbt_add_torrent_cfg cfg = {0};
	struct mbt_storage *own;
	struct mbt_torrent *t = NULL;
	int rv = add_opts(L, 3, &cfg, &own);

	if (rv == 0)
		rv = mbt_session_add_torrent_buf(ls->s, data, len, &cfg, &t);
	return add_done(L, own, rv, t);
}

static int
l_add_torrent_file(lua_State *L)
{
	struct lsession *ls = check_session(L, 1);
	const char *path = luaL_checkstring(L, 2);
	struct mbt_add_torrent_cfg cfg = {0};
	struct mbt_storage *own;
	struct mbt_torrent *t = NULL;
	int rv = add_opts(L, 3, &cfg, &own);

	if (rv == 0)
		rv = mbt_session_add_torrent_file(ls->s, path, &cfg, &t);
	return add_done(L, own, rv, t);
}

static int
l_add_magnet(lua_State *L)
{
	struct lsession *ls = check_session(L, 1);
	const char *uri = luaL_checkstring(L, 2);
	struct mbt_add_torrent_cfg cfg = {0};
	struct mbt_storage *own;
	struct mbt_torrent *t = NULL;
	int rv = add_opts(L, 3, &cfg, &own);

	if (rv == 0)
		rv = mbt_session_add_magnet(ls->s, uri, &cfg, &t);
	return add_done(L, own, rv, t);
}

static int
l_add_infohash(lua_State *L)
{
	struct lsession *ls = check_session(L, 1);
	const char *hex = luaL_checkstring(L, 2);
	struct mbt_add_torrent_cfg cfg = {0};
	struct mbt_infohash *ih = NULL;
	struct mbt_storage *own;
	struct mbt_torrent *t = NULL;
	int rv = add_opts(L, 3, &cfg, &own);

	if (rv == 0)
		rv = mbt_infohash_new(&ih, MBT_INFOHASH_SHA1, hex);
	if (rv == 0) {
		rv = mbt_session_add_infohash(ls->s, ih, &cfg, &t);
		mbt_infohash_unref(ih);
	}
	return add_done(L, own, rv, t);
}

static int
l_find(lua_State *L)
{
	struct lsession *ls = check_session(L, 1);
	struct mbt_infohash *ih = NULL;
	int rv = mbt_infohash_new(&ih, MBT_INFOHASH_SHA1, luaL_checkstring(L, 2));

	if (rv < 0)
		return fail(L, rv);
	if (mbt_session_find_torrent(ls->s, ih) == NULL)
		lua_pushnil(L);
	else
		torrent_push(L, 1, ih);
	mbt_infohash_unref(ih);
	return 1;
}

static int
l_torrents(lua_State *L)
{
	struct lsession *ls = check_session(L, 1);
	struct mbt_torrent *t;
	lua_Integer i = 1;

	lua_newtable(L);
	for (t = mbt_session_torrent_first(ls->s); t != NULL;
	    t = mbt_session_torrent_next(ls->s, t)) {
		torrent_push(L, 1, mbt_torrent_infohash(t));
		lua_rawseti(L, -2, i++);
	}
	return 1;
}

static int
l_session_listen_port(lua_State *L)
{
	lua_pushinteger(L, mbt_session_listen_port(check_session(L, 1)->s));
	return 1;
}

static int
l_session_dht_port(lua_State *L)
{
	lua_pushinteger(L, mbt_session_dht_port(check_session(L, 1)->s));
	return 1;
}

static int
l_session_dht_bootstrap(lua_State *L)
{
	struct lsession *ls = check_session(L, 1);
	const char *addr = luaL_checkstring(L, 2);
	lua_Integer port = luaL_checkinteger(L, 3);
	int rv;

	luaL_argcheck(L, port > 0 && port <= UINT16_MAX, 3, "bad port");
	rv = mbt_session_dht_bootstrap(ls->s, addr, (uint16_t)port);
	if (rv < 0)
		return fail(L, rv);
	lua_pushboolean(L, 1);
	return 1;
}

static int
l_session_reannounce(lua_State *L)
{
	int rv = mbt_session_reannounce(check_session(L, 1)->s, NULL);

	if (rv < 0)
		return fail(L, rv);
	lua_pushboolean(L, 1);
	return 1;
}

static uint64_t
check_rate(lua_State *L, int idx)
{
	lua_Integer v = luaL_checkinteger(L, idx);

	luaL_argcheck(L, v >= 0, idx, "must not be negative");
	return (uint64_t)v;
}

static int
l_session_set_upload_rate(lua_State *L)
{
	mbt_session_set_upload_rate(check_session(L, 1)->s, check_rate(L, 2));
	return 0;
}

static int
l_session_set_download_rate(lua_State *L)
{
	mbt_session_set_download_rate(check_session(L, 1)->s, check_rate(L, 2));
	return 0;
}

static int
l_session_upload_rate(lua_State *L)
{
	lua_pushinteger(L, (lua_Integer)mbt_session_upload_rate(check_session(L, 1)->s));
	return 1;
}

static int
l_session_download_rate(lua_State *L)
{
	lua_pushinteger(L, (lua_Integer)mbt_session_download_rate(check_session(L, 1)->s));
	return 1;
}

/* ------------------------------------------------------------------ */
/* Torrent                                                             */
/* ------------------------------------------------------------------ */

/*
 * A torrent handle names its torrent by infohash, so a removed torrent or a
 * closed session raises an error and never reaches freed memory. The user
 * value keeps the session userdata alive.
 */
static void
torrent_push(lua_State *L, int session_idx, struct mbt_infohash *ih)
{
	struct ltorrent *lt;

	session_idx = lua_absindex(L, session_idx);
	lt = lua_newuserdatauv(L, sizeof(*lt), 1);
	lt->ih = mbt_infohash_ref(ih);
	luaL_setmetatable(L, TORRENT_META);
	lua_pushvalue(L, session_idx);
	lua_setiuservalue(L, -2, 1);
}

static struct lsession *
torrent_session(lua_State *L, int idx)
{
	struct lsession *ls;

	lua_getiuservalue(L, idx, 1);
	ls = lua_touserdata(L, -1);
	lua_pop(L, 1);
	return ls;
}

static struct mbt_torrent *
check_torrent(lua_State *L, int idx, struct lsession **lsp)
{
	struct ltorrent *lt = luaL_checkudata(L, idx, TORRENT_META);
	struct lsession *ls = torrent_session(L, idx);
	struct mbt_torrent *t;

	if (ls->s == NULL)
		luaL_error(L, "mbt: session is closed");
	if ((t = mbt_session_find_torrent(ls->s, lt->ih)) == NULL)
		luaL_error(L, "mbt: torrent was removed");
	if (lsp != NULL)
		*lsp = ls;
	return t;
}

static int
l_torrent_gc(lua_State *L)
{
	struct ltorrent *lt = luaL_checkudata(L, 1, TORRENT_META);

	mbt_infohash_unref(lt->ih);
	lt->ih = NULL;
	return 0;
}

static int
l_torrent_eq(lua_State *L)
{
	struct ltorrent *a = luaL_checkudata(L, 1, TORRENT_META);
	struct ltorrent *b = luaL_checkudata(L, 2, TORRENT_META);

	lua_pushboolean(L, torrent_session(L, 1) == torrent_session(L, 2) &&
	    mbt_infohash_cmp(a->ih, b->ih) == 0);
	return 1;
}

static void
push_infohash(lua_State *L, struct mbt_infohash *ih)
{
	char hex[MBT_MBT_INFOHASH_SHA1HEXLEN + 1];

	if (mbt_infohash_string(ih, hex, sizeof(hex)) < 0)
		lua_pushnil(L);
	else
		lua_pushstring(L, hex);
}

static int
l_torrent_tostring(lua_State *L)
{
	struct ltorrent *lt = luaL_checkudata(L, 1, TORRENT_META);

	lua_pushliteral(L, "mbt.torrent: ");
	push_infohash(L, lt->ih);
	lua_concat(L, 2);
	return 1;
}

static int
l_infohash(lua_State *L)
{
	push_infohash(L, ((struct ltorrent *)luaL_checkudata(L, 1, TORRENT_META))->ih);
	return 1;
}

static int
l_name(lua_State *L)
{
	lua_pushstring(L, mbt_torrent_name(check_torrent(L, 1, NULL)));
	return 1;
}

static const char *
state_name(enum mbt_torrent_state s)
{
	switch (s) {
	case MBT_TORRENT_INIT:		return "init";
	case MBT_TORRENT_VERIFYING:	return "verifying";
	case MBT_TORRENT_DOWNLOADING:	return "downloading";
	case MBT_TORRENT_STALLED:	return "stalled";
	case MBT_TORRENT_SEEDING:	return "seeding";
	case MBT_TORRENT_ERROR:		return "error";
	}
	return "unknown";
}

static int
l_state(lua_State *L)
{
	lua_pushstring(L, state_name(mbt_torrent_state(check_torrent(L, 1, NULL))));
	return 1;
}

static int
l_progress(lua_State *L)
{
	lua_pushnumber(L, mbt_torrent_progress(check_torrent(L, 1, NULL)));
	return 1;
}

static int
l_complete(lua_State *L)
{
	lua_pushboolean(L, mbt_torrent_complete(check_torrent(L, 1, NULL)));
	return 1;
}

static int
l_has_metadata(lua_State *L)
{
	lua_pushboolean(L, mbt_torrent_has_metadata(check_torrent(L, 1, NULL)));
	return 1;
}

#define SET_INT(k, v) \
	do { lua_pushinteger(L, (lua_Integer)(v)); lua_setfield(L, -2, (k)); } while (0)
#define SET_BOOL(k, v) \
	do { lua_pushboolean(L, (v)); lua_setfield(L, -2, (k)); } while (0)
#define SET_STR(k, v) \
	do { lua_pushstring(L, (v)); lua_setfield(L, -2, (k)); } while (0)

static int
l_stats(lua_State *L)
{
	struct mbt_torrent *t = check_torrent(L, 1, NULL);

	lua_createtable(L, 0, 14);
	SET_STR("name", mbt_torrent_name(t));
	SET_STR("state", state_name(mbt_torrent_state(t)));
	lua_pushnumber(L, mbt_torrent_progress(t));
	lua_setfield(L, -2, "progress");
	SET_BOOL("complete", mbt_torrent_complete(t));
	SET_BOOL("has_metadata", mbt_torrent_has_metadata(t));
	SET_BOOL("private", mbt_torrent_is_private(t));
	SET_INT("piece_count", mbt_torrent_piece_count(t));
	SET_INT("piece_length", mbt_torrent_piece_length(t));
	SET_INT("have_count", mbt_torrent_have_count(t));
	SET_INT("bytes_downloaded", mbt_torrent_bytes_downloaded(t));
	SET_INT("peer_count", mbt_torrent_peer_count(t));
	SET_INT("upload_rate", mbt_torrent_upload_rate(t));
	SET_INT("download_rate", mbt_torrent_download_rate(t));
	SET_STR("dir", mbt_torrent_data_dir(t));
	if (mbt_torrent_error(t) != 0)
		SET_STR("error", strerror(-mbt_torrent_error(t)));
	return 1;
}

/* t:error(): nil, or the message and errno that stopped the torrent. */
static int
l_error(lua_State *L)
{
	int err = mbt_torrent_error(check_torrent(L, 1, NULL));

	if (err == 0) {
		lua_pushnil(L);
		return 1;
	}
	lua_pushstring(L, strerror(-err));
	lua_pushinteger(L, -err);
	return 2;
}

static const char *
source_name(enum mbt_peer_source s)
{
	switch (s) {
	case MBT_SOURCE_TRACKER:	return "tracker";
	case MBT_SOURCE_DHT:		return "dht";
	case MBT_SOURCE_PEX:		return "pex";
	case MBT_SOURCE_LSD:		return "lsd";
	case MBT_SOURCE_INCOMING:	return "incoming";
	case MBT_SOURCE_MANUAL:		return "manual";
	case MBT_SOURCE_MAGNET:		return "magnet";
	}
	return "unknown";
}

static int
l_peers(lua_State *L)
{
	struct mbt_torrent *t = check_torrent(L, 1, NULL);
	int i, n = mbt_torrent_peer_count(t);
	lua_Integer out = 1;

	lua_createtable(L, n, 0);
	for (i = 0; i < n; i++) {
		struct mbt_peer_stats ps;

		if (mbt_torrent_peer_stats(t, i, &ps) < 0)
			continue;
		lua_createtable(L, 0, 15);
		SET_STR("addr", ps.addr);
		SET_STR("client", ps.client_version);
		SET_INT("port", ps.port);
		SET_INT("conn_port", ps.conn_port);
		SET_BOOL("choked", ps.choked);
		SET_BOOL("interested", ps.interested);
		SET_BOOL("snubbed", ps.snubbed);
		SET_BOOL("utp", ps.utp);
		SET_INT("outstanding", ps.outstanding);
		SET_INT("done_pct", ps.done_pct);
		SET_INT("hash_failures", ps.hash_failures);
		SET_INT("score", ps.score);
		SET_STR("source", source_name(ps.source));
		SET_INT("downloaded", ps.downloaded);
		SET_INT("uploaded", ps.uploaded);
		lua_rawseti(L, -2, out++);
	}
	return 1;
}

static int
l_files(lua_State *L)
{
	struct mbt_torrent *t = check_torrent(L, 1, NULL);
	int i, n = mbt_torrent_file_count(t);

	lua_createtable(L, n, 0);
	for (i = 0; i < n; i++) {
		const struct mbt_file *f = mbt_torrent_file(t, i);

		lua_createtable(L, 0, 3);
		SET_STR("path", f->path);
		SET_INT("offset", f->offset);
		SET_INT("length", f->length);
		lua_rawseti(L, -2, i + 1);
	}
	return 1;
}

static int
l_trackers(lua_State *L)
{
	struct lsession *ls;
	struct mbt_torrent *t = check_torrent(L, 1, &ls);
	int i, n = mbt_session_torrent_tracker_count(ls->s, t);

	lua_createtable(L, n, 0);
	for (i = 0; i < n; i++) {
		lua_pushstring(L, mbt_session_torrent_tracker(ls->s, t, i));
		lua_rawseti(L, -2, i + 1);
	}
	return 1;
}

static int
l_add_tracker(lua_State *L)
{
	struct lsession *ls;
	struct mbt_torrent *t = check_torrent(L, 1, &ls);
	int rv = mbt_session_add_tracker(ls->s, t, luaL_checkstring(L, 2));

	if (rv < 0)
		return fail(L, rv);
	lua_pushboolean(L, 1);
	return 1;
}

/* t:add_peer(addr, port[, utp]) */
static int
l_add_peer(lua_State *L)
{
	struct lsession *ls;
	struct mbt_torrent *t = check_torrent(L, 1, &ls);
	const char *addr = luaL_checkstring(L, 2);
	lua_Integer port = luaL_checkinteger(L, 3);
	int rv;

	luaL_argcheck(L, port > 0 && port <= UINT16_MAX, 3, "bad port");
	rv = mbt_session_add_peer(ls->s, mbt_torrent_infohash(t), addr,
	    (uint16_t)port, MBT_SOURCE_MANUAL, lua_toboolean(L, 4) ? 0x04 : 0);
	if (rv < 0)
		return fail(L, rv);
	lua_pushboolean(L, 1);
	return 1;
}

static int
l_reannounce(lua_State *L)
{
	struct lsession *ls;
	struct mbt_torrent *t = check_torrent(L, 1, &ls);
	int rv = mbt_session_reannounce(ls->s, mbt_torrent_infohash(t));

	if (rv < 0)
		return fail(L, rv);
	lua_pushboolean(L, 1);
	return 1;
}

static int
l_set_upload_rate(lua_State *L)
{
	mbt_torrent_set_upload_rate(check_torrent(L, 1, NULL), check_rate(L, 2));
	return 0;
}

static int
l_set_download_rate(lua_State *L)
{
	mbt_torrent_set_download_rate(check_torrent(L, 1, NULL), check_rate(L, 2));
	return 0;
}

/* t:wait(): park the coroutine until the torrent completes. */
static int
l_wait(lua_State *L)
{
	struct lsession *ls;
	struct mbt_torrent *t = check_torrent(L, 1, &ls);
	struct ltorrent *lt = lua_touserdata(L, 1);
	struct lwaiter *w;

	if (mbt_torrent_complete(t)) {
		lua_pushboolean(L, 1);
		return 1;
	}
	if (mbt_torrent_error(t) != 0) {
		lua_pushnil(L);
		lua_pushstring(L, TORRENT_FAILED);
		return 2;
	}
	if ((w = calloc(1, sizeof(*w))) == NULL)
		return luaL_error(L, "mbt: out of memory");
	if (wait_begin(ls->c, L, &w->w) < 0) {
		free(w);
		return luaL_error(L, "mbt: call wait inside mbt.run");
	}
	w->ih = mbt_infohash_ref(lt->ih);
	TAILQ_INSERT_TAIL(&ls->waiters, w, entries);
	return lua_yieldk(L, 0, lua_gettop(L), wait_k);
}

static int
l_remove(lua_State *L)
{
	struct lsession *ls;
	struct mbt_torrent *t = check_torrent(L, 1, &ls);
	struct mbt_infohash *ih = mbt_infohash_ref(mbt_torrent_infohash(t));
	int rv;

	waiters_ready(ls, ih, "torrent removed");
	done_forget(ls, ih);
	rv = mbt_session_remove_torrent(ls->s, ih);
	mbt_infohash_unref(ih);
	if (rv < 0)
		return fail(L, rv);
	lua_pushboolean(L, 1);
	return 1;
}

/* ------------------------------------------------------------------ */
/* Metainfo and magnet                                                 */
/* ------------------------------------------------------------------ */

/* mbt.metainfo(data): parse .torrent bytes into a table. */
static int
l_metainfo(lua_State *L)
{
	size_t len;
	const char *data = luaL_checklstring(L, 1, &len);
	struct mbt_metainfo *mi = NULL;
	int i, n, rv = mbt_metainfo_parse(&mi, data, len);

	if (rv < 0)
		return fail(L, rv);
	lua_createtable(L, 0, 9);
	SET_STR("name", mbt_metainfo_name(mi));
	SET_STR("infohash", mbt_metainfo_infohash_hex(mi));
	SET_STR("comment", mbt_metainfo_comment(mi));
	SET_INT("piece_count", mbt_metainfo_piece_count(mi));
	SET_INT("piece_length", mbt_metainfo_piece_length(mi));
	SET_INT("length", mbt_metainfo_total_length(mi));
	SET_BOOL("private", mbt_metainfo_is_private(mi));
	n = mbt_metainfo_tracker_count(mi);
	lua_createtable(L, n, 0);
	for (i = 0; i < n; i++) {
		lua_pushstring(L, mbt_metainfo_tracker(mi, i));
		lua_rawseti(L, -2, i + 1);
	}
	lua_setfield(L, -2, "trackers");
	n = mbt_metainfo_file_count(mi);
	lua_createtable(L, n, 0);
	for (i = 0; i < n; i++) {
		lua_createtable(L, 0, 2);
		SET_STR("path", mbt_metainfo_file_path(mi, i));
		SET_INT("length", mbt_metainfo_file_length(mi, i));
		lua_rawseti(L, -2, i + 1);
	}
	lua_setfield(L, -2, "files");
	mbt_metainfo_free(mi);
	return 1;
}

/* mbt.create(path[, opts]): hash path, return .torrent bytes. Blocks. */
static int
l_create(lua_State *L)
{
	const char *path = luaL_checkstring(L, 1);
	struct mbt_metainfo *mi = NULL;
	lua_Integer plen = 256 * 1024;
	unsigned char *out = NULL;
	size_t len = 0;
	int rv;

	if (!lua_isnoneornil(L, 2)) {
		luaL_checktype(L, 2, LUA_TTABLE);
		plen = opt_int(L, 2, "piece_length", plen);
	}
	luaL_argcheck(L, plen > 0 && plen <= UINT32_MAX, 2, "bad piece_length");
	if ((rv = mbt_metainfo_new(&mi)) < 0)
		return fail(L, rv);
	if (lua_istable(L, 2)) {
		lua_Integer i;

		lua_getfield(L, 2, "comment");
		if (lua_isstring(L, -1))
			mbt_metainfo_set_comment(mi, lua_tostring(L, -1));
		lua_pop(L, 1);
		lua_getfield(L, 2, "private");
		mbt_metainfo_set_private(mi, lua_toboolean(L, -1));
		lua_pop(L, 1);
		lua_getfield(L, 2, "trackers");
		if (lua_istable(L, -1)) {
			for (i = 1; rv >= 0 && lua_rawgeti(L, -1, i) == LUA_TSTRING; i++) {
				rv = mbt_metainfo_add_tracker(mi, lua_tostring(L, -1));
				lua_pop(L, 1);
			}
		}
	}
	if (rv >= 0)
		rv = mbt_metainfo_set_content(mi, path, (uint32_t)plen);
	if (rv >= 0)
		rv = mbt_metainfo_encode(mi, &out, &len);
	mbt_metainfo_free(mi);
	if (rv < 0)
		return fail(L, rv);
	lua_pushlstring(L, (const char *)out, len);
	free(out);
	return 1;
}

/* mbt.magnet(uri): parse a magnet URI into a table. */
static int
l_magnet(lua_State *L)
{
	const char *uri = luaL_checkstring(L, 1);
	struct mbt_magnet m;
	int i, rv;

	memset(&m, 0, sizeof(m));
	if ((rv = mbt_magnet_parse(uri, &m)) < 0)
		return fail(L, rv);
	lua_createtable(L, 0, 4);
	SET_STR("infohash", m.infohash_hex);
	SET_STR("name", m.display_name);
	lua_createtable(L, m.tracker_count, 0);
	for (i = 0; i < m.tracker_count; i++) {
		lua_pushstring(L, m.trackers[i]);
		lua_rawseti(L, -2, i + 1);
	}
	lua_setfield(L, -2, "trackers");
	lua_createtable(L, m.peer_count, 0);
	for (i = 0; i < m.peer_count; i++) {
		lua_createtable(L, 0, 2);
		SET_STR("addr", m.peers[i].addr);
		SET_INT("port", m.peers[i].port);
		lua_rawseti(L, -2, i + 1);
	}
	lua_setfield(L, -2, "peers");
	mbt_magnet_free(&m);
	return 1;
}

/* ------------------------------------------------------------------ */
/* Logging                                                             */
/* ------------------------------------------------------------------ */

static uint32_t
log_mask(lua_State *L, int idx, const char *field)
{
	static const struct {
		const char *name;
		uint32_t bit;
	} comps[] = {
	    {"session", MBT_LOG_SESSION}, {"torrent", MBT_LOG_TORRENT},
	    {"peer", MBT_LOG_PEER}, {"conn", MBT_LOG_CONN},
	    {"lsd", MBT_LOG_LSD}, {"dht", MBT_LOG_DHT},
	    {"storage", MBT_LOG_STORAGE}, {"picker", MBT_LOG_PICKER},
	    {"tracker", MBT_LOG_TRACKER}, {"trackerd", MBT_LOG_TRACKERD},
	    {"utp", MBT_LOG_UTP}, {"all", MBT_LOG_ALL},
	};
	size_t ncomps = sizeof(comps) / sizeof(comps[0]);
	uint32_t mask = 0;
	lua_Integer i;

	lua_getfield(L, idx, field);
	if (lua_istable(L, -1)) {
		for (i = 1; lua_rawgeti(L, -1, i) != LUA_TNIL; i++) {
			const char *s = lua_tostring(L, -1);
			size_t j;

			for (j = 0; j < ncomps; j++)
				if (s != NULL && strcmp(s, comps[j].name) == 0)
					break;
			if (j == ncomps)
				luaL_error(L, "mbt.log: bad component in %s", field);
			mask |= comps[j].bit;
			lua_pop(L, 1);
		}
		lua_pop(L, 1);
	}
	lua_pop(L, 1);
	return mask;
}

/*
 * mbt.log(false) is silent; mbt.log{debug={...}, trace={...}} logs to
 * stderr. libmbt logging is per process, not per Lua state.
 */
static int
l_log(lua_State *L)
{
	if (lua_istable(L, 1)) {
		uint32_t debug = log_mask(L, 1, "debug");
		uint32_t trace = log_mask(L, 1, "trace");

		mbt_log_set(mbt_log_stderr, NULL);
		mbt_log_set_debug(debug);
		mbt_log_set_trace(trace);
	} else {
		mbt_log_set(lua_toboolean(L, 1) ? mbt_log_stderr : NULL, NULL);
		mbt_log_set_debug(0);
		mbt_log_set_trace(0);
	}
	return 0;
}

/* ------------------------------------------------------------------ */
/* Event loop                                                          */
/* ------------------------------------------------------------------ */

struct lsleep {
	TAILQ_ENTRY(lsleep) entries;
	struct wait w;
	struct mbt_ev_timer *timer;
};

static void
sleep_cb(struct mbt_ev_timer *timer, void *arg)
{
	struct lsleep *s = arg;
	struct cctx *c = s->w.c;

	TAILQ_REMOVE(&c->sleeps, s, entries);
	mbt_ev_timer_free(timer);
	wait_end(&s->w, 0);
	free(s);
}

/* mbt.sleep(ms): wait inside a coroutine. */
static int
l_sleep(lua_State *L)
{
	struct cctx *c = ctx_of(L);
	lua_Integer ms = luaL_checkinteger(L, 1);
	struct lsleep *s;
	int rv;

	luaL_argcheck(L, ms >= 0, 1, "must not be negative");
	if ((s = calloc(1, sizeof(*s))) == NULL)
		return luaL_error(L, "mbt.sleep: out of memory");
	if (wait_begin(c, L, &s->w) < 0) {
		free(s);
		return luaL_error(L, "mbt.sleep: call it inside mbt.run");
	}
	rv = mbt_ev_timer_new(c->ev, &s->timer, (uint64_t)ms * 1000, sleep_cb, s);
	if (rv < 0) {
		luaL_unref(c->L, LUA_REGISTRYINDEX, s->w.ref);
		free(s);
		return fail(L, rv);
	}
	TAILQ_INSERT_TAIL(&c->sleeps, s, entries);
	return lua_yieldk(L, 0, lua_gettop(L), wait_k);
}

/* mbt.spawn(fn, ...) runs fn in a new coroutine; errors are printed. */
static int
l_spawn(lua_State *L)
{
	int n = lua_gettop(L);

	luaL_checktype(L, 1, LUA_TFUNCTION);
	(void)task_start(ctx_of(L), L, n - 1, false);
	return 0;
}

/* The loop callbacks push on the main thread, so it must be the caller. */
static struct cctx *
check_loop_caller(lua_State *L, const char *what)
{
	struct cctx *c = ctx_of(L);

	if (L != c->L)
		luaL_error(L, "%s: call it from the main chunk", what);
	if (c->in_loop > 0)
		luaL_error(L, "%s: the loop is already running", what);
	return c;
}

/*
 * mbt.run(fn, ...): run fn in a coroutine, driving the loop until it
 * returns. Raises fn's error.
 */
static int
l_run(lua_State *L)
{
	struct cctx *c = check_loop_caller(L, "mbt.run");
	int n = lua_gettop(L);
	struct task *t;
	char *err;
	bool ok;

	luaL_checktype(L, 1, LUA_TFUNCTION);
	/* Busy from the first resume on, so fn cannot start a second run. */
	c->in_loop++;
	t = task_start(c, L, n - 1, true);
	if (t == NULL) {
		c->in_loop--;
		return luaL_error(L, "mbt.run: out of memory");
	}
	while (!t->done)
		mbt_ev_step(c->ev, -1);
	c->in_loop--;
	ok = t->ok;
	err = t->err;
	free(t);
	if (!ok) {
		lua_pushstring(L, err != NULL ? err : "error");
		free(err);
		return lua_error(L);
	}
	return 0;
}

/* mbt.step([ms]): run one loop iteration, waiting at most ms. */
static int
l_step(lua_State *L)
{
	struct cctx *c = check_loop_caller(L, "mbt.step");
	lua_Integer ms = luaL_optinteger(L, 1, 0);

	luaL_argcheck(L, ms >= 0 && ms <= INT32_MAX, 1, "out of range");
	c->in_loop++;
	mbt_ev_step(c->ev, (int)ms);
	c->in_loop--;
	return 0;
}

/* ------------------------------------------------------------------ */
/* Module                                                              */
/* ------------------------------------------------------------------ */

static const luaL_Reg session_methods[] = {
    {"add_torrent_file", l_add_torrent_file},
    {"add_torrent_buf", l_add_torrent_buf},
    {"add_magnet", l_add_magnet},
    {"add_infohash", l_add_infohash},
    {"find", l_find},
    {"torrents", l_torrents},
    {"listen_port", l_session_listen_port},
    {"dht_port", l_session_dht_port},
    {"dht_bootstrap", l_session_dht_bootstrap},
    {"reannounce", l_session_reannounce},
    {"set_upload_rate", l_session_set_upload_rate},
    {"set_download_rate", l_session_set_download_rate},
    {"upload_rate", l_session_upload_rate},
    {"download_rate", l_session_download_rate},
    {"close", l_session_close},
    {NULL, NULL},
};

static const luaL_Reg torrent_methods[] = {
    {"infohash", l_infohash},
    {"name", l_name},
    {"state", l_state},
    {"progress", l_progress},
    {"complete", l_complete},
    {"has_metadata", l_has_metadata},
    {"error", l_error},
    {"stats", l_stats},
    {"peers", l_peers},
    {"files", l_files},
    {"trackers", l_trackers},
    {"add_tracker", l_add_tracker},
    {"add_peer", l_add_peer},
    {"reannounce", l_reannounce},
    {"set_upload_rate", l_set_upload_rate},
    {"set_download_rate", l_set_download_rate},
    {"wait", l_wait},
    {"remove", l_remove},
    {NULL, NULL},
};

static const luaL_Reg storage_methods[] = {
    {"dir", l_storage_dir},
    {NULL, NULL},
};

static const luaL_Reg storage_fns[] = {
    {"file", l_storage_file},
    {NULL, NULL},
};

static const luaL_Reg module_fns[] = {
    {"session", l_session},
    {"run", l_run},
    {"spawn", l_spawn},
    {"sleep", l_sleep},
    {"step", l_step},
    {"metainfo", l_metainfo},
    {"create", l_create},
    {"magnet", l_magnet},
    {"log", l_log},
    {NULL, NULL},
};

/* The state is closing: its sessions went first, so only the loop is left. */
static int
ctx_gc(lua_State *L)
{
	struct cctx *c = luaL_checkudata(L, 1, CTX_META);
	struct lwaiter *w;
	struct lsleep *s;
	struct task *t;

	if (c->L == NULL)
		return 0;
	while ((t = TAILQ_FIRST(&c->tasks)) != NULL) {
		TAILQ_REMOVE(&c->tasks, t, entries);
		free(t);
	}
	while ((s = TAILQ_FIRST(&c->sleeps)) != NULL) {
		TAILQ_REMOVE(&c->sleeps, s, entries);
		mbt_ev_timer_free(s->timer);
		free(s);
	}
	while ((w = TAILQ_FIRST(&c->ready)) != NULL) {
		TAILQ_REMOVE(&c->ready, w, entries);
		mbt_infohash_unref(w->ih);
		free(w);
	}
	if (c->wake != NULL)
		mbt_ev_timer_free(c->wake);
	mbt_ev_destroy(c->ev);
	c->L = NULL;
	return 0;
}

int luaopen_mbt(lua_State *L);

int
luaopen_mbt(lua_State *L)
{
	/* Made before any session, so the state closes the sessions first. */
	if (ctx_of(L) == NULL) {
		struct cctx *c = lua_newuserdatauv(L, sizeof(*c), 0);
		int rv;

		memset(c, 0, sizeof(*c));
		TAILQ_INIT(&c->tasks);
		TAILQ_INIT(&c->ready);
		TAILQ_INIT(&c->done);
		TAILQ_INIT(&c->sleeps);
		if ((rv = mbt_ev_new_default(&c->ev)) < 0)
			return luaL_error(L, "mbt: cannot make an event loop: %s",
			    strerror(-rv));
		lua_rawgeti(L, LUA_REGISTRYINDEX, LUA_RIDX_MAINTHREAD);
		c->L = lua_tothread(L, -1);
		lua_pop(L, 1);
		luaL_newmetatable(L, CTX_META);
		lua_pushcfunction(L, ctx_gc);
		lua_setfield(L, -2, "__gc");
		lua_setmetatable(L, -2);
		lua_setfield(L, LUA_REGISTRYINDEX, CTX_KEY);
		lua_newtable(L);
		lua_setfield(L, LUA_REGISTRYINDEX, TASKS_KEY);
		lua_newtable(L);
		lua_createtable(L, 0, 1);
		lua_pushstring(L, "v");
		lua_setfield(L, -2, "__mode");
		lua_setmetatable(L, -2);
		lua_setfield(L, LUA_REGISTRYINDEX, SESSIONS_KEY);
		/* Peer sockets use plain write: a reset peer must not kill Lua. */
		signal(SIGPIPE, SIG_IGN);
	}
	if (luaL_newmetatable(L, SESSION_META)) {
		lua_newtable(L);
		luaL_setfuncs(L, session_methods, 0);
		lua_setfield(L, -2, "__index");
		lua_pushcfunction(L, l_session_gc);
		lua_setfield(L, -2, "__gc");
		lua_pushcfunction(L, l_session_close);
		lua_setfield(L, -2, "__close");
	}
	lua_pop(L, 1);
	if (luaL_newmetatable(L, TORRENT_META)) {
		lua_newtable(L);
		luaL_setfuncs(L, torrent_methods, 0);
		lua_setfield(L, -2, "__index");
		lua_pushcfunction(L, l_torrent_gc);
		lua_setfield(L, -2, "__gc");
		lua_pushcfunction(L, l_torrent_eq);
		lua_setfield(L, -2, "__eq");
		lua_pushcfunction(L, l_torrent_tostring);
		lua_setfield(L, -2, "__tostring");
	}
	lua_pop(L, 1);
	if (luaL_newmetatable(L, STORAGE_META)) {
		lua_newtable(L);
		luaL_setfuncs(L, storage_methods, 0);
		lua_setfield(L, -2, "__index");
		lua_pushcfunction(L, l_storage_gc);
		lua_setfield(L, -2, "__gc");
	}
	lua_pop(L, 1);
	luaL_newlib(L, module_fns);
	luaL_newlib(L, storage_fns);
	lua_setfield(L, -2, "storage");
	return 1;
}
