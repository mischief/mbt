# libmbt — BitTorrent Toolkit Library Design

## Goal

A C library (`libmbt`) providing composable BitTorrent primitives. Programs built on top select which components they need. The library makes no policy decisions — no hardcoded event loops, no assumptions about storage, no single "client" main loop.

## Use Cases (example programs)

- `mbt-lsd` — LSD listener + auto-replicate (LAN file distribution)
- `mbt-get` — one-shot download by infohash/magnet
- `mbt-seed` — seed a file/directory
- `mbt-tracker` — embeddable tracker (UDP + HTTP, shared store)
- `mbt-daemon` — long-running multi-torrent daemon with DBus/control API
- Enterprise file distribution (deploy updates via torrent to fleet)
- NAS/seedbox appliance (headless, resource-constrained)
- Desktop GUI client (GTK/Qt event loop integration)

## Architecture

```
┌─────────────────────────────────────────────────────────────┐
│  Application (mbt-get, mbt-lsd, etc.)                       │
├─────────────────────────────────────────────────────────────┤
│  Session (session.c)                                        │
│  - torrent collection (add/remove/find)                     │
│  - async peer connect                                       │
│  - LSD integration                                          │
│  - mbt_session_default() zero-config constructor            │
├─────────────────────────────────────────────────────────────┤
│  Torrent (torrent.c)                                        │
│  - peer set management                                      │
│  - piece state machine (bitmap = authority)                  │
│  - drives requests via picker                               │
│  - SHA1 verification                                        │
│  - metadata assembly (BEP 9)                                │
│  - request timeout + peer banning + endgame mode            │
├─────────────────────────────────────────────────────────────┤
│  Peer (peer.c)                                              │
│  - state machine: HANDSHAKE → EXTENSION → ACTIVE → CLOSED  │
│  - callbacks up (on_piece, on_choke, on_bitfield, ...)      │
│  - commands down (send_request, send_interested, ...)       │
│  - BEP 10 extension protocol                               │
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
│  Utilities (no deps on layers above)                        │
│  ┌────────┐ ┌──────┐ ┌────────┐ ┌──────┐ ┌─────┐ ┌─────┐ │
│  │bencode │ │bitmap│ │infohash│ │ pack │ │ hex │ │ log │ │
│  └────────┘ └──────┘ └────────┘ └──────┘ └─────┘ └─────┘ │
├─────────────────────────────────────────────────────────────┤
│  Protocol Components                                        │
│  ┌─────┐ ┌────────┐ ┌─────────┐ ┌─────────┐ ┌──────────┐ │
│  │ LSD │ │ Magnet │ │ Tracker │ │ DHT     │ │ Peer DB  │ │
│  └─────┘ └────────┘ └─────────┘ └─────────┘ └──────────┘ │
├─────────────────────────────────────────────────────────────┤
│  Pluggable Backends                                         │
│  ┌────────────────────┐  ┌───────────────────┐             │
│  │ Storage            │  │ Picker            │             │
│  │ - file (dir+name)  │  │ - sequential      │             │
│  │ - memory (testing) │  │ - random          │             │
│  │ - blockdev (later) │  │ - rarest (later)  │             │
│  └────────────────────┘  └───────────────────┘             │
└─────────────────────────────────────────────────────────────┘
```

## Design Principles

1. **No global state.** Everything explicit — pass contexts, don't use globals.
2. **Callbacks up, calls down.** Peer reports events via callbacks. Torrent/session calls down.
3. **Backend agnostic.** Core never calls epoll/sd-event directly — goes through event ops.
4. **Testable in isolation.** Each component works standalone.
5. **Silent by default.** No output unless caller sets a log callback.
6. **Safe for release builds.** External input validated with error returns, not asserts.
7. **`mbt_` namespace.** All public types and functions use the `mbt_` prefix.
8. **Dual-stack networking.** All socket code must handle IPv4 and IPv6. No AF_INET-only assumptions.
9. **Policy via callbacks.** The library never makes policy decisions — callers decide:
   - Which incoming peers to accept or reject
   - Which torrents to add, pause, or remove
   - Per-torrent and per-peer resource limits
   - Whether to allow DHT/PEX/LSD on a per-torrent basis
   - Custom logging, metrics, and audit hooks
10. **Observable.** Callers can inspect all runtime state: peer lists, piece maps, transfer rates, tracker status. Expose enough for a GUI, monitoring dashboard, or Prometheus exporter.
11. **Embeddable.** The library must work correctly in:
    - Headless daemons (long-running, no terminal)
    - GUI applications (integrated event loop)
    - Containers and orchestrated environments (no filesystem assumptions beyond what storage backend provides)
    - Multi-tenant servers (multiple sessions, independent lifecycle)
12. **Configurable timers.** All internal timers (request timeout, choke interval, DHT refresh, maintenance tick, LSD announce, tracker re-announce) must be configurable by the caller. This enables fast-running tests and tuning for specific deployment environments.

## Build

```
meson setup build -Devent_backend=sd-event   # Linux/systemd
meson setup build -Devent_backend=libevent   # portable (BSDs, macOS)
ninja -C build && meson test -C build
```

## File Layout

```
├── mbt/                                 # public headers (installed to <mbt/*.h>)
│   ├── mbt.h                           # convenience super-header
│   ├── event.h, conn.h, peer.h         # core abstractions
│   ├── session.h, torrent.h            # high-level API
│   ├── storage.h, picker.h            # pluggable backends
│   ├── be.h, bitmap.h, infohash.h     # utilities
│   ├── log.h, hex.h, pack.h           # helpers
│   ├── lsd.h, magnet.h, tracker.h     # protocol components
│   └── peerdb.h                        # peer database
├── *.c                                  # implementation files
├── tools/                               # CLI tools (mbt-get, mbt-lsd, mbt-torrent)
├── test/                               # unit + integration tests
├── fuzz/                               # AFL++ harness + corpus
├── libmbt.map                          # linker version script (symbol visibility)
├── meson.build
├── DESIGN.md
└── ROADMAP.md
```
