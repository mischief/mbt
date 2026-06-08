# libmbt

A production-grade BitTorrent client library and embeddable tracker, written in C.

Downloads, seeds, and tracks torrents. Pluggable event loop (sd-event or libevent), pluggable storage, no global state. Suitable for embedding in daemons, GUI applications, containers, or CLI tools.

## Features

- Full download + seed (piece verification, endgame, resume)
- DHT (BEP 5), PEX (BEP 11), LSD (BEP 14)
- UDP + HTTP tracker client and server
- Magnet URI support with BEP 9 metadata exchange
- Fast Extension (BEP 6), private torrents (BEP 27)
- Choking algorithm (BEP 3 tit-for-tat)
- Async I/O throughout (no blocking, no threads)
- Memory-bounded (one piece buffer per peer)
- 28 tests, ASan + valgrind clean

## Building

```
meson setup build -Devent_backend=sd-event   # or libevent
ninja -C build
meson test -C build
```

Install:
```
meson install -C build
# Headers → <prefix>/include/mbt/
# Library → <prefix>/lib/libmbt.so
```

Production build (no tests):
```
meson setup build -Devent_backend=sd-event -Dtests=false
```

## Testing

```
./test.sh              # full suite: build, ASan, valgrind, symbol check
./test.sh libevent     # test with libevent backend
```

## Tools

| Tool | Description |
|------|-------------|
| `mbt-get` | Download by .torrent or magnet URI |
| `mbt-torrent` | Create, inspect, convert .torrent files |
| `mbt-dht` | Standalone DHT peer lookup |
| `mbt-tracker` | UDP + HTTP tracker daemon |
| `mbt-lsd` | LSD auto-discovery + replicate |

## BEP Compliance

| BEP | Name | Status |
|-----|------|--------|
| 3 | Protocol | ✅ Complete |
| 5 | DHT | ✅ Complete |
| 6 | Fast Extension | ✅ Complete |
| 9 | Metadata Exchange | ✅ Complete |
| 10 | Extension Protocol | ✅ Complete |
| 11 | PEX | ✅ Send + receive |
| 12 | Multi-tracker | ✅ Complete |
| 14 | LSD | ✅ Complete |
| 15 | UDP Tracker | ✅ Client + server |
| 23 | Compact Peer Lists | ✅ Complete |
| 27 | Private Torrents | ✅ Complete |
| 19 | HTTP Seeding | ❌ Not implemented |
| 29 | µTP | ❌ Not implemented |
| 55 | Holepunch | ❌ Not implemented |

## Quick Start

```c
#include <mbt/mbt.h>

struct mbt_ev *ev;
mbt_ev_new_default(&ev);

struct mbt_session_config cfg = { .ev = ev, .listen_port = 6881 };
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
