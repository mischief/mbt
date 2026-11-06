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

`examples/mbt-get.lua` is a full download client.

## Errors

Calls that can fail at run time return `nil, message, errno`. Wrong use
raises an error: a bad argument, a call on a closed session, or a call on a
removed torrent.

## Module

- `mbt.session(opts)` makes a session, or returns nil and an error. `opts`:
  - `dir`: the default data directory (default `"."`).
  - `storage`: the default storage, in place of `dir`. `false` means none:
    each torrent then needs its own storage.
  - `webseed`: a table with `mode` (`default`, `off`, `fallback` or
    `always`), `no_peer_delay_ms`, `low_rate_delay_ms`, `low_rate`, `run_min` and
    `run_max`.
    The add calls take the same field in `opts`.
  - `ca`: PEM data to use in place of the CA files, for HTTPS.
  - `http_low_speed`, `http_low_speed_sec`: a web seed request fails below
    this many bytes per second for this long. Default 1024 and 30.
  - `webseed_max_requests`: range requests in the session at once.
  - `port`: the listen port. 0 (the default) binds a free port; see
    `s:listen_port()`.
  - `tcp`, `utp`: default true.
  - `dht`, `lsd`, `pcp`: default false.
  - `dht_bootstrap`: `false` for no default node, or a list of `"host:port"`
    strings to use in its place.
  - `state`: a directory that saves the torrent list. A new session with the
    same directory adds them again, with their trackers and web seeds.
    Fallback trackers are not saved.
  - `dht_max_stored`: infohashes the session's DHT node stores (default 64).
  - `dht_send_rate`: queries per second from the session's DHT node (default 20).
  - `picker`: `"rarest"` (default), `"random"` or `"sequential"`.
  - `upload_rate`, `download_rate`: bytes per second, 0 for no limit.
  - `encryption`: `"prefer"` (default), `"require"` or `"disable"`. See
    `enum mbt_encryption`.
  - `max_peers`, `max_pieces_per_peer`, `min_peers_before_download`, `reqq`,
    `max_pipeline`, `utp_window_pkts`: see `struct mbt_session_config`.
  - `http`: default true in builds with libcurl. false stops HTTP and HTTPS
    trackers.
  - `timers`: a table with the fields of `struct mbt_timers`, in seconds.
  - `on_complete`: `function(torrent)`. It runs in its own coroutine.
  - `on_error`: `function(torrent, message, errno)`, for a storage or
    metadata failure. It runs in its own coroutine.
- `mbt.storage.fd(fd [, {readonly = true}])` makes a storage on an open
  descriptor, for one single-file torrent. It uses no path calls. On
  success it owns `fd`. `readonly` seeds and never requests pieces.
- `st:close()` drops the Lua reference now. A torrent holds its own
  reference until it is removed. `<close>` works too.
- `mbt.storage.file(dir)` makes a file storage. Pass it to any number of
  sessions and torrents. `st:dir()` returns its directory. Two torrents on
  one storage that want one file path fail with `EEXIST`.
- `mbt.run(fn, ...)` runs `fn` in a coroutine and drives the event loop until
  `fn` returns. An error in `fn` is raised again. Call it from the main
  chunk. It does not nest.
- `mbt.spawn(fn, ...)` starts `fn` in its own coroutine. Its errors are
  printed.
- `mbt.sleep(ms)` waits inside a coroutine.
- `mbt.wait_fd(fd, mode [, ms])` waits inside a coroutine until `fd` is ready,
  so a program can serve its own descriptors without polling. `mode` is
  `"r"`, `"w"` or `"rw"`. It returns the ready mode, or `nil`, `"timeout"`
  and an errno. A descriptor has one wait at a time, and it must stay open
  until the wait ends. It needs a pollable descriptor: a regular file fails.
- `mbt.loop_fd()` is for a program with its own poll loop. It runs ready work,
  then returns a descriptor and a timeout in ms, `-1` for no limit. Call it
  before each poll. When the descriptor is readable or the timeout ends, call
  `mbt.step(0)`. Call it from the main chunk. It works on the sd-event backend,
  and on the poll backend where the system has kqueue. Others return `nil`, a
  message and `ENOTSUP`. OpenBSD needs `-Devent_backend=poll` for it.
- `mbt.tracker{port = 0, http = false, http_port = 0}` runs a tracker on the
  event loop, over UDP and, if asked, HTTP. `tr:port()`, `tr:http_port()`,
  `tr:peers()`, `tr:torrents()`, `tr:close()`. `<close>` works too.
- `mbt.now_ms()` is a monotonic clock in milliseconds, for intervals.
- `mbt.step([ms])` runs one loop iteration and waits at most `ms` (default
  0). Use it in place of `mbt.run` to drive the loop from your own loop.
- `mbt.metainfo(data)` parses `.torrent` bytes into a table with `name`,
  `infohash`, `comment`, `piece_count`, `piece_length`, `length`, `private`,
  `trackers`, `webseeds` and `files` (`path`, `length`).
- `mbt.create(path[, opts])` hashes a file or directory and returns
  `.torrent` bytes. It blocks the loop. `opts`: `piece_length` (default
  262144), `trackers`, `webseeds`, `comment` and `private`.
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
  file storage for this torrent. `opts.storage = false` adds it with no
  storage: it fetches metadata, then holds in state `no_storage` until
  `t:set_storage(st)`. Adding a torrent the session has returns the
  same torrent and ignores `opts`; a `.torrent` for a magnet with no metadata
  yet supplies its metadata. `.torrent` files are limited to 64 MiB.
- `s:find(hex)` returns a torrent or nil. `s:torrents()` returns a list.
- `s:listen_port()`, `s:dht_port()`.
- `s:enable_dht()`, `s:disable_dht()`, `s:enable_lsd()`, `s:disable_lsd()`.
- `s:dht_bootstrap(addr, port)`, `s:reannounce()`.
- `s:stop_announce()` sends event=stopped for all torrents, and returns the
  replies to wait for. `s:stops_pending()` counts them. Wait a short time
  before `s:close()`. `t:remove()` sends it for one torrent.
- `s:bytes_downloaded()`, `s:bytes_uploaded()`: totals from peers, with removed
  torrents.
- `s:set_upload_rate(n)`, `s:set_download_rate(n)`, `s:upload_rate()`,
  `s:download_rate()`.
- `s:close()` frees the session. A pending `t:wait()` returns
  `nil, "session closed"`. The `<close>` attribute also closes it.

## Torrent

A torrent value holds the infohash. Two values for one torrent compare equal.

- `t:infohash()` is uppercase hex, and works after the torrent is gone.
- `t:name()`, nil before metadata arrives.
- `t:state()`: `init`, `verifying`, `downloading`, `stalled`, `seeding`,
  `error` or `no_storage`.
- `t:progress()`, `t:complete()`, `t:has_metadata()`.
- `t:error()` returns nil, or the message and errno that stopped the torrent.
- `t:stats()` returns a table with `name`, `state`, `progress`, `complete`,
  `has_metadata`, `private`, `piece_count`, `piece_length`, `have_count`,
  `bytes_downloaded`, `bytes_uploaded`, `hash_failures`, `webseed_bytes`,
  `peer_count`, `upload_rate`, `download_rate`, `dir`
  and, after a failure, `error`.
- `t:peers()` returns a list of tables with `addr`, `client`, `port`,
  `conn_port`, `choked`, `interested`, `snubbed`, `utp`, `encryption`
  (`none`, `mse_plaintext` or `rc4`), `outstanding`,
  `done_pct`, `hash_failures`, `score`, `source`, `downloaded` and
  `uploaded`.
- `t:files()` returns a list of tables with `path`, `offset` and `length`.
- `t:trackers()`, `t:add_tracker(url)`, `t:reannounce()`. A tracker from
  `t:add_tracker` gets its first announce from `t:reannounce()` or the next
  periodic announce.
- `t:add_fallback_trackers()` adds public trackers and announces. It does
  nothing for a private torrent.
- `t:piece_avail(i)` counts peers with piece `i`, from 0. `t:metadata()` returns
  the raw info dictionary, or nothing before it is known.
- `t:set_no_upload(on)` stops sending pieces to peers. The add calls take
  `no_upload = true`. Metadata requests still get an answer.
- `t:add_webseed(url)` adds a BEP 19 web seed, for a single-file torrent.
  `t:webseeds()` lists them.
  `t:webseed_bytes()` returns the bytes from web seeds.
- `t:add_peer(addr, port[, utp])`.
- `t:set_upload_rate(n)`, `t:set_download_rate(n)`.
- `t:wait([timeout_ms])` waits inside a coroutine until the torrent completes.
  It returns true, or nil and a message when the torrent fails or the torrent
  or session goes away. After the timeout the message is `"timeout"`, and the
  torrent keeps going.
- `t:wait_metadata([timeout_ms])` works the same, until metadata arrives.
- `t:set_storage(st)` attaches storage to a torrent with none.
- `t:move(st)` moves the torrent to `st` as it runs, and returns when done.
  Each piece is verified on `st`, then the old data is deleted. On failure it
  returns nil, a message and an errno, the old data stays in use, and data on
  `st` is deleted. It fails if `st` already has these files, unless a crashed
  move left them. Call it inside a coroutine.
- `t:piece_map()` returns a string of `"0"` and `"1"`, one for each piece, or
  an empty string before metadata arrives.
- `t:read_piece(index)` returns the bytes of a verified piece, or nil and an
  error. `index` starts at 0, as on the wire, so piece `i` is byte `i + 1` of
  the map. It blocks the loop on disk.
- `t:remove()` removes the torrent. Its data stays on disk.

## DHT

`mbt.dht{port = 0, id = hex, verify = fn, peer_ttl = 7200, max_stored = 64,
max_items = 64, send_rate = 20}` makes a DHT node on its own, with
no session. `port` 0 binds a free port. `id` is 40 hex digits, and the default
comes from the machine and port. `verify` is `function(k, sig, payload)` for
BEP 44 mutable items: return true to accept. It runs at once under libmbt, so
it must not yield or close the node, and an error rejects. With no `verify`,
mutable items are refused.
It stays alive while Lua can reach it. Its lookups need `mbt.run` or
`mbt.step`.

- `d:port()`, `d:node_count()`.
- `d:bootstrap(addr, port)` sends find_node to a node.
- `d:nodes()` returns the routing table: a list of tables with `id`, `addr`
  and `port`.
- `d:get_peers(infohash)` waits inside a coroutine until the lookup ends, and
  returns a list of tables with `addr` and `port`. Up to 8 lookups run at one
  time.
- `d:announce(infohash, port)` announces to the nodes that gave a token in the
  last get_peers for this infohash, and returns how many.
- `d:id()` returns our node id in hex.
- `d:query(addr, port, name[, args])` sends a raw KRPC query to an IP address,
  waits inside a coroutine, and returns the whole reply as a table: bencode
  strings stay binary, so `reply.r.id` is 20 bytes. An error reply has
  `y == "e"`. No reply in 5 seconds returns nil, a message and an errno.
  `args` becomes the `a` dict, a table with `[1]` becomes a list.
  `args.id` defaults to our id, and `id = false` leaves it out.
- `d:send_raw(addr, port, bytes)` sends bytes as they are.
- `d:close()` frees the node. A pending `d:get_peers` returns
  `nil, "dht closed"`. The `<close>` attribute also closes it.

### BEP 44 items

The node stores items and serves `get` and `put` from other nodes. It checks
tokens, and leaves signatures to `verify`. These calls use the local store:

- `d:put(value)` stores an immutable item and returns its target in hex.
- `d:put_mutable{k = ..., sig = ..., seq = ..., v = ..., salt = ...}` stores a
  mutable item through `verify`, and returns its target.
- `d:get(target)` returns a table with `v`, and `k` and `seq` if mutable.

There is no client for the network. Build one with `d:query`: send `get` to
collect a token and `put` with it. These help:

- `mbt.bencode(value)` returns bytes. A table with `[1]` is a list, and other
  tables are dicts, so an empty table is an empty dict. Use `mbt.empty_list`
  for an empty list, also inside other values and in `d:query` args.
- `mbt.bdecode(bytes)` returns the value and the number of bytes it used, so
  data can follow it, as in a BEP 9 message. An empty list decodes to an empty
  table.
- `mbt.sha1(bytes)` returns hex. An immutable target is the sha1 of the
  bencoded value.

`examples/mbt-dht.lua` is a small lookup tool. `examples/dht-crawl.lua` walks
the DHT at a set query rate and logs replies as TSV.

## Limits

- HTTP and HTTPS trackers need a libmbt built with libcurl.
- Sessions do not save state across runs.
- Under the `lua` program, Ctrl-C raises `interrupted!` from `mbt.run`. Other
  hosts must handle SIGINT themselves.

## Process-wide effects

Loading the module sets SIGPIPE to ignored, because peer sockets write with
no `MSG_NOSIGNAL`. `mbt.log` applies to the whole process. Everything else
belongs to one Lua state, so states on separate threads do not interfere.
