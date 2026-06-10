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

## Phase 4: Protocol extensions (mostly complete)

- [x] BEP 6 Fast Extension — HAVE_ALL, HAVE_NONE, REJECT, ALLOWED_FAST
- [x] Deferred HAVE_ALL (applied when piece_count becomes known)
- [x] REJECT handling — instant piece failover
- [x] Drop unsolicited/duplicate piece data
- [x] Named protocol constants (no magic numbers)

## Phase 5: API polish (complete)

- [x] All symbols namespaced `mbt_*`, internals hidden (linker script + visibility)
- [x] Headers in `mbt/` — `#include <mbt/mbt.h>`
- [x] `mbt_ev_new_default()` — backend-agnostic
- [x] DHT/LSD enable/disable at runtime, order-independent
- [x] Custom DHT bootstrap nodes
- [x] Piece map accessor for GUI/TUI
- [x] Safe bencode constructors, sorted dict keys
- [x] `mbt_metainfo` API — parse/create/encode
- [x] `mbt_session_add_torrent_buf` — add torrent from memory
- [x] Shared tracker store (UDP + HTTP see same peers)
- [x] Man page infrastructure (mdoc format, check-docs.sh validator)
- [x] Tools: all use getopt, -h/--help

## Phase 6: Production hardening (in progress)

### Immediate (spec compliance, small)

### Short-term (infrastructure)
- [x] Configurable timers (all hardcoded intervals exposed via mbt_timers struct)
- [x] NAT traversal — PCP client (RFC 6887, MAP opcode, auto-refresh)
- [x] PCP wired into session (auto-map on startup, retry on failure, -P in mbt-get)
- [x] Rarest-first picker (three-phase: random first → rarest → endgame)
- [x] Anti-snubbing (peer silent → stop requesting, auto-unsnub on data)
- [x] MBT_API visibility macro (-fvisibility=hidden, explicit exports)
- [x] sys/queue.h refactor (SLIST for all linked lists)
- [x] Tracker stats snapshot API (mbt_tracker_store_stats)
- [x] DHT: reject bogus nodes (0.0.0.0, port 0)
- [x] Dual-stack IPv6 (net.c helpers, all sockets v6 clean, IPV6_V6ONLY=1)
- [x] Tracker re-announce jitter (prevents thundering herd)
- [ ] Rate limiting (upload/download caps)

### Medium-term (swarm quality)
- [ ] Upload/download rate limiting

### Longer-term (enterprise/embedding)
- [ ] Event + policy callback infrastructure on session/tracker configs
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
- [x] ASan clean (36/36), valgrind clean
- [x] PCP client test (stub server, refresh verification)
- [x] Rarest-first picker test (two-seeder, availability preference)
- [x] Anti-snubbing test (dead seeder + working seeder)
- [x] Hash failure + peer ban test (corrupt seeder, ban after 5)
- [x] Endgame mode test (10 pieces, 2 seeders, endgame triggers)
- [x] Swarm scaling test (parameterized: direct + tracker, up to 100 leechers)
- [ ] IPv6 tracker test (announce over ::1)
- [ ] HTTP tracker client test (needs async client)

## Phase 8: Tools

- [x] `mbt-get` — download by .torrent/magnet (-S -D -L -l -d flags)
- [x] `mbt-lsd` — LSD auto-replicate
- [x] `mbt-torrent` — create, show, magnet (uses metainfo API)
- [x] `mbt-dht` — standalone DHT lookup
- [x] `mbt-tracker` — UDP + HTTP tracker daemon
- [x] `mbt-pcp` — standalone PCP port mapping tool (-k for persistent)
- [ ] `mbt-seed` — seed a file given a .torrent
- [ ] `mbt-daemon` — long-running daemon + DBus/control interface

## Phase 9: Advanced protocols

- [ ] µTP transport (BEP 29)
- [ ] Holepunch (BEP 55)
- [ ] HTTP seeding (BEP 19)
