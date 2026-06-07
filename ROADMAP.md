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
- [x] Storage backend (storage_file.c) — download dir, filename from metadata
- [x] Pickers — sequential + random (rarest stub)
- [x] Request timeout (15s default, configurable)
- [x] Peer banning (3 hash failures → disconnect)
- [x] Endgame mode (<=10 pieces → request from all, cancel on receive)
- [x] Multi-peer download verified (SHA1 correct, both peers contribute)
- [x] Full end-to-end: connect → BEP 9 metadata → pieces → verify → disk

## Phase 1.5: Delete legacy code

- [ ] Rewrite `tools/mbt-lsd.c` using session API
- [ ] Delete `bt_legacy.c`, `bt_legacy.h`, `remote.c`, `remote.h`, `node.c`, `node.h`
- [ ] Delete `test/bt_test.c` (replaced by session_test)

## Phase 2: Discovery (mostly complete)

- [x] Magnet URI parser (`magnet.h` — infohash, display name, trackers, peer hints)
- [x] Wire `mbt_session_add_magnet` to parse URI + connect hints + announce
- [x] HTTP tracker client (BEP 3) — via caller-provided HTTP hook (curl in mbt-get)
- [x] UDP tracker client (BEP 15) — async, built-in
- [x] Auto-announce to trackers from .torrent announce-list (BEP 12)
- [x] Re-announce timer (clamped 30s–15min)
- [x] Peer pool + maintenance timer for refill
- [x] PEX receive (BEP 11) — parse incoming ut_pex, insert into peerdb
- [ ] PEX send — batch connected peers, send ut_pex every 60s to all peers
- [ ] DHT (BEP 5) — future
- [ ] PEX (BEP 11) — future

## Phase 3: Seeding

- [ ] Accept incoming peer connections (listen socket in session)
- [ ] Route incoming handshake to correct torrent
- [ ] Respond to request messages (read from storage, send piece)
- [ ] Choking algorithm (unchoke top-4 by upload rate + optimistic every 30s)
- [ ] Send have messages on piece completion to all connected peers
- [ ] LSD announcement (outbound multicast, BEP 14)
- [ ] Announce `event=completed` to tracker when download finishes
- [ ] Announce `event=stopped` on graceful shutdown

## Phase 4: Peer database + connection management

- [ ] `struct mbt_peer_db` — unified known-peer store (replaces fixed pool array)
- [ ] Source tracking: tracker, DHT, PEX, LSD, incoming
- [ ] State machine: known → connecting → active → disconnected → banned
- [ ] Priority scoring: prefer recent, low-failure, low-latency peers
- [ ] Dedup: don't connect to same addr:port twice
- [ ] Persist across re-announces (don't lose peers on timer)
- [ ] Integrate with session maintenance timer for smart refill

## Phase 4a: Storage improvements

- [ ] Multi-file torrents (piece spans across file boundaries)
- [ ] Resume (SHA1 verify existing pieces on startup)
- [ ] `storage_memory.c` — for testing
- [ ] `storage_blockdev.c` — write ISO directly to /dev/sdX

## Phase 5: Tools

- [x] `mbt-get` — .torrent + magnet one-shot downloader (sequential/random picker, -d debug, -p peers, -o dir)
- [ ] `mbt-lsd` — LSD auto-replicate (watch + download + seed)
- [ ] `mbt-seed` — seed a file given a .torrent
- [ ] `mbt-daemon` — long-running daemon + control socket

## Phase 6: Tracker server (separate library)

- [ ] `libmbt-tracker` — embeddable tracker in the same repo
- [ ] HTTP announce endpoint (BEP 3)
- [ ] UDP announce (BEP 15)
- [ ] Scrape support
- [ ] Use case: LAN auto-replication tracker, zero-config

## Phase 7: Advanced

- [ ] µTP transport (BEP 29)
- [ ] HTTP seeding (BEP 19)
- [ ] Fast Extension (BEP 6)
- [ ] Holepunch (BEP 55)
- [ ] Private torrents (BEP 27)
- [ ] Rarest-first picker with peer availability tracking

## Low Priority: Bencode cleanup

Working, fuzz-tested, ASan-clean. Ergonomic issues only:

- [ ] Namespace rename: `be_*` → `mbt_be_*`, `struct be_value` → `struct mbt_be_value`
- [ ] Replace `struct iovec` with `struct { char *data; size_t len; }`
- [ ] Add `mbt_be_dict_append_string(dict, key, buf, len)`
- [ ] Single-pass growable-buffer encoder
- [ ] List access by index
- [ ] Path-based `mbt_be_find`
