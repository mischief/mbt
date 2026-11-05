# mbt for Lua

`require("mbt")` runs BitTorrent sessions from a Lua 5.4 script. Build it
with `meson setup build -Dlua=lua5.4`. The build makes `mbt.so` and installs
it in `LIBDIR/lua/5.4`, or in `-Dlua-module-directory`. Point `LUA_CPATH` at
it, for example `LUA_CPATH="$HOME/.local/lib64/lua/5.4/?.so;;"`.

## Example

```lua
local mbt = require("mbt")

local s = assert(mbt.session{dir = "downloads", port = 6881, dht = true})
local t = assert(s:add_torrent_file("debian.torrent"))

mbt.run(function()
    mbt.spawn(function()
        while not t:complete() do
            print(t:state(), t:progress())
            mbt.sleep(1000)
        end
    end)
    print(t:wait())
end)
s:close()
```

## Errors

Calls that can fail at run time return `nil, message, errno`. Wrong use
raises an error: a bad argument, a call on a closed session, or a call on a
removed torrent.

## Module

- `mbt.session(opts)` makes a session, or returns nil and an error. `opts`:
  - `dir`: the default data directory (default `"."`).
  - `storage`: the default storage, in place of `dir`.
  - `port`: the listen port. 0 (the default) binds a free port; see
    `s:listen_port()`.
  - `tcp`, `utp`: default true.
  - `dht`, `lsd`, `pcp`: default false.
  - `picker`: `"rarest"` (default), `"random"` or `"sequential"`.
  - `upload_rate`, `download_rate`: bytes per second, 0 for no limit.
  - `max_peers`, `max_pieces_per_peer`, `min_peers_before_download`, `reqq`,
    `max_pipeline`, `utp_window_pkts`: see `struct mbt_session_config`.
  - `http`: default true in builds with libcurl. false stops HTTP and HTTPS
    trackers.
  - `timers`: a table with the fields of `struct mbt_timers`, in seconds.
  - `on_complete`: `function(torrent)`. It runs in its own coroutine.
  - `on_error`: `function(torrent, message, errno)`, for a storage or
    metadata failure. It runs in its own coroutine.
- `mbt.storage.file(dir)` makes a file storage. Pass it to any number of
  sessions and torrents. `st:dir()` returns its directory. Two torrents on
  one storage that want one file path fail with `EEXIST`.
- `mbt.run(fn, ...)` runs `fn` in a coroutine and drives the event loop until
  `fn` returns. An error in `fn` is raised again. Call it from the main
  chunk. It does not nest.
- `mbt.spawn(fn, ...)` starts `fn` in its own coroutine. Its errors are
  printed.
- `mbt.sleep(ms)` waits inside a coroutine.
- `mbt.step([ms])` runs one loop iteration and waits at most `ms` (default
  0). Use it in place of `mbt.run` to drive the loop from your own loop.
- `mbt.metainfo(data)` parses `.torrent` bytes into a table with `name`,
  `infohash`, `comment`, `piece_count`, `piece_length`, `length`, `private`,
  `trackers` and `files` (`path`, `length`).
- `mbt.create(path[, opts])` hashes a file or directory and returns
  `.torrent` bytes. It blocks the loop. `opts`: `piece_length` (default
  262144), `trackers`, `comment` and `private`.
- `mbt.magnet(uri)` parses a magnet URI into a table with `infohash`, `name`,
  `trackers` and `peers` (`addr`, `port`).
- `mbt.log(false)` stops logging. `mbt.log(true)` logs to stderr.
  `mbt.log{debug = {"session", "peer"}, trace = {"utp"}}` also sets the
  debug and trace components: `session`, `torrent`, `peer`, `conn`, `lsd`,
  `dht`, `storage`, `picker`, `tracker`, `trackerd`, `utp` or `all`.

## Session

A session and its loop stay alive while Lua can reach the session, so keep a
reference. A session that the collector frees stops its transfers.

- `s:add_torrent_file(path[, opts])`, `s:add_torrent_buf(data[, opts])`,
  `s:add_magnet(uri[, opts])` and `s:add_infohash(hex[, opts])` return a
  torrent. `opts.storage` selects a storage. `opts.dir` makes a separate
  file storage for this torrent. Adding a torrent the session has returns the
  same torrent and ignores `opts`; a `.torrent` for a magnet with no metadata
  yet supplies its metadata. `.torrent` files are limited to 64 MiB.
- `s:find(hex)` returns a torrent or nil. `s:torrents()` returns a list.
- `s:listen_port()`, `s:dht_port()`.
- `s:dht_bootstrap(addr, port)`, `s:reannounce()`.
- `s:set_upload_rate(n)`, `s:set_download_rate(n)`, `s:upload_rate()`,
  `s:download_rate()`.
- `s:close()` frees the session. A pending `t:wait()` returns
  `nil, "session closed"`. The `<close>` attribute also closes it.

## Torrent

A torrent value holds the infohash. Two values for one torrent compare equal.

- `t:infohash()` is uppercase hex, and works after the torrent is gone.
- `t:name()`, nil before metadata arrives.
- `t:state()`: `init`, `verifying`, `downloading`, `stalled`, `seeding` or
  `error`.
- `t:progress()`, `t:complete()`, `t:has_metadata()`.
- `t:error()` returns nil, or the message and errno that stopped the torrent.
- `t:stats()` returns a table with `name`, `state`, `progress`, `complete`,
  `has_metadata`, `private`, `piece_count`, `piece_length`, `have_count`,
  `bytes_downloaded`, `peer_count`, `upload_rate`, `download_rate`, `dir`
  and, after a failure, `error`.
- `t:peers()` returns a list of tables with `addr`, `client`, `port`,
  `conn_port`, `choked`, `interested`, `snubbed`, `utp`, `outstanding`,
  `done_pct`, `hash_failures`, `score`, `source`, `downloaded` and
  `uploaded`.
- `t:files()` returns a list of tables with `path`, `offset` and `length`.
- `t:trackers()`, `t:add_tracker(url)`, `t:reannounce()`.
- `t:add_peer(addr, port[, utp])`.
- `t:set_upload_rate(n)`, `t:set_download_rate(n)`.
- `t:wait()` waits inside a coroutine until the torrent completes. It returns
  true, or nil and a message when the torrent fails or the torrent or session
  goes away.
- `t:remove()` removes the torrent. Its data stays on disk.

## Limits

- HTTP and HTTPS trackers need a libmbt built with libcurl.
- `t:wait()` has no timeout. Bound it with `mbt.spawn` and `mbt.sleep`.
- Sessions do not save state across runs.
- `mbt.run` blocks in the event loop, so Ctrl-C takes effect at the next Lua
  code that runs.

## Process-wide effects

Loading the module sets SIGPIPE to ignored, because peer sockets write with
no `MSG_NOSIGNAL`. `mbt.log` applies to the whole process. Everything else
belongs to one Lua state, so states on separate threads do not interfere.
