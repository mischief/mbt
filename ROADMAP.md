# libmbt Roadmap

## Phase 0: Foundation (complete)

- [x] Bencode parser (fuzz-tested, ASan-clean, leak-free)
- [x] Bitmap (`mbt_bitmap` — bit-accurate, bounds checked)
- [x] Infohash type (refcounted)
- [x] LSD multicast (BEP 14)
- [x] Event loop abstraction (sd-event + libevent, caller-owned loops)
- [x] Buffered connection abstraction (mbt_conn)
- [x] Wire format helpers (pack.c)
- [x] Meson build with backend selection
- [x] Logging subsystem (component mask, stderr/syslog/custom)

## Phase 1: Clean layering (complete)

- [x] Peer state machine (peer.c) — callbacks up, commands down
- [x] Torrent coordinator (torrent.c) — piece state, SHA1, storage, picker
- [x] Session (session.c) — torrent list, async connect, LSD integration
- [x] Storage backend (storage_file.c) — multi-file, piece spans file boundaries
- [x] Pickers — sequential + random (rarest stub)
- [x] Request timeout (15s default, configurable)
- [x] Peer banning (hash failures only, reset on success)
- [x] Endgame mode (<=10 pieces → request from all, cancel on receive)
- [x] Multi-peer download verified (SHA1 correct, both peers contribute)
- [x] Full end-to-end: connect → BEP 9 metadata → pieces → verify → disk
- [x] Peer database (peerdb.c) — RB tree, state/source/backoff, torrent-owned

## Phase 2: Discovery (mostly complete)

- [x] Magnet URI parser
- [x] HTTP tracker client (BEP 3) — via caller-provided HTTP hook
- [x] UDP tracker client (BEP 15) — async, built-in
- [x] Auto-announce to trackers from .torrent announce-list (BEP 12)
- [x] Re-announce timer (clamped 30s–15min)
- [x] Maintenance timer — select best peers from peerdb
- [x] PEX receive (BEP 11) — parse incoming ut_pex, insert into peerdb
- [ ] PEX send — 60s timer, batch added/dropped peers
- [ ] DHT (BEP 5)

## Phase 3: Seeding + incoming connections (complete)

- [x] Listen socket — accept incoming peers
- [x] Route incoming handshake to correct torrent by infohash
- [x] Respond to request messages (read from storage, send piece)
- [x] Serve BEP 9 metadata requests
- [x] Send bitfield on peer activation
- [x] Unchoke all connected peers (simple strategy)
- [x] LSD announcer (opt-in)
- [ ] Send have messages on piece completion
- [ ] Choking algorithm (unchoke top-4 + optimistic every 30s)
- [ ] Announce `event=completed` / `event=stopped` to tracker

## Phase 4: Resume + robustness (started)

- [x] SHA1 verify existing pieces on startup (resume)
- [ ] Per-peer write buffering (replace EAGAIN spin with EPOLLOUT queue)
- [ ] Torrent remove/pause at runtime
- [ ] Upload/download rate limiting

## Phase 5: API polish (complete)

- [x] All public symbols namespaced `mbt_*`
- [x] Internal symbols hidden via linker version script (libmbt.map)
- [x] Headers in `mbt/` subdir — `#include <mbt/mbt.h>`
- [x] `meson install` works (headers + shared library)
- [x] Safe bencode constructors (string_new, list_new, list_append, dict_append_string)

## Phase 6: Tools

- [x] `mbt-get` — download by .torrent/magnet (-S seed, -L no LSD, -l port, SIGINT)
- [x] `mbt-lsd` — LSD listener, auto-add discovered torrents
- [x] `mbt-torrent` — create .torrent, show metadata, emit magnet URI
- [ ] `mbt-seed` — seed a file given a .torrent
- [ ] `mbt-daemon` — long-running daemon + DBus control interface

## Phase 7: Tracker server

- [ ] `libmbt-tracker` — embeddable tracker
- [ ] HTTP announce (BEP 3), UDP announce (BEP 15), scrape

## Phase 8: Advanced

- [ ] µTP transport (BEP 29)
- [ ] Fast Extension (BEP 6)
- [ ] Holepunch (BEP 55)
- [ ] Private torrents (BEP 27)
- [ ] Rarest-first picker with peer availability tracking
