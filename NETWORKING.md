# Networking, replication and deterministic simulation

[Network library guide](libs/network/README.md) · [frontier_online](subprojects/frontier_online/README.md) · [Playground catalog](subprojects/playgrounds/README.md)

## Simulation and transport

- The project owns the causal schema, content and rules fingerprints, intent semantics and admission policy. The simulation consumes sealed, canonically ordered intents by tick. Local input, a remote participant and replay can all produce the same project records.
- A tick is the coordinate of a committed simulation step. The session defines its tick rate and conversion to active gameplay time. UI polling, presentation animation and network service run on their own schedules. Renderer callbacks do not commit gameplay.
- Authority sends canonical tick bundles and, where a project uses regular replication, state frames with their own cadence, relevance and baseline rules. A complete checkpoint is used for join, resynchronization or replay. Replay from a checkpoint applies the ordered tick history and verifies the root of the canonical causal state before publishing.
- Derived data can be rebuilt after loading. When gameplay reads it, reconstruction and tie-breaking follow the causal determinism contract. Presentation-only data can use a relaxed numeric profile as long as the simulation never reads it as causal input.
- `libs/network` provides bounded, caller-driven protocol primitives. The project composes them with simulation, transport and publication. `network_gns` adapts GameNetworkingSockets. A fresh connection can carry a reconnect handshake, which restores identity and state according to the session policy.
- Protocol refusals, backpressure and exhausted history return explicit results. A valid replacement is checked before publication. Standalone GNS encryption and project identity authentication are separate requirements.

The in-memory stand `NET06`, the real multi-process stand `NETLAB01`, and the `frontier_online` join, live tick and rejoin tests exercise different parts of this contract. Their README files describe what each run covers.
