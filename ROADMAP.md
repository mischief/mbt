# libmbt Roadmap

Steps to reach the architecture described in DESIGN.md.

## Phase 0: Foundation (done)

- [x] Bencode parser (fuzz-tested)
- [x] Bitmap (tbmap)
- [x] Infohash type
- [x] LSD multicast
- [x] Event loop abstraction (sd-event + libevent)
- [x] Buffered connection abstraction (mbt_conn)
- [x] Basic peer wire protocol working end-to-end
- [x] Meson build with backend selection

## Phase 1: Clean layering

### 1.1 Split bt.c
- [x] Extract `tpack`/`tunpack` into `pack.c/h` (wire format helpers)
- [x] New `torrent.c` — piece state, peer callbacks, hash verify, storage/picker

### 1.2 Peer layer
- [x] Define `mbt_peer_callbacks` (on_active, on_piece, on_bitfield, on_choke, on_have, on_metadata, on_error)
- [x] `peer.c` takes a connected `mbt_conn`, state machine + callbacks only

### 1.3 Torrent layer
- [x] Owns peer set (add/remove peers)
- [x] Piece state machine: pending → active → complete
- [x] Drives piece requests using picker callback
- [x] Verifies SHA1 on piece completion
- [x] Calls storage backend to write verified pieces
- [x] Metadata fetch (BEP 9) as pre-download phase

### 1.4 Session layer
- [x] Prototype session API (mbt_session_default, add_torrent, enable_lsd)
- [ ] Implement `mbt_session_add_peer` — async TCP connect → mbt_conn → torrent

### 1.5 Delete legacy code
Blocked on: bt_test and mbt-lsd rewritten against new API.
- [ ] Rewrite `bt_test` using session/torrent/peer stack
- [ ] Rewrite `tools/mbt-lsd.c` using session with LSD
- [ ] Delete `bt_legacy.c`, `bt_legacy.h`
- [ ] Delete `remote.c`, `remote.h`
- [ ] Delete `node.c`, `node.h`

## Phase 2: Pluggable backends

### 2.1 Storage interface
- [ ] Define `mbt_storage_ops` (open, write_piece, read_piece, have_piece, close)
- [ ] `storage_file.c` — single-file (piece_index × piece_length → file offset)
- [ ] `storage_file.c` — multi-file (piece spans across file boundaries)
- [ ] `storage_memory.c` — for testing

### 2.2 Piece picker interface
- [ ] `int (*pick)(torrent, peer, ctx)` → piece index or -1
- [ ] `picker_sequential.c` — in-order
- [ ] `picker_random.c` — random available
- [ ] `picker_rarest.c` — rarest-first from peer bitfields

## Phase 3: Complete download

- [ ] End-to-end: LSD announce → connect → metadata → pieces → file on disk
- [ ] Handle last piece (shorter than piece_length)
- [ ] Pipeline requests (keep N outstanding per peer)
- [ ] Multiple peers per torrent (parallel download)
- [ ] Endgame mode (request remaining from all peers, cancel on receive)
- [ ] Resume (check existing pieces via storage.have_piece on startup)

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
