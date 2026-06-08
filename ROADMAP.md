# libmbt Roadmap

## Phase 0: Foundation (complete)

- [x] Bencode parser (fuzz-tested, ASan-clean, leak-free, sorted dict keys)
- [x] Bitmap (`mbt_bitmap` — bit-accurate, bounds checked)
- [x] Infohash type (refcounted)
- [x] LSD multicast (BEP 14)
- [x] Event loop abstraction (sd-event + libevent, caller-owned loops)
- [x] Buffered connection abstraction (mbt_conn, async writes)
- [x] Wire format helpers (pack.c)
- [x] Meson build with backend selection, -Dtests=false option
- [x] Logging subsystem (component mask, stderr/syslog/custom, MBT_LOG_ALL)

## Phase 1: Clean layering (complete)

- [x] Peer state machine (peer.c) — callbacks up, commands down
- [x] Torrent coordinator (torrent.c) — piece state, SHA1, storage, picker
- [x] Session (session.c) — torrent list, async connect, LSD/DHT integration
- [x] Storage backends: file (multi-file) + memory (testing)
- [x] Pickers — sequential + random (default: random)
- [x] Request timeout, peer banning (hash failures), endgame mode
- [x] Peer database (peerdb.c) — RB tree, state/source/backoff, torrent-owned
- [x] Out-of-order block assembly (per-piece bitmap tracking)
- [x] Bounded memory: one active piece per peer, strict enforcement
- [x] Metainfo API (parse, create, encode, accessors, auto piece-length)

## Phase 2: Discovery (complete)

- [x] Magnet URI parser
- [x] HTTP tracker client (BEP 3) — via caller HTTP hook
- [x] UDP tracker client (BEP 15) — async, built-in
- [x] UDP tracker server (BEP 15) — HMAC connection_id, shared store
- [x] HTTP tracker server (BEP 3) — minimal, shared store
- [x] Auto-announce from announce-list (BEP 12), re-announce timer
- [x] PEX receive (BEP 11)
- [x] DHT (BEP 5) — full Kademlia: routing, lookup, announce, storage, stable ID
- [x] DHT periodic re-lookup (5 min), runtime enable/disable
- [x] LSD listener + announcer (opt-in, runtime toggle)
- [ ] PEX send — 60s timer, batch added/dropped peers
- [ ] Built-in async HTTP tracker client (replace blocking callback)

## Phase 3: Seeding + incoming (complete)

- [x] Listen socket — async accept, route by infohash, enforce max_peers
- [x] Respond to piece requests (read from storage, send)
- [x] Serve BEP 9 metadata requests
- [x] Send bitfield on peer activation
- [x] Resume: SHA1 verify existing pieces on startup
- [x] HAVE broadcast on piece completion
- [x] Metadata retry from multiple peers (resilient to disconnects)
- [x] Immediate piece reassignment on peer choke
- [x] Choking algorithm (BEP 3: top-4 + optimistic every 30s)
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
- [x] Piece map accessor for GUI/TUI
- [x] Safe bencode constructors, sorted dict keys
- [x] `mbt_metainfo` API — parse/create/encode
- [x] `mbt_session_add_torrent_buf` — add torrent from memory
- [x] Shared tracker store (UDP + HTTP see same peers)

## Phase 6: Production hardening (next)

- [ ] Dual-stack networking (IPv6 throughout: session, tracker, DHT)
- [ ] Configurable timers (all hardcoded intervals exposed via config)
- [ ] NAT-PMP / UPnP-IGD port mapping
- [ ] Anti-snubbing (peer silent 60s → request elsewhere)
- [ ] Rarest-first picker with peer availability tracking
- [ ] Upload/download rate limiting
- [ ] Tracker policy callbacks (accept/reject announce, infohash whitelist)
- [ ] Tracker resource limits (max peers/torrent, max torrents, eviction)
- [ ] Session peer accept/reject callback
- [ ] Per-torrent feature toggles (DHT/PEX/LSD enable per torrent)
- [ ] Metrics/audit hooks (bytes transferred, peer churn, hash failures)

## Phase 7: Testing

- [x] Unit tests: bencode, bitmap, pack, hex, infohash, magnet, picker, peerdb, metainfo
- [x] Integration: e2e, tracker server (UDP), DHT localhost, resume, partial resume
- [x] Magnet flow, multi-peer, choke cycling, LSD, picker coverage
- [x] Shared tracker store test (UDP+HTTP same store)
- [x] ASan clean (28/28), valgrind clean
- [ ] Endgame mode test
- [ ] Hash failure + peer ban test
- [ ] IPv6 tracker test (announce over ::1)
- [ ] HTTP tracker client test (needs async client)

## Phase 8: Tools

- [x] `mbt-get` — download by .torrent/magnet (-S -D -L -l -d flags)
- [x] `mbt-lsd` — LSD auto-replicate
- [x] `mbt-torrent` — create, show, magnet (uses metainfo API)
- [x] `mbt-dht` — standalone DHT lookup
- [x] `mbt-tracker` — UDP + HTTP tracker daemon
- [ ] `mbt-seed` — seed a file given a .torrent
- [ ] `mbt-daemon` — long-running daemon + DBus/control interface

## Phase 9: Advanced protocols

- [ ] µTP transport (BEP 29)
- [ ] Holepunch (BEP 55)
- [ ] HTTP seeding (BEP 19)
