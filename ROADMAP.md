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

## Phase 3: Seeding + incoming connections

- [ ] Listen socket in session — accept incoming peers
- [ ] Route incoming handshake to correct torrent by infohash
- [ ] Respond to request messages (read from storage, send piece)
- [ ] Send have messages on piece completion to all connected peers
- [ ] Choking algorithm (unchoke top-4 by upload rate + optimistic every 30s)
- [ ] Announce `event=completed` to tracker when download finishes
- [ ] Announce `event=stopped` on graceful shutdown

## Phase 4: Resume + robustness

- [ ] SHA1 verify existing pieces on startup (resume partial downloads)
- [ ] `storage_memory.c` — for testing
- [ ] `storage_blockdev.c` — write ISO directly to /dev/sdX

## Phase 5: Tools

- [x] `mbt-get` — .torrent + magnet one-shot downloader
- [x] `mbt-lsd` — LSD listener, auto-add discovered torrents
- [ ] `mbt-seed` — seed a file given a .torrent
- [ ] `mbt-daemon` — long-running daemon + control socket

## Phase 6: Tracker server (separate library)

- [ ] `libmbt-tracker` — embeddable tracker
- [ ] HTTP announce endpoint (BEP 3)
- [ ] UDP announce (BEP 15)
- [ ] Scrape support

## Phase 7: Advanced

- [ ] µTP transport (BEP 29)
- [ ] HTTP seeding (BEP 19)
- [ ] Fast Extension (BEP 6)
- [ ] Holepunch (BEP 55)
- [ ] Private torrents (BEP 27)
- [ ] Rarest-first picker with peer availability tracking

## Low Priority: Bencode cleanup

- [ ] Namespace rename: `be_*` → `mbt_be_*`
- [ ] Replace `struct iovec` with `struct { char *data; size_t len; }`
- [ ] Single-pass growable-buffer encoder
