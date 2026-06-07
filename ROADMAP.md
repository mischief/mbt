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
- [x] Delete legacy code (bt_legacy, remote, node, test/bt_test, utp.h)

## Phase 2: Discovery (mostly complete)

- [x] Magnet URI parser (`magnet.h` — infohash, display name, trackers, peer hints)
- [x] Wire `mbt_session_add_magnet` to parse URI + connect hints + announce
- [x] HTTP tracker client (BEP 3) — via caller-provided HTTP hook (curl in mbt-get)
- [x] UDP tracker client (BEP 15) — async, built-in
- [x] Auto-announce to trackers from .torrent announce-list (BEP 12)
- [x] Re-announce timer (clamped 30s–15min)
- [x] Maintenance timer — select best peers from peerdb for connection
- [x] PEX receive (BEP 11) — parse incoming ut_pex, insert into peerdb
- [ ] PEX send — 60s timer, batch added/dropped peers, send to all connected
- [ ] DHT (BEP 5)

## Phase 3: Seeding + incoming connections (complete)

- [x] Listen socket in session — accept incoming peers
- [x] Route incoming handshake to correct torrent by infohash
- [x] Respond to request messages (read from storage, send piece)
- [x] Serve BEP 9 metadata requests to peers
- [x] Send bitfield on peer activation
- [x] Unchoke all connected peers (simple strategy)
- [x] LSD announcer (opt-in via session config)
- [ ] Send have messages on piece completion to all connected peers
- [ ] Choking algorithm (unchoke top-4 by upload rate + optimistic every 30s)
- [ ] Announce `event=completed` to tracker when download finishes
- [ ] Announce `event=stopped` on graceful shutdown

## Phase 4: Resume + robustness (started)

- [x] SHA1 verify existing pieces on startup (resume partial downloads)
- [ ] Per-peer write buffering (replace EAGAIN spin with proper EPOLLOUT queue)
- [ ] Torrent remove/pause at runtime
- [ ] Upload/download rate limiting
- [ ] `storage_memory.c` — for testing

## Phase 5: API polish (next)

- [ ] Namespace all public symbols under `mbt_*` (be_*, lsd_*, hex utils)
- [ ] Hide internal symbols (sdbuf, tpack/tunpack) via linker version script
- [ ] Install headers under `<mbt/*.h>` via meson install
- [ ] Stable public API surface: session.h, torrent.h, event.h, storage.h

## Phase 6: Tools

- [x] `mbt-get` — .torrent + magnet one-shot downloader (-S seed, -L no LSD, -l port)
- [x] `mbt-lsd` — LSD listener, auto-add discovered torrents
- [ ] `mbt-seed` — seed a file given a .torrent
- [ ] `mbt-daemon` — long-running daemon + DBus control interface

## Phase 7: Tracker server (separate library)

- [ ] `libmbt-tracker` — embeddable tracker
- [ ] HTTP announce endpoint (BEP 3)
- [ ] UDP announce (BEP 15)
- [ ] Scrape support

## Phase 8: Advanced

- [ ] µTP transport (BEP 29)
- [ ] HTTP seeding (BEP 19)
- [ ] Fast Extension (BEP 6)
- [ ] Holepunch (BEP 55)
- [ ] Private torrents (BEP 27)
- [ ] Rarest-first picker with peer availability tracking
