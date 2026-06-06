# libmbt Roadmap

Steps to reach the architecture described in DESIGN.md.

## Phase 0: Foundation (done)

- [x] Bencode parser (fuzz-tested, ASan-clean, leak-free)
- [x] Bitmap (`mbt_bitmap` — bit-accurate sizing, bounds checked)
- [x] Infohash type (refcounted)
- [x] LSD multicast
- [x] Event loop abstraction (sd-event + libevent, caller-owned loops supported)
- [x] Buffered connection abstraction (mbt_conn)
- [x] Wire format helpers (pack.c — tpack/tunpack)
- [x] Meson build with backend selection (`-Devent_backend=sd-event|libevent`)
- [x] Logging subsystem (component mask, stderr/syslog/custom callbacks)

## Phase 1: Clean layering

### 1.1 Split bt.c (done)
- [x] Extract `tpack`/`tunpack` into `pack.c/h`
- [x] New `torrent.c` — piece state, peer callbacks, hash verify, storage/picker

### 1.2 Peer layer (done)
- [x] `peer.c` — state machine (HANDSHAKE → EXTENSION → ACTIVE → CLOSED)
- [x] Callbacks up: on_active, on_piece, on_bitfield, on_choke, on_have, on_metadata, on_error
- [x] Commands down: send_request, send_interested, send_choke, send_cancel, send_have, send_piece
- [x] Takes connected `mbt_conn`, no TCP logic

### 1.3 Torrent layer (done)
- [x] Owns peer set (add/remove, tracks ownership)
- [x] Piece state: PENDING → ACTIVE (bitmap is authority for COMPLETE)
- [x] Drives piece requests via picker, pipeline depth 128
- [x] SHA1 verification on piece completion
- [x] Calls storage backend to write verified pieces
- [x] Metadata fetch (BEP 9) as pre-download phase
- [x] Piece ownership tracking — reset on peer disconnect
- [x] Multiple peers per torrent (parallel download)

### 1.4 Session layer (done)
- [x] `mbt_session_default(path)` — zero-config constructor
- [x] `mbt_session_add_infohash` — create torrent with session defaults
- [x] `mbt_session_add_torrent_file` — parse .torrent, extract infohash + metadata
- [x] `mbt_session_add_peer` — async TCP connect → mbt_conn → torrent
- [x] LSD integration (auto-creates torrents on announce, connects peers)
- [x] Torrent list management (find, remove)

### 1.5 Delete legacy code
Blocked on: bt_test and mbt-lsd rewritten against new API.
- [ ] Rewrite `bt_test` using session/torrent/peer stack
- [ ] Rewrite `tools/mbt-lsd.c` using session with LSD
- [ ] Delete `bt_legacy.c`, `bt_legacy.h`
- [ ] Delete `remote.c`, `remote.h`
- [ ] Delete `node.c`, `node.h`

## Phase 2: Pluggable backends (mostly done)

### 2.1 Storage interface
- [x] Define `mbt_storage_ops` (open, write_piece, read_piece, have_piece, close)
- [x] `storage_file.c` — download directory, filename from torrent name
- [ ] `storage_file.c` — multi-file (piece spans across file boundaries)
- [ ] `storage_memory.c` — for testing
- [ ] Resume (check existing pieces via SHA1 on startup)

### 2.2 Piece picker interface
- [x] `mbt_picker_fn` typedef
- [x] `mbt_picker_sequential` — in-order
- [x] `mbt_picker_random` — random available, skips active
- [ ] `mbt_picker_rarest` — rarest-first from peer bitfields (stub)

## Phase 3: Complete download (in progress)

- [x] End-to-end: connect → metadata → pieces → SHA1 verify → file on disk
- [x] Handle last piece (shorter than piece_length)
- [x] Pipeline requests (128 outstanding per peer, across multiple pieces)
- [x] Multiple peers per torrent
- [ ] Endgame mode (request remaining from all peers, cancel on receive)
- [ ] Proper piece_length tracking in storage (currently inferred from first write)
- [ ] Live tested: downloading real torrent from Transmission peers on LAN

## Phase 4: Seeding

- [ ] Respond to request messages (read from storage, send piece)
- [ ] Choking algorithm (unchoke top-4 by upload rate + optimistic)
- [ ] Send have messages on piece completion
- [ ] LSD announcement (not just listening)

## Phase 5: Discovery

- [ ] Magnet URI parser → `struct mbt_magnet` (infohash, display name, trackers[], peer hints[])
- [ ] `mbt_magnet_parse(uri, &magnet)` — extract fields from magnet:?xt=urn:btih:...
- [ ] Wire `mbt_session_add_magnet` to parse URI, create torrent, connect peer hints
- [ ] DHT (BEP 5) — bootstrap, routing table, get_peers, announce_peer
- [ ] PEX (BEP 11) — exchange peer lists via extension messages
- [ ] UDP tracker (BEP 15) — announce + scrape
- [ ] HTTP tracker (BEP 3) — announce + scrape

## Phase 6: Tools

- [ ] `mbt-get` — magnet/infohash one-shot downloader
- [ ] `mbt-lsd` — LSD auto-replicate (watch + download + seed)
- [ ] `mbt-seed` — seed a file given a .torrent
- [ ] `mbt-daemon` — long-running multi-torrent daemon + control socket

## Phase 7: Advanced

- [ ] µTP transport (BEP 29)
- [ ] HTTP seeding (BEP 19)
- [ ] Fast Extension (BEP 6) — have-all, have-none, reject, allowed-fast
- [ ] Holepunch (BEP 55)
- [ ] Private torrents (BEP 27) — disable DHT/PEX
- [ ] `storage_blockdev.c` — write directly to block device
- [ ] `storage_cas.c` — content-addressed dedup store

## Low Priority: Bencode cleanup

Working, fuzz-tested, ASan-clean. Ergonomic issues only:

- [ ] Replace `struct iovec` with `struct { char *data; size_t len; }`
- [ ] Add `be_dict_append_string(dict, key, buf, len)`
- [ ] Switch encoder from fragile two-pass to single-pass growable buffer
- [ ] Add list access by index helper
- [ ] Consider path-based `be_find(be, "info.pieces")` or document shallow-only
