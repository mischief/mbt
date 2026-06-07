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
- [x] Pickers — sequential + random (default)
- [x] Request timeout (15s), peer banning (hash failures), endgame mode
- [x] Peer database (peerdb.c) — RB tree, state/source/backoff, torrent-owned

## Phase 2: Discovery (complete)

- [x] Magnet URI parser
- [x] HTTP tracker client (BEP 3) — via caller HTTP hook
- [x] UDP tracker client (BEP 15) — async, built-in
- [x] Auto-announce from announce-list (BEP 12), re-announce timer
- [x] PEX receive (BEP 11)
- [x] DHT (BEP 5) — full Kademlia: routing table, iterative lookup, announce
- [x] DHT periodic re-lookup (5 min) for incomplete torrents
- [x] LSD listener + announcer (opt-in)
- [ ] PEX send — 60s timer, batch added/dropped peers

## Phase 3: Seeding + incoming connections (complete)

- [x] Listen socket — async accept, route by infohash
- [x] Respond to piece requests (read from storage, send)
- [x] Serve BEP 9 metadata requests
- [x] Send bitfield on peer activation, unchoke all
- [x] Resume: SHA1 verify existing pieces on startup
- [ ] Send have messages on piece completion
- [ ] Choking algorithm (top-4 + optimistic)
- [ ] Announce event=completed / event=stopped to tracker

## Phase 4: Swarm quality (next)

- [ ] Per-peer write buffering (replace EAGAIN spin with EPOLLOUT queue)
- [ ] Adaptive pipeline depth (ramp up from 4, not burst 512)
- [ ] Anti-snubbing (peer silent 60s → request elsewhere)
- [ ] Faster bad-peer detection (ban on 3 total hash failures, not 5 consecutive)
- [ ] Rarest-first picker with peer availability tracking
- [ ] Upload/download rate limiting

## Phase 5: API polish (complete)

- [x] All public symbols namespaced `mbt_*`
- [x] Internal symbols hidden via linker version script
- [x] Headers in `mbt/` subdir — `#include <mbt/mbt.h>`
- [x] `meson install` works
- [x] Safe bencode constructors
- [x] `mbt_ev_new_default()` — backend-agnostic event loop
- [x] DHT/LSD enable/disable at runtime, order-independent

## Phase 6: Tools (mostly complete)

- [x] `mbt-get` — download by .torrent/magnet (-S -D -L -l -d flags, SIGINT)
- [x] `mbt-lsd` — LSD auto-replicate
- [x] `mbt-torrent` — create .torrent, show metadata, emit magnet
- [x] `mbt-dht` — standalone DHT lookup tool
- [ ] `mbt-seed` — seed a file given a .torrent
- [ ] `mbt-daemon` — long-running daemon + control interface

## Phase 7: Advanced

- [ ] Private torrents (BEP 27) — disable DHT/PEX/LSD when private=1
- [ ] Fast Extension (BEP 6) — have-all/have-none
- [ ] µTP transport (BEP 29)
- [ ] Holepunch (BEP 55)
