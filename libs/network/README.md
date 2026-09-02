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
- invalid lifecycle operations throw rather than silently resetting state.

The comparator must be a strict weak order and semantically equivalent records
must form one adjacent equivalence class under that order. `TickOf` must return
the exact `Tick` type; implicit narrowing is rejected because it could alias two
different project ticks.

Explicit non-goals of this slice: sockets, threads, ACKs, packet encoding,
authentication, tick acceptance windows, prediction, rollback, checkpointing,
ECS knowledge, compression and encryption.

The target is header-only and depends only on C++23 plus
`devils_engine::options`. `devils_engine::network` contains no GNS type.
