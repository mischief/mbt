# libmbt — BitTorrent Toolkit Library Design

## Goal

A C library (`libmbt`) providing composable BitTorrent primitives. Programs built on top select which components they need. The library makes no policy decisions — no hardcoded event loops, no assumptions about storage, no single "client" main loop.

## Use Cases (example programs)

- `mbt-lsd` — LSD listener + auto-replicate (the original goal)
- `mbt-get` — one-shot download by infohash/magnet
- `mbt-seed` — seed a file/directory
- `mbt-daemon` — long-running multi-torrent daemon (like transmission-daemon)
- `mbt-bench` — raw peer wire protocol benchmark

## Library Layers

```
┌─────────────────────────────────────────────────────────┐
│  Application (mbt-lsd, mbt-get, etc.)                   │
├─────────────────────────────────────────────────────────┤
│  Session                                                │
│  - collection of torrents                               │
│  - drives event backend                                 │
│  - routes events to torrents/peers                      │
├─────────────────────────────────────────────────────────┤
│  Torrent                                                │
│  - metadata (info dict)                                 │
│  - piece state machine                                  │
│  - peer set                                             │
│  - picker strategy                                      │
│  - storage backend reference                            │
├─────────────────────────────────────────────────────────┤
│  Peer                                                   │
│  - single TCP connection                                │
│  - protocol framing                                     │
│  - handshake + extensions                               │
│  - choke/interest/bitfield state                        │
│  - request/piece/cancel messages                        │
├─────────────────────────────────────────────────────────┤
│  Protocol Components (independent, composable)          │
│  ┌──────────┐ ┌─────┐ ┌─────┐ ┌────────────────────┐  │
│  │ Bencode  │ │ LSD │ │ DHT │ │ PEX (BEP 11)       │  │
│  └──────────┘ └─────┘ └─────┘ └────────────────────┘  │
├─────────────────────────────────────────────────────────┤
│  Backend Interfaces (pluggable)                         │
│  ┌────────────────────┐  ┌───────────────────────────┐ │
│  │ Event Backend      │  │ Storage Backend            │ │
│  │ - sd-event         │  │ - file (single/multi)     │ │
│  │ - libevent         │  │ - memory                  │ │
│  │ - poll (minimal)   │  │ - block device            │ │
│  │                    │  │ - CAS (content-addressed)  │ │
│  └────────────────────┘  └───────────────────────────┘ │
└─────────────────────────────────────────────────────────┘
```

## Core Components

### Bencode (`be.c`)
Already done. Parse/encode/walk bencode values. Fuzz-tested.

### LSD (`lsd.c`)
Already done. Multicast send/recv for BEP 14 Local Service Discovery.

### DHT (`dht.c`)
Kademlia-based distributed hash table (BEP 5).
- Routing table (k-buckets)
- `get_peers` / `announce_peer` RPCs
- Token management
- Bootstrap from known nodes

### PEX (`pex.c`)
Peer Exchange (BEP 11). Exchange peer lists with connected peers via extension messages.

### Peer (`peer.c`)
One TCP connection to one remote BitTorrent client.

**Interface:**
```c
struct mbt_peer_callbacks {
    void (*on_handshake)(struct mbt_peer *, void *ctx);
    void (*on_metadata)(struct mbt_peer *, void *buf, size_t len, void *ctx);
    void (*on_piece)(struct mbt_peer *, uint32_t index, uint32_t offset, void *data, size_t len, void *ctx);
    void (*on_bitfield)(struct mbt_peer *, struct tbmap *bits, void *ctx);
    void (*on_choke)(struct mbt_peer *, int choked, void *ctx);
    void (*on_error)(struct mbt_peer *, int err, void *ctx);
};

int mbt_peer_new(struct mbt_peer **p, struct mbt_event *ev, struct mbt_peer_callbacks *cb, void *ctx);
int mbt_peer_connect(struct mbt_peer *p, const char *addr, uint16_t port, struct infohash *ih);
int mbt_peer_accept(struct mbt_peer *p, int fd, struct infohash *ih);
int mbt_peer_send_request(struct mbt_peer *p, uint32_t piece, uint32_t offset, uint32_t length);
int mbt_peer_send_piece(struct mbt_peer *p, uint32_t piece, uint32_t offset, void *data, size_t len);
int mbt_peer_send_interested(struct mbt_peer *p, int interested);
int mbt_peer_send_choke(struct mbt_peer *p, int choke);
void mbt_peer_free(struct mbt_peer *p);
```

The peer does framing and protocol only. It does NOT decide what to request — it reports events up via callbacks. The torrent/session decides.

### Torrent (`torrent.c`)
One active torrent (one infohash).

**Interface:**
```c
struct mbt_torrent_config {
    struct infohash *ih;
    struct mbt_storage *storage;
    struct mbt_picker *picker;      /* NULL = default (rarest-first) */
};

int mbt_torrent_new(struct mbt_torrent **t, struct mbt_torrent_config *cfg);
int mbt_torrent_set_metadata(struct mbt_torrent *t, void *info, size_t len);
int mbt_torrent_add_peer(struct mbt_torrent *t, struct mbt_peer *p);
int mbt_torrent_remove_peer(struct mbt_torrent *t, struct mbt_peer *p);
int mbt_torrent_tick(struct mbt_torrent *t);  /* drive piece requests, choking decisions */
float mbt_torrent_progress(struct mbt_torrent *t);
void mbt_torrent_free(struct mbt_torrent *t);
```

### Session (`session.c`)
Collection of torrents. Drives the event loop.

**Interface:**
```c
int mbt_session_new(struct mbt_session **s, struct mbt_event *ev);
int mbt_session_add_torrent(struct mbt_session *s, struct mbt_torrent *t);
int mbt_session_remove_torrent(struct mbt_session *s, struct mbt_torrent *t);
struct mbt_torrent *mbt_session_find(struct mbt_session *s, struct infohash *ih);
int mbt_session_run(struct mbt_session *s);  /* enter event loop */
void mbt_session_free(struct mbt_session *s);
```

## Backend Interfaces

### Event Backend (`event.h`)

```c
struct mbt_event_ops {
    int (*add_fd)(void *ctx, int fd, uint32_t events, mbt_fd_cb cb, void *arg);
    int (*del_fd)(void *ctx, int fd);
    int (*add_timer)(void *ctx, uint64_t usec, mbt_timer_cb cb, void *arg, void **handle);
    int (*del_timer)(void *ctx, void *handle);
    int (*loop)(void *ctx);
    int (*exit)(void *ctx);
};

struct mbt_event {
    struct mbt_event_ops *ops;
    void *ctx;
};
```

Implementations: `event_sd.c` (sd-event), `event_libevent.c`, `event_poll.c` (minimal).

### Storage Backend (`storage.h`)

```c
struct mbt_storage_ops {
    int (*open)(void *ctx, struct be_value *info);
    int (*write_piece)(void *ctx, size_t index, void *data, size_t len);
    int (*read_piece)(void *ctx, size_t index, void *data, size_t len);
    int (*have_piece)(void *ctx, size_t index);  /* for resume */
    void (*close)(void *ctx);
};

struct mbt_storage {
    struct mbt_storage_ops *ops;
    void *ctx;
};
```

### Piece Picker (`picker.h`)

```c
/* Returns piece index to request next, or -1 if nothing available from this peer */
typedef int (*mbt_picker_fn)(struct mbt_torrent *t, struct mbt_peer *p, void *ctx);
```

Built-in implementations:
- `mbt_picker_sequential` — in-order, good for streaming/block devices
- `mbt_picker_random` — simple, avoids hotspots
- `mbt_picker_rarest` — standard BitTorrent strategy, maximizes swarm health

The torrent holds a picker function pointer + context. Endgame mode can be implemented as a wrapper that switches strategy when few pieces remain.

Implementations:

| Backend | Use Case |
|---------|----------|
| `storage_file.c` | Normal: write pieces to file(s) on disk |
| `storage_memory.c` | Testing, small torrents, tmpfs |
| `storage_blockdev.c` | Write ISO/image directly to /dev/sdX |
| `storage_cas.c` | Content-addressed store with rolling hash (dedup) |

## File Layout

```
torrent/
├── include/
│   └── mbt/
│       ├── be.h
│       ├── peer.h
│       ├── torrent.h
│       ├── session.h
│       ├── event.h
│       ├── storage.h
│       ├── lsd.h
│       ├── dht.h
│       ├── pex.h
│       ├── infohash.h
│       └── tbmap.h
├── src/
│   ├── be.c
│   ├── peer.c
│   ├── torrent.c
│   ├── session.c
│   ├── lsd.c
│   ├── dht.c
│   ├── pex.c
│   ├── infohash.c
│   ├── tbmap.c
│   ├── hex.c
│   └── sdbuf.c
├── backends/
│   ├── event_sd.c
│   ├── event_libevent.c
│   ├── event_poll.c
│   ├── storage_file.c
│   ├── storage_memory.c
│   ├── storage_blockdev.c
│   └── storage_cas.c
├── tools/
│   ├── mbt-lsd.c        # LSD auto-replicate
│   ├── mbt-get.c        # one-shot download
│   ├── mbt-seed.c       # seed files
│   └── mbt-daemon.c     # long-running daemon
├── tests/
│   ├── be_test.c
│   ├── tbmap_test.c
│   ├── peer_test.c
│   └── ...
├── fuzz/
│   └── ...
├── DESIGN.md
└── meson.build
```

## Phased Implementation

### Phase 1: Minimal download (get it working)
- [ ] Refactor peer.c with callback interface
- [ ] Implement storage_file.c (single-file only)
- [ ] Fix torrent piece request loop
- [ ] Wire up: LSD → session → torrent → peer → storage
- [ ] Test: download one torrent from transmission on LAN

### Phase 2: Robustness
- [ ] Multiple peers per torrent
- [ ] Rarest-first piece picker
- [ ] Endgame mode
- [ ] Proper choking algorithm (tit-for-tat)
- [ ] Incoming connections (accept)
- [ ] Seeding (respond to requests from storage)

### Phase 3: Features
- [ ] Multi-file torrents in storage_file
- [ ] Resume (check existing pieces on startup)
- [ ] DHT (BEP 5)
- [ ] PEX (BEP 11)
- [ ] LSD announcement (not just listening)
- [ ] storage_blockdev, storage_cas

### Phase 4: Tools
- [ ] mbt-lsd (the original use case)
- [ ] mbt-get (magnet link one-shot downloader)
- [ ] mbt-seed
- [ ] mbt-daemon + control socket

## Design Principles

1. **No global state.** Everything is explicit — pass contexts, don't use globals.
2. **Callbacks up, calls down.** Peer reports events via callbacks. Torrent/session calls down to peer/storage.
3. **No allocation policy.** Caller provides memory or library mallocs with explicit free. Refcounted where shared.
4. **Backend agnostic.** Core library never calls epoll/select/sd-event directly — goes through event ops.
5. **Testable in isolation.** Each component works standalone. Peer can be tested with a fake socket. Torrent can be tested with memory storage.
