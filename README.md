# libmbt

A production-grade BitTorrent client library and embeddable tracker, written in C.

Downloads, seeds, and tracks torrents. Pluggable event loop (sd-event or libevent), pluggable storage, pluggable DNS resolver, no global state. Suitable for embedding in daemons, GUI applications, containers, or CLI tools.

## Features

- Full download + seed (piece verification, endgame, resume)
- DHT (BEP 5) with PORT message bootstrap from peers
- PEX (BEP 11), LSD (BEP 14), dual-stack IPv6
- UDP + HTTP tracker client and server
- Magnet URI support with BEP 9 metadata exchange
- Fast Extension (BEP 6), private torrents (BEP 27)
- Choking algorithm (BEP 3 tit-for-tat, optimistic unchoke)
- Rate limiting (token bucket, per-session and per-torrent)
- NAT traversal via PCP (RFC 6887, TCP + UDP mapping)
- Rarest-first piece picker with anti-snubbing
- Async I/O throughout (no blocking in event loop)
- Async HTTP tracker callback API
- Memory-bounded (configurable pieces per peer, default 4)
- 43 tests, ASan + valgrind clean, AFL fuzz-tested, zero leaks
- Portable: Linux, OpenBSD 7.9 (amd64 + riscv64), FreeBSD 15.0

## Building

```
meson setup build -Devent_backend=sd-event   # or libevent
ninja -C build
meson test -C build
```

Options:
```
-Devent_backend=sd-event|libevent
-Ddns_backend=blocking|cares|asr
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

Targets: `fuzz_bencode`, `fuzz_metainfo`, `fuzz_lsd`, `fuzz_dht`, `fuzz_magnet`.

## Tools

| Tool | Description |
|------|-------------|
| `mbt-get` | Download by .torrent or magnet URI (-r for rate limit) |
| `mbt-tui` | ncurses client (torrent list, peers, piece map, log) |
| `mbt-torrent` | Create, inspect, convert .torrent files |
| `mbt-dht` | Standalone DHT peer lookup |
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
| 23 | Compact Peer Lists | ✅ Complete |
| 27 | Private Torrents | ✅ Complete |
| 32 | DHT IPv6 | ✅ Complete |
| 19 | HTTP Seeding | ❌ Not implemented |
| 29 | µTP | ❌ Not implemented |
| 55 | Holepunch | ❌ Not implemented |

## Quick Start

```c
#include <mbt/mbt.h>

struct mbt_ev *ev;
mbt_ev_new_default(&ev);

struct mbt_session_config cfg = {
    .ev = ev,
    .listen_port = 6881,
    .download_rate = 1024 * 1024, /* 1 MB/s */
};
struct mbt_session *s;
mbt_session_new(&s, &cfg);
mbt_session_enable_dht(s);
mbt_session_add_magnet(s, "magnet:?xt=urn:btih:...");
mbt_session_run(s);

mbt_session_free(s);
mbt_ev_destroy(ev);
```

## License

ISC License — Copyright 2026 Nick Owens <mischief@offblast.org>

See [LICENSE](LICENSE) for details.
