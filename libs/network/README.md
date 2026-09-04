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
- semantic duplicates are rejected after canonical sorting;
- unsealed storage is never exposed;
- `consume` is once-only and transfers an owning bundle which exposes records
  only through a const view instead of leaving a view into a reusable slot;
- every open cycle has a 64-bit generation tag, so reuse after tick-number
  wrap cannot accept a stale tag;
- capacity/duplicate rejection is an explicit `tick_seal_result`;
- invalid lifecycle operations are programming errors routed through the
  engine's fatal `utils::error` handler rather than exception subtypes.

The comparator must be a strict weak order and semantically equivalent records
must form one adjacent equivalence class under that order. `TickOf` must return
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

The history exposes only const entries and bundle pointers. Those borrowed
views remain valid until that entry is evicted, the history is cleared, or the
history is destroyed. Tick ordering is normal strict ordering; modular packet
sequence handling belongs to `sequence_window`. Neither template is
thread-safe by itself.

Explicit non-goals of this slice: ACK encoding, delivery promises, peer
penalties, sockets, wire serialization, replay execution and checkpointing.

## Implemented slice: canonical state schema

`state_schema<Host, Writer, Reader, Sections...>` is the project-neutral
manifest for a complete causal state. Every project-owned section declares an
explicit 32-bit ID and version plus `write`, `read` and `validate` operations.
The parameter-pack order is erased: the schema sorts sections by ID, rejects
duplicate IDs at compile time and derives a stable schema digest from the
canonical `(id, version)` sequence.

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
future state-hash sink. It emits the complete envelope, section metadata and
payload bytes identically to either consumer. The built-in `state_writer` and
`state_reader` provide minimal canonical little-endian adapters; compatible
project adapters may be substituted.

The schema digest is format compatibility metadata, not a cryptographic state
identity. Strong content hashing remains NET-05; checkpoint retention and
replay remain NET-04. The schema owns no sockets, threads, ECS types, systems
or callbacks.

The target is header-only and depends only on C++23, `devils_engine::options`
and the common `devils_engine::utils` error facility. `devils_engine::network`
contains no GNS type.
