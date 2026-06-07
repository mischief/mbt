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
- [x] Session (session.c) — torrent list, async connect, LSD/DHT integration
- [x] Storage backends: file (multi-file) + memory (testing)
- [x] Pickers — sequential + random (default: random)
- [x] Request timeout (15s), peer banning (hash failures), endgame mode
- [x] Peer database (peerdb.c) — RB tree, state/source/backoff, torrent-owned
- [x] Out-of-order block assembly (bitmask tracking)
- [x] Bounded memory: one active piece per peer

## Phase 2: Discovery (complete)

- [x] Magnet URI parser
- [x] HTTP tracker client (BEP 3) — via caller HTTP hook
- [x] UDP tracker client (BEP 15) — async, built-in
- [x] Auto-announce from announce-list (BEP 12), re-announce timer
- [x] PEX receive (BEP 11)
- [x] DHT (BEP 5) — Kademlia routing, iterative lookup, announce, stable node ID
- [x] DHT periodic re-lookup (5 min), runtime enable/disable
- [x] LSD listener + announcer (opt-in, runtime toggle)
- [ ] PEX send — 60s timer, batch added/dropped peers

## Phase 3: Seeding + incoming (complete)

- [x] Listen socket — async accept, route by infohash
- [x] Respond to piece requests (read from storage, send)
- [x] Serve BEP 9 metadata requests
- [x] Send bitfield on peer activation, unchoke all
- [x] Resume: SHA1 verify existing pieces on startup
- [x] HAVE broadcast on piece completion
- [x] Metadata retry from multiple peers (resilient to disconnects)
- [x] Immediate piece reassignment on peer choke
- [ ] Choking algorithm (top-4 + optimistic)
- [ ] Announce event=completed / event=stopped to tracker

## Phase 4: Protocol extensions (mostly complete)

- [x] BEP 6 Fast Extension — HAVE_ALL, HAVE_NONE, REJECT, ALLOWED_FAST
- [x] Deferred HAVE_ALL (applied when piece_count becomes known)
- [x] REJECT handling — instant piece failover
- [x] Drop unsolicited/duplicate piece data
- [x] Named protocol constants (no magic numbers)
- [ ] Private torrents (BEP 27) — disable DHT/PEX/LSD when private=1

## Phase 5: API polish (complete)

- [x] All symbols namespaced `mbt_*`, internals hidden (linker script)
- [x] Headers in `mbt/` — `#include <mbt/mbt.h>`
- [x] `mbt_ev_new_default()` — backend-agnostic
- [x] DHT/LSD enable/disable at runtime, order-independent
- [x] Custom DHT bootstrap nodes
- [x] Piece map accessor (`mbt_torrent_piece_map`) for GUI/TUI
- [x] Safe bencode constructors, backend-guarded headers

## Phase 6: Swarm quality + networking (next)

- [ ] NAT-PMP / UPnP-IGD port mapping (auto-punch NAT)
- [ ] Per-peer write buffering (EPOLLOUT queue, replace EAGAIN spin)
- [ ] Anti-snubbing (peer silent 60s → request elsewhere)
- [ ] Adaptive pipeline depth (ramp up, not burst)
- [ ] Rarest-first picker with peer availability tracking
- [ ] Upload/download rate limiting
- [ ] Piece buffer memory pool (fixed-size allocator per torrent)

## Phase 7: Tools

- [x] `mbt-get` — download by .torrent/magnet (-S -D -L -l -d flags)
- [x] `mbt-lsd` — LSD auto-replicate
- [x] `mbt-torrent` — create .torrent, show, magnet
- [x] `mbt-dht` — standalone DHT lookup
- [ ] `mbt-seed` — seed a file given a .torrent
- [ ] `mbt-daemon` — long-running daemon + DBus control

## Phase 8: Advanced

- [ ] µTP transport (BEP 29)
- [ ] Holepunch (BEP 55)
- [ ] HTTP seeding (BEP 19)
