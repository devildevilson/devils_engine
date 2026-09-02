# Networking implementation status

Last updated: 2026-09-02.

This file is the mutable implementation and verification journal for the networking work. Architectural
decisions, terminology, invariants and the ordered roadmap remain in [NETWORKING.md](NETWORKING.md). A result is
recorded here only after it is reproduced by an executable test or directly observed in the pinned dependency.

## Current position

| Item | Status |
| --- | --- |
| Gameplay transport | GameNetworkingSockets v1.6.0 selected |
| PRE-01 capability spike | complete |
| PRE-02 causal-state/checkpoint audit | complete; implementation gaps routed to NET-03/04 |
| Repeated Release resume simulation | flaky existing test; must be diagnosed before production replay |
| Production `devils_engine::network_gns` adapter | not started |
| Session handshake, reconnect recovery and peer authority | not started |
| Internet P2P/signaling | not tested; infrastructure is not yet present |
| Trusted public-session authentication | not designed; standalone GNS has no configured CA |
| Yojimbo comparison | deferred indefinitely; not an implementation gate |

The current executable is intentionally a direct GNS consumer. It passes opaque bytes and does not invent an
engine transport abstraction, network entity type, serialization scheme or client/server policy before those
contracts are known.

## How GameNetworkingSockets works in this project

### Runtime and service model

The standalone library is initialized with `GameNetworkingSockets_Init`, optionally with a local
`SteamNetworkingIdentity`, and shut down with `GameNetworkingSockets_Kill`. The application primarily uses two
interfaces:

- `ISteamNetworkingSockets` owns listen sockets, connections, poll groups, message submission and receive;
- `ISteamNetworkingUtils` owns message allocation, configuration, callbacks and diagnostic facilities.

GNS performs encryption, packet I/O and queued transport work on its service machinery. The application must
still call `RunCallbacks` frequently to dispatch connection state changes. The future adapter therefore needs a
clear runtime owner and callback pump; simulation code must not own or poll GNS directly.

### Connection forms

The tested API exposes three useful connection forms:

1. `CreateSocketPair(false)` creates an internal pipe. It preserves the connection/message API without opening
   an OS socket and is useful for fast transport-contract tests.
2. `CreateSocketPair(true)` uses encrypted UDP loopback. It exercises real packetization, MTU fragmentation,
   reassembly and retransmission while keeping both ends in one process.
3. `CreateListenSocketIP` plus `ConnectByIPAddress` creates an ordinary IPv4/IPv6 UDP endpoint. An incoming
   attempt appears as a `Connecting` callback and must be accepted with `AcceptConnection` or explicitly closed.

`CreateListenSocketIP` requires a concrete port; port zero is rejected. The PRE-01 test binds IPv4 localhost
and searches the bounded development range `39000..39127`. Production code must take a configured endpoint and
must not silently search ports.

GNS also exposes `CreateListenSocketP2P` plus `ConnectP2P`. These use peer identities and virtual ports rather
than a public IP endpoint. A real remote P2P connection additionally needs the platform's default rendezvous
service or project-owned signaling through `ConnectP2PCustomSignaling`; the API by itself is not a rendezvous
server.

### Connection events and receive queues

Connection lifecycle is asynchronous. A `SteamNetConnectionStatusChangedCallback_t` reports connecting,
connected and terminal states. The server-side callback receives a new connection handle; a listen-socket
handle is not itself a connected peer.

Connections can be assigned to one poll group. `ReceiveMessagesOnPollGroup` drains complete messages from every
connection in that group, and `SteamNetworkingMessage_t::m_conn` identifies their source. Alternatively,
`ReceiveMessagesOnConnection` drains one connection. This is the transport receive queue the adapter should
reuse instead of adding a second raw-packet queue.

Every returned `SteamNetworkingMessage_t` makes its lifetime the caller's responsibility and must eventually be
released with `Release`. It may be retained briefly or transferred into an adapter-owned job, but the adapter
must have a bounded policy for that lifetime.

### Messages, reliability and ordering

GNS is message-oriented: the receiver obtains the same application message boundary and size that the sender
submitted, rather than a TCP-style byte stream.

- reliable messages are retransmitted and preserve send order with other reliable messages on the same lane;
- unreliable messages may be lost, duplicated or reordered relative to unreliable or reliable traffic;
- lane priority controls send scheduling, not a universal receive order across lanes;
- each direction configures its outbound lanes independently.

Consequently every intent bundle and state frame still needs an application tick/sequence. GNS packet numbers
cannot replace the engine's `tick_journal`, rollback history, checkpoint ring, replication baseline or duplicate
policy.

Messages larger than one MTU are fragmented and reassembled internally. For an unreliable fragmented message,
losing one fragment discards the complete application message. Engine checkpoint chunking is still useful for
budgets, pacing, resumability and targeted recovery, but it must not recreate IP/UDP fragmentation.

### Send ownership and backpressure

The simple send API accepts a byte span. The efficient batch path uses `SteamNetworkingMessage_t` objects from
`ISteamNetworkingUtils::AllocateMessage` and can also point one at an application-owned buffer with a release
callback.

For `SendMessages(..., bDeleteFailedMessages=false)`:

- a successfully queued entry is replaced by `nullptr` and ownership transfers to GNS;
- a failed entry remains caller-owned and receives a negative `EResult`;
- later entries for the same connection are not attempted after a failure and receive result zero;
- an application buffer must remain valid until its release callback runs;
- that callback can run from any thread, including before `SendMessages` returns, so it must be fast and
  thread-safe.

Queue exhaustion is observable rather than silently unbounded. PRE-01 reproduced `-k_EResultLimitExceeded` with
a deliberately restricted send buffer. `GetConnectionRealTimeStatus` exposes aggregate and per-lane queued and
unacknowledged bytes; `GetDetailedConnectionStatus` supplies textual transport diagnostics. The adapter must
translate these into neutral pressure/statistics values and define retry, drop or disconnect policy per logical
message class.

### Lanes and the intended mapping

`ConfigureConnectionLanes` assigns priorities and weights to a small number of outbound lanes. Lower-priority
bulk traffic did not block a later small reliable message on a higher-priority lane in the measured test. The
first adapter experiment should keep the mapping small:

| Lane purpose | Delivery | Policy |
| --- | --- | --- |
| current intent/control | reliable ordered | highest priority; overflow is a session fault |
| checkpoint bulk | reliable ordered | lower priority and separately budgeted |
| transform/state frame | unreliable with project sequence | obsolete frames may be dropped |

This is a candidate mapping, not a frozen wire protocol. GNS guarantees reliable ordering only within the same
lane. Lane numbers, priorities and weights must be negotiated or fixed by a compatible session schema.

### Nagle, batching and flush

GNS enables a Nagle-style batching interval by default (`5000 us` upstream default, supported range
`0..20000 us` in the pinned v1.6.0 implementation). Small messages can be held so several application messages
share a packet. `FlushMessagesOnConnection` requests transmission at the next opportunity, and
`k_nSteamNetworkingSend_NoNagle` bypasses the timer.

The interval is an upper batching delay, not a guaranteed minimum hold time: a packet may leave earlier when
enough data or another flush condition exists. A likely tick-loop policy is to queue that tick's messages and
flush once at the end, but NET-08 must measure traffic, CPU cost and latency before freezing it.

### Fault injection and diagnostics

The library provides configuration values for artificial packet lag, loss and reorder, plus connection rate and
buffer limits. PRE-01 uses these against encrypted UDP loopback, so the loss tests exercise the real GNS
reliability and packet scheduling paths rather than an engine mock.

These controls are valuable but do not replace the deterministic in-memory fault laboratory planned by NET-06.
GNS fault injection is transport-level and timing-dependent; replay/session tests need a recorded logical fault
schedule that is exactly repeatable.

### Encryption, identity and authentication

Ordinary GNS connections are encrypted by default, which protects against casual passive observation. Encryption
alone does not prove who is at the other endpoint and does not prevent a man-in-the-middle attack without a
trusted certificate or an out-of-band shared secret.

The pinned standalone build can store and report a generic `SteamNetworkingIdentity`, but PRE-01 observes:

```text
InitAuthentication()       -> k_ESteamNetworkingAvailability_CannotTry
GetAuthenticationStatus() -> k_ESteamNetworkingAvailability_CannotTry
debug message              -> No certificate authority
```

For the localhost IP probe both sides explicitly use `IP_AllowWithoutAuth=2`. Upstream marks this as a development
setting: it disables authentication attempts and warnings and must not be user-configurable in production. A
future localhost-only adapter test can prefer the narrower `IPLocalHost_AllowWithoutAuth`; a public session needs
an explicit CA/certificate issuance, validation, expiry and rotation design.

A generic identity string is an address/label, not an authenticated account. The session layer must distinguish
transport connection, claimed peer identity, authenticated principal and current authority role.

### Reconnect and session recovery

Closing a connection and calling `ConnectByIPAddress` again produces a new client handle and a new accepted
server handle. GNS reconnect therefore means establishing a new transport connection; it does not restore the
engine session.

After reconnect, the future session handshake must establish at least protocol/content fingerprints, peer and
principal identity, authority epoch, last acknowledged input tick, retained replication baseline and whether a
checkpoint is required. No GNS connection handle may be serialized into gameplay state.

### P2P boundary

PRE-01 creates a P2P listen socket and connects to the same configured identity inside one process. GNS selects a
local fast path and successfully exchanges an opaque message. This proves only identity/virtual-port API wiring
and callback/poll-group compatibility.

It does **not** prove remote discovery, signaling, ICE negotiation, NAT traversal, relay fallback, public
authentication or authority migration. An honest Internet P2P test needs two independently identified processes
and either a platform rendezvous service or a project signaling service, plus the chosen ICE/STUN/TURN/relay and
certificate infrastructure. That belongs to NET-LAB-01 after the session handshake exists.

## PRE-01 implementation

The probe is [gamenetworking_sockets_capability_test.cpp](subprojects/tests/gamenetworking_sockets_capability_test.cpp).
It is registered in [subprojects/tests/CMakeLists.txt](subprojects/tests/CMakeLists.txt) and links
`GameNetworkingSockets::static` directly. GNS types have not entered an engine library or project state.

### Scenarios and observations

| Scenario | Observed result | Consequence |
| --- | --- | --- |
| internal pipe, messages and poll group | complete opaque messages retain connection, lane and reliability metadata; reliable messages stay ordered in lane 0 | drain GNS messages directly; do not reconstruct message boundaries |
| GNS-allocated send batch | accepted pointers become `nullptr`; internal-pipe receive retained the submitted payload address | ownership transfer can remove a duplicate adapter send record; the address observation is not generalized beyond the internal pipe |
| application-owned send buffer | the custom release callback executes exactly once after the received object is released | pooled buffers are viable if callbacks are fast and thread-safe |
| real UDP loopback and MTU | one 128 KiB reliable and one 4 KiB unreliable message arrive intact as two messages | transport owns packet fragmentation/reassembly |
| Nagle/flush and injected lag | queued status is visible, explicit flush delivers the message, and 40 ms artificial send lag delays an intact reliable `NoNagle` message | expose batching/flush policy and reuse GNS fault controls |
| lane priority | a 79-byte reliable intent queued after 400 KiB reliable bulk completes first at a throttled 512 KiB/s | keep current control/intents separate from checkpoint bulk |
| sustained mixed load/loss | after a 100% loss blackout a reliable intent retransmits; at 15% loss plus 15% reorder all reliable intent/bulk data arrives intact and lane-ordered while some unreliable state frames disappear | reliable transport works, but state frames still require project sequence/staleness handling |
| real IP listen/connect | IPv4 localhost accepts asynchronously, joins a poll group and exchanges data in both directions | the adapter requires an active callback pump and explicit accept policy |
| reconnect | closing the first connection and connecting again creates fresh handles and exchanges a fresh payload | session resume/baseline recovery is engine-owned |
| standalone auth | generic identity round-trips but authentication reports `CannotTry` and `No certificate authority` | unauthenticated development mode is not public identity security |
| local P2P fast path | same-process `CreateListenSocketP2P`/`ConnectP2P` exchanges data | useful local API check, not an Internet P2P result |
| bounded send queue | with 64 KiB/s and a 160 KiB buffer, one 100 KiB reliable message is accepted, the next returns `LimitExceeded`, and the following entry is not attempted | submission needs explicit ownership and backpressure handling |
| status API | aggregate/per-lane queue status and detailed diagnostics are available | adapter can expose transport observations without shadowing the transport queue |

### Current test results

Latest complete runs on 2026-09-02:

| Configuration | Result | Total wall time |
| --- | ---: | ---: |
| Debug CTest discovery | 8/8 cases passed | 3.34 s |
| Release CTest discovery | 8/8 cases passed | 3.27 s |
| latest direct Debug executable | 8/8 cases, 1882/1882 assertions | 3.2 s |

The assertion count is not a frozen test-count contract: the loss scenario validates each unreliable message
that happens to arrive, so the count changes with permitted packet loss. Test-case count and all reliable
cardinality/order checks are fixed.

Latest individual Debug/Release CTest times were:

| Test case | Debug | Release |
| --- | ---: | ---: |
| internal pipe/ownership | 0.04 s | 0.03 s |
| real UDP large messages | 0.57 s | 0.57 s |
| Nagle/lag | 0.09 s | 0.07 s |
| priority lane | 0.86 s | 0.85 s |
| sustained load/loss | 1.64 s | 1.60 s |
| IP listen/connect/reconnect | 0.02 s | 0.03 s |
| auth/local P2P | 0.02 s | 0.02 s |
| bounded backpressure | 0.03 s | 0.03 s |

The priority and sustained-loss cases passed five consecutive stress repetitions. After a boundary-timing
assertion based on an out-of-range `250000 us` Nagle value was replaced with the supported `20000 us` setting
and an explicit queue/flush check, Nagle/lag, IP reconnect and auth/local-P2P each passed ten consecutive Release
repetitions.

These are capability smoke timings including GNS initialization and shutdown, not throughput benchmarks. Tests
that create real UDP sockets require permission to bind localhost in a restricted runner. The internal pipe and
same-process P2P fast path do not require an OS network socket.

### Build and dependency observation

The current CMake configuration builds GNS v1.6.0 with shared and static targets enabled, ICE enabled, OpenSSL
crypto, shared protobuf and its Abseil dependencies. The capability test links the static target.

On the current Linux build without project LTO:

| Artifact | Debug | Release |
| --- | ---: | ---: |
| GNS static archive | about 29 MiB | about 2.7 MiB |
| capability executable | about 18 MiB | about 1.6 MiB |

The Release executable contains about 1.26 MiB of text and dynamically links libcrypto, protobuf and the
distribution's split Abseil libraries. This is a reproducible baseline, not a shipping-size conclusion.
Minimal-feature size and the need for ICE and both library variants remain NET-08 measurements.

### Reproduction

```sh
cmake --build build-debug --target gamenetworking_sockets_capability_test
ctest --test-dir build-debug -R gamenetworking_sockets_capability_test --output-on-failure

cmake --build build-release --target gamenetworking_sockets_capability_test
ctest --test-dir build-release -R gamenetworking_sockets_capability_test --output-on-failure
```

Targeted repetition example:

```sh
ctest --test-dir build-release -R 'Nagle|IP listen|standalone auth' \
  --repeat until-fail:10 --output-on-failure
```

## PRE-01 conclusion

PRE-01 is complete for the GNS-only decision. The library already owns transport message queues, packet
sequencing, acknowledgement, retransmission, duplicate handling, fragmentation/reassembly, encryption, lane
scheduling and transport statistics. The engine should reuse or narrowly wrap those public mechanisms.

The engine still needs independently tested primitives for simulation/session semantics: canonical tick
bundles, bounded prediction history, checkpoint storage, replay, application state-frame sequencing and
baselines, compatibility handshake, reconnect recovery and authority migration. None of these should be coupled
to a GNS handle or internal packet sequence.

Remaining transport work is routed as follows:

- NET-08: production adapter, longer multi-connection soak, Nagle/CPU/copy tuning and worker scheduling;
- NET-11: hostile input limits, receive overflow, rate limiting and abuse policy;
- NET-LAB-01: IPv6, multiple processes/machines, real reconnect recovery and Internet P2P infrastructure;
- session/auth design: certificate authority, principal authentication and credential lifecycle;
- NET-09: Yojimbo only if a concrete GNS limitation later justifies reopening the comparison.

## PRE-02 — causal state and checkpoint audit

PRE-02 is complete as an audit, not as a production checkpoint implementation. The executable
[checkpoint_audit.cpp](subprojects/tile_frontier/checkpoint_audit.cpp) works over the real
`tile_frontier::core::actor_world_slice` and leaves its live save/load format unchanged. It parses the current
canonical payload into 19 independently comparable regions (world header, 17 component blocks and project
globals), reconstructs full payloads from reused sections or 4 KiB pages, then requires the unchanged loader to
accept them and reproduce exactly the same canonical bytes.

### Causal-state boundary found by the audit

The current checkpoint is a complete resume image for the isolated actor slice at a committed tick boundary,
subject to the content/config and transactional-load gaps below. It is not yet a checkpoint of the complete
`tile_frontier` host.

| Owner | Current treatment | Classification / required contract |
| --- | --- | --- |
| ECS entity allocator (`cur_index`, removed IDs) | serialized in `world.header` | causal; exact entity identity and future allocation order depend on it |
| 17 registered ECS component pools | serialized in stable component-hash order, with entities in pool order | causal except for the mixed presentation fields noted below |
| actor tick, accumulated game time, food spawn sequence, bounds/target and scheduling knobs | serialized in the 56-byte `sim_globals` tail | causal; future deadlines, scheduling, movement bounds and spawn order depend on them |
| `player_intent_queue` | deliberately omitted and recreated empty | transient ingress; correct only when capture occurs after intents for the tick have been committed and the durable tick journal is stored separately |
| deferred call journals, due/kill worklists, sound emits, query/system objects and perception kD tree | omitted, cleared or rebuilt | derived/transient; capture is legal only between ticks after all worker barriers and commits complete |
| player entity cache and obstacle cache | omitted and rebuilt from components | derived; reconstruction is already implemented |
| GOAP/FSM registries, prefab bodies, scripts, `actor_tuning` and other loaded resources | omitted | immutable session content, not checkpoint bytes; a content fingerprint is mandatory because these values determine future simulation and future spawns |
| `actor_visual` | serialized as one component | mixed: texture/color are presentation, but `size` participates in perception. Split causal body/size from cosmetics before a gameplay-only state root is claimed |
| tile grid/chunk loading, camera, batches, metrics, UI RNG, sound outboxes and render/resource handles | outside the actor snapshot | currently presentation/lifecycle for this slice. Mutable tile gameplay must later become its own causal section; scene/content identity belongs in checkpoint/session metadata |
| host clocks, calendar, pause and lifecycle | outside the actor snapshot | most wall/UI/lifecycle state is non-causal; effective gameplay time is already in the slice. Pause/scale changes must nevertheless be ordered session commands so future `game_delta_ticks` is reproduced |
| input history, authority epoch, peer/baseline acknowledgements | outside world state by design | session recovery state; stored beside checkpoints, never mixed into the world root |
| future physics world | absent | PRE-03 must enumerate body-ID allocation, transforms/velocities, sleeping/activation and any solver/cache state required by its restore contract |

The serialized causal component set is: position, velocity, brain seed/phase/speed, perception, cognition,
stats, FSM state, GOAP/FSM resource references, eating/grab relationships, flags, food, obstacle, spawn point and
player-controller identity. A normal simulation tick changed exactly seven current regions: cognition, FSM
state, perception, position, velocity, stats and `sim_globals`.

### Format and replacement findings

Useful properties already exist:

- scalar encoding is canonical little-endian and IEEE floats are stored by their exact bit pattern;
- the world header preserves allocator state, component blocks have stable IDs and byte lengths, and maps in the
  generic serializer are sorted;
- the outer container has a version, raw/payload sizes, zstd compression and a Murmur64 integrity checksum;
- full save/load already round-trips byte-identically in the audit.

The current payload is not yet a durable network checkpoint schema:

- `sim_globals` is an unframed, unversioned tail and is absent from the component schema fingerprint;
- the component layout fingerprint does not mix field names and the registry fingerprint is cached on first
  use, without an explicit freeze point;
- the loader does not require end-of-payload/end-of-container, so trailing data is not part of a strict schema
  decision;
- Murmur64 is corruption detection only, not authentication and not the production page/state digest;
- exact float serialization preserves divergence; it does not create cross-platform numeric determinism.

Most importantly, load is destructive on failure. `actor_world_slice::load` replaces the live world and clears
caches before `unseal` has validated the packet; `load_world` then installs allocator state and component pools
progressively. The corruption probe confirms that a rejected packet changes the destination. NET-03/04 must
decode and validate every section into staging state, rebuild required derived state there, and publish it with
one replacement operation only after success. Snapshot-loaded notifications must happen after commit, not while
the candidate is partial.

### Measurements

Release measurements on 2026-09-02 use 20 warm-up ticks and the default audit fixtures. They are local
microbenchmarks, not budgets. The large fixture has 8192 actors plus 1024 food entities, 1024 spawn points and 32
obstacles.

| Fixture | Raw | zstd-fast container | zstd-normal container | ECS dump | Full save | Unseal | Full load |
| --- | ---: | ---: | ---: | ---: | ---: | ---: | ---: |
| 512 actors | 81,221 B | 34,320 B (42.3%) | 34,217 B (42.1%) | 44.8 us | 197.8 us | 82.4 us | 152.2 us |
| 8192 actors | 1,291,685 B | 543,872 B (42.1%) | 518,129 B (40.1%) | 734.6 us | 3.23 ms | 1.37 ms | 2.10 ms |

On the large fixture, one full-payload Murmur hash, all 19 section hashes and all 334 page hashes each cost
about `0.17 ms`: changing hash granularity does not avoid scanning the bytes. zstd-fast alone costs about
`2.20 ms` and dominates the current full save.

The following delta estimates concatenate changed material, compress it with zstd-fast and add an explicitly
optimistic manifest allowance of 32 bytes plus 24 bytes per changed unit. This is a comparison instrument, not
a proposed wire format.

| Change from the 8192-actor base | Whole-section reuse | 4 KiB page reuse | Estimated page delta | % of full zstd |
| --- | ---: | ---: | ---: | ---: |
| one position write | 90.5% bytes reused | 99.7% | 3,483 B | 0.6% |
| one simulation tick | 38.4% | 69.9% | 209,596 B | 38.5% |
| five simulation ticks | 38.4% | 44.2% | 357,702 B | 65.8% |
| twenty simulation ticks | 38.4% | 42.9% | 361,449 B | 66.5% |
| one food entity spawn | 78.4% | 99.0% | 6,430 B | 1.2% |
| one food entity removal | 78.4% | 96.4% | 19,181 B | 3.5% |

Section-level explicit dirty/version declarations were exact for the three audited mutations (`missed=0`,
`redundant=0`), but they materialize an entire component pool: 104,143 B for one position write and about
150,560 B for one spawn/removal, versus 3,483 B, 6,430 B and 19,181 B with pages. Coarse component dirty flags
therefore cannot substitute for page-level reuse. Page dirty tracking could later avoid hashing unchanged
pages, but only after every mutation path can be proven to mark the correct storage generation.

### PRE-02 decision

The first authoritative representation remains a full, sectioned, canonical checkpoint with transactional
replacement. It is the recovery anchor and the only representation that may stand alone.

A 4 KiB page-manifest delta is earned as an optional storage/transfer encoding on top of that representation:
the measured bandwidth savings are material for both sparse changes and active simulation. It must identify an
explicit retained base checkpoint, reconstruct the exact canonical full bytes before apply, use a strong
production digest, be reliable, and have a bounded rebase policy. It is not regular transform/state-frame
replication and it cannot become a chain with an unavailable base.

The prototype still performs a full serialization and byte scan, so it proves bandwidth/storage potential, not
incremental capture cost. NET-03 should first introduce neutral section manifests, schema/content fingerprints
and staging replacement. NET-04 should then implement full checkpoint/replay; page deltas follow behind the
same API and remain optional until capture cadence, retained-baseline memory and dirty-page bookkeeping are
measured in the real online stand.

### Reproduction

```sh
cmake --build build-debug --target tile_frontier_checkpoint_audit
ctest --test-dir build-debug -R '^tile_frontier_checkpoint_audit$' --output-on-failure

cmake --build build-release --target tile_frontier_checkpoint_audit
ctest --test-dir build-release -R '^tile_frontier_checkpoint_audit$' --output-on-failure
```

### Verification and adjacent runtime finding

The new checkpoint audit passes in Debug and Release, and its Release CTest passed 20 consecutive repetitions.
The existing `tile_frontier_snapshot_smoke` also passes. A repeated run exposed a separate intermittent failure
in the existing four-worker `tile_frontier_resume_smoke`: four Release repetitions passed and the fifth
segfaulted; an earlier combined run ended with `std::length_error` from `vector::reserve`. A direct run and a
gdb run passed, so there is no stable stack yet. No resume-simulation source was changed by PRE-02.

This does not invalidate the exact reconstruction checks in the new executable, but it does block a production
claim that repeated multithreaded resume/replay is stable. The race/lifetime failure must be isolated before
NET-04 closes; it should not be disguised by weakening or removing the existing resume test.
