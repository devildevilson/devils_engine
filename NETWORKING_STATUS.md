# Networking implementation status

Last updated: 2026-09-05.

This file is the mutable implementation and verification journal for the networking work. Architectural
decisions, terminology, invariants and the ordered roadmap remain in [NETWORKING.md](NETWORKING.md). A result is
recorded here only after it is reproduced by an executable test or directly observed in the pinned dependency.

## Current position

| Item | Status |
| --- | --- |
| Gameplay transport | GameNetworkingSockets v1.6.0 selected |
| PRE-01 capability spike | complete |
| PRE-02 causal-state/checkpoint audit | complete; transactional replacement follow-up complete |
| PRE-03 Jolt spike | deferred until adding Jolt has a concrete engine consumer |
| NET-00 neutral contract/fixtures | complete |
| NET-01 bounded tick journal | complete; 8/8 cases pass in Debug and Release |
| NET-02 sequence window/history | complete; 9/9 cases pass in Debug and Release |
| NET-03 explicit state sections/schema | complete; 8/8 cases pass in Debug and Release |
| NET-04 checkpoint ring/replay | complete; 6/6 cases pass in Debug and Release |
| NET-05 digest diagnostics | complete; 4/4 cases pass in Debug and Release |
| NET-06 in-memory transport playground | complete; 121/121 checks pass in Debug and Release |
| NET-07 replication baselines/deltas | complete; audited recovery, 7/7 unit cases and revised 58/58 playground checks pass in Debug and Release |
| Pre-NET08 audit / prepared memory | fixed core findings; 11/11 new cases, full focused networking set 56/56 in Debug and Release |
| ECS transactional world replacement | complete; 13/13 focused cases pass in Debug and Release |
| TIME-00 strong simulation time | complete; tick/rate/conversion/pacing primitives pass 5/5 cases |
| TIME-01 gameplay timeline/presentation split | complete; generic timeline, flow, turn pipeline and cardgame proof |
| TIME-02 fixed-step host/project migration | complete; host and tile actor consume an external 60 Hz tick |
| Native-float GCC/Clang micro-corpus | complete baseline; equal in the currently available runtime matrix |
| Complete project suite | last whole-suite run before this follow-up: 402/402; neutral NET-01..06 set passes 36/36 in Debug and Release |
| `devils_engine::network_gns` adapter | NET-08A message/ownership boundary implemented; NET-08 as a whole remains in progress |
| NET-08B listen/connect/accept lifecycle | next; currently the adapter adopts fresh caller-created connections |
| NET-08C shared in-memory/GNS session fixture | pending; socket-pair byte tests do not close this gate |
| Session handshake, reconnect recovery and peer authority | not started |
| Internet P2P/signaling | not tested; infrastructure is not yet present |
| Trusted public-session authentication | not designed; standalone GNS has no configured CA |
| Yojimbo comparison | deferred indefinitely; not an implementation gate |

## NET-08A — GNS opaque-message and ownership boundary

The optional compiled `devils_engine::network_gns` target provides `gns_transport` in
`libs/network/{include/devils_engine/network,src/network}/gns_transport.{h,cpp}`. Neutral `network` headers and
linkage are unchanged. This is a single-owner transport object, not a worker or session: it borrows initialized
GNS interfaces, owns adopted connections/poll group and does not install a process-global callback or call
Init/Kill. The runtime must outlive native receive leases even when they outlive the transport.

### Implemented and checked

- Opaque messages preserve payload bytes and lane IDs. Reliable ordered lanes and lower-priority reliable bulk
  are independent declarations; unreliable-sequenced lanes filter native older/duplicate message numbers.
  The latter is a transport filter, not an application tick/baseline gate; adversarial reorder coverage remains
  part of NET-08C, beyond the current ordinary unreliable byte-transfer check.
- Fixed per-lane send slots and retained-byte limits are shared across this adapter's peers. Saturating the bulk
  reservation does not consume the high-priority lane's slots. Data is copied once from the caller into a slab,
  then passed through GNS custom payload ownership; caller storage can be overwritten immediately after send.
- `m_pfnFreeData` publishes slot release atomically. The owner observes it with `poll_send_releases`; unread
  completions retain their slots and produce backpressure. Completion means **payload memory released**, not
  remote delivery. Failure through `SendMessages(..., false)` leaves the native message with the adapter,
  which releases it and returns the negative native EResult without leaking a slot.
- Release is not FIFO: tests release the second message before the first, reclaim only that slot, reconnect,
  and keep the old first payload readable. Its later release still names the old peer generation, never the new
  connection. A separate test destroys both adapters while a lease remains alive and releases it on another
  thread. Native callbacks retain their slab lifetime, not a dangling transport pointer.
- Receive outputs are move-only native-message leases. The limit counts all outstanding leases, not merely the
  current poll batch. Nonempty output is refused without overwriting a lease; native examination has a work
  budget even when filtering stale unreliable messages. Invalid native lane/size/reliability closes the peer.
- Adopting a new connection reuses preallocated metadata but assigns a process-unique generation. Repeated
  internal-pipe reconnects (100 cycles) reject old IDs and use the **same payload storage address** each time.
- State notifications are polled observations with a caller-provided output span, not a lossless callback log.
  Native connection/lane statistics expose RTT, quality, maximum jitter and lane queue depth/time without
  conflating packet quality with a measured loss percentage.
- Real UDP socket pairs deliver a reliable message after a deliberate 100% loss interval ends. A 400 KiB
  fragmented reliable bulk transfer at 512 KiB/s does not prevent a later high-priority message arriving first.

### Memory findings in the pinned GNS source

1. `CSteamNetworkingMessage::New` still performs `new CSteamNetworkingMessage`, even when `AllocateMessage(0)`
   is requested. Our slab removes the additional payload allocation, **not the native header allocation**.
   The adapter does not fabricate/recycle private GNS objects; native allocator profiling/control is a separate
   work item. The byte-address reuse test proves slab reuse, not zero allocations inside GNS.
2. GNS silently clamps configuration values: send/receive buffers have a 4 KiB minimum, maximum received message
   size has a 64 B minimum, and queued receive count has a minimum of two in this pinned implementation.
   Adoption reads back explicitly set limits and refuses mismatches. Only the derived send-queue cap includes
   an explicit 4 KiB floor; application per-lane byte budgets remain exact. A test requests a 256 B backend receive
   cap, detects the backend's clamp and verifies that the rejected native handle was closed.
3. Memory budgets have distinct owners: prepared send slab bytes, outstanding received leases, and backend queued
   messages. Backend receive limits do not describe all packet/reassembly/crypto working memory. A total process
   allocation/high-water budget has not been claimed or measured here.
4. `thread::byte_ring` requires FIFO reclamation and therefore cannot own out-of-order native send releases.
   Its existing `payload_channel` remains suitable for the later main↔worker hop: the worker copies into an
   independently owned send slot before releasing the FIFO message. No second inter-thread queue was introduced.

### Remaining NET-08 work

- **NET-08B:** listen/connect/accept, bounded callback routing, lifecycle/terminal events, owner-thread rules,
  shutdown and fresh-generation reconnect; then integrate an engine worker through existing bounded channels.
- **NET-08C:** run the shared NET06/NET07 simulation/state scenario through GNS, including reorder, saturation,
  delayed ownership release and recovery. This is still required to close NET-08.
- After NET-08: multi-process loopback/LAN (NET-LAB-01), compatible/incompatible cross-build exchange
  (NET-LAB-02), dedicated headless authority and the online tile_frontier stand. HTTP and Yojimbo remain deferred.

### Verification — 2026-09-05

- Debug and Release focused networking sets: **65/65** registered tests pass in each configuration.
- New adapter target: **9/9** cases, **1644/1644** assertions; all nine cases also pass five consecutive Debug
  repetitions, including both real-UDP scenarios.
- Clang 22.1.8 + libstdc++ with ASan/UBSan/LeakSanitizer: **9/9** pass without reported errors/leaks. Adapter
  and test sources were instrumented; vendor GNS/dependency archives were reused, not rebuilt with sanitizers.
- UDP and LeakSanitizer runs needed execution outside the sandbox (socket creation and ptrace restrictions).
  No Internet peers, authentication service or P2P signaling were exercised.
- Builds used at most `-j4` and only networking targets/dependencies. No Originator target or whole-project
  test suite was run for this change.

```sh
cmake --build build-debug --target network_gns_transport_test -j4
ctest --test-dir build-debug -R '^(network_|NET0[67]_)' --output-on-failure -j4
ctest --test-dir build-debug -R '^network_gns_transport_test::' --repeat until-fail:5 --output-on-failure -j4

cmake --build build-release --target network_gns_transport_test -j4
ctest --test-dir build-release -R '^(network_|NET0[67]_)' --output-on-failure -j4
```

The full focused commands assume the preceding NET-01..07 targets have already been built.

## Pre-NET08 audit follow-up — 2026-09-05

The fixes below precede the real transport adapter. No Originator sources or targets were changed/built by
this networking follow-up; concurrent generator work is outside the verification claim.

### Correctness fixes

- NET07 coalesces multiple missing-base reports into one pending reliable request. Recovery carries a request
  token; only a matching response from the fixture's trusted authority can explicitly reset a distant sequence
  horizon. A token is correlation, not authentication. Duplicated replies and identical baseline IDs under new
  frame sequences are harmless; conflicting content/tick under the current immutable ID is refused without
  publishing state or sequence. The authority now chooses a coherent `(ID, tick, payload)` from its current
  state, not from the request ordinal. Checked gaps: 33, 300, 30000, including uint16 wrap.
- NET06 count/byte budgets now cover the entire retained lifetime: outbound, delayed deliveries and unread
  inbox messages. With count=1, bytes=24 and 1000 send/advance iterations without a consumer, **only one** send
  succeeds; retained=1/24 B even though outbound=0. Consumption frees the reservation. Extra unreliable
  duplicates cannot exceed either budget and suppressed copies are counted. Reliable originals are not dropped
  because a consumer is slow. Trace storage has an independent fixed cap and omitted-event counter.
- Keyed delta apply merges sorted inputs in O(base + delta + output), replacing per-element insert/erase.
  Deleting 1000/2000/4000 entries now performs **zero payload copies and zero payload moves**; the old erase
  algorithm needed 499500/1999000/7998000 move assignments. Keeping N survivors copies each value once into
  prepared output, rather than copying the whole base before patching it.
- Tick journal rejects comparator ties between distinct records with `ambiguous_order`. Stable sorting would
  only preserve arrival-dependent order. The existing policy requirement that equivalent provenance forms
  an adjacent ordering class remains explicit.
- Added shared `utils::float_bits_equal`, applied fieldwise where canonical bytes preserve signed zero/NaN
  payloads. Checked +0/-0 version mismatch, advanced-version reproduction, identical NaN and distinct NaN
  payloads. Ordinary value equality remains a project policy, not silently replaced for arbitrary structs.
- Added composition proof: native-float prediction differs after tick 2, authoritative intent crosses reliable
  loss/retry and delay, then checkpoint 0 restores a staging host and ticks 1..6 replay. Final canonical Murmur
  digest matches authority; the original six presentation events are not repeated.

### Memory paths actually measured

`network_hot_path_test` replaces `operator new` only inside its own executable. Measurement begins after all
fixture preparation; assertions and reference-result construction are outside the measured region.

| Measured path | Work | Allocations after preparation |
| --- | --- | ---: |
| Prepared keyed delta build + apply | 10000 mixed create/update/delete iterations | 0 |
| Link send/retry/duplicate/delivery/consume, trace rotation and reconnect | 1000 two-way cycles | 0 |
| Journal → immutable history → take retired batch → recycle | 10000 ticks | 0 |
| Canonical checkpoint + full/section Murmur diagnostics | 10000 iterations, matching convenience API bytes/roots | 0 |
| Delayed input → restore → replay → final digest | Complete six-tick correction path | 0 |

Mechanisms: fixed history slots instead of deque allocation, explicit `take_oldest` ownership recycling,
`sealed_tick_batch::release_storage`/journal `recycle`, capacity-checked delta `*_into`, bounded
`state_writer`/`Schema::try_write`, and `try_murmur64_digest` over already serialized bytes. The logical link
uses one prepared slot pool per direction with per-lane index lists, prepared delivery/inbox buffers and
in-place sorting with a total ordering key. NET07 consumes inboxes by borrowed callback rather than allocating
an owning drain vector each pump.

Important boundary: these counters cover the neutral algorithms and the tested payload policies, **not every
project payload**. Convenience APIs still allocate. Copying a dynamic Message/Value, dropping its nested
vector/string, or decoding a real ECS staging world can allocate/free. Logical byte budgets do not measure
reserved nested capacity or allocator metadata. NET07's fake snapshots still own vectors. GNS payload ownership,
worker-channel arenas, real ECS decode buffers and large-world memory high-water measurements remain explicit
work at their respective integration boundaries; none is claimed allocation-free here.

`entries()` now returns a const-element random-access view by value, not `const deque&`. Reacquire the view
after a mutation; individual entry addresses remain stable until their own eviction. A test pins rotation,
pointer stability and safe empty moved-from histories.

### Verification and reproduction

Debug and Release: **56/56** focused registered tests pass, including the native-float compiler corpus, NET06
`121/121`, revised NET07 `58/58`, and new hot-path tests `11/11` (`99/99` assertions). The NET07 assertion count
changed because expected protocol refusals now take ordinary branches; compare scenarios, not historical totals.
The whole project suite was not run during this follow-up.

The same 11 hot-path cases also pass with Clang 22.1.8 + libstdc++ under AddressSanitizer,
UndefinedBehaviorSanitizer and LeakSanitizer, with no reported errors/leaks. Leak checking needed a run outside
the ptrace-based sandbox. A libc++ build was attempted but its development headers (`array`, `ciso646`) are
not installed in this environment; libc++ is not claimed verified by this follow-up.

```sh
cmake --build build-debug --target network_hot_path_test network_tick_journal_test network_sequence_history_test network_state_schema_test network_checkpoint_replay_test network_state_digest_test network_replication_test NET06_in_memory_transport NET07_replication_baselines -j4
ctest --test-dir build-debug -R '^(network_|NET0[67]_)' --output-on-failure -j4

cmake --build build-release --target network_hot_path_test network_tick_journal_test network_sequence_history_test network_state_schema_test network_checkpoint_replay_test network_state_digest_test network_replication_test NET06_in_memory_transport NET07_replication_baselines -j4
ctest --test-dir build-release -R '^(network_|NET0[67]_)' --output-on-failure -j4
```

The PRE-01 executable is intentionally a direct GNS consumer. It passes opaque bytes and does not move an
engine transport abstraction, network entity type, serialization scheme or client/server policy into the new
neutral library.

## NET-00/01/02/03/04/05/06 — neutral core through deterministic delivery

`libs/network` is now a header-only `devils_engine::network` target. It depends only on common engine options and
the `utils::error` facility; it does not include or link GNS, `aesthetics`, `act`, `tile_frontier`, a socket API,
Lua or a background-thread implementation. `README.md` fixes the meanings of tick, principal, player,
sequence, intent, bundle, state frame and checkpoint without assigning a client/server topology.

The first template is:

```cpp
tick_journal<Record, Tick, TickOf, SemanticLess, SemanticEquivalent>
```

The project owns `Record` and all semantic policies. The journal owns only a single tick's collection
lifecycle:

```text
idle -> recording -> sealed -> consumed -> idle
                      \-> faulted -------> idle
```

Measured/checked behavior:

- physical arrival permutations seal into the same canonical record bytes;
- another tick is rejected without mutation;
- duplicate `(principal, sequence)` provenance faults the tick;
- exact capacity succeeds, while one extra record latches overflow and returns
  `tick_seal_result::capacity_exceeded`; duplicate provenance returns `tick_seal_result::duplicate`;
- records are inaccessible before seal, recording after seal fails, and consume is once-only;
- `consume` transfers the vector into an owning batch which exposes only a const span;
- a 64-bit generation accompanies the project tick, so stale tags are rejected after the tick type wraps;
- both a small intent and an unrelated float transform-state fixture instantiate the same template without
  inheritance or an engine/project record base.

Expected seal failures are explicit values. Invalid lifecycle phases and stale/foreign tags are programming
invariants and use the common fatal `utils::error` path; NET-01 exposes no exception subtype contract.

The journal is deliberately single-owner. It does not decide whether a tick is late/future, authenticate a
principal, define a wire format, open a socket or perform replay. Those boundaries remain visible for NET-04+
instead of being hidden in a session singleton.

NET-02 adds two independent header-only templates, still without GNS or project types:

```cpp
sequence_window<Sequence, WindowBits>
bounded_history<Tick, Bundle>
```

`sequence_window` classifies an unsigned modular value as new, duplicate, stale or too far ahead. It remembers
late arrivals inside its bitmap, accepts wrap through the unsigned maximum, and refuses the ambiguous half-range
distance. `WindowBits` is also the largest forward gap accepted implicitly: a larger jump must be authenticated
by the future session owner and applied through an explicit reset. A read-only classification does not mutate
the window, and an accepted gap is not represented as a delivery promise.

`bounded_history` stores project-sealed bundles under strictly increasing ticks; gaps are valid. The project
supplies a logical byte cost because the generic library cannot infer serialized size or memory reachable from
an arbitrary bundle. Count and byte limits are runtime budgets. A successful insert evicts the oldest entries
until both fit and reports exact evicted count/bytes; duplicate tick, out-of-order tick and impossible budget are
explicit statuses and leave retained history unchanged. Empty bundles are retained as real ticks and still use
a count slot. Lookups and iteration expose const borrowed data only, valid until eviction, clear or destruction.

The nine NET-02 cases cover gaps and late arrival, duplicate/stale/too-far classification, unsigned wrap, epoch
reset, count eviction, multi-entry byte eviction, transactional refusal, explicit empty bundles and zero count
capacity. NET-01 plus NET-02 passes `17/17` in both Debug and Release. The implementation contains no `throw` or
`try`/`catch`; all expected refusal in this slice is a returned status.

NET-03 adds:

```cpp
state_schema<Host, Writer, Reader, Sections...>
```

Each project section declares an explicit 32-bit ID and version and supplies typed write, staging-read and
staging-validation operations. The schema sorts policies by ID regardless of parameter-pack order, rejects
duplicate IDs at compile time, and derives a stable 32-bit Murmur3 schema fingerprint from the canonical
format/count/ID/version byte sequence through the shared `utils::murmur_hash3_32` primitive. Version zero is
reserved. The fingerprint is compatibility metadata, not the strong state identity planned for NET-05.

The initial compatibility policy is deliberately exact. Missing, unknown, duplicate and reordered sections,
version mismatches, malformed or truncated bodies, section/document trailing bytes and a bad schema fingerprint all
return an explicit `state_load_status`; section failures carry the relevant ID and version information. No
migration or optional-section assumption is hidden in the first format.

All foreign bytes are decoded into a caller-provided `Host::staging_type`. Every section validation and the
project's final cross-section validation finish before the schema invokes one project-supplied `noexcept`
replacement. The exhaustive truncation test cuts a valid document at every byte and proves the whole fake live
host remains unchanged. Decode, section-validation and host-validation refusals are equally transactional.

`emit_canonical` is shared by ordinary checkpoint writing and arbitrary state sinks; a test hash sink receives
exactly the same envelope, metadata and payload bytes as the stored checkpoint. Built-in minimal little-endian
`state_writer`/`state_reader` are available, but the template accepts compatible project adapters. The library
still knows no ECS, GNS, project component, thread, callback or cache type.

The eight NET-03 cases pass in Debug and Release. The adjacent `aesthetics` registry is now explicitly frozen by
the first fingerprint/dump/load: public inspection is const and late component registration is a fatal invariant
violation. This closes the nested ECS schema identity, but not real-world replacement: safely swapping a loaded
`aesthetics::world` together with systems and materialized queries remains the next dedicated ECS task.

The closing hash audit removed the private FNV implementation from NET-03. Schema compatibility now uses the
shared constexpr Murmur3 utility; `utils::murmur_hash3_32` also accepts `span<const byte>` and treats string bytes
as unsigned, including values with the high bit set. The affected utility, schema and aesthetics slices pass
`33/33` in both Debug and Release.

Reproduction:

```sh
cmake --build build-debug -j4 --target network_state_schema_test aesthetics_serialization_test
ctest --test-dir build-debug -R '^(network_state_schema_test|aesthetics_serialization_test)::' --output-on-failure
```

### NET-04 checkpoint ring and replay

NET-04 adds two independent mechanisms in `checkpoint_ring.h` and `replay.h`:

```cpp
checkpoint_ring<Tick, Blob, SizeOf>
replay_to(host, checkpoint_tick, checkpoint, target_tick, bundles,
          restore, apply_bundle, step, verify_state, next_tick)
```

The ring is a checkpoint-semantic wrapper over the already tested bounded immutable history. It stores no world
or serializer type, computes logical byte cost through `SizeOf`, evicts oldest checkpoints deterministically
under count and byte budgets, and can select the latest retained checkpoint not later than a requested tick.

Replay defines checkpoint `K` as committed state after `K` and applies bundle `T` before stepping `T`. It checks
the whole input range before restore, so target bounds, duplicate/out-of-order entries, a missing first retained
tick and an internal gap are values in `replay_status` and leave the host untouched. A tick successor is an
injected policy: mere ordering cannot prove adjacency for an arbitrary project strong tick type. An overflow-safe
successor for integral ticks is supplied as a convenience, not imposed on projects.

`Restore`, `ApplyBundle`, `Step` and `VerifyState` are plain callables. No inheritance or virtual intent source is
introduced. Apply/step receive an explicit presentation-suppressed context. Verify runs immediately after restore
and after every replayed tick, so its first refusal reports the exact divergent tick while the production root
algorithm remains deferred to NET-05. Replay mutates the supplied host after restore; a recoverable correction
therefore uses a detached staging host and publishes it only after successful completion.

The generated fake run contains six bundles, including an explicit empty tick, and seven checkpoints. Restoring
from every possible `K` through the final tick produces the same causal root as uninterrupted execution while
leaving the presentation counter unchanged. Further cases cover count/byte eviction, checkpoint selection,
history bounds, missing/duplicate/out-of-order bundles, successor exhaustion, callback refusal and the first
mismatching root. All `6/6` cases pass in Debug and Release.
The combined NET-01/02/03/04 neutral regression set passes `31/31` in both configurations.

Reproduction:

```sh
cmake --build build-debug -j4 --target network_checkpoint_replay_test
ctest --test-dir build-debug -R '^network_checkpoint_replay_test::' --output-on-failure

cmake --build build-release -j4 --target network_checkpoint_replay_test
ctest --test-dir build-release -R '^network_checkpoint_replay_test::' --output-on-failure
```

### NET-05 state digest diagnostics

NET-05 adds the hash-policy concept, canonical sink and diagnostic report in `state_digest.h`:

```cpp
make_state_digest<Schema, Hasher>(host)
compare_state_digests(expected, actual)
```

The complete root covers exactly the bytes produced by `Schema::write`: the canonical envelope, section metadata
and payloads, without compressed-container representation. During that same schema traversal the report records
one root for every canonical `[id, version, byte_size, payload]` frame. A root disagreement is localized as a
different section set, the first differing section, or an envelope-only mismatch. Derived state absent from the
schema does not affect either root.

The algorithm is a policy. `buffered_murmur64_state_hasher` is the initial frequent diagnostic implementation;
it is deliberately named `buffered` because the shared Murmur64A primitive is one-shot. Callers that have already
materialized checkpoint bytes should hash them directly and request a section report only after disagreement.
`sha256_state_hasher` remains an independent, wider reference/durable-identity option. A CRC64 would be appropriate
for accidental corruption but not selected as state identity; 32 bits are too narrow for a long-running oracle.
None of these unkeyed hashes provides authentication.

The four cases prove direct SHA-256 equivalence with checkpoint bytes, parameter-pack-order independence, derived
state exclusion, section/set/envelope localization, an explicit Murmur64 policy and replay integration. The replay
case injects one actor-field divergence at tick three and reports tick three plus the actor section. Together with
the adjacent schema and replay targets, `19/19` focused cases pass in Debug and Release.

The checkpoint audit now times both hashes independently of compression. One local Release run measured:

| Canonical state | Murmur64 | SHA-256 | zstd-fast |
| ---: | ---: | ---: | ---: |
| 81,221 B | 10.81 us | 268.35 us | 133.44 us |
| 1,291,685 B | 176.65 us | 4,305.80 us | 2,251.43 us |

These are local policy-selection measurements, not portable performance promises. They reject unconditional use
of the current scalar SHA-256 on every frequent state check; they do not justify page trees or incremental hashing
yet.

Reproduction:

```sh
cmake --build build-debug -j4 --target network_state_digest_test network_state_schema_test network_checkpoint_replay_test tile_frontier_checkpoint_audit
ctest --test-dir build-debug -R '^(network_state_digest_test|network_state_schema_test|network_checkpoint_replay_test)::' --output-on-failure
ctest --test-dir build-debug -R '^tile_frontier_checkpoint_audit$' --output-on-failure

cmake --build build-release -j4 --target network_state_digest_test network_state_schema_test network_checkpoint_replay_test tile_frontier_checkpoint_audit
ctest --test-dir build-release -R '^(network_state_digest_test|network_state_schema_test|network_checkpoint_replay_test)::' --output-on-failure
ctest --test-dir build-release -R '^tile_frontier_checkpoint_audit$' --output-on-failure
```

### NET-06 deterministic in-memory transport playground

`network::in_memory_link<Message, SizeOf, FaultPolicy>` is the first executable transport/session boundary. The
message stays project-owned and opaque; only logical size and deterministic fault output are injected. An explicit
transport step deliberately has no built-in relationship to `chrono` or gameplay tick.

The implementation owns independent directional queues, count/byte budgets, bandwidth, per-lane sequences,
scheduled deliveries, inboxes, connection epoch and a replayable trace. Smaller lane IDs receive bandwidth first.
Reliable messages retry loss and preserve exactly-once lane order; unreliable messages expose declared loss,
duplicate and reorder. Disconnect removes all session data and reconnect starts a new epoch. Expected disconnected
or budget refusal is returned as `link_send_status`; only configuration/counter exhaustion uses the fatal error
path.

The `subprojects/playgrounds/NET06_in_memory_transport` fixture sends ten reliable intent bundles and five
unreliable state frames from an authority simulation to an independent follower. The declared schedule drops the
first attempt of intent sequence two, delays frame zero beyond newer frames, duplicates frame two and permanently
drops frame three. All intents still arrive in tick order; the follower converges to the uninterrupted authority at
`30/30`. State-frame acceptance observes three advancing, one duplicate and one obsolete frame. Separate scenarios
prove bulk preemption by a smaller control lane, reliable ordering under unequal delay, exact queue budgets,
reverse-direction delivery and removal of an in-flight old-epoch message across reconnect.

Running the main schedule twice yields identical 65-event traces and final results. The complete executable reports
`121/121` checks in both Debug and Release and is registered as `NET06_in_memory_transport_verify`.
Together with every neutral NET-01..05 test, the networking set through NET06 passes `36/36` in both configurations.

Reproduction:

```sh
cmake --build build-debug -j4 --target NET06_in_memory_transport
ctest --test-dir build-debug -R '^NET06_in_memory_transport_verify$' --output-on-failure

cmake --build build-release -j4 --target NET06_in_memory_transport
ctest --test-dir build-release -R '^NET06_in_memory_transport_verify$' --output-on-failure
```

### NET-07 explicit replication baselines and deltas

`replication.h` deliberately keeps three identities separate: simulation tick, state-frame sequence and baseline
ID. `state_frame_header` also carries an explicit format version and acknowledged input sequence. A full replication
baseline has no base ID; every delta names both the exact base it requires and the result baseline it will produce.
None of these values is inferred from transport delivery order.

`state_frame_window<Sequence, MaxForwardAdvance>` implements latest-state acceptance rather than reliable-command
deduplication: a compatible forward frame may be committed, the current sequence is a duplicate, and every older
frame is stale even if it was never observed. Classification does not mutate the horizon, so decode/materialization
can fail first. An authenticated full recovery can explicitly reset a jump larger than the normal acceptance window.

`baseline_store<BaselineId, Snapshot, SizeOf>` wraps immutable complete snapshots in a baseline-specific API with
strictly increasing IDs, deterministic oldest-first eviction and independent count/logical-wire-byte budgets.
`try_materialize_delta` performs exact lookup, asks the injected project codec for an optional complete candidate,
and only then inserts the result. Missing base, codec rejection, duplicate/out-of-order result and budget overflow
are separate statuses; all leave retained baselines unchanged.

The optional `keyed_snapshot<Key, Value, Version>` codec is one independently testable default, not a required ECS
shape. Its canonical sorted delta expresses create as “key absent -> value”, update as “expected version -> new
version/value”, and erase as “expected version -> absent”. Construction and application reject unsorted/duplicate
keys, value changes without version advance, stale versions, repeated creates/deletes and meaningless operations
transactionally. Entity identity, enumeration, interest, visibility, quantization and wire encoding remain project
policy.

The `NET07_replication_baselines` playground composes this with the NET06 in-memory transport. Authority states
`100..105` include updates, creates and deletes. The scripted channel loses `100 -> 101`, delivers `101 -> 102`
without its base, delays `102 -> 103` behind `103 -> 104`, and duplicates `104 -> 105`. The follower issues two
reliable baseline requests, installs full replication baselines 102 and 104, rejects the delayed frame as stale and
applies one copy of the final delta. It converges at baseline 105 with `60/60` internal checks. These recovery frames
are scoped replication snapshots, never NET04 causal world checkpoints.

Seven primitive test cases plus the playground pass in Debug and Release:

```sh
cmake --build build-debug -j4 --target network_replication_test NET07_replication_baselines
ctest --test-dir build-debug -R '^(network_replication_test::|NET07_replication_baselines_verify)' --output-on-failure

cmake --build build-release -j4 --target network_replication_test NET07_replication_baselines
ctest --test-dir build-release -R '^(network_replication_test::|NET07_replication_baselines_verify)' --output-on-failure
```

### Native-float cross-compiler micro-corpus

The initial worst-case numeric assumption is now executable. A 384-step native transition uses ordinary
float/double expressions plus `sin`, `cos`, `atan2`, `hypot`, `exp` and `log1p`, then writes every state as
canonical little-endian bits. Both binaries are run twice; a same-build mismatch is a test failure, while a
cross-build mismatch reports its first tick/state-byte as evidence that correction is required rather than as
a broken test.

Current result with `-std=c++23 -O3 -mavx -fno-fast-math`:

| Build | Runtime | Output | SHA-256 |
| --- | --- | ---: | --- |
| GCC 16.1.1 | libstdc++ + system libm | 21,505 B hex trace | `77e13886fa5dd6706add0856195b81e18738d9750f75cc7410bc7f69b74e66e6` |
| Clang 22.1.8 | libstdc++ + system libm | 21,505 B hex trace | `77e13886fa5dd6706add0856195b81e18738d9750f75cc7410bc7f69b74e66e6` |

The two outputs are bit-identical for this corpus. This proves only this compiler/ISA/library/flag/input
combination. It does not prove cross-platform determinism, and the protocol continues to assume divergence.
Together with the compiler probe, the relevant networking slice is 9/9 in both Debug and Release.

The installed Arch package `llvm-libs` is LLVM's runtime library package, not libc++: the machine currently
lacks libc++ headers and `libc++.so`. The test first attempts `Clang -stdlib=libc++` and records an explicit
libstdc++ fallback, so installing the separate `libc++` package will automatically extend the comparison to
the intended standard-library runtime.

Reproduction:

```sh
cmake --build build-debug -j4 --target network_tick_journal_test network_sequence_history_test
ctest --test-dir build-debug -R '^network_(sequence_history|tick_journal)_test::' --output-on-failure
ctest --test-dir build-debug -R 'network_(tick_journal|native_float)' --output-on-failure
cat build-debug/cmake/subprojects/tests/network_native_float_probe/result.txt
```

## TIME-00/01/02 — causal tick time, animation separation and fixed-step host

`utils/simulation_time.h` now distinguishes an absolute `simulation_tick`, a relative `simulation_duration` and
an `authored_duration` in integer microseconds. `simulation_rate` performs checked quotient/remainder conversion
without compiler-specific 128-bit integers. Deadline conversion rounds upward; deriving nominal microseconds from
an absolute tick avoids accumulating `16,666 us` truncation sixty times per second.

`simul::gameplay_timeline<Payload, Key>` is a single-owner bounded min-heap with project-owned value payloads. It
has no callback/source hierarchy, thread API, renderer type or network dependency. Events are identified by
`(source, ordinal)`, backward scheduling and capacity overflow are explicit, due events are returned in canonical
`(tick, source, ordinal)` order, and the snapshot stores that canonical order rather than the physical heap.
Corrupt/over-capacity/duplicate snapshot replacement is rejected before live timeline state changes.

The first consumer proves the thread boundary:

- `simul::turn_pipeline` schedules causal animation markers on the gameplay timeline and no longer exposes a
  presentation notification entry point;
- pending event ticks, task IDs and the barrier are causal snapshot state and are cross-validated on load;
- `flow::sample_presentation` advances sprite/UV display from presentation microseconds and emits no gameplay;
- `flow::sample_gameplay` advances an independent playback from tick durations and returns action data;
- `cardgame` schedules the same gameplay/recovery deadlines in headless and animated modes. Animated mode alone
  publishes visual commands; a test now requires both modes to consume the same gameplay ticks.

TIME-02 extends this seam through the live host:

- `fixed_step_accumulator` accepts wall elapsed time in any partition, emits only complete fixed steps, bounds
  catch-up per main frame and retains debt;
- `game_host` pumps platform/services once, executes zero or more fixed simulation steps, then publishes/UI-updates
  once. A measured frame duration no longer reaches an authoritative transition;
- `utils::timelines` now has the three-coordinate model: discrete session tick, unscaled real microseconds and
  pausable/scalable active-gameplay microseconds. Calendar/turn are projections rather than extra clocks. Its
  causal snapshot preserves tick, gameplay time, turn, pause/scale and the fractional projection remainder;
- `tile_frontier` has separate presentation and simulation contexts. Its actor world accepts the host tick instead
  of incrementing a private counter; integer-microsecond cooldowns advance only from the tick-derived game delta;
- `simulation.tick_rate=60` and `max_steps_per_frame=8` are explicit config values. Tick rate is immutable after
  the first tick and is ready to participate in the future session fingerprint/handshake.
- camera movement, camera/actor render interpolation and metric accumulation are active-gameplay-gated: menu pause
  freezes them without stopping UI/platform wall-time work. A multiplayer session must later reject local
  authoritative pause commands.

TIME code also follows the engine error policy: invariant violations call the single `utils::error` fatal path,
whose call operator now works in constant-evaluation contexts; a malformed causal-clock snapshot returns `false`
transactionally. Gameplay-timeline and turn-pipeline snapshot replacement follows the same rule. NET-01 seal
rejection is an explicit `tick_seal_result`, while invalid journal lifecycle/tag use is fatal. No exception
subtype is part of the NET/TIME API.

The headless `tile_frontier_time_smoke` supplies one semantic intent at tick 30 and compares two pacing schedules:
one 2,000,000-us frame versus 100 irregular frames. Within each build both execute 120 ticks, reach exactly
2,000,000 game us and produce the same 7,472-byte full actor checkpoint; cross-build byte identity is not claimed.
Debug and Release both pass 10 timeline cases, 5 simulation
time/pacing cases and the four affected tile executables (`time`, `resume`, `checkpoint_audit`, `config_effect`).
The previously observed Release resume flake did not recur in the author's complete Debug/Release runs or the
post-NET-03 complete runs and is no longer an active blocker; future replay stress can still retain the scenario.

On 2026-09-04 the consolidated error-policy regression set passed `42/42` in both Debug and Release: the cases
above plus 5 gameplay-timeline, 8 tick-journal, 6 turn-pipeline and 4 cardgame consumer executables. Both complete
`tile_frontier` targets also built successfully. Release emitted only pre-existing third-party `opusfile` compiler
warnings during the dependency rebuild.

After NET-03, the complete project suite passes `385/385` in both Debug and Release. These runs include the real
GNS capability tests, tile_frontier snapshot/resume/checkpoint/time tests and all eight new state-schema cases;
the earlier cached-Abseil link failure is resolved. CTest wall time was `157.18 s` in Debug and `25.53 s` in
Release; both builds were limited to `-j4`.

Reproduction:

```sh
cmake --build build-debug -j4 --target timeline_test simulation_time_test tile_frontier_time_smoke tile_frontier_resume_smoke tile_frontier_checkpoint_audit tile_frontier_config_effect_smoke
./build-debug/subprojects/tests/bin/timeline_test
./build-debug/subprojects/tests/bin/simulation_time_test
ctest --test-dir build-debug -R '^tile_frontier_(time_smoke|resume_smoke|checkpoint_audit|config_effect_smoke)$' --output-on-failure
```

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

These controls complement rather than replace the deterministic NET-06 in-memory fault laboratory. GNS fault
injection is transport-level and timing-dependent; replay/session tests use NET-06's recorded logical fault
schedule when they require an exactly repeatable trace.

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
cmake --build build-debug -j4 --target gamenetworking_sockets_capability_test
ctest --test-dir build-debug -R gamenetworking_sockets_capability_test --output-on-failure

cmake --build build-release -j4 --target gamenetworking_sockets_capability_test
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

PRE-02 is complete as an audit, and its transactional replacement follow-up is now implemented. The executable
[checkpoint_audit.cpp](subprojects/tile_frontier/checkpoint_audit.cpp) works over the real
`tile_frontier::core::actor_world_slice`. It parses the current
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
- Murmur64 is corruption detection only, not authentication and not the production page/state digest;
- exact float serialization preserves divergence; it does not create cross-platform numeric determinism.

NET-03 resolves the registry part of this list: the component table is now externally read-only and an explicit
freeze precedes the cached fingerprint.

### Transactional ECS/world replacement — complete 2026-09-04

- `aesthetics::serial::stage_world` decodes a detached candidate without notifying live subscribers;
- the generic loader requires exactly the registered component count, canonical section-ID order and exact
  declared section lengths, then calls `world::replace_state` only on success;
- `replace_state` keeps the `world` object's address, subscribers and owned systems stable, moves only allocator
  and component storage state, and emits `snapshot_loaded_event` after commit;
- `actor_world_slice::load` stages the ECS body, requires strict end-of-project-payload, validates unique player
  identity, recreates transient intent ingress and derives the obstacle cache before touching the live slice;
- a recoverable outer-container, ECS, project-tail or derived-state refusal leaves world bytes, scalars, systems
  and registries unchanged. Exceptions are not used for these outcomes.

The registered checkpoint audit now starts a real running slice, injects outer-container corruption and a
validly sealed truncation immediately after every one of its 19 state regions (the last region receives a
trailing byte), verifies the live canonical state after each refusal, and successfully advances the preserved
instance afterward. This closes the destructive-load finding. The remaining schema concern is not framing but
that `sim_globals` still has no explicit project section ID/version and is absent from the project schema
fingerprint.

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
incremental capture cost. NET-03, NET-04 and transactional ECS replacement now provide section manifests,
staging, checkpoint retention and replay. Page deltas remain behind the same API and stay optional until capture
cadence, retained-baseline memory and dirty-page bookkeeping are measured in the real online stand.

### Reproduction

```sh
cmake --build build-debug -j4 --target tile_frontier_checkpoint_audit
ctest --test-dir build-debug -R '^tile_frontier_checkpoint_audit$' --output-on-failure

cmake --build build-release -j4 --target tile_frontier_checkpoint_audit
ctest --test-dir build-release -R '^tile_frontier_checkpoint_audit$' --output-on-failure
```

### Verification

The original checkpoint audit passed in Debug and Release, including 20 consecutive Release repetitions. The
transactional follow-up's focused set now passes `13/13` in both configurations: eleven aesthetics serializer
cases, the real multithreaded `tile_frontier_resume_smoke`, and `tile_frontier_checkpoint_audit`. The generic
test rejects every truncated byte prefix without changing a live queried world; each real audit fixture rejects
20 targeted inputs (outer corruption plus every state-region boundary) and advances the preserved slice again.
