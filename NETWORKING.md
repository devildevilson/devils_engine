# Networking, replication and deterministic simulation

Status: design audit plus completed GNS capability spike, 2026-09-02. This document collects the networking
discussion and turns it into independently testable engine and project slices. Mutable implementation details,
GNS behavior and test results live in [NETWORKING_STATUS.md](NETWORKING_STATUS.md). No production adapter,
session protocol or multiplayer game exists yet.

The repository is exploratory, therefore networking should be added in the same way as `resolve`,
`catalogue` and `originator`: a minimal mechanism is implemented only when its contract can be stated,
bounded and tested without a real game. Project policy remains in the project until a second consumer or a
clearly neutral boundary proves that it belongs in the engine.

## Decisions already made

1. A project has one explicit simulation-world schema. Every client and server exchanging packets for that
   project must have the same causal schema and compatible content/rules fingerprints. Different projects may
   have completely different schemas.
2. A process-global serializable component registry is acceptable under that rule. Registration must finish
   before an explicit `freeze`; adding a component after `freeze` is a loud error.
3. There is no polymorphic `intent_source`. Local input, a network worker, a replay loader and a test fixture
   produce the same project intent records. An owner places them in a tick-indexed buffer; simulation later
   consumes a sealed span from that buffer.
4. Full world snapshots are not ordinary synchronization packets. They are checkpoints for initial join,
   reconnect, deep resynchronization, debugging and rollback history.
5. The first useful model is server-authoritative float simulation: clients send intents, while the server
   sends small authoritative state frames and reliable discrete state/events. Fixed-point and stricter float
   profiles remain later A/B alternatives, not blockers for the first networked slice.
6. Gameplay traffic should use a message-oriented UDP transport. GameNetworkingSockets is the selected current
   direction. Yojimbo remains a possible future comparison, not an implementation gate. ASIO remains useful for
   HTTP/TCP/control services, but ASIO by itself is an asynchronous socket layer, not a ready reliable game
   protocol.
7. Transport, serialization, state replication, replay and gameplay intent meaning are separate layers. A
   library dependency must not be allowed to decide the project state model.
8. Before implementing `libs/network`, run a bounded capability spike for GameNetworkingSockets. Reuse its
   public transport mechanisms where the contract fits; do not copy an internal queue merely because it happens
   to be implemented as a ring. Engine primitives are added only for the simulation/session semantics GNS does
   not own. A Yojimbo spike is deferred indefinitely and does not block the current roadmap.
9. A network tick is a logical simulation coordinate, not a UDP packet. Intent bundles, motion state frames,
   state digests and checkpoints are separate messages even when a transport aggregates them into one datagram.
10. Intent bundles are produced every simulation tick. Transform/physics state can be sent less often and on a
    different delivery policy. Every replicated data class declares its own tick cadence and maximum staleness.
11. The implementation campaign proceeds from capability research to isolated primitives/tests, two independent
    simulations in memory, real multi-process loopback/LAN transport, compatible cross-build exchange, a headless
    authority process and only then a scaled online `tile_frontier` experiment.
12. GameNetworkingSockets v1.6.0 is now pinned through root `FetchContent`. The first consumer is a direct
    capability test, not `libs/network`: transport behavior must be measured before its types are hidden by an
    adapter.
13. Transport reconnect means creating a new connection. Restoring peer identity, authority epoch, acknowledged
    input tick, replication baseline and checkpoint is an engine/session handshake, not a GNS reconnect feature.
14. Standalone GNS supplies encrypted transport but no certificate authority. `IP_AllowWithoutAuth=2` is valid
    only for explicitly unauthenticated development/LAN sessions; public identity authentication needs a chosen
    certificate/signaling service and must never be inferred from packet encryption alone.
15. Every project has exactly three primary time coordinates: the discrete simulation tick; unscaled continuous
    microseconds used for wall timestamps, authored durations and human-facing configuration; and active-gameplay
    microseconds derived from committed ticks through the session rate/scale. Calendar and turns are projections,
    while the gameplay event heap is a data structure rather than a fourth clock.
16. Active-gameplay time stops during a local paused session. Camera/world presentation and their metric sampling
    are gated by that same active state; UI/platform polling may continue in wall time. A multiplayer participant
    cannot locally stop the authoritative active-gameplay timeline: opening a menu only changes local input and
    presentation policy while session ticks continue.
17. Content-facing durations remain authored in integer microseconds. A session-wide integral tick rate converts
    deadlines to ticks or projects each fixed step into active-gameplay microseconds with an explicit remainder,
    so choosing 30/60/120 Hz does not require rewriting content.
18. Gameplay animation and rendered animation are separate consumers of the same request. The main/simulation
    owner schedules data-only gameplay events on its tick timeline; the renderer independently advances visuals
    and never sends a callback which can commit gameplay.
19. Programming-invariant failures use the single fatal `utils::error` path; exception subclasses are not an API.
    Invalid network/checkpoint/config data uses an explicit result (`bool`, `optional`, `expected` or a project
    status enum) and validates a replacement before mutation. Production `try/catch` is audit debt unless an
    unavoidable throwing dependency is being translated at one narrow boundary.

## Terms and invariants

`simulation tick`
: The only coordinate of authoritative progression. It advances after a complete causal commit, not after a
  render frame, network poll or wall-clock interval.

`network tick`
: The session view of a simulation tick: the key by which inputs, predictions, corrections, roots and replay
  history are correlated. It is not a wire packet or necessarily one C++ aggregate. In exact lockstep its main
  payload is the canonical intent bundle. In an authoritative-float session, state frames additionally refer to
  the state after an explicit network tick.

`intent`
: A requested semantic action. It is project-owned data with a player/principal, sequence, requested or
  assigned tick, kind and payload. `act::intent` may remain an internal execution seam, but is not the stable
  network command format.

`canonical bundle`
: The ordered, validated set of intents assigned to one simulation tick. Arrival order, socket callback order
  and worker completion order have no gameplay meaning.

`state frame`
: A small authoritative replication message, normally containing a subset of entity/component state and an
  explicit server tick/baseline. It is not a complete world checkpoint.

`checkpoint`
: A complete canonical causal state at a committed tick. Loading checkpoint `K` and replaying canonical
  bundles `K+1..N` must produce the same authoritative result as an uninterrupted run to `N` for an exact
  domain.

`state root`
: A digest of canonical, uncompressed causal state. Compression bytes are never state identity.

`derived state`
: Data reconstructible from authoritative state, such as queries or a spatial index. It is excluded from a
  checkpoint and rebuilt after load. If gameplay reads it, reconstruction and all tie-breaks must obey the
  relevant determinism contract.

`authored duration`
: An integer duration stored in content/configuration in microseconds. It expresses design time without choosing
  the session tick rate and is quantized to an integer tick duration before becoming causal state.

`active gameplay time`
: A pausable/scalable continuous microsecond projection produced only by committed simulation ticks. Cooldowns,
  calendars and other rate-sensitive gameplay quantities may use it. Its exact rational remainder is causal state.

`gameplay event timeline`
: A bounded, single-owner queue of project-owned data events keyed by simulation tick. It returns every due event
  as a canonical batch and is checkpointed with the world. It contains no callbacks, wall-clock timestamps,
  renderer handles or thread synchronization.

## Simulation time and presentation separation

The engine exposes three time coordinates with different jobs:

```text
real microseconds    -> authored durations, wall timestamps, pacing and human-facing values
simulation tick      -> discrete authoritative ordering and fixed-step execution
active gameplay us   -> pausable/scalable tick-derived time for cooldowns/calendar/world presentation
```

Render interpolation may sample wall time internally, but it only interpolates committed snapshots and freezes its
world-facing state when active gameplay is paused. Metrics still measure real CPU/frame durations,
yet their accumulation window is active-only so a menu pause does not dilute the first sample after resume.
Changing render FPS, pausing a render thread, losing a window or running without graphics must not change which tick
commits an action. A request such as “play attack animation” is fanned out in one direction:

```text
project/main thread
  +-> gameplay timeline: (due_tick, task_id, gameplay marker)
  \-> render outbox:      (task_id, visual animation command)   [optional]
```

The task ID only correlates the two views; render completion is not an authoritative fact. Headless and animated
modes schedule the same causal marker at the same tick. Only the animated mode emits the render command.

The generic gameplay timeline is a bounded min-heap because scheduling and due extraction need priority-queue
behavior. Its public checkpoint is nevertheless a canonical vector sorted by `(tick, source key, ordinal)` so
heap layout and insertion order never become serialized state. It returns owned due batches rather than invoking
functions. Capacity, duplicate identity and backward-time errors are explicit.

Tick rate is session/configuration identity and must enter the compatibility fingerprint before networking can
start. Replaying a checkpoint with a different rate is invalid even though content durations remain in
microseconds. Conversion uses integer quotient/remainder arithmetic and does not require `int128`.

## Logical histories, not packet histories

The minimal session model owns several bounded histories with different lifetimes:

```text
intent history       tick -> predicted bundle + optional authoritative bundle + confirmation state
checkpoint ring      tick -> complete locally restorable causal state
state-frame history  state sequence/tick -> authoritative baseline or delta needed for reconciliation
inbound queues       decoded owned messages waiting for the session/simulation owner
```

The network worker polls raw transport messages, validates their transport envelope, copies or transfers owned
payloads into bounded logical queues and releases the backend message. Raw UDP packets and backend reassembly
fragments do not enter replay history.

Incoming messages should be queued by delivery/backpressure class rather than by every packet opcode:

```text
control and canonical intents    reliable ordered
current motion/state             unreliable sequenced
checkpoint transfer              reliable bulk on an independent lane
telemetry                         lossy
```

When authoritative intents for tick `T` differ from prediction, the owner restores the newest checkpoint before
`T` and replays through the current tick, using canonical bundles where known and saved predictions afterwards.
The first correct implementation replays the complete causal simulation. A smaller prediction island is allowed
only after a project proves that no excluded system can be affected by it.

When an authoritative state frame for `T` arrives, it is compared with the locally retained predicted state for
`T`, not with the current state. A predicted object is restored/corrected at `T` and later local inputs are
replayed; a remote non-predicted object normally just receives a new interpolation sample.

When a full checkpoint for `T` arrives, an equal local state root confirms an anchor without replacing anything.
A mismatch replaces a staging-validated causal state at `T` and replays bundles `T+1..current`. History is not
discarded merely because one message was processed: it is pruned only behind a confirmed anchor and the bounded
rollback/retransmission/authority-migration window.

## State domains

Every project field/component must be classified. The classification is about causal ownership, not merely
whether C++ can serialize its type.

| Domain | Examples | Checkpoint | Exact state root | Regular replication |
| --- | --- | ---: | ---: | --- |
| authoritative exact | health, money, inventory, cooldown, entity allocator, deterministic workflow cursor | yes | yes | intents or reliable state |
| authoritative predicted | transforms, velocity, contacts, rigid bodies | yes | server only | sequenced state frames |
| durable command history | canonical tick bundles and server sequence | separate log | bundle digest | reliable ordered |
| derived cache/index | ECS query cache, kD-tree, navigation cache, render batch | no | normally no | no; rebuild |
| presentation | camera, interpolation, animation, particles, audio, UI, GPU state | no | no | optional cosmetic data |
| transient event/outbox | sound request, impact particle, notification | no | no | derived/deduplicated event |
| immutable content/rules | modules, scripts, prefabs, balance configuration | fingerprint | compatibility digest | handshake only |
| server-only/secret | hidden information, anti-cheat truth | server checkpoint | server only | filtered project view |

Determinism is a property of a transition, not of a serialized value. IEEE float bits can be written
canonically and still evolve differently on the next tick.

An exact subsystem must not read an uncorrected local value from a predicted subsystem. If local float
physics can produce `9.999` while server physics produces `10.001`, an exact AI must consume a canonical
quantized position, a server-owned spatial result, or a server-issued event. Otherwise the prediction error
can change target selection and become a discrete causal divergence which a later position correction does
not repair.

## Proposed library boundary

The neutral core should live in a new `libs/network` library, but the name must not imply that every header
opens sockets. Its first job is bounded cross-process/session synchronization primitives.

The core must not depend on:

- ASIO, GameNetworkingSockets, Yojimbo or an operating-system socket API;
- `aesthetics::world`, `act::intent`, `tile_frontier` or any game entity type;
- Lua, devils_script, renderer, resources or a particular app runtime;
- a client/server topology, player count, prediction policy or hidden-information model;
- compression, encryption or a particular hash implementation;
- background threads.

Likely target split after the relevant slices are proven:

```text
devils_engine::network             header-first neutral primitives
devils_engine::network_gns         optional GameNetworkingSockets adapter
devils_engine::network_yojimbo     optional experimental Yojimbo adapter
devils_engine::network_asio        optional TCP/service adapter, only with a consumer
```

The first target should be header-only like `resolve` unless a concrete non-template implementation earns a
source file. Optional adapters must not leak their third-party types into `devils_engine::network`.

Tentative headers are listed to expose ownership, not to commit to implementing all of them at once:

```text
libs/network/
  CMakeLists.txt
  README.md
  include/devils_engine/network/
    tick_journal.h
    sequence_window.h
    bounded_history.h
    replay.h
    state_schema.h
    state_digest.h
    delivery.h
    in_memory_transport.h
    network.h
```

`tick_journal.h`, `sequence_window.h` and `bounded_history.h` are implemented;
the remaining names are only ownership sketches for later slices.

## Template design rules

The library should follow the proven policy/template style in nearby engine libraries:

- concrete values and phases, not object-oriented service hierarchies;
- concepts/static assertions for the required shape;
- project-owned record/payload types;
- explicit runtime budgets;
- no heap fallback after a deterministic budget is exceeded;
- phase violations and ambiguous provenance fail loudly;
- physical append/arrival order is not semantic order;
- no hidden singleton except the separately accepted per-project schema registry;
- free algorithms or small owning templates instead of a universal session manager;
- an abstraction is added only with a headless test that proves its boundary.

Templates must isolate choices that genuinely vary. They must not turn every integer, allocator and function
call into a policy merely for configurability.

## NET-01: tick journal — the first self-contained slice

The first library addition is a bounded tick journal. It knows neither network nor gameplay:

```cpp
template <
  class Record,
  class Tick,
  class TickOf,
  class SemanticLess,
  class SemanticEquivalent
>
class tick_journal;
```

Possible lifecycle:

```text
idle
  -> begin(tick, capacity)
recording
  -> try_record(record)...
  -> seal()
sealed
  -> records()
  -> consume()
consumed
  -> retire/reset
```

Contract:

- `TickOf(record)` must equal the open tick;
- `try_record` never partially writes a record;
- capacity overflow is latched and reported by an explicit `tick_seal_result`;
- `seal` sorts by `SemanticLess` and rejects semantic duplicates through `SemanticEquivalent`;
- `records()` exposes only sealed storage;
- a consumed slot cannot be reopened under the same generation accidentally;
- late/future policy is not owned by the journal; its owner chooses another tick or rejects the record;
- the first slice is single-owner. A network worker reaches it through an existing SPSC channel or passes an
  owned batch to the session thread. MPSC/atomic recording is a separate proven need, not a default;
- no virtual source API exists.

The project intent envelope can be one aggregate while passing through two journal instances:

```text
untrusted ingress journal
  -> authentication/ownership/range/sequence validation
  -> canonical tick journal
  -> simulation
```

Required `network_tick_journal_test` checks:

1. shuffled insertion seals into the same semantic byte sequence;
2. a record for another tick is rejected;
3. duplicate `(principal, sequence)` provenance is rejected;
4. exact capacity succeeds and capacity plus one poisons `seal`;
5. `records` before seal and record after seal fail;
6. consume is once-only;
7. slot/tag wrap cannot expose records from an old tick;
8. two different project record structs work without inheritance or library changes.

Not in NET-01: sockets, wire encoding, replay, checkpointing, validation semantics, timeouts, players, ECS or
rollback.

## NET-02: sequence windows and bounded bundle history — complete 2026-09-04

The journal is followed by two independent generic primitives:

```cpp
template<class Sequence, size_t WindowBits>
class sequence_window;

template<class Tick, class Bundle>
class bounded_history;
```

`sequence_window` classifies a sequence as new, duplicate, stale or too far ahead and handles unsigned wrap
explicitly. It does not send ACKs or decide whether duplicates are malicious.

`bounded_history` owns immutable sealed bundles indexed by tick. Runtime count/byte budgets and eviction are
explicit. It can be used by replay tests, a server, a client prediction owner or an offline tool.

The implemented window uses `WindowBits` both as its duplicate-retention window and as the largest forward gap
accepted implicitly. A larger authenticated session jump requires an explicit `reset`; the half-range distance
is ambiguous and is never accepted implicitly. Classification without observation does not mutate state.

The implemented history requires monotonically increasing insertion but permits gaps. Its byte budget is the
logical cost supplied with each project-owned bundle, not guessed from allocator capacity or memory reachable
through an arbitrary type. A successful insertion reports exact count/bytes evicted. Duplicate, out-of-order and
budget refusal are ordinary statuses which do not mutate retained entries. Zero-byte bundles remain explicit
ticks and consume count capacity; returned access is const and borrowed only until eviction/clear/destruction.

Required tests:

- sequence wrap at the maximum value;
- duplicate and stale classification;
- gaps do not silently become delivery guarantees;
- tick lookup and ordered iteration;
- oldest eviction under count and byte budgets;
- an existing immutable bundle is never mutated through a returned view.

All nine NET-02 cases pass in Debug and Release. Together with NET-01, the neutral networking core passes
17/17 cases in both configurations. There are no GNS, ACK, wire-format or replay dependencies in this slice.

## NET-03: explicit full-state sections — complete 2026-09-04

The project world is one schema, but ECS bytes are not the whole state. A checkpoint also needs clocks,
remainders, global sequences, project configuration and resumable workflow cursors.

The neutral mechanism is a compile-time list of project-owned section policies:

```cpp
template<class Host, class Writer, class Reader, class... Sections>
class state_schema;
```

A section policy supplies stable metadata and typed operations, for example:

```cpp
struct actor_world_section {
  static constexpr uint32_t id = /* explicit stable id */;
  static constexpr uint32_t version = 1;

  static void write(const project_state&, writer&);
  static bool read(staging_state&, reader&);
  static bool validate(const staging_state&);
};
```

The implemented spelling uses `Host::staging_type`; sections operate on the live host only while writing and on
the staging type while reading/validating. Cross-section validation and one final `noexcept` replacement are
project-supplied callables. These properties are enforced:

- section IDs and versions are explicit and unique at compile time;
- section order is canonical and independent of registration/static-init order;
- the same traversal feeds checkpoint bytes and the state hasher;
- unknown/missing/duplicate required sections fail according to an explicit compatibility policy;
- decode targets staging data and never mutates the live simulation;
- a project supplies the final validation and replacement operation;
- state schema contains data ownership, not system callbacks, threads or caches.

The initial compatibility policy is explicitly `exact`: unknown, missing, duplicate and non-canonical sections,
version mismatches, malformed bodies and trailing bytes are rejected as `state_load_status` values. Results carry
the responsible section ID and expected/actual version where applicable. Parameter-pack order is erased by a
compile-time ID sort; duplicate IDs are rejected by `static_assert`; version zero is reserved.

`emit_canonical` is the single traversal for the complete envelope and every section payload. A normal writer
produces checkpoint bytes, while a sink with the same three scalar/byte operations hashes exactly that byte
stream. The 32-bit Murmur3 schema fingerprint uses the shared `utils::murmur_hash3_32` implementation and covers
canonical `(format, count, id, version)` metadata only: it is format compatibility metadata, not the strong
state/content digest planned for NET-05.

The fake-host test uses two unrelated sections and checks every required boundary, including every possible
truncation of a valid document. All eight NET-03 cases pass in Debug and Release. The live host stays byte-for-byte
unchanged on decode, section-validation and whole-host-validation failure; success performs exactly one replacement.

The `aesthetics` component registry now has an explicit freeze phase. Its public table is const, the first
fingerprint/dump/load freezes registration, and a later `SERIALIZABLE_COMPONENT` registration is a fatal invariant
violation. This makes an ECS world suitable as one nested project section. Transactionally replacing the real
world plus its query-owning systems remains the deliberately separate ECS follow-up below.

The intended project split is:

```cpp
struct simulation_state {        // serialized and hashed
  aesthetics::world world;
  timeline_state timeline;
  project_globals globals;
  causal_pipeline_state pipelines;
  deterministic_sequences sequences;
};

struct simulation_derived {      // rebuilt
  spatial_indices spatial;
  query_cache queries;
  navigation_cache navigation;
};

struct simulation_services {     // neither serialized nor hashed
  script/config references;
  system executors;
  worker access;
};
```

The actual `aesthetics::world` can remain an owning container rather than a POD. “State through data” means
that every future-affecting fact has a canonical data representation; it does not require flattening all
storage into one public struct.

Required tests use a fake host with two unrelated section types:

1. canonical bytes are independent of construction order;
2. section ID/version changes alter the schema fingerprint;
3. duplicate IDs fail at compile time where possible;
4. a truncated/bad section leaves the live host byte-identical;
5. validation failure leaves the live host byte-identical;
6. successful staging replacement changes the whole host atomically;
7. trailing bytes and duplicate/missing sections follow an explicit tested policy;
8. state hash covers exactly the same canonical content as checkpoint writing.

### Incremental world checkpoint, only after the full form

The first implementation materializes a complete canonical checkpoint. Only after its size/write/read/hash cost
is measured may an incremental checkpoint codec be added. Incremental checkpointing is an optimization of
checkpoint storage/transfer; it is not the same mechanism as regular entity replication or an unreliable state
frame.

A possible generic form is a manifest of independently replaceable section pages:

```text
incremental checkpoint T
  schema/content/profile identity
  explicit base checkpoint ID and tick K
  complete ordered page manifest
  changed page payloads
  explicit removed pages/entities
  canonical materialized state root for T
```

Required invariants:

- reconstruction occurs in staging and live state is replaced only after the entire target validates;
- absent change data means "use the named base page", never an implicit zero or deletion;
- missing/wrong base is rejected and requests a full baseline;
- the number and total size of dependent deltas are bounded; periodic full checkpoints cut the chain;
- deletions and entity allocator evolution are explicit;
- identity is the root of materialized canonical state, never the compressed/incremental byte stream;
- a checkpoint page boundary is stable schema policy, not current ECS allocation addresses;
- dirty/version tracking must prove that every future-affecting mutation marks the owning page. Until that is
  proven, comparing canonical page roots is slower but correct;
- Jolt physics state, project globals, timeline remainders and workflow cursors participate through their own
  state sections and cannot be inferred from changed ECS transforms alone.

The research spike must compare three starting choices: full checkpoint plus compression; section/page hash reuse
with full manifest; and explicit version/dirty tracking. The smallest measured sufficient mechanism wins.

## NET-04: checkpoint ring and replay algorithm

Checkpoint storage and replay are separate generic mechanisms:

```cpp
template<class Tick, class Blob, class SizeOf>
class checkpoint_ring;

template<class Host, class Checkpoint, class BundleRange,
         class Restore, class ApplyBundle, class Step, class Digest>
replay_result replay_to(...);
```

`checkpoint_ring` knows only immutable blobs, their ticks and byte/count budgets. Compression is performed by
the owner before insertion if desired.

`replay_to` is a free/template algorithm. It receives operations or a Host concept; it does not require
inheritance. It restores checkpoint `K`, applies every canonical bundle in order and stops at target tick `N`.

The central property test is performed for every possible checkpoint in a short run:

```text
uninterrupted run 0..N
==
restore checkpoint K + replay bundles K+1..N
```

Additional tests:

- missing bundle is a fault, not an implicit empty tick unless the project explicitly stored an empty bundle;
- duplicate/out-of-order bundle is rejected;
- replay does not publish presentation effects;
- target before/after available history is reported;
- byte/count eviction remains deterministic;
- digest reports the first divergent tick.

## NET-05: state digest and mismatch localization

The first digest may simply hash canonical uncompressed state bytes with SHA-256. The transmitted root can be
truncated if measured safe for diagnostics, while full roots remain in logs.

Later, the same section traversal may expose hierarchical roots:

```text
state root
  globals
  entity allocator
  component A / page 0..N
  component B / page 0..N
```

This is diagnostic structure, not necessarily a dynamic Merkle tree. Do not add incremental hashing before a
full canonical hash is measured as a real bottleneck.

The Murmur64 checksum in the current snapshot envelope remains accidental-corruption detection. It is neither
state identity nor authentication.

## NET-06: in-memory transport and fault injection

Before a real socket backend, a deterministic in-memory transport must exercise the message contract:

```cpp
template<class Message, class Clock, class DeliveryPolicy, class FaultPolicy>
class in_memory_link;
```

The mechanism may use injected policies/callables rather than exactly these template parameters. It must be
able to model:

- fixed and varying latency;
- packet loss;
- duplication;
- reordering;
- bounded bandwidth/queue pressure;
- disconnect and reconnect;
- reliable ordered, reliable bulk and unreliable sequenced logical delivery.

The fault schedule is explicit input data with a seed, so a failing run is replayable. The library does not
pretend that its reliable policy is a production congestion protocol; it only supplies controllable delivery
semantics for session tests.

This slice proves client/server orchestration without mixing simulation bugs with sockets.

## NET-07: replication primitives

Replication is distinct from checkpoint serialization. A state frame references an explicit tick, state
sequence and optional baseline:

```cpp
struct state_frame_header {
  uint64_t server_tick;
  uint64_t state_sequence;
  uint64_t baseline_sequence;
  uint64_t acknowledged_input_sequence;
};
```

The engine may later provide generic baseline/change-mask primitives parameterized by project codecs. It must
not define what a transform, entity or relevant field is.

Possible neutral templates:

```cpp
template<class Key, class Value, class Version, class Equal>
class baseline_store;

template<class Snapshot, class DeltaPolicy>
auto make_delta(const Snapshot& base, const Snapshot& current);
```

Project policy owns:

- mapping resident ECS IDs to network/logical IDs;
- interest management;
- quantization;
- ownership and visibility;
- which components are reliable or sequenced;
- materialization and entity lifecycle;
- correction and presentation smoothing.

Required tests cover an explicit baseline ID, lost deltas, stale sequenced frames, duplicate frames, entity
create/delete, full baseline recovery and byte budgets. A delta whose baseline is missing is rejected; it never
guesses.

## Transport channels

### Pre-implementation transport capability audit

Transport capability is a research gate, not the adapter itself. Build a disposable GNS probe before freezing
the neutral API. A future Yojimbo comparison, if reopened, must run the same fixture. The probe sends opaque byte
messages; it must not introduce a project entity, checkpoint or custom serialization model.

The audit must answer from public API behavior and a small executable, not from feature-list inference:

- what receive queue/poll-group abstraction exists and who owns a received payload;
- whether sends copy bytes or can transfer caller-owned buffers, and when ownership is returned;
- reliable ordering scope: connection, channel or lane;
- unreliable duplicate/out-of-order behavior and whether an application sequence is still required;
- aggregation/Nagle behavior, explicit flush and the cost of an intent message every tick;
- message and data-block fragmentation thresholds, failure behavior and maximum configured size;
- independent priority/weight lanes and whether checkpoint bulk can delay current intents;
- queue byte/message limits, backpressure result codes and per-lane high-water/status statistics;
- loopback/socket-pair support, fault injection, RTT/jitter/loss/bandwidth statistics and traceability;
- connection identity, authentication, reconnect and endpoint/P2P support;
- thread-affinity and callback constraints;
- build footprint, optional dependencies and whether transport types can remain inside an adapter;
- serialization overlap: can the engine send one already-canonical byte span without adopting a second state
  schema or message class hierarchy?

Current source-level finding: both reviewed libraries own packet sequencing, acknowledgement, duplicate handling,
fragmentation/reassembly and transport send/receive queues. GNS additionally exposes connection poll groups,
caller-owned send messages, configurable lanes and lane queue status. Yojimbo exposes configured
reliable-ordered and unreliable-unordered message channels, large data blocks and its own bitpacking/message
factory layer. These mechanisms must be reused by their adapter.

They do **not** remove the need for simulation histories. A transport message number or packet sequence does not
identify a simulation tick, predicted bundle, authoritative replacement, checkpoint or state baseline. Even if a
backend internally uses a ring/sequence buffer, coupling gameplay rollback to an internal transport container
would give it the wrong eviction, lifetime and compatibility contract. `tick_journal`, `bounded_history`,
`checkpoint_ring` and `baseline_store` remain engine-owned only if their independent tests prove those semantics.

The GNS spike ends with a checked-in result table and one of three decisions per transport feature:

```text
reuse through adapter
wrap a stable public primitive
implement a neutral engine primitive because no fitting public contract exists
```

The intended logical channels are independent of a backend:

| Channel | Delivery | Notes |
| --- | --- | --- |
| handshake/session control | reliable ordered | compatibility before the first tick |
| canonical intent bundles | reliable ordered | overflow/disconnect is a fault, never silent drop |
| entity lifecycle/discrete state | reliable ordered | project-owned schemas |
| checkpoint chunks | reliable bulk/separate lane | must not head-of-line block current intents |
| transform/state frames | unreliable sequenced | latest applicable frame wins |
| state roots/mismatch reports | reliable or repeated | diagnostic, never client authority |
| telemetry | lossy | no causal effect |

Using separate TCP and UDP connections for gameplay reliability is not the default plan. They have separate
connection lifetimes, ordering and congestion behavior. A message-oriented UDP/QUIC transport can expose both
reliable and sequenced lanes through one session.

### GameNetworkingSockets

Current selected backend. It provides connection-oriented, message-oriented UDP; reliable and
unreliable messages; fragmentation/reassembly/retransmission; encryption; lanes; IPv6; latency/loss simulation
and detailed statistics. It deliberately does not provide entity serialization or delta state, which matches
the desired ownership boundary.

Its public receive API returns complete `SteamNetworkingMessage_t` objects per connection or poll group. Received
messages may be retained briefly but must eventually be released. Its send API can allocate a message through
GNS or accept an application buffer with a release callback; lanes govern outbound priority/weight and expose
queue status. This is enough to avoid an extra transport packet/message ring inside the adapter, but not enough
to implement tick rollback history.

The adapter should translate only neutral connection/message/delivery concepts. GNS connection handles and
flags do not enter project state or replay data.

#### Current implementation evidence

PRE-01 is complete for the current GNS-only decision. The direct probe covers message ownership, internal and
real UDP loopback, above-MTU fragmentation/reassembly, lanes, Nagle/flush, backpressure, sustained loss/reorder,
real IP listen/connect, new-handle reconnect, the standalone authentication boundary and the local P2P fast
path. Detailed behavior, exact scenarios, current timings, reproduction commands and unresolved infrastructure
are maintained in [NETWORKING_STATUS.md](NETWORKING_STATUS.md).

Yojimbo remains unbuilt and its comparison is deferred indefinitely. It may be reopened later only if a concrete
GNS limitation justifies the cost.

### Yojimbo (comparison deferred indefinitely)

This is retained only as an archival reference and possible future comparison if a concrete GNS limitation is
found. It provides encrypted client/server UDP, connect tokens, reliable ordered and unreliable channels,
fragmentation, bitpacking and statistics. It is a more vertical stack and brings its own
serialization/reliability dependencies, so a future experiment would have to check whether those layers fight
the engine's existing schema and ownership model.

Yojimbo is explicitly single-threaded: all calls for an instance belong to one network thread. Its client and
server must share an identical channel/configuration and message factory. This fits a dedicated network owner,
but the adapter must prove that already-serialized engine bytes can pass without duplicating the canonical state
schema. Its internal channel/sequence buffers are transport implementation, not a public tick-history API.

### ASIO and HTTP/TCP

Standalone ASIO is suitable for asynchronous TCP, UDP, timers and service I/O. It does not itself implement an
HTTP message/parser/client policy, nor reliable game UDP. HTTP/TLS may use ASIO plus a proven HTTP layer, or a
separate HTTP library. This service path can coexist with GNS:

```text
network/service worker
  GNS poll/send        gameplay UDP
  asio::io_context     HTTP/TCP/control consumers
```

They communicate with session/gameplay through bounded broker channels. Neither callback source mutates the
ECS directly.

### Encryption dependency ownership

Do not add libsodium unconditionally to the engine networking core. GNS already owns encrypted packet transport;
Yojimbo bundles libsodium as part of its encrypted/signed UDP stack. Double-encrypting the same realtime payload
would add key lifecycle, bytes and failure modes without defining a new trust boundary.

An explicit libsodium target is earned only by an application-level need not supplied by the selected transport,
such as signing durable content, encrypting saved credentials/state, end-to-end protection through an untrusted
relay or a backend with no suitable authenticated encryption. Keys, nonce/sequence rules and replay protection
then belong to that named consumer rather than to `tick_journal` or state serialization.

### QUIC

MsQuic is a later candidate because one encrypted connection can contain reliable streams and unreliable QUIC
datagrams. It also supplies congestion control, pacing and migration. Its integration and deployment weight
is greater than a game-specific transport and does not belong in the first slice.

## Peer-oriented session and authority role

The neutral surface names participants `peer_id` and does not build separate inheritance trees for clients and
servers. The protocol still has an explicit authority role:

```cpp
struct session_membership {
  peer_id local_peer;
  peer_id authority_peer;
  authority_epoch epoch;
};
```

A follower proposes intents, predicts permitted state and consumes the authoritative timeline. The authority
validates ownership/ranges/rates, assigns or accepts ticks, fixes semantic ordering, seals canonical bundles and
publishes state frames, roots and recovery checkpoints. Thus the intended design is peer-oriented at the API and
authority-oriented in protocol semantics.

A dedicated server is the same causal project linked into a process that has an authority peer but no local
player or presentation. Its target must not initialize or require Vulkan, GLFW, audio, UI or GPU resources. It
runs a fixed simulation clock, the selected transport owner, checkpoint/save coordination, metrics/admin seams
and clean bounded shutdown. Client and dedicated targets link the same causal component/schema registration;
client-only presentation registration must not alter the causal fingerprint.

Authority migration is a later protocol, not an implication of using peer terminology. It requires an epoch,
rejection of messages from old epochs, recent confirmed checkpoint and canonical bundle log at every candidate,
a deterministic or externally coordinated election and split-brain prevention. Initially the creator or a
dedicated process remains authority. Connection-quality-based automatic election is postponed because different
peers can observe incompatible rankings.

## First synchronization model: server-authoritative float

The initial model intentionally does not demand cross-platform lockstep:

```text
client input
  -> intent record with client sequence
  -> gameplay UDP reliable lane
server
  -> validate principal/ownership/range/rate
  -> assign tick and canonical order
  -> authoritative float simulation
  -> reliable discrete changes/events
  -> sequenced motion state frame
client
  -> reconcile owned prediction
  -> interpolate other entities
```

A correction frame normally needs more than position:

- position and velocity;
- orientation and angular velocity if present;
- movement mode;
- grounded/contact state where it affects the next integration;
- last processed local input sequence.

For the locally controlled entity the client restores server state at tick `T`, discards acknowledged inputs,
replays remaining local inputs and lets presentation converge smoothly. Remote autonomous actors can initially
be interpolated rather than authoritatively simulated on the client.

If client-side float simulation changes AI target, collision outcome, eating state, entity lifetime or another
discrete fact, transform correction alone is insufficient. The first `tile_frontier` slice should therefore
keep actor AI and structural outcomes server-authoritative. Later experiments can replicate the necessary
causal components or rollback from an authoritative checkpoint.

### Replication cadence is project data

Intent and transform cadence are independent. Intents are proposed/closed every simulation tick; transforms can
be sent less frequently because a newer sequenced frame supersedes an older one and presentation can interpolate.
The project/session declares a schedule per replicated data class rather than one global `snapshot_rate`:

| Data class | Starting cadence for a 20 Hz simulation | Delivery/use |
| --- | --- | --- |
| local intent proposal | every tick, often redundantly carrying a short recent input window | reliable or application-repeated |
| canonical intent bundle | every tick, including an explicit empty bundle | reliable ordered |
| owned predicted transform/physics | every 1–2 ticks | unreliable sequenced correction + acknowledged input |
| remote transform/physics | every 2–4 ticks initially | unreliable sequenced interpolation sample |
| reliable discrete/lifecycle state | on change, with bounded batching | reliable ordered |
| full replication baseline | every several seconds, on missing baseline or on request | reliable lane independent of intents |
| exact-domain state root | every 20–100 ticks initially | repeated/reliable diagnostic and confirmation anchor |
| local rollback checkpoint | every measured `K` ticks | not normally transmitted |
| full world checkpoint | join/reconnect/deep resync/migration/debug | reliable bulk, exceptional |

These are experiment defaults, not library constants. A cadence policy contains at least interval in simulation
ticks, maximum tolerable age, priority, delivery class and byte budget. An adaptive sender may later vary the
interval within declared bounds using relevance, velocity and connection conditions. Correctness must not depend
on receiving every transform frame.

Client state hashes are diagnostics, never security or server authority. In a float-authoritative model the
client cannot independently derive the authoritative physics root; it can only identify the last accepted
server baseline and hash exact domains it truly executes.

## Math plan and experiments

The project currently pins GLM 1.0.1. GLM is a vector expression library, not a cross-platform bitwise math
contract. Its trigonometric wrappers call standard-library functions, while vector operations may select
scalar or SIMD implementations through the existing `GLM_FORCE_PURE/AVX/AVX2/INTRINSICS` configuration.

The current build does not explicitly enable fast-math, which is good but insufficient. FMA contraction,
SIMD reduction order, libm implementations, denormal handling, rounding environment and NaN/signed-zero
behavior remain relevant.

The math work should be a separate library/contract rather than part of `network`. It needs only the causal
operations proved by a consumer, not a replacement for all GLM rendering math.

Candidate backends:

| Backend | Purpose | Main limitation |
| --- | --- | --- |
| current native float/double | baseline | not guaranteed cross-platform exact |
| strict native float/double | measure controlled compiler/ISA profile | libm and platform state remain |
| CORE-MATH elementary functions | correctly rounded trig/exp/etc. for strict float | not vector/basic-operation determinism |
| Berkeley SoftFloat | reference/oracle for IEEE operations | too heavy and incomplete for realtime general math |
| `fpm` | header-only C++ fixed point plus math functions | WIP; policies/UB need audit |
| FixPointCS C++ core | deterministic Q16.16/Q32.32 scalar algorithms | limited high-level C++ vector layer; overflow compromises |
| CNL | explicit representation/rounding/overflow type system | not a full fixed-point libm |
| libfixmath | simple Q16.16 reference | old, inactive and inflexible |

### Jolt as both physics and a bounded math backend

Jolt deserves a capability spike before adopting CORE-MATH broadly. Its public `Trigonometry.h` deliberately
exists because platform `std::sin` and peers are not deterministic and provides float `Sin`, `Cos`, `Tan`,
`ASin`, `ACos`, `ATan` and `ATan2`; its vector/quaternion code and `Math.h` provide the scalar/vector operations
needed by its own simulation, including square root and normalization. These functions are conveniently isolated
enough to test behind a small project facade.

This is not evidence that Jolt is a complete `libm`: its deterministic public trig set does not cover every
possible `exp`, `log`, `pow`, rounding and special-function need of unrelated gameplay. The experiment therefore
starts by inventorying actual causal calls in `tile_frontier`, not by declaring Jolt or CORE-MATH the engine-wide
backend.

Proposed facade experiment:

```text
devils_engine::causal_math API required by one recorded scenario
  native/GLM backend         baseline
  Jolt backend               use isolated Jolt scalar/vector/trig implementation
  CORE-MATH backend          only for elementary operations Jolt does not cover or where correct rounding matters
```

The facade owns project names and tests; Jolt types must not leak into unrelated libraries merely to obtain
`sin`. License/build/compile-profile coupling and generated code size are measured. Raw bits and exceptional
inputs are compared across supported Debug/Release/compiler/ISA builds. Correctly rounded CORE-MATH remains a
candidate per function rather than an automatic second full math stack.

Jolt also provides `CROSS_PLATFORM_DETERMINISTIC`, `StateRecorder`, `PhysicsSystem::SaveState` and
`RestoreState`. Its documented contract still requires identical source/defines and simulation mutation order,
precise application FP flags, no contraction, consistent FPU state and ordered handling of queries/listener
callbacks. `SaveState` stores state modified by the physics update, not arbitrary application mutations; body
creation/destruction and stable `BodyID` handling need project replay/state support.

The physics spike consequently runs the same recorded scene in two modes:

```text
exact Jolt candidate
  canonical intents + per-tick physics/state roots
  cross-build corpus must remain identical before correction traffic is removed

authoritative corrected Jolt
  canonical intents + less frequent transform/velocity/contact state frames
  checkpoint/replay remains recovery and diagnostics
```

If Jolt exactness holds for the supported project profile, transform cadence can be reduced toward diagnostics.
If it does not, the authoritative correction mode remains valid without forcing fixed-point general physics.
The full project checkpoint must include a Jolt state section plus stable logical entity-to-body mapping and every
external causal setting/command required to reconstruct identical update calls.

MSVC has no ordinary native `__int128`. It exposes x64 `_umul128` and `_udiv128` intrinsics, which can back a
portable two-limb wrapper but do not make libraries written directly for `unsigned __int128` portable.

This does not eliminate all fixed point:

- a raw `int32_t` fixed value uses an ordinary `int64_t` intermediate;
- Q16.16 covers approximately `+/-32768` with about `1.5e-5` resolution;
- a measured local/chunk coordinate may fit fixed32 easily;
- scaled `int64_t` health/money/ratios do not automatically require a persistent Q32.32 type;
- raw `int64_t` fixed point with full precision is the case that needs a 128-bit multiplication/division
  backend.

Using Clang on every platform reduces compiler variability but does not make native floats bit-identical
across x86/ARM, different SIMD paths, FMA/libm and denormal settings. It is not a substitute for fixed point if
full lockstep is the requirement.

The eventual `tile_frontier` experiment matrix should include:

```text
native float
native double
strict/pure float
strict/pure double
fixed32 with measured format
fixed64 only if measured range requires it
```

For each mode record first divergent tick, raw/ULP/spatial error, first divergent component/entity, changed
discrete decisions, correction count/magnitude, bandwidth, replay cost and CPU time. Numeric mode, fixed scale,
rounding rules and determinism profile belong in the session handshake.

## Current determinism audit

The repository already contains several good exactness mechanisms:

- `act::exec_context::random(purpose)` derives a result from immutable seed/entity/tick/purpose inputs instead
  of consuming a shared mutable RNG stream;
- `catalogue` deferred effects recover semantic order after the worker barrier rather than treating atomic
  append order as gameplay order;
- `originator` has fixed chunking/deterministic reductions and enables `FASTNOISE2_STRICT_FP` for its proved
  generator contract;
- `tile_frontier_resume_smoke` compares 1-worker and 4-worker actor pipelines and verifies same-build
  checkpoint continuation;
- `cardgame::player_intent` already demonstrates that a project command can own its own sequence and payload
  without expanding the generic `act::intent_kind` ABI.

These are local guarantees, not yet a complete network simulation guarantee. Current blockers found by the
audit are:

1. `tile_frontier` authoritative position, velocity, speed, size, perception and drive values use native
   float/GLM types. Integration and spatial decisions use square root, normalization and float comparisons.
2. `act::real_t` is currently `double`; GOAP/A* cost propagation therefore participates in the float profile.
3. `utils::kd_tree` partitions with `std::nth_element` using only one axis coordinate. Equal coordinates have
   no total semantic tie-break, and nearest-distance ties choose the first node found. Tree structure may
   therefore change a causal target independently of numeric precision.
4. `acumen::astar::node_compare` compares only `f`. Equal-cost nodes have no project-semantic tie-break, so a
   different heap arrangement may choose a different plan.
5. the root build supports AVX, AVX2 and NATIVE, and `libs/options` selects matching GLM intrinsic paths. The
   current common target has no separate deterministic simulation compile profile.
6. `utils::timelines` now exposes a validated causal snapshot containing the session tick, game/calendar
   projection, scale, pause state and fractional remainder. NET-03/04 must still place that section beside the
   project world in a full application checkpoint; saving actor-local `game_ticks` alone remains insufficient.
7. `tile_frontier::player_intent_queue` is intentionally ephemeral and its promised durable replay log does
   not yet exist. It currently rejects a second pending intent by `intent.kind`, not by
   `(principal, client_sequence)`.
8. the current click command converts screen coordinates through camera-dependent float `screen_to_world`
   before producing an intent. A wire command must contain a semantic cell/entity target or an explicitly
   quantized world value, never raw screen pixels.
9. exact serialization order cannot compensate for C++ undefined behavior, signed overflow, hash-table
   iteration used as gameplay order, uninitialized padding read as state, or scheduling-dependent reductions.

Before claiming exact lockstep, every sort, heap, election, nearest query and conflict resolver needs a total
semantic order. Integer/fixed arithmetic also needs explicit overflow and rounding; replacing float with a
signed integer type without those rules is not a determinism solution.

The existing smoke run is valuable but limited: Debug and Release each proved internal 1/4-worker identity and
120 resumed ticks; it did not compare the resulting bytes between Debug and Release, compilers, operating
systems or CPU architectures. Future tests must exchange golden per-tick roots between binaries.

During this audit, 36 targeted Debug tests covering serialization, resolve/catalogue ordering, acumen/act RNG
and `tile_frontier` snapshot/resume/config behavior passed. This confirms the starting point but does not widen
the guarantees beyond their current same-build contracts.

## Long-term hybrid model

Both strategies discussed in the audit can coexist by domain:

1. exact causal domains consume canonical intent bundles and can be independently hashed/replayed by every
   replica;
2. server-authoritative predicted domains use local prediction plus authoritative state frames;
3. presentation consumes either domain but never feeds uncanonical local results back into causal state.

Health, money, inventory, cooldowns, many modifiers, graph decisions and project workflows are natural exact
domains. Character kinematics may become exact after the numeric experiments. General rigid bodies, vehicles,
ragdolls and complex contact solvers are natural candidates to remain server-authoritative float until a real
game proves otherwise.

Full lockstep also exposes the full causal state to every participant. A competitive or hidden-information
project must keep secret truth on the server and replicate filtered views, even if other domains are exact.
State hashes sent by clients remain diagnostics and cannot establish honesty.

## Current serializer audit

Existing strengths:

- fixed-width little-endian scalar representation;
- IEEE float bit serialization;
- canonical sorting for map-like containers;
- component blocks ordered by component hash;
- entity allocator state included;
- `tile_frontier` already adds tick/game time/spawn sequence/config globals;
- content modules already have an ordered SHA-256 fingerprint;
- existing resume tests prove 1/4-worker identity and same-build continuation.

Required evolution:

1. Component ID currently derives from `murmur32(type_name<T>)`. A finite supported compiler matrix can test
   and pin equal results, but an explicit canonical component name/ID would make the guarantee a schema fact
   rather than a compiler-spelling accident.
2. The registry is global per executable/library instance. This is accepted because a project has one complete
   world schema. Headless server and graphical client must still register the same causal component set.
3. Fingerprint caching is acceptable only after explicit `freeze`; registration after freeze is fatal.
4. Aggregate layout hashing does not capture semantic field changes and may miss permutation of same-typed
   fields. Explicit component/section schema versions are required. Field names improve diagnostics but do not
   replace a version: rename may be harmless, while units/enum meaning can change without rename.
5. Loading must be transactional. The current `actor_world_slice::load` discards/rebuilds the old world before
   the complete packet is proven. Decode and rebuild a new simulation instance, then swap it at a tick boundary.
6. Container/string/entity/decompression budgets and overflow-safe length checks may follow the trusted
   in-memory prototype, but must be complete before accepting untrusted packets.
7. Murmur64 is corruption detection only. Authentication/encryption remains a transport/session layer.
8. `dump_world` is only one state section. Timeline remainder/scale/pause, project globals, pipeline cursors,
   authoritative config and sequence watermarks must be included in the complete project state.
9. Canonical raw state, not compressed output, is hashed.

The existing synthetic serialization measurement provides a scale warning:

| Entities | Raw | Fast compressed | Dump | Pack | Unpack |
| ---: | ---: | ---: | ---: | ---: | ---: |
| 2,000 | 141,937 B | 35,431 B | 0.335 ms | 0.820 ms | 0.242 ms |
| 20,000 | 1,428,801 B | 315,574 B | 0.806 ms | 3.091 ms | 2.198 ms |
| 100,000 | 7,165,937 B | 1,519,528 B | 4.080 ms | 15.738 ms | 11.540 ms |

Therefore full checkpoints are storage/resync objects, not tick packets. Full canonical hashing may still be
acceptable initially and should be optimized only after measurement.

## Handshake

Before the first simulation tick, peers compare at least:

- protocol major/minor;
- full project state schema fingerprint;
- ordered content/module fingerprint;
- gameplay rules/configuration digest;
- simulation tick quantum;
- numeric/determinism profile and fixed scales if used;
- RNG algorithm version;
- entity ID layout/version bits;
- authoritative component/section versions;
- mod list/order and server policy relevant to causal behavior.

Cosmetic content has a separate optional digest. A schema/content mismatch is rejected before simulation; a
live network session does not migrate causal schema in place.

## Security and hostile input

These items do not block NET-01 through the in-memory prototype, but block public networking:

- authenticated encrypted transport;
- connection principal separated from player and ECS entity IDs;
- server ownership/legality/rate validation for every intent;
- maximum packet, message, section, decompressed, container, string, entity and component counts;
- overflow-safe `remaining >= requested` checks rather than `position + length` arithmetic;
- exact protocol version/flags/algorithm validation;
- duplicate/missing section rejection;
- enum/range/NaN/Inf/reference/invariant validation;
- transactional decode and replacement;
- fuzzing packet, envelope and state decoders;
- no trust in client hashes, transforms or actor IDs.

## Delivery and backpressure

Budgets are part of the contract:

- canonical intents/control cannot be silently dropped; overflow is a session fault/disconnect;
- reliable checkpoints need their own lane/budget so bulk transfer cannot stall current intents;
- sequenced state may discard obsolete frames before delivery;
- telemetry may be lossy/latest-wins;
- every queue exposes capacity, high-water mark, rejection count and stalled duration;
- payload ownership is explicit until backend send completion;
- a one-megabyte generic broker arena must not be assumed sufficient for a multi-megabyte checkpoint.

## Detailed implementation roadmap

Each task below has a narrow observable result. A later task must not be pulled into an earlier one merely for
future flexibility.

Dependency/order map:

```text
PRE-01 GNS capability spike (complete) -------------------+
PRE-02 complete/incremental state gap audit (complete) ---+-> NET-00 contract
PRE-03 Jolt math/determinism/rollback spike (deferred; not a NET-00/01 gate)

TIME-00 strong simulation time + authored-duration conversion (complete)
  -> TIME-01 bounded gameplay timeline + render/gameplay split (complete)
       -> TIME-02 fixed-step host/calendar/cooldown migration (complete)
            +-> NET-04 checkpoint + replay
            \-> SERVER-01 headless authority

NET-00 contract (complete)
  -> NET-01 tick journal (complete)
       -> NET-02 sequence + bundle history (complete)
            -> NET-06 in-memory delivery laboratory
                 -> NET-07 replication baselines
                      -> NET-08 selected real-transport adapter
                           -> NET-LAB-01 multi-process loopback/LAN
                                -> NET-LAB-02 compatible cross-build exchange
                                     -> SERVER-01 headless authority
                                          -> TF-NET-01 online authoritative float stand

NET-03 state sections + schema freeze (complete)
  -> ECS transactional replacement
       -> NET-04 checkpoint + replay
            -> NET-05 digest diagnostics
                 -> NET-06

NUM-01 native math corpus
  -> NUM-02 strict float
  -> NUM-03 fixed32
       -> NUM-04 fixed64 only if necessary
            -> TF-NET-02 numeric A/B

NET-09 may implement a Yojimbo adapter only if a concrete GNS limitation later earns a measured comparison.
NET-10 ASIO services are a consumer-driven side branch.
NET-11 hardening starts before public traffic, after the chosen transport/state codecs exist.
```

The numeric branch can run alongside state/session work. It does not block the first server-authoritative
float slice, but the slice must keep a clean numeric seam so strict/fixed backends can later run the identical
recorded scenario. Until that branch earns a stronger profile, the working assumption is deliberately the
worst useful one: native float/double transitions and platform `libm` may diverge, so protocol correctness must
come from detection, authoritative state frames/checkpoints and replay rather than from presumed determinism.

### PRE-01 — GNS capability spike (`S-M`) — complete 2026-09-02

GNS v1.6.0 is pinned and the direct probe covers opaque reliable/unreliable/bulk payloads, internal and real UDP
loopback, fragmentation/reassembly, poll groups, lanes, caller/GNS ownership, Nagle/flush, injected lag,
guaranteed-loss recovery, sustained mixed traffic, IP listen/connect, new-handle reconnect, standalone auth and
the local P2P fast path. It also records what cannot be proven without signaling and certificate infrastructure.
Yojimbo comparison is explicitly deferred and no longer blocks the next tasks. Current executable evidence is
kept in [NETWORKING_STATUS.md](NETWORKING_STATUS.md).

- Build an isolated GNS probe without adding `libs/network` architecture.
- Run the public-API checklist in the transport capability audit above.
- Send opaque canonical byte spans over reliable, unreliable and bulk paths.
- Measure ownership/copies, aggregation, MTU fragmentation, queue limits, lane/channel blocking and diagnostics.
- Verify whether GNS loopback/fault tools are sufficient for adapter tests.
- Identify transport rings/queues that can be reused and explicitly distinguish them from simulation history.
- Record exact dependency/build/threading/serialization costs.

Done with checked-in measured results and a list of only the missing neutral/session primitives. GNS was selected
from executable behavior rather than documentation alone.

### PRE-02 — complete/incremental state gap audit (`S-M`) — complete 2026-09-02

The real `tile_frontier` actor slice now has a self-checking checkpoint audit. It inventories the causal and
derived owners, measures full canonical save/load/hash/compression at 512 and 8192 actors, and reconstructs
byte-identical checkpoints through both stable component sections and 4 KiB pages. Every reconstruction is
accepted by the unchanged live loader and reserializes to the exact expected bytes.

The decision is to make a full, sectioned, canonical checkpoint with transactional replacement the first
standalone representation. A reliable 4 KiB page-manifest delta is earned as an optional encoding over an
explicit retained base: on the large fixture it measured about 210 KiB after one simulation tick and 358–361
KiB after five/twenty ticks, versus a 544 KiB full zstd checkpoint. This currently saves transfer/storage only;
the prototype still serializes and scans the complete state. Component-level dirty flags are too coarse for
sparse changes.

The audit also proved that current failed loads destroy the destination and that the project-global tail lacks
its own framing/schema. The complete state manifest, measurements, format gaps and reproduction commands live
in [NETWORKING_STATUS.md](NETWORKING_STATUS.md). Repetition also exposed an intermittent crash in the existing
Release multithreaded resume smoke; the new audit itself is 20/20, but NET-04 cannot close until that separate
simulation race/lifetime problem is isolated.

- Inventory every causal state owner beyond ECS bytes, including timeline, allocators, workflow cursors and
  physics mapping/state.
- Measure current full snapshot write/read/hash/compression on representative small and large worlds.
- Prototype full checkpoint, stable page-hash reuse and explicit dirty/version approaches without changing the
  live loader contract.
- Decide whether an incremental checkpoint is earned; keep regular state-frame delta replication separate.

Done with a manifest of missing state sections, measured costs and a chosen first checkpoint representation.
The implementation gaps are inputs to NET-03 and NET-04, not unfinished PRE-02 research.

### PRE-03 — Jolt math/determinism/rollback spike (`M`) — deferred

- Inventory the scalar/vector/transcendental operations needed by one causal movement/physics fixture.
- Put Jolt trig/vector math behind a disposable facade and list operations it does not implement independently
  of platform `libm`.
- Compare native, Jolt and selected CORE-MATH functions by raw bits across available build profiles.
- Run Jolt deterministic mode with recorded ordered mutations and per-step `StateRecorder` validation.
- Exercise `SaveState`/`RestoreState`, body creation/removal, stable IDs and project-owned external state.

Done with an evidence-backed decision for Jolt math reuse and with an explicit exact-versus-corrected physics
test matrix; it does not yet commit the engine to Jolt.

### TIME-00 — strong simulation time (`S`) — complete 2026-09-02

- Add distinct `simulation_tick`, `simulation_duration` and `authored_duration` value types.
- Add an integral session `simulation_rate` and overflow-checked microseconds/ticks conversions without `int128`.
- Round causal deadlines upward and derive nominal elapsed microseconds from the absolute tick without per-step
  truncation drift.
- Test zero/sub-tick/exact/non-integral durations, multiple rates, invalid rates and arithmetic overflow.

Done in `utils/simulation_time.h`; these primitives deliberately do not own sleeping, frame pacing, pause,
calendar policy or a global clock singleton.

### TIME-01 — gameplay timeline and presentation separation (`S-M`) — complete 2026-09-02

- Add a bounded `gameplay_timeline<Payload, Key>` containing data rather than callbacks.
- Canonicalize due batches and checkpoint form independently of heap/insertion layout.
- Reject past, duplicate and over-capacity events; validate replacement snapshots before publishing them.
- Move `simul::turn_pipeline` animation barriers onto that timeline and remove the renderer-to-gameplay notify API.
- Split `flow` sampling into microsecond-driven presentation playback and tick-driven gameplay event playback.
- Prove in `cardgame` that headless and animated paths wait for identical causal deadlines; rendering remains an
  optional one-way outbox, while pending animation event IDs/ticks survive snapshot/resume.

Done without a network, ECS, client/server topology, animation backend or project event base class.

### TIME-02 — fixed-step host and project migration (`M`) — complete 2026-09-02

- Make the simulation host accumulate wall time only to decide how many fixed ticks to execute; never pass
  measured frame duration into gameplay.
- Migrate `utils::timelines`, `game_host` and `tile_frontier` from microsecond deltas/local counters to the strong
  tick coordinate while preserving explicit pause/scale commands.
- Derive game calendar/time-of-day from absolute tick plus project policy; serialize the causal origin/remainders
  only where the chosen scale cannot be expressed exactly.
- Convert authored durations at their causal boundary. Existing tile cooldown/flag values remain integer
  game-microseconds by project choice, but advance exclusively by the tick-derived rational projection; gameplay
  animation deadlines use simulation ticks directly.
- Make tick rate an explicit immutable boot/session value. NET-03 and the later handshake must include it in the
  state/session fingerprint once those fingerprints exist.
- Test different render rates, pacing jitter, catch-up, headless execution and snapshot/replay against identical
  tick bundles.

Done in the current host and `tile_frontier` slice:

- `fixed_step_accumulator` converts arbitrarily partitioned wall/presentation elapsed time into bounded catch-up
  steps while retaining unserved debt;
- `game_host` executes `begin frame -> 0..N fixed simulation steps -> end frame`; only non-causal pacing,
  interpolation and profiling see a frame delta;
- `utils::timelines` advances gameplay only for the next explicit session tick and snapshots the exact rational
  remainder needed for continuation;
- `tile_frontier::actor_world_slice` consumes an external monotonic tick plus derived game duration; its former
  local per-frame tick increment is gone, and paused/loading session-tick gaps are explicit;
- tile render snapshots are published once per presentation frame from the final completed simulation state.
- camera movement, render interpolation and performance-sample accumulation stop with active gameplay during a
  local single-player/menu pause; render/UI service work itself continues.

`tile_frontier_time_smoke` feeds the same recorded tick-30 intent through a one-frame 2-second schedule and a
100-frame jittered schedule. In each Debug/Release build both schedules execute 120 ticks at 60 Hz, reach exactly
2,000,000 game microseconds and emit the same 7,472-byte checkpoint. This does not claim cross-build byte identity.
The catch-up cap is eight steps per main frame and retained
debt is drained rather than discarded. Timeline snapshot continuation, speed changes, pause gaps, worker-count
identity and save/load continuation are covered by the adjacent tests. Network bundle ownership/replay belongs to
NET-02/04, and putting tick rate into an actual session handshake belongs to the handshake task rather than TIME-02.

### NET-00 — written contracts and fixtures (`S`) — complete 2026-09-02

`libs/network` now exists as the header-only `devils_engine::network` target. Its README freezes the terms and
non-goals, while the test fixtures provide unrelated intent and transform-state record shapes without any
project, ECS or transport inheritance. The neutral target depends only on common engine options and the
`utils::error` facility; GNS remains outside it.

- Preserve this document as the architectural decision record.
- Define project terminology for tick, principal, player, sequence, intent, bundle, state frame and checkpoint.
- Add two tiny fake project record/state types used by engine tests.
- Decide namespace/target names and dependency graph.

Done when no test fixture includes `aesthetics`, `act`, `tile_frontier` or a socket library.

### NET-01 — bounded tick journal (`S-M`) — complete 2026-09-02

The first primitive is implemented in `tick_journal.h`. `begin` reserves the complete runtime budget before
publishing a tick tag, `try_record` rejects another tick and latches overflow without a partial append, and
`seal` canonicalizes physical arrival order while rejecting duplicate project provenance. `consume` moves the
storage into a const-view owning batch. A monotonically increasing 64-bit generation is part of every tag, so
reusing a journal after the project tick type wraps cannot validate an old tag. Expected seal rejection returns
`tick_seal_result`; phase/tag misuse follows the fatal `utils::error` invariant path.

- Add header-only `tick_journal` with explicit phases and runtime capacity.
- Inject tick extraction, semantic ordering and duplicate equivalence as template policies/callables.
- Add lifecycle/order/overflow/wrap/type-independence tests listed above.
- Add README containing guarantees and explicit non-goals.

Done when shuffled physical insertion produces identical sealed records and every invalid phase/budget case is
tested.

### NET-02 — sequence window and immutable bundle history (`S-M`) — complete 2026-09-04

- Add wrap-safe sequence classification.
- Add bounded tick-indexed immutable history with count/byte budgets.
- Test duplicates, gaps, wrap, eviction and immutable views.

Done without ACK packets, networking or replay driver. The result is two independent header-only templates;
expected refusal is returned as a value and invariant-free classification does not use exceptions.

### NET-03 — project state sections and schema freeze (`M-L`) — complete 2026-09-04

- Add compile-time section schema mechanism with explicit IDs/versions.
- Make `aesthetics` component registry freeze explicit and reject late registration.
- Add project-level complete-state manifest over ECS plus side sections.
- Use the same canonical traversal for write and hash.
- Build staging decode/validation/replacement against fake hosts first.

Done: every truncated/bad load preserves the complete live fake host, schema failures identify their section,
and checkpoint writing/hash input share one canonical traversal. Applying it to the real world owner proceeds in
the already separate transactional ECS follow-up.

### ECS-transactional follow-up (`M`)

- Add a safe way to construct/load a clean `aesthetics::world` and replace an owner.
- Do not swap only `world_` while systems retain pointers/queries to the old object.
- Rebuild derived queries/caches after a complete new simulation instance is valid.
- Add failure injection after each state section.

Done when `tile_frontier` preserves the old running instance after every deliberately corrupt load.

### NET-04 — checkpoint ring and replay (`M`)

- Add generic immutable checkpoint ring.
- Add generic replay free algorithm/Host concept.
- Store explicit empty bundles for ticks with no intents.
- Suppress/deduplicate presentation emission during replay.
- Compare state root after every replayed tick.

Done when every checkpoint `K` in a generated run resumes to the same final fake-state root.

### NET-05 — digest diagnostics (`S-M`)

- Hash canonical full state first.
- Record per-section roots and first mismatch.
- Add page/component hierarchy only if diagnostics require it.
- Benchmark hash cost independently of compression.

Done when an injected one-field corruption identifies tick and owning section.

### NET-06 — in-memory transport laboratory (`M`)

- Add deterministic fault schedule and bounded logical channels.
- Test delay, loss, reorder, duplicate, bandwidth, disconnect and reconnect.
- Drive two independent fake simulation instances through intent bundles/state frames.
- Keep fault schedule and packet trace replayable.

Done when all session behavior is testable without the OS network stack.

### NET-07 — replicated baseline/delta primitives (`M-L`)

- Define neutral baseline IDs and versioned sequenced-frame acceptance.
- Add generic key/value baseline store and project codec seam.
- Test lost baseline, stale delta, duplicate create/delete and full baseline recovery.
- Do not add ECS dirty tracking yet; a project can initially enumerate replicated state.

Done when a fake entity set converges after arbitrary permitted loss/reordering without a full checkpoint.

### NET-08 — selected gameplay transport adapter (`M-L`)

- Add optional dependency/target isolated from neutral core.
- Use PRE-01's selected GNS backend.
- Map reliable ordered, reliable bulk and unreliable sequenced messages.
- Expose connection events, RTT/jitter/loss and bounded send completion ownership.
- Exercise built-in latency/loss simulation and loopback processes.
- Verify checkpoint lane does not stall current intent lane.

Done when the same NET-06 session tests can run over the selected backend with no change to simulation/state
code.

### NET-09 — Yojimbo comparison (`M`, deferred indefinitely)

- Implement only enough adapter to run the same fixture.
- Measure dependency/build footprint, message behavior, serialization overlap and diagnostics.
- Reopen the PRE-01 choice only if a concrete GNS limitation contradicts the spike.

Done with a written GNS/Yojimbo comparison; maintaining both production adapters is not a goal.

### NET-10 — ASIO service seam (`M`, consumer-driven)

- Add standalone ASIO only with the first real HTTP/TCP/control consumer.
- Keep HTTP parsing/TLS policy outside raw socket wrappers or use a proven layer.
- Integrate through bounded broker channels; no callback mutates gameplay state.
- Test shutdown, timeout, partial reads/writes and backpressure.

Done independently of gameplay UDP.

### NET-LAB-01 — real multi-process loopback and LAN (`M`)

- Run the same fake simulation/session fixture as separate authority and follower processes.
- Start with OS loopback, then several followers on one machine, then several machines on a controlled LAN.
- Preserve the recorded application fault schedule where possible and separately record real RTT/jitter/loss.
- Test connect/join, steady tick exchange, lane pressure, stale state, disconnect/reconnect and checkpoint recovery.
- Prove that transport callbacks/workers never mutate a simulation instance directly.

Done when multiple independent processes converge under the same assertions as NET-06 and all failures carry a
replayable logical-message trace.

### NET-LAB-02 — compatible and incompatible build exchange (`M-L`)

- Exchange the same recorded session across Debug/Release and supported compiler/ISA builds.
- Separate wire compatibility from numeric identity: compatible schemas must decode and run; exact profiles must
  additionally match per-tick roots.
- Verify explicit rejection for protocol/schema/content/numeric-profile incompatibility before the first tick.
- Test a deliberately older compatible message version and a deliberately breaking version.
- Compare canonical decoded messages rather than backend packet bytes.

Done when the compatibility matrix reports either successful exchange with the promised root contract or one
precise pre-simulation rejection reason; "different build" never means silently accepting an unknown schema.

### SERVER-01 — headless authority process (`M-L`)

- Split the causal project target from presentation dependencies and link it into client and server executables.
- Start a fixed-tick authority peer without window, Vulkan, audio, UI or GPU resources.
- Add bounded network ownership, checkpoint/save coordination, metrics and clean shutdown.
- Verify identical causal schema/content/profile fingerprints against a graphical follower.
- Run the synthetic NET-LAB fixtures before attaching a real project world.

Done when a clean machine can run the complete authoritative causal simulation without graphical dependencies
and graphical followers can join/reconnect through the same protocol.

### TF-NET-01 — online `tile_frontier` authoritative-float stand (`L-XL`)

- Reuse the already-proven session, state and transport layers; do not introduce a second project-only netcode.
- Client sends project intents with sequence and tick.
- Server owns AI, structural changes and authoritative float transforms.
- Send reliable entity lifecycle/discrete state and sequenced motion frames.
- Predict/replay only the controlled actor; interpolate remote actors.
- Instantiate the declared per-data cadence schedule and measure/adapt transform frequency independently of
  per-tick intents.
- Add correction, rollback, baseline, queue/traffic and root-divergence visualization/metrics.
- Scale unit count, relevant-set size, peer count, bandwidth pressure and rollback depth toward project limits.
- Run creator-as-authority and dedicated-authority topologies; authority migration remains a later slice.

Done when join, normal play, packet loss, stale/missing baselines, disconnect/reconnect, corrections and rare full
checkpoint recovery are structurally verified at measured high unit counts. Full checkpoints are never routine
tick packets.

### NUM-01 — causal math facade and baseline corpus (`M`)

A deliberately tiny compiler probe now precedes the full task: 384 ordinary native-float integration steps
exercise `sin`, `cos`, `atan2`, `hypot`, `exp` and `log1p`, emit canonical little-endian state bits, and compare
GCC with Clang. It detects/reports equality or divergence but treats neither as a portability guarantee. The
full tile actor facade/profile matrix below remains open.

- Extract only tile actor operations needed for position/velocity/integration/spatial queries.
- Preserve GLM conversion at the presentation boundary.
- Add native float/double and the PRE-03-selected Jolt/CORE-MATH function backends to a raw-bit corpus.
- Test Debug/Release, GCC/Clang and PURE/AVX/AVX2 where available.
- Record FP environment and forbid accidental fast-math in the strict target.

Done when the first divergent operation/tick can be reported rather than merely the final world mismatch.

### NUM-02 — strict float backend (`M-L`)

- Separate deterministic compile options target as already anticipated by `libs/options`.
- Fix contraction/FMA, ISA baseline, rounding and denormal policy.
- Replace causal platform `libm` calls with audited implementations such as CORE-MATH where needed.
- Reject/canonicalize invalid float state according to an explicit policy.
- Treat supported compiler/platform matrix as part of the profile.

Done when golden corpus results are measured across the supported matrix. This is a bounded supported-profile
claim, not universal float determinism.

### NUM-03 — fixed32 backend (`M-L`)

- Measure coordinate/speed/intermediate ranges before choosing Q format.
- Implement/audit fixed32 with `int64_t` intermediates and explicit rounding/overflow.
- Add deterministic sqrt/normalization only where tile movement proves a need.
- Avoid runtime conversion from float constants; freeze raw integer constants.
- Run the identical movement and network scenario.

Done when precision/range/cost/correction metrics can be compared directly with native/strict float.

### NUM-04 — fixed64/wide arithmetic decision (`L`, optional)

- Prove that fixed32 range/precision is insufficient first.
- Compare a small portable two-limb backend, MSVC intrinsics and Clang `__int128` behind one audited helper.
- Test signed edge cases, rounding and division on every supported architecture.
- Do not adopt a library that silently changes overflow policy by compiler.

Done with a measured decision; fixed64 is not mandatory.

### TF-NET-02 — numeric A/B matrix (`L`)

- Run identical recorded input/fault schedules over native float, double, strict and fixed backends.
- Compare first divergence, decisions, correction traffic, CPU and visible error.
- Decide which systems become exact and which remain server-authoritative predicted.
- Update the state-domain classification from evidence.

Done when the networking model is chosen per subsystem rather than globally by preference.

### NET-11 — public-network hardening (`L`)

- Complete all decode budgets and overflow-safe parsing.
- Authenticate/encrypt the session through the chosen backend.
- Add rate/ownership validation and structured rejection.
- Fuzz envelopes, intent codecs, state sections and checkpoint loading.
- Add two-process soak tests, sanitizer runs and hostile packet corpus.

Done before accepting traffic outside a controlled local test network.

### POST-NET-AUDIT-01 — engine consistency pass (`M-L`, after the networking campaign)

- Inventory direct standard-library throws and production `try/catch` blocks.
- Route impossible engine/programming states through `utils::error`; replace expected parse/I/O/wire failures with
  explicit result values and transactional mutation.
- Unify the three-coordinate time vocabulary and remove local frame/tick counters which duplicate an owner.
- Audit duplicated ownership, naming and boundary conventions exposed while networking crosses existing systems.

Done when remaining catches are documented narrow adapters around unavoidable throwing dependencies, not ordinary
control flow, and equivalent engine subsystems use the same failure/time/ownership conventions.

## Cross-platform and regression matrix

Long-term deterministic/exact tests should run one recorded canonical bundle corpus on:

- Debug and Release;
- GCC, Clang and MSVC where the profile supports them;
- Linux and Windows;
- x86-64 and AArch64 where available;
- worker counts `0/1/4/N` and randomized scheduling;
- different reserve/capacity/insertion orders;
- full run and checkpoint-at-every-possible-`K` replay;
- two processes with latency/loss/reorder/duplication;
- ASan/UBSan/TSan where applicable.

Compare per-tick roots and section diagnostics, not only final state. Same-compiler tests characterize a build;
they do not prove another compiler/platform.

## Explicit non-goals of the first campaign

- peer-to-peer authority;
- hidden-information replication;
- anti-cheat beyond basic server authority and validation;
- massive-MMO interest management;
- generic ECS delta tracking before a project demonstrates the needed form;
- live schema migration inside a running session;
- a universal deterministic physics engine;
- a universal HTTP stack;
- custom congestion control/reliable UDP over raw ASIO;
- replacing rendering GLM with fixed point;
- sending full checkpoints every few ticks;
- trusting client state hashes.

## Existing repository work related to this plan

The root roadmap already names several compatible directions:

- `ECS-04` compact delta/change tracking;
- `ECS-05` transaction journal;
- `ECS-06` replication codecs and baselines;
- `ECS-08` authoritative/derived/ephemeral classification;
- `SIM-05` checkpoint/save coordinator;
- `SIM-06` server process/session topology;
- `SIM-07` cross-process transport seam;
- `ACT-03` versioned command envelope;
- `ACT-06` stable disk/network command codecs.

This document refines their order: bounded transport/state/math capability research first; only the proven
missing generic tick/history/state/replay primitives next; independent in-memory simulations, real local
transport and cross-build exchange after that; headless and scaled project consumers last. Optimization and
hardening follow measured data, except hostile-input bounds which must precede public traffic.

## External references considered

- GLM 1.0.1 is pinned in the root `CMakeLists.txt`; its local manual and implementation are available under
  the configured FetchContent build tree.
- GameNetworkingSockets: <https://github.com/ValveSoftware/GameNetworkingSockets>
- GameNetworkingSockets public connection/message/poll-group API:
  <https://github.com/ValveSoftware/GameNetworkingSockets/blob/master/include/steam/isteamnetworkingsockets.h>
- Yojimbo: <https://github.com/mas-bandwidth/yojimbo>
- Yojimbo usage/channel/threading guide: <https://github.com/mas-bandwidth/yojimbo/blob/main/USAGE.md>
- standalone ASIO: <https://think-async.com/Asio/>
- MsQuic: <https://github.com/microsoft/msquic>
- CORE-MATH: <https://core-math.gitlabpages.inria.fr/>
- Jolt deterministic simulation and rollback contract:
  <https://github.com/jrouwe/JoltPhysics/blob/master/Docs/Architecture.md#deterministic-simulation>
- Jolt deterministic trigonometry implementation:
  <https://jrouwe.github.io/JoltPhysics/_trigonometry_8h_source.html>
- Berkeley SoftFloat: <https://www.jhauser.us/arithmetic/SoftFloat.html>
- fpm: <https://github.com/MikeLankamp/fpm>
- FixPointCS: <https://github.com/XMunkki/FixPointCS>
- CNL: <https://github.com/johnmcfarlane/cnl>
- libfixmath: <https://github.com/PetteriAimonen/libfixmath>
- MSVC wide multiplication/division intrinsics:
  <https://learn.microsoft.com/en-us/cpp/intrinsics/umul128> and
  <https://learn.microsoft.com/en-us/cpp/intrinsics/udiv128>.
