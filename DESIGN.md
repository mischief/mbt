# libmbt — BitTorrent Toolkit Library Design

## Goal

A C library (`libmbt`) providing composable BitTorrent primitives. Programs built on top select which components they need. The library makes no policy decisions — no hardcoded event loops, no assumptions about storage, no single "client" main loop.

## Use Cases (example programs)

- `mbt-tui` — ncurses interactive client (torrent list, peers, piece map, log)
- `mbt-get` — one-shot download by .torrent/magnet (-r rate limit)
- `mbt-lsd` — LSD listener + auto-replicate (LAN file distribution)
- `mbt-torrent` — create, inspect, convert .torrent files
- `mbt-dht` — standalone DHT peer lookup
- `mbt-tracker` — embeddable tracker daemon (UDP + HTTP, shared store)
- `mbt-pcp` — PCP port mapping tool (persistent with -k)
- `mbt-seed` — seed a file given a .torrent (planned)
- `mbt-daemon` — long-running daemon with control interface (planned)
- Desktop GUI client (GTK/Qt event loop integration)
- Enterprise file distribution (deploy updates via torrent to fleet)

## Architecture

```
┌─────────────────────────────────────────────────────────────┐
│  Application (mbt-tui, mbt-get, mbt-lsd, etc.)             │
├─────────────────────────────────────────────────────────────┤
│  Session (session.c)                                        │
│  - torrent collection (add/remove/find/iterate)             │
│  - async peer connect + accept                              │
│  - DHT, LSD, PEX integration                               │
│  - PCP NAT traversal (TCP + UDP)                            │
│  - rate limiting (per-session + per-torrent)                │
│  - self-connection rejection                                │
├─────────────────────────────────────────────────────────────┤
│  Torrent (torrent.c)                                        │
│  - peer set management                                      │
│  - piece state machine (bitmap = authority)                  │
│  - drives requests via picker (multi-piece per peer)         │
│  - SHA1 verification + resume                               │
│  - metadata assembly (BEP 9)                                │
│  - request timeout + peer banning + endgame + anti-snubbing │
│  - rate-limit-aware request pipelining                      │
│  - state: init/downloading/stalled/seeding                  │
│  - choking algorithm (tit-for-tat + optimistic)             │
│  - seed-to-seed disconnect                                  │
├─────────────────────────────────────────────────────────────┤
│  Peer (peer.c)                                              │
│  - state machine: HANDSHAKE → EXTENSION → ACTIVE → CLOSED  │
│  - callbacks up (on_piece, on_choke, on_bitfield, on_port)  │
│  - commands down (send_request, send_interested, send_port) │
│  - BEP 5 PORT, BEP 6 Fast, BEP 10 Extension Protocol       │
│  - bitfield tracking (allocated lazily on HAVE)             │
├─────────────────────────────────────────────────────────────┤
│  Connection (conn.h)                                        │
│  - buffered bidirectional stream                            │
│  - watermark-based read callbacks                           │
│  ┌──────────────┐  ┌────────────────────┐                  │
│  │ conn_sd.c    │  │ conn_libevent.c    │                  │
│  │ (sdbuf+epoll)│  │ (bufferevent)      │                  │
│  └──────────────┘  └────────────────────┘                  │
├─────────────────────────────────────────────────────────────┤
│  Event Loop (event.h)                                       │
│  - io watch, timers, run/stop                               │
│  - caller-owned loop support (_new_from)                    │
│  ┌──────────────┐  ┌────────────────────┐                  │
│  │ ev_sd.c      │  │ ev_libevent.c      │                  │
│  └──────────────┘  └────────────────────┘                  │
├─────────────────────────────────────────────────────────────┤
│  DNS Resolver (resolve.h)                                   │
│  - async hostname resolution                                │
│  ┌──────────────┐  ┌────────────────┐  ┌───────────────┐  │
│  │ blocking     │  │ c-ares         │  │ asr (OpenBSD) │  │
│  └──────────────┘  └────────────────┘  └───────────────┘  │
├─────────────────────────────────────────────────────────────┤
│  Utilities (no deps on layers above)                        │
│  ┌────────┐ ┌──────┐ ┌────────┐ ┌──────┐ ┌─────┐ ┌─────┐ │
│  │bencode │ │bitmap│ │infohash│ │ pack │ │ hex │ │ log │ │
│  └────────┘ └──────┘ └────────┘ └──────┘ └─────┘ └─────┘ │
├─────────────────────────────────────────────────────────────┤
│  Protocol Components                                        │
│  ┌─────┐ ┌────────┐ ┌─────────┐ ┌─────┐ ┌────────┐       │
│  │ LSD │ │ Magnet │ │Metainfo │ │ DHT │ │Peer DB │       │
│  └─────┘ └────────┘ └─────────┘ └─────┘ └────────┘       │
│  ┌──────────────────┐ ┌────────────────┐ ┌─────────┐      │
│  │ Tracker (client) │ │Tracker (server)│ │   PCP   │      │
│  └──────────────────┘ └────────────────┘ └─────────┘      │
├─────────────────────────────────────────────────────────────┤
│  Pluggable Backends                                         │
│  ┌────────────────────┐ ┌───────────────┐ ┌─────────────┐ │
│  │ Storage            │ │ Picker        │ │ Rate Limit  │ │
│  │ - file (multi-file)│ │ - sequential  │ │ token bucket│ │
│  │ - memory (testing) │ │ - random      │ │ per-session │ │
│  │                    │ │ - rarest-first│ │ per-torrent │ │
│  └────────────────────┘ └───────────────┘ └─────────────┘ │
└─────────────────────────────────────────────────────────────┘
```

## Public Headers

| Header | Purpose |
|--------|---------|
| `mbt/mbt.h` | Convenience super-header (includes all below) |
| `mbt/session.h` | Session lifecycle, torrent management, DHT/LSD/PCP |
| `mbt/torrent.h` | Torrent state, piece info, per-torrent rate limiting |
| `mbt/peer.h` | Peer callbacks, send commands, peer stats |
| `mbt/event.h` | Event loop abstraction (io, timers, run/stop) |
| `mbt/conn.h` | Buffered connection (watermark, read/write) |
| `mbt/resolve.h` | Async DNS resolution |
| `mbt/storage.h` | Storage ops interface (file, memory) |
| `mbt/picker.h` | Piece picker interface (sequential, random, rarest) |
| `mbt/ratelimit.h` | Token bucket rate limiter |
| `mbt/tracker.h` | Tracker client (UDP built-in, HTTP via async callback) |
| `mbt/tracker_server.h` | Embeddable tracker (UDP + HTTP) |
| `mbt/dht.h` | DHT node (BEP 5, BEP 32) |
| `mbt/pcp.h` | PCP client (RFC 6887, NAT port mapping) |
| `mbt/lsd.h` | Local Service Discovery (BEP 14) |
| `mbt/metainfo.h` | .torrent parse/create/encode |
| `mbt/magnet.h` | Magnet URI parser |
| `mbt/be.h` | Bencode parser/encoder |
| `mbt/bitmap.h` | Bit-accurate bitmap (refcounted) |
| `mbt/infohash.h` | Infohash type (refcounted, SHA1/SHA256) |
| `mbt/peerdb.h` | Peer database (RB tree, source tracking, backoff) |
| `mbt/timers.h` | Timer configuration struct |
| `mbt/log.h` | Logging (component mask, custom callback) |
| `mbt/hex.h` | Hex encode/decode |
| `mbt/pack.h` | Wire format pack/unpack |

## Design Principles

1. **No global state.** Everything explicit — pass contexts, don't use globals.
2. **Callbacks up, calls down.** Peer reports events via callbacks. Torrent/session calls down.
3. **Backend agnostic.** Core never calls epoll/sd-event directly — goes through event ops.
4. **Compile-time backend selection.** Event loop, DNS resolver, conn layer chosen at build time via meson options. Auto-detected per platform.
5. **Testable in isolation.** Each component works standalone. 42 tests, ASan + valgrind clean.
6. **Silent by default.** No output unless caller sets a log callback.
7. **`mbt_` namespace.** All public symbols use `mbt_` prefix. Internal symbols hidden via `-fvisibility=hidden`.
8. **Dual-stack networking.** All sockets handle IPv4 and IPv6. `IPV6_V6ONLY=1` on all v6 sockets.
9. **Portable.** Tested on Linux (gcc + clang), OpenBSD 7.9, FreeBSD 15.0. Conditional libbsd/libmd on Linux only.
10. **Async HTTP callback.** HTTP tracker uses caller-provided async callback (can be curl, thread+pipe, or any mechanism).
11. **Configurable timers.** All internal intervals exposed via `mbt_timers` struct.
12. **Rate limiting.** Token bucket per-session and per-torrent. Consume at request time, retry timer on empty bucket.
13. **Ephemeral ports.** All listeners support port=0 with accessors for actual bound port.

## Build

```
meson setup build                         # auto-detect everything
meson setup build -Devent_backend=libevent # explicit backend
meson setup build -Ddns_backend=cares     # async DNS via c-ares
ninja -C build && meson test -C build
```

## File Layout

```
├── mbt/                    # public headers (installed to <mbt/*.h>)
├── *.c                     # library implementation
├── internal.h              # shared internal definitions
├── net.h / net.c           # portable listener helpers
├── tools/                  # CLI tools
├── test/                   # 42 unit + integration tests
├── fuzz/                   # AFL++ harness + corpus
├── man/                    # mdoc man pages
├── libmbt.map              # linker version script
├── meson.build
├── meson.options
├── test.sh                 # full CI: build + ASan + valgrind + symbols
├── DESIGN.md
├── ROADMAP.md
└── README.md
```
