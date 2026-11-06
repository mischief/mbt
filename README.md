# libmbt

A production-grade BitTorrent client library and embeddable tracker, written in C.

Downloads, seeds, and tracks torrents. Pluggable event loop (sd-event, libevent or poll), pluggable storage, pluggable DNS resolver, no global state. Suitable for embedding in daemons, GUI applications, containers, or CLI tools.

## Features

- Full download + seed (piece verification, endgame, resume)
- DHT (BEP 5) with PORT message bootstrap from peers
- PEX (BEP 11), LSD (BEP 14), dual-stack IPv6
- UDP + HTTP tracker client and server
- Magnet URI support with BEP 9 metadata exchange
- Fast Extension (BEP 6), private torrents (BEP 27)
- uTP (BEP 29), holepunch (BEP 55), MSE/PE encryption with a policy
- Web seeds (BEP 19), BEP 44 DHT items, Lua bindings
- Storage on a passed descriptor, for a sandboxed process
- Choking algorithm (BEP 3 tit-for-tat, optimistic unchoke)
- Rate limiting (token bucket, per-session and per-torrent)
- NAT traversal via PCP (RFC 6887, TCP + UDP mapping)
- Rarest-first piece picker with anti-snubbing
- Async I/O throughout (no blocking in event loop)
- Async HTTP tracker callback API
- Memory-bounded (configurable pieces per peer, default 4)
- Test suite, ASan and valgrind runs, AFL fuzz targets
- Portable: Linux, OpenBSD, FreeBSD, illumos, ESP32

## Building

```
meson setup build     # debugoptimized by default, -Devent_backend=auto
ninja -C build
meson test -C build
```

Options:
```
-Devent_backend=auto|sd-event|libevent|libevent1|poll
-Ddns_backend=auto|blocking|cares|asr
-Dlua=lua5.4     # build the Lua module; see lua/README.md
-Dcurl=disabled  # no HTTPS trackers, no web seeds
-Dtests=false    # skip tests and fuzz targets
```

## Testing

```
./test.sh              # full suite: build, ASan, valgrind, symbol check
./test.sh libevent     # test with libevent backend
```

## Fuzzing

```
CC=afl-clang-fast meson setup build-fuzz -Ddefault_library=static
ninja -C build-fuzz
afl-fuzz -i fuzz/corpus -o fuzz/out_bencode -- ./build-fuzz/fuzz_bencode
```

`./fuzz.sh [name]` runs one target with ASan and UBSan.
Targets: `fuzz_bencode`, `fuzz_metainfo`, `fuzz_torrent`, `fuzz_lsd`, `fuzz_dht`,
`fuzz_magnet`, `fuzz_mse`.

## Tools

| Tool | Description |
|------|-------------|
| `mbt-get` | Download by .torrent or magnet URI (-r rate limit, -e encryption, -w web seed) |
| `mbt-tui` | ncurses client (torrent list, peers, piece map, log) |
| `mbt-torrent` | Create, inspect, convert .torrent files (-w web seed) |
| `mbt-dht` | Standalone DHT peer lookup |
| `mbt-dhtd` | Persistent DHT node |
| `mbt-tracker` | UDP + HTTP tracker daemon |
| `mbt-pcp` | PCP port mapping tool (-k for persistent) |
| `mbt-lsd` | LSD auto-discovery + replicate |
| `mbt-probe` | Connect to a peer and print handshake info |

## BEP Compliance

| BEP | Name | Status |
|-----|------|--------|
| 3 | Protocol | ✅ Complete |
| 5 | DHT + PORT | ✅ Complete |
| 6 | Fast Extension | ✅ Complete |
| 7 | IPv6 Tracker Extension | ✅ Complete |
| 9 | Metadata Exchange | ✅ Complete |
| 10 | Extension Protocol | ✅ Complete |
| 11 | PEX | ✅ Send + receive |
| 12 | Multi-tracker | ✅ Complete |
| 14 | LSD | ✅ Complete |
| 15 | UDP Tracker | ✅ Client + server |
| 19 | HTTP Seeding | ✅ Single-file torrents |
| 23 | Compact Peer Lists | ✅ Complete |
| 27 | Private Torrents | ✅ Complete |
| 29 | µTP | ✅ Complete |
| 32 | DHT IPv6 | ✅ Complete |
| 44 | DHT Items | ✅ Store and query; no signing |
| 47 | Padding Files | ✅ Storage |
| 48 | Tracker Scrape | ✅ Server |
| 51 | DHT Infohash Indexing | ✅ Server side |
| 55 | Holepunch | ✅ Complete |

MSE/PE stream encryption has no BEP. mbt implements it: see `mbt_mse(3)`.

Web seeds (BEP 19) work for single-file torrents: see `mbt_webseed(3)`.
`mbt_storage_fd_new` stores one torrent on an open descriptor, for a sandbox.

## Quick Start

```c
#include <mbt/mbt.h>

struct mbt_ev *ev;
mbt_ev_new_default(&ev);

struct mbt_storage *st = mbt_storage_file_new("downloads");
struct mbt_session_config cfg = {
    .ev = ev,
    .default_storage = st,
    .listen_port = 6881,
    .download_rate = 1024 * 1024, /* 1 MB/s */
};
struct mbt_session *s;
mbt_session_new(&s, &cfg);
mbt_storage_unref(st); /* the session holds a reference */
mbt_session_enable_dht(s);
mbt_session_add_magnet(s, "magnet:?xt=urn:btih:...", NULL, NULL);
mbt_session_run(s);

mbt_session_free(s);
mbt_ev_destroy(ev);
```

## License

ISC License — Copyright 2026 Nick Owens <mischief@offblast.org>

See [LICENSE](LICENSE) for details.
