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
- [ ] Extract `tpack`/`tunpack` into `pack.c/h` (wire format helpers)
- [ ] Remaining `bt.c` becomes `torrent.c` — piece state, peer set, hash verify

### 1.2 Peer layer (rename remote.c → peer.c)
- [ ] Define `mbt_peer_callbacks` (on_handshake, on_piece, on_bitfield, on_choke, on_have, on_extension, on_error)
- [ ] Peer takes a connected `mbt_conn`, not an address — no TCP connect logic in peer
- [ ] Peer does framing + callbacks only; remove all strategy (what to request, when)
- [ ] Peer sends messages via explicit calls: `mbt_peer_send_request`, `mbt_peer_send_interested`, etc.

### 1.3 Torrent layer (from bt.c + strategy parts of remote.c)
- [ ] Owns peer set (add/remove peers)
- [ ] Piece state machine: pending → requested → received → verified
- [ ] Drives piece requests using picker callback
- [ ] Verifies SHA1 on piece completion
- [ ] Calls storage backend to write verified pieces
- [ ] Metadata fetch (BEP 9) as a sub-state before piece download

### 1.4 Session layer (rename node.c → session.c)
- [ ] Torrent collection (add/remove by infohash)
- [ ] Listen socket + accept incoming peers
- [ ] Route incoming handshakes to correct torrent
- [ ] Outgoing connect (async TCP → mbt_conn → hand to peer → route to torrent)
- [ ] Integrate LSD listener (on announce → find/create torrent → connect)
- [ ] Event loop ownership: run/stop

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
