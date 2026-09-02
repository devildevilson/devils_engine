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
