# `libs/network`: project-neutral synchronization primitives

`network` owns bounded data/lifecycle primitives used between a project and a
transport adapter. It does not know what an intent means, which peer is a
server, how an ECS stores components, or whether bytes eventually travel
through GameNetworkingSockets.

## Terms fixed by NET-00

- **tick** — project simulation step identifier. The library does not choose
  its duration or require that it equals a rendered frame;
- **principal** — authenticated logical author of a record. A player may own
  one or more principals; transport peers and principals are not the same ID;
- **sequence** — monotonically interpreted record identity within a declared
  principal/stream. Its wrap and acceptance window belong to NET-02;
- **intent** — project-owned request/input whose validation and simulation
  meaning remain in the project;
- **bundle** — immutable, canonically ordered records associated with one tick;
- **state frame** — regular authoritative correction data, normally sequenced
  and allowed to supersede an older frame;
- **checkpoint** — complete causal state at a declared tick, suitable for
  validation/replacement followed by replay;
- **message** — opaque bytes with delivery metadata. Only a future transport
  adapter sees sockets or GameNetworkingSockets handles.

Nothing in these terms assigns a permanent client/server topology. A session
may nominate one peer as authority while all transport endpoints remain peers.

## Implemented slice: `tick_journal`

`tick_journal<Record, Tick, TickOf, SemanticLess, SemanticEquivalent>` is a
single-owner bounded collection phase. The project supplies both its record
type and semantic policies. Physical arrival order is erased by `seal()`.

Guarantees:

- a record for a different tick is not written;
- capacity is reserved at `begin`; exceeding it latches a fault and makes
  `seal` fail;
- semantic duplicates and distinguishable records tied by the comparator are rejected after canonical sorting;
- unsealed storage is never exposed;
- `consume` is once-only and transfers an owning bundle which exposes records
  only through a const view instead of leaving a view into a reusable slot;
- every open cycle has a 64-bit generation tag, so reuse after tick-number
  wrap cannot accept a stale tag;
- capacity/duplicate rejection is an explicit `tick_seal_result`;
- invalid lifecycle operations are programming errors routed through the
  engine's fatal `utils::error` handler rather than exception subtypes.

The comparator must be a strict weak order and semantically equivalent records
must form one adjacent equivalence class under that order. Distinct records must
have distinct ordering keys; otherwise `seal` returns `ambiguous_order` (a stable
sort would merely preserve the nondeterministic arrival order). `TickOf` must return
the exact `Tick` type; implicit narrowing is rejected because it could alias two
different project ticks.

Explicit non-goals of this slice: sockets, threads, ACKs, packet encoding,
authentication, tick acceptance windows, prediction, rollback, checkpointing,
ECS knowledge, compression and encryption.

## Implemented slice: sequence window and bounded history

`sequence_window<Sequence, WindowBits>` accepts an unsigned modular sequence
without assigning it any transport meaning. It classifies observations as
`new_value`, `duplicate`, `stale` or `too_far_ahead`. Bit zero is the newest
accepted value; the remaining bits remember accepted late arrivals. Both
unsigned wrap and the ambiguous half-range distance are handled explicitly.

`WindowBits` is deliberately both the duplicate-retention window and the
largest forward gap accepted implicitly. An authenticated session recovery
which proves a larger jump must establish a new epoch with `reset`; a random
packet cannot move the acceptance horizon arbitrarily. Classification alone
does not mutate the window, and gaps do not imply delivery.

`bounded_history<Tick, Bundle>` is a single-owner, strictly increasing tick
history. The project inserts an already sealed bundle and declares its logical
byte size. Count and byte budgets are runtime values; successful insertion
evicts as many oldest entries as necessary and returns the exact evicted count
and byte total. Duplicate ticks, out-of-order insertion and impossible budgets
are ordinary status values and leave retained history unchanged. A zero-byte
bundle is still an explicit tick and consumes one count slot.

The history preallocates fixed ring slots and exposes const entries and bundle
pointers. Entry addresses remain valid until their own eviction/clear/destruction;
the borrowed `entries()` random-access range must be obtained again after any
mutation. It is a view returned by value, no longer a `const deque&`.
Tick ordering is normal strict ordering; modular packet
sequence handling belongs to `sequence_window`. Neither template is
thread-safe by itself.

Explicit non-goals of this slice: ACK encoding, delivery promises, peer
penalties, sockets, wire serialization, replay execution and checkpointing.

## Implemented slice: canonical state schema

`state_schema<Host, Writer, Reader, Sections...>` is the project-neutral
manifest for a complete causal state. Every project-owned section declares an
explicit 32-bit ID and version plus `write`, `read` and `validate` operations.
The parameter-pack order is erased: the schema sorts sections by ID, rejects
duplicate IDs at compile time and derives a stable schema fingerprint from
the canonical `(format, count, id, version)` sequence.

The first compatibility policy is deliberately `exact`. Unknown, missing,
duplicate or reordered sections, version mismatches, malformed section data
and trailing bytes are returned as `state_load_status` values with the
relevant section ID. The schema has no migration or optional-section policy
yet.

Decode writes only into a caller-provided `Host::staging_type`. Section
validation and the project-supplied whole-state validation run before one
project-supplied `noexcept` replacement operation. Thus a foreign-data failure
cannot partially mutate the live host. Rebuilding derived caches belongs in
that final project replacement operation, not in the serialized sections.

`emit_canonical` is the single traversal used by checkpoint writing and a
state-hash sink. It emits the complete envelope, section metadata and payload
bytes identically to either consumer, and may expose each borrowed canonical
section payload to a diagnostic observer during that traversal. The built-in
`state_writer` and `state_reader` provide minimal canonical little-endian
adapters; compatible project adapters may be substituted.

The 32-bit schema fingerprint is produced by the shared
`utils::murmur_hash3_32` primitive over canonical format/count/ID/version
bytes. It is format compatibility metadata, not a cryptographic state
identity. Content-state diagnostics are supplied by NET-05; checkpoint
retention and replay remain NET-04. The schema owns no sockets, threads, ECS
types, systems or callbacks.

The target is header-only and depends only on C++23, `devils_engine::options`
and the common `devils_engine::utils` error facility. `devils_engine::network`
contains no GNS type.

## Implemented slice: checkpoint retention and replay

`checkpoint_ring<Tick, Blob, SizeOf>` retains immutable project-owned
checkpoint blobs under count and logical-byte budgets. It uses the same
strictly increasing tick and deterministic oldest-first eviction contract as
`bounded_history`, and adds selection of the newest checkpoint not later than
a requested tick. Compression and the meaning of logical retained size remain
owner policies.

`replay_to` is a free templated algorithm over a host, checkpoint, forward
bundle range and injected restore/apply/step/verify operations. Tick extraction,
bundle extraction and the successor relation are policies too. This lets strong
project tick types participate without teaching the library arithmetic or a
bundle representation.

A checkpoint at `K` is committed state after `K`; replay applies bundle `T`
before stepping `T` for every tick in `K+1..N`. The entire relevant range is
preflighted before restore, so missing, duplicate, out-of-order and unavailable
history never partially restore a world. Empty ticks must therefore exist as
explicit empty bundles.

Apply and step receive `replay_context` with presentation suppressed.
`VerifyState` runs at the restored checkpoint and after every replayed tick,
allowing the caller to report the first divergent state root without coupling
NET-04 to a hash implementation. Replay advances the supplied host in place;
recoverable callers should replay a detached staging host and publish it only
after successful completion.

## Implemented slice: state digest diagnostics

`make_state_digest<Schema, Hasher>` feeds the exact canonical, uncompressed
state document into an injected hash policy. Its report contains one complete
root plus roots over every canonical `[id, version, byte_size, payload]`
section frame. Section roots are diagnostics: peers need exchange only the
full root normally and request a section report after a mismatch.

`compare_state_digests` distinguishes an identical state, an envelope-only
mismatch, a different section set and the first differing canonical section.
Together with `replay_to` this identifies both the first divergent tick and
the project-owned state section without teaching either mechanism a project
type.

Hash choice is an explicit policy rather than a wire-format property. The
provided `buffered_murmur64_state_hasher` is the initial frequent diagnostic
policy; it is non-cryptographic and buffers because the shared Murmur64A
utility is one-shot. A project that already owns canonical checkpoint bytes
should hash those bytes directly and build section diagnostics only after a
mismatch. `sha256_state_hasher` is the wider reference policy for rare or
durable identities. Neither policy authenticates a peer or message.

There is deliberately no page tree, incremental dirty tracking, socket,
thread, checkpoint retention or correction policy in this slice.

## Implemented slice: deterministic in-memory link

`in_memory_link<Message, SizeOf, FaultPolicy>` is a single-owner logical
transport for session tests. `Message` remains project-owned and opaque; the
library asks only for its logical wire size. Calling `advance()` moves an
explicit transport step, so the library does not equate network time with a
simulation tick or wall-clock duration.

Each direction has independent count, byte and bandwidth budgets. Count/byte
budgets cover outbound + scheduled + unread inbox data until consumption, not
just the send queue. Extra injected duplicates use the same budget; copies which
cannot fit are suppressed and counted by `suppressed_duplicates()`. The original
successful delivery always retains its reserved slot, including reliable traffic.
`queued_*` still measures outbound data; `retained_*` measures the full lifetime.
Lower lane
IDs consume the current step's bandwidth first. A reliable ordered lane
retries injected loss, delivers exactly once and preserves its order; an
unreliable lane may lose, duplicate and reorder messages according to the
injected policy. Expected submission failures are `link_send_status` values.
Disconnect drops all queued, scheduled and received data; reconnect creates a
fresh epoch and restarts per-lane sequences.

The retained trace records acceptance/refusal, byte transmission, injected
loss, retry, scheduling and delivery, up to `trace_count_budget` (default 4096;
zero disables storage). Further events increment `omitted_trace_events()`;
`clear_trace()` reuses storage. A truncated trace is not a complete replay log.
Supplying the same message stream and
fault policy therefore gives a directly comparable trace. This mechanism does
not simulate packets, ACKs, MTU, congestion control or GNS internals; it models
only the application-visible delivery contract that NET-08 must reproduce.

## Implemented slice: replication baselines and deltas

`state_frame_header` carries independent simulation tick, application sequence,
format version, acknowledged input sequence and explicit base/result baseline
IDs. A missing base ID identifies a full replication baseline; it does not turn
that frame into a causal world checkpoint.

`state_frame_window<Sequence, MaxForwardAdvance>` is the latest-state acceptance
gate. It rejects an incompatible format, a duplicate, every older frame and an
untrusted forward jump outside the configured modular window. Classification is
non-mutating; the owner commits only after decoding and state materialization
succeed. An authenticated recovery may establish a distant sequence explicitly
with `reset`.

`baseline_store<BaselineId, Snapshot, SizeOf>` retains immutable complete
replication snapshots under strictly increasing IDs and count/logical-byte
budgets. `try_materialize_delta` looks up exactly the named base, invokes a
project codec returning `optional<Snapshot>`, then publishes the complete
candidate under its result ID. A missing base, codec refusal, duplicate or
out-of-order result and budget overflow are distinct values and never mutate the
store.

The optional default codec represents canonical key-sorted state as
`keyed_snapshot<Key, Value, Version>`. Its delta records an expected version and
an optional result per key, making create, update and erase preconditions
explicit. Build/apply reject duplicate or unsorted keys, a changed value without
a changed version, repeated create/delete and stale versions transactionally.
Projects remain free to use another snapshot and delta representation.

Entity IDs, ECS enumeration/dirty tracking, component declarations, interest,
ownership, visibility, quantization, wire serialization and correction remain
project/session policy rather than properties of these templates.

## Prepared storage and hot-path ownership

Preparation is explicit; a runtime budget is not a claim that a generic payload
fits in `sizeof(T)`. These are separate resource limits:

| Path | Prepared API | Ownership / refusal |
| --- | --- | --- |
| Tick collection | `recycle(vector&&)`, then `begin` within prepared capacity | `consume` transfers immutable ownership; only after retirement may the owner call `std::move(batch).release_storage()` |
| Histories/checkpoints/baselines | Fixed slots allocated by constructor; move prepared payload in | `take_oldest()` returns ownership for reuse; automatic eviction destroys it |
| Delta build/apply | Reserve output, use `make_keyed_delta_into` / `apply_keyed_delta_into` | Linear merge; capacity/precondition refusal leaves output unchanged; input/output may not alias |
| Canonical serialization | Reserve document and largest-section scratch, `Schema::try_write` | No vector growth; `false` means partial scratch, never publish it; buffers must be distinct |
| Murmur diagnostics | `try_murmur64_digest<Schema>(bytes, report)` with reserved sections | Hashes the existing canonical document/section slices directly, no second full byte buffer; rejects bad framing without changing report |
| Logical delivery | Constructor prepares shared per-direction queue slots, delivery/inbox vectors and trace; `consume` borrows inbox messages | Capacity is returned only after callback; callback may enqueue a reply, but must not recursively consume/advance/disconnect |

The link allocates queue slots once and links them by indices per lane: there
are no 512 independently allocating deques. Delivery sorting uses a total
`(ready_step, insertion_order)` order and in-place `sort`, not allocating
`stable_sort`. `drain()` remains an allocating owning convenience API; use
`consume()` for a prepared hot loop.

The owning convenience `make_keyed_delta`, `apply_keyed_delta`, `Schema::write`
and generic `make_state_digest` may allocate. So can project `Message`/`Value`
copy/assignment, serialization callbacks and dynamic staging. Prepared `*_into`
overloads retain outer vector capacity and assign existing elements, but erasing
a nested owning value destroys its allocation. Truly bounded dynamic payloads
need a project/adapter-owned arena or recycled ownership handles; a borrowed
`span` must not outlive that ownership. Logical wire-byte budgets do not measure
allocator overhead or reserved capacity of nested containers.

For float values, the equality policy must agree with canonical bytes.
`utils::float_bits_equal` (`utils/float_bits.h`) compares IEEE float/double
fields, distinguishing signed zero and NaN payloads. Apply it fieldwise; never
compare raw padded structs. A project may instead normalize values before
versioning, serialization and hashing, consistently in all three places.

`network_hot_path_test` counts allocations after preparation for these paths,
including checkpoint restore/replay/digest over the faulty logical link. The
test's allocation hooks belong only to that executable, not to the engine.
This does not claim allocation freedom inside GNS or the real ECS serializer.

## Optional GNS adapter — NET-08A/B

Link `devils_engine::network_gns` and include `network/gns_transport.h` explicitly.
The neutral `network` target and umbrella header remain free of GNS headers and
linkage. `DEVILS_ENGINE_BUILD_NETWORK_GNS=OFF` disables this target; it does not
undo the repository's existing top-level GNS fetch or the independent PRE-01 probe.

`gns_transport` is a single-owner concrete backend, not a session or a worker.
The caller lends initialized `ISteamNetworkingSockets`/`ISteamNetworkingUtils`;
their runtime must outlive the transport **and all outstanding received leases**.
`adopt` takes exclusive ownership of a fresh connection even on refusal (it
closes rejected handles); duplicate adoption into the same object is a refusal
without close. Generational `gns_peer` values reject stale and foreign-instance
references. They are local transport handles, not player or authority IDs.

The lane declaration supplies delivery, priority/weight, Nagle policy, number of
send slots, maximum payload size and retained-byte budget. A bulk channel is simply another
reliable lane with its own reservation and priority. Opaque messages acquire no
application framing, tick, component or snapshot knowledge. Unreliable-sequenced
delivery discards native message numbers older than the newest observed number
on that connection/lane. It does not replace application state-frame validation.

Memory and lifetime:

- Send slabs allocate `sum(lane.send_slots * lane.max_payload_bytes)` bytes at
  preparation, plus fixed metadata. Budgets are per lane across this adapter's
  peers. `try_send` copies the caller's bytes into one free slab slot; GNS receives
  a native `AllocateMessage(0)` header with a custom payload-release callback.
- GNS can free payloads out of order and from a different thread. A FIFO
  `thread::byte_ring` therefore cannot directly own this boundary. Each slot
  publishes release atomically; `poll_send_releases` copies completion metadata
  into caller storage and makes slots reusable. Completion is **memory release,
  not delivery ACK**; disconnect can release an undelivered message.
- Native callbacks retain their slab's shared lifetime, not a transport pointer.
  Adapter destruction does not invalidate a message still retained by GNS.
  Rejected native sends are released immediately and emit no accepted-send completion.
- Receive returns move-only `gns_received_message` leases without copying the
  payload. A shared atomic count bounds outstanding leases across repeated polls,
  including leases moved to another thread. The byte upper bound of leased
  payloads is `receive_leases * max_receive_bytes`. `reset` returns ownership to
  GNS; spans into that message expire immediately.
- Backend queued-message byte/count/max-message limits are separate. GNS may
  clamp settings, so adoption reads them back and refuses non-exact limits.
  Its send queue is derived from summed lane budgets with the pinned backend's
  4 KiB floor; the smaller application slab budgets still hold exactly.
- These limits do not account for all of GNS's packet/reassembly/crypto working
  memory. In the pinned source, `AllocateMessage(0)` still calls
  `new CSteamNetworkingMessage`: the payload allocation is removed, the native
  header allocation is **not**. No wrapper vector grows during send/receive/poll.
  Profiling/controlling allocations inside GNS is a separate integration step;
  do not fabricate/recycle its private message implementation from the adapter.

`receive` requires empty output elements and has an explicit work budget,
including discarded stale frames. A non-OK result may still carry earlier
successful messages in `count`; process/release those leases. `poll_connections`
reports observed state changes without an internal growing event log: intermediate
states may coalesce between calls. `statistics` preserves native RTT, quality,
jitter (including unavailable values) and per-lane queue data without inventing
a packet-loss metric from quality or using global queue time for multiple lanes.

### Endpoint lifecycle

For real IP endpoints, prepare one `gns_dispatcher(sockets, utils, transport_capacity)`
per native interface and construct transports with that dispatcher. The original
interface-taking constructor remains an adopt-only boundary. A full dispatcher
registration table leaves a new transport `!ready()`; it never grows on demand.
The dispatcher must outlive registered transports; explicit `shutdown()` detaches
a transport early and releases its registration slot.

- `listen(address, options)` and `connect(address, options)` install a static
  connection callback atomically with native endpoint creation. At most 64 native
  creation options are accepted; replacing this routing callback is refused.
  Authentication remains caller policy; no unauthenticated-IP option is inserted.
  The tests explicitly allow unauthenticated localhost peers, not trusted identities.
- `dispatcher.pump()` is the **only** callback pump for that interface, on the same
  owner thread as its transports. It dispatches to currently registered owners
  using native connection/listener handles, not callback snapshots of user data.
  No captured transport pointer survives in GNS. Late events for closed endpoints
  are ignored; destruction and registration-slot reuse do not redirect them to a
  replacement. Do not mix raw `RunCallbacks` or a second dispatcher for this interface.
- An incoming connection reserves a peer slot and gets its lane/receive budgets
  configured before admission. `poll_connections` reports `needs_accept`; the
  caller must promptly choose `accept(peer)` or `close(peer)`. No automatic project
  admission, authority selection or second session state machine is hidden here.
  Native accept may refuse because the connection ended meanwhile: return status,
  then observe/close the terminal peer. Capacity/configuration refusal closes the
  native connection immediately and increments `refused_incoming_count()`.
- Routing tables and listener/peer metadata are prepared once. There is no growing
  wrapper event queue: observations coalesce by peer, an empty/full caller output
  does not consume a state, and terminal peers stay owned until explicit close.
  This is **not** a lossless log of intermediate transitions. GNS `RunCallbacks`
  drains its native callback queue without an exposed work budget; bounded wrapper
  memory does not mean bounded native callback work or allocation freedom in GNS.
- `close_listener` invalidates its generational ID **and all its child peer IDs**:
  native GNS closes accepted children too. Local close/shutdown do not emit a second
  event to acknowledge the caller's own operation. `shutdown` is final, idempotent,
  unregisters routing and closes listeners/connections/poll group without linger.
  Outstanding receive leases and send release storage retain their existing lifetime
  contract; completions can still be reclaimed after shutdown.
- Reconnect is a new `connect`, not implicit retry: fresh peer generation, reset
  lane sequence filters, reused prepared slots. This is transport reconnection,
  **not** resuming a session or replaying its checkpoints. GNS v1.6.0 requires a
  nonzero listen port; native shutdown may defer OS-port release, so a subsequent
  bind can return `backend_rejected`. Tests try a bounded localhost port range.

NET-08C will run the same full session fixture over both backends. Engine worker
integration should use existing bounded FIFO channels where appropriate; this
slice deliberately remains caller-driven. No HTTP, P2P signaling, trusted-session
authentication, automatic reconnect or gameplay-thread mutation is added.
