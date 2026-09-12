# Networking implementation status

Last updated: 2026-09-12.

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
| SESSION-02 handshake wire format and ordered exchange | complete; now 13/13 cases, 381/381 assertions in Debug and Release, including retained v1, pinned exchange versions and breaking v3 |
| HOT-01 hot-path intent class and fixed point | complete; 10/10 cases, 325/325 assertions in Debug, Release and Clang |
| HOT-02 transform frames and relevant set | complete; 10/10 cases, 598/598 assertions in GCC Debug and Release, and carried by NET-LAB-01 over real UDP between separate processes |
| SESSION-03 reconnect credential | complete; 7/7 cases, 144/144 assertions in Debug, Release and Clang |
| SESSION-04 automatic reconnect policy and recovery feasibility | complete; 7/7 cases, 173/173 assertions in Debug, Release and Clang |
| Vendored protobuf for GNS | fixed; Linux and Windows now take protobuf from the same place |
| Complete project suite | **578/578** in GCC Debug on 2026-09-09 |
| Focused networking set | 138/138 by `ctest -R "network|NET0|NETLAB"` in GCC Debug and Release on 2026-09-09, including real localhost UDP and the multi-process stand |
| Second toolchain (Clang + libc++) | networking/serialization set passes; all four portability defects now closed, the `devils_script` one upstream in v1.3.1 |
| `devils_engine::network_gns` adapter | NET-08A/B/C complete; its closing focused set was 75/75 in Debug and Release |
| NET-08B listen/connect/accept lifecycle | complete; explicit admission, bounded routing/observations, shutdown and fresh-generation reconnect |
| NET-08C shared in-memory/GNS session fixture | complete; 4/4 cases pass in Debug and Release, five repeated Debug runs pass |
| SESSION-01 strict compatibility/identity/recovery primitives | neutral slice complete; 6/6 cases, 76/76 assertions pass in Debug and Release |
| Session wire handshake and challenge/response | complete as a neutral slice; see SESSION-02 below |
| HOT-02 over the stand | complete; 2 981 samples verified against the receiver's own computation, 0 corrections, both halves of the two-lane generation race staged and refused |
| Automatic transport reconnect and multi-process exchange | NET-LAB-01 slices 1-3 complete: authority + 3 followers + intruder as separate processes, all roots equal; artifact relocatable (4 shared deps, glibc 2.38 floor) and addressed by `--listen`/`--connect`; a second machine is the remaining gap |
| Compatible/incompatible build exchange | NET-LAB-02 complete locally: current v2 and compatible v1 run to one root; six compatibility-field mismatches and breaking v3 produce exact pre-tick refusals; 125/125 harness checks in GCC Debug and Release |
| SERVER-01 headless authority process | complete; causal/presentation split builds and ships as a separate executable, 593/593 project suite |
| TF-NET-01 join (authority listens, client joins) | first slice complete; real GNS handshake, world declaration, chunked checkpoint and a cross-process causal root match, plus two exact refusals; `frontier_join_smoke` in ctest |
| Dedicated-server health/readiness probes | SERVER-02 planned; separate from gameplay GNS/peer capacity |
| Internet P2P/signaling | not tested; infrastructure is not yet present |
| Trusted public-session authentication | not designed; standalone GNS has no configured CA |
| Yojimbo comparison | deferred indefinitely; not an implementation gate |

## TF-NET-01 slice 1 — join in the real project, 2026-09-12

The first networking slice inside `frontier_online` rather than a synthetic stand. Session, handshake and
transport come from `libs/network` unchanged; the project adds exactly four message classes (world declaration,
checkpoint begin, checkpoint chunk, join report).

### Result

An authority listening on a declared port serves a joining process: handshake, world declaration, an 81 KB
canonical checkpoint over the bulk lane, and the joiner's own recomputed causal root reported back. The
authority compares and reports:

```
authority: peer 2 join ok (loaded), tick 221 root 13552259054345674819 vs local 13552259054345674819
```

This is the first cross-process causal root match in a real project world, not a stand fixture.

### Land does not travel; its identity does

The land decision holds: a chunk is a pure function of (world seed, chunk key), so only the world parameters
travel. The joiner computes the world itself and verifies two independent things — the generator fingerprint,
which is hashed from the config TEXTS and catches a different config, and the root of probe chunk (0,0), which
catches the same config computed by a different `originator` build.

`content_root` is SHA-256 over the FILES of the causal subtree (predicates, FSM, GOAP, prefabs, generator).
Equal build versions prove nothing: editing a threshold in `values.tavl` changes the world without touching a
line of C++. The directory list is explicit, because client and authority hold different resource trees and a
whole-tree hash would refuse a join over a font; the builder therefore REFUSES a declared directory that is
missing or empty, so a silently empty manifest cannot become a root two different worlds share.

### Two defects, one cause: sending is not delivery

`try_send` only copies bytes into a prepared slot, and `gns_transport::close` closes WITHOUT linger. Both
mistakes followed:

- The joiner sent its report and exited. The authority recorded "peer dropped before reporting" — the join
  looked failed although the state had arrived intact.
- The authority sent a refusal and closed immediately. The reason was discarded, so "incompatible build" looked
  exactly like "silently does not work" — the one outcome the handshake exists to prevent.

Both were invisible as themselves: each appeared to be somebody else's fault. The fix is symmetric — the joiner
waits for the authority's close as its acknowledgement, and a refused peer's close is deferred 500 ms, not
forever.

### Verification

`frontier_join_smoke` runs two endpoints over real loopback UDP in one process and one thread, because GNS has
one dispatcher per interface with a single owner thread. Three cases: a completed join with matching roots, and
two DIFFERENT compatibility fields each refused with its own reason. Project suite 593/593 in GCC Release.

## NET-LAB-02 — one success contract and seven exact refusals, 2026-09-09

The compatible and incompatible halves now use the SAME real multi-process UDP stand. The successful matrix
does not compare GameNetworkingSockets packets — encryption, acknowledgements and timing make those transport
artifacts intentionally different. It compares decoded protocol effects: every process reaches the same
canonical state root at the same tick.

### Wire compatibility is a bounded range, not `version <= mine`

`session_wire.h` now declares envelope version 2 and an explicit oldest compatible version 1. Both versions
carry the same frozen canonical payload grammar. A client selects one in its hello; both roles retain that
version through challenge, response and acceptance, and changing version halfway through an exchange is
malformed. Supporting version 1 is therefore a named retained decoder, not a promise that every historical
number is readable forever.

The three-follower continuous scenario was then run with every handshake through version 1. The established
session reached tick 70 and root `8518737655127057956`, identical to version 2, including intents, canonical
bundles, transform frames and two reconnects. Unit coverage separately decodes the old and current hello to the
same logical message.

Version 3 is deliberately unsupported. The authority reads only enough envelope to identify that fact, returns
the new stable reason `unsupported_wire_version`, and does not parse compatibility, issue a challenge or inspect
a credential. The refusal itself uses the current known grammar: emitting a response in an unknown grammar
would be guessing what the other implementation owns.

### Compatibility fails before identity and before time exists

The process harness launches one authority and one follower for each independently perturbed field:

| Follower differs in | Exact refusal |
| --- | --- |
| handshake format | `handshake_format_mismatch` |
| protocol version | `protocol_version_mismatch` |
| content root | `content_mismatch` |
| state schema fingerprint | `state_schema_mismatch` |
| intent schema fingerprint | `intent_schema_mismatch` |
| numeric profile | `numeric_profile_mismatch` |
| envelope version 3 | `unsupported_wire_version` |

Each side asserts tick zero and the follower asserts zero applied bundles. The authority additionally counts the
two policy boundaries and asserts **zero challenges issued and zero credential admission checks**. That is the
meaning of pre-simulation refusal here: it is not inferred from a short run and it does not trust a later state
root to imply that identity work was skipped.

Verification: `network_session_wire_test` is **13/13 cases, 381/381 assertions** in GCC Debug and Release. The
complete `NETLAB01_multi_process --verify --quiet` matrix is **125/125 harness checks** in both configurations;
the per-message in-process count varies with scheduling as designed. The focused networking set is **138/138**
in GCC Debug and Release. Existing cross-build compatible evidence remains GCC 14/16, Debug/Release, baseline/AVX and two
Linux machines over 5G; Linux↔Windows/MSVC remains additional matrix evidence.

## HOT-02 carried by NET-LAB-01 — observed against modelled, 2026-09-09

The stand now runs the downstream class beside everything else it already carried: a per-session relevant set
on the reliable ordered lane, transform frames on the unreliable one, three declared cadence classes, and a
periodic full pass. The point of doing it here rather than in a test is that the arithmetic budget could not
speak for a real link, and that a primitive nobody has driven is a primitive whose footguns are still loaded.

### The payload is verifiable, which is what makes this more than byte counting

The stand's causal state is one scalar, so the replicated table is DERIVED: an entity's transform is a pure
function of its index and the tick, computed from integers by correctly-rounded double operations with no
`<cmath>` call anywhere. A follower therefore recomputes what a frame should contain and compares in CODE
space through `evaluate_correction`. What is deliberately not derivable is MEMBERSHIP — which entities are
relevant follows the causal position, and a follower cannot compute it for a tick it has not reached — so the
reliable lane carries the only part that actually informs.

Result over a 600-tick Release run at 20 ms pacing, three followers, loopback: **2 981 samples verified, 0
corrections needed**, and the same state root as the previous run of the same schedule. Zero corrections means
the quantizer produced identical codes in two processes; the reproduced root means no order was lost, which is
the free integrity check this campaign already relies on.

### Observed bytes against the modelled ladder

Mean relevant set 16.5 of a declared capacity of 32, 64 entities in the table, 50 Hz, three axes plus turn:

| Quantity | Modelled | Observed |
| --- | ---: | ---: |
| transform payload per follower | 3 577 B/s | **3 514 B/s** |
| membership payload per follower | ~250 B/s | **237 B/s** |
| membership share of the two | 6.5% | **6.3%** |
| records per dense run | 16.5 (one run) | **14.4 (1.14 runs per pass)** |

**The arithmetic was within 2%.** That is the useful result: the ladder in NETWORKING.md can be trusted to
size a budget, because the thing it predicts is the payload and the payload is what the codec controls. What
it still cannot predict is the per-packet overhead, which belongs to the transport and which the link's own
statistics report separately — the report keeps the two apart for exactly that reason.

**Density survived churn.** 697 enters and 628 leaves over the run, and a full pass still took 1.14 dense runs
on average. Reusing the lowest free slot keeps the set packed, so the dense mode's saving — the slot index it
can omit — is not eroded by relevance turnover. That was an assumption in the header until this run.

### The two-lane race, staged because loopback will not produce it

On loopback the reliable lane is the higher-priority one and nothing is lost, so a frame never overtakes the
membership that gives its slots meaning: the natural rate was **zero in both directions**. A refusal branch
nothing reaches is not a verified refusal, so the schedule now stages both halves. One membership publication
is DELAYED by a tick (held, not dropped — dropping a reliable ordered message would break the generation
chain, which is a different fault), and the frames of that tick arrive first: `generation_ahead`. One frame is
built against the previous generation and sent after the current membership has landed: `generation_behind`.
Both are refused and counted, and the harness asserts each is non-zero for the follower the schedule names.

Staging the delay taught its own lesson: it fires only where a publication and a frame send fall on the same
tick, and only if the set actually changed. Membership publishes every four ticks and the near class every
three, so the race can only be staged where those coincide — and in a short run the set may not have changed
at all, so the schedule now forces one eviction at that tick. **A staged race needs both a message and a
listener; naming a tick was not enough.**

### Three defects the stand found in HOT-02 itself

- **The frame gate's forward window could freeze a class permanently.** Copied in reasoning from
  `state_frame_window`, where a window is necessary because sequence low bits wrap and a distant sequence is
  ambiguous. A transform frame carries an ABSOLUTE tick, widened against what the receiver knows, so a jump
  forward is unambiguous — and refusing it meant a follower which fell behind found every later frame "too far
  ahead" and never accepted another. The stand froze one class for **91 consecutive frames** after a rejoin.
  The window and the `too_far_ahead` outcome are gone; whether a tick far ahead of the RECEIVER'S OWN progress
  is plausible at all is the caller's question, because only the caller knows its own tick.
- **The format could not express a full set, so a restarted process could never be told anything.** A rejoining
  process comes back with an empty mirror at generation zero, and a diff is only meaningful against exactly
  the previous generation — `relevant_set_mirror::adopt` existed for that path but nothing on the wire could
  say "this is the whole set". Byte zero now says which of the two a membership message is
  (`relevant_set_full`), and an empty full set is a legitimate statement — "you are relevant to nothing" —
  where an empty diff is not.
- **The stand's own message classes collided with the engine's.** `lab_message` had taken 64..67 from the
  authority range before HOT-02 claimed 64..66 for membership and frames. The type byte space is shared by
  every class on a connection, so the stand's classes moved to 96..99. Silent until both were on one lane at
  once, and an argument for byte zero deciding everything.

### And one the stand found in its own assumption

The receiver's sanity bound on a frame's tick was written against the follower's OWN tick. A rejoining
follower legitimately receives frames for ticks far beyond anything it has applied, because its checkpoint has
not landed yet — so the first thing that check caught was my assumption rather than any corrupt frame. The
basis is now the furthest tick the AUTHORITY is known to have reached, which a bundle or a recovery plan
establishes, and an implausible frame is dropped and counted rather than fatal: the connection is
authenticated, so a frame cannot have been tampered with in flight, while accepting a nonsense future tick
would poison the latest-value gate and every frame after it.

### What the format still lacks, measured rather than guessed

A declared maximum staleness is a per-class bound, and a receiver evaluating it needs to know **which of its
relevant entities belong to which class** — otherwise it cannot tell "the class went silent" from "the class
has nothing for me". With a relevance radius that left fewer than `lab_near_slots` entities relevant, the far
class had nothing to send and its age bound was exceeded on almost every tick; the counter was right and the
stand's tuning was wrong. Membership carries a slot and a handle and no class, so today only the sender can
answer that question. Naming the cadence class per slot in the membership message is the obvious fix and is
left for a later slice rather than added on the way past.

Verification: the full harness is **41/41 harness checks and 8 562 in-process checks across 11 processes**,
both scenarios, all roots equal at the announced final tick. Focused networking set **138/138** in GCC Debug
and Release.

## HOT-02 — transform frames and the relevant set, 2026-09-09

`network/transform_wire.h` is the downstream counterpart of HOT-01: what the authority tells ONE client about
the entities that client is allowed to know about, and how often. Three decisions are kept apart on purpose —
who is replicated (the relevant set), what travels (a declared per-class shape), and how often it travels (a
cadence policy the sender may adapt) — because only the middle one is session identity.

### The measured ladder is the result, and it inverts the usual effort

Profile: 60 Hz simulation, 2048 entities in the loaded neighbourhood, 96 relevant to one client, three axes on
the 1/1024 lattice plus an independent turn. Every figure is the encoder's own arithmetic at that profile,
produced by the test rather than written on paper.

| Rung | What it adds | bytes/s per client | gain |
| --- | --- | ---: | ---: |
| 0 | every entity, every tick, by 64-bit handle, absolute keys | 3 440 640 | — |
| 1 | relevance: 96 of 2048 | 161 280 | 21.3x |
| 2 | dense slot and one frame origin: 11 bytes a record, not 28 | 65 760 | 2.45x |
| 3 | declared cadence: own at 20 Hz, remote at 10 Hz | 11 470 | 5.73x |
| 4 | change-only sparse frames, a 1 Hz full pass, membership | 6 927 | 1.66x |

Four clients at the last rung are 27 KiB/s, about 222 kbit/s. **Relevance and cadence are worth 21x and 5.7x;
all the encoding cleverness together is worth about 4x.** A record 17 bytes narrower cannot rescue a relevance
function that admits too much, and this is the number to reach for when a future frame format is proposed.

Two further measurements came out of the same test:

- **Membership costs 91 of the 6 927 bytes, 1.3%.** Making the set its own reliable ordered class is therefore
  almost free, which is what allows the frames themselves to stay unreliable.
- **Sparse beats dense until the change set reaches 81 of 96 slots, 84%.** A sparse record costs 13 bytes
  against a dense record's 11, but it sends only what changed. So the dense mode earns its place only on the
  periodic complete pass the declared staleness bound demands — the opposite of where a "keyframe" instinct
  would put it. The crossover is asserted in the test, so a width change moves it loudly.

### What the two-lane split forced into the format

Membership is reliable ordered and frames are unreliable sequenced, which means **either lane can be ahead of
the other**. A frame therefore carries the set generation it was built against, and a generation which is not
the receiver's is refused in both directions rather than read against a different set of entities. The
authority bumps the generation only when a publication actually changes something: a generation advancing for
free would invalidate every frame in flight for nothing.

The membership diff is computed by differencing the current table against the last published one, not by
accumulating a change log. Two properties fall out for free: an entity which entered and left between two
publications produces no traffic at all, and a slot which changed occupant travels as one enter rather than a
leave/enter pair whose order could matter. The mirror's table equals the sender's slot for slot by
construction, and the test asserts exactly that.

A gap in the membership generation is reported (`generation_gap`), not absorbed. The lane is reliable and
ordered, so a gap is a lane misuse rather than a network event, and absorbing it would leave the mirror
silently describing a set the authority never had.

### The correction threshold, derived rather than tuned

A client's predicted position is split onto the same lattice as the authoritative sample and the two are
compared **as integers**, so the threshold is exactly the declared quantum on every platform and there is no
epsilon to tune. One quantum is its floor: snapping onto a quantized value injects up to half a quantum even
when nothing diverged, so a tighter threshold corrects noise the format created. The verdict names the axis
that diverged most, which makes a correction diagnosable instead of merely visible. Verified exact at a
distance of 2^20 cells, where a world-space epsilon would already be the wrong size.

### Two hazards closed on the way

- **A refused message left a prefix behind, in both directions.** Every hot codec fills caller storage as it
  works, so a caller which inspected the output after a refusal would find the beginning of a message that was
  never accepted — the one shape of "partially accepted batch" the format exists to make impossible. The
  encoders had the same hazard for a different reason: reach and handle validity are checked per record while
  writing, so a relevance fault handed back a truncated frame the sender's own encoder had rejected. All five
  codecs — the two new pairs and HOT-01's intent pair — now clear the output (and the view's count) on every
  refusal. `clear()` keeps capacity, so the no-growth guarantee is untouched, and the tests assert both the
  emptiness and the unchanged capacity.
- **A build directory configured before the protobuf install fix could no longer GENERATE.** protobuf passes
  its install switch down to the bundled `utf8_range` without forcing it, so `utf8_range_ENABLE_INSTALL` stayed
  `ON` in an older cache and its `install(EXPORT)` demanded the vendored Abseil in an export set it
  deliberately is not in. `build-release` configured cleanly and `build-debug` did not, which is exactly the
  shape of a trap that only bites developers with an existing tree. The root `CMakeLists.txt` now forces the
  switch off next to `protobuf_INSTALL`, and the old cache recovers without being wiped.

Also extracted while adding the class: `widen_tick_low16`, the reconstruction of a wide tick from the low
sixteen bits a hot message carries. Every hot class needs it, and two implementations of that wrap rule would
be two chances to disagree exactly once every 65536 ticks — the least reproducible bug this campaign could
produce.

### What is deliberately not here

No transport call, no live traffic and no observed byte rate. The ladder is the codec's arithmetic, which is
honest about what it is: it fixes the ORDER of the levers and the cost of each format decision, and it cannot
speak for loss, jitter or lane contention. Carrying the class over real sockets belongs to the lab item,
exactly as HOT-01's intent class was closed as a primitive and then driven by NET-LAB-01 over 5G.

Verification: **10/10 cases, 598/598 assertions** in GCC Debug and Release; the header and test are
syntax-clean with `-Wall -Wextra` under GCC 14, Clang with libstdc++ and Clang with libc++. Focused networking
set **138/138** in GCC Debug and Release; complete project suite **578/578** in GCC Debug.

## NET-LAB-01 slice 3 — a relocatable artifact, 2026-09-08

The point of this slice is that the stand can be **built here and carried to machines which
cannot build**, which is the closest available approximation of a real network. Two things
had to be true: the artifact must not need the build host, and the peers must be able to
find each other without a shared directory.

### The dependency audit found the interesting thing first

The binary declared **eighty-five** direct shared libraries, seventy-nine of them system
Abseil. Protobuf requires Abseil, and protobuf's dependency script prefers
`find_package(absl CONFIG)` — so on a machine that has Abseil installed, the *vendored*
protobuf was only half vendored, and the artifact quietly depended on the host's Abseil
ABI. The root build now declares Abseil at the version protobuf 36.1 names for itself,
ahead of protobuf, which is sufficient: protobuf's script opens with
`if (NOT TARGET absl::strings)` and skips its search entirely.

With that, plus `-Wl,--as-needed` on the playground target, the artifact is down to **six**
entries:

```
ld-linux-x86-64.so.2  libc.so.6  libm.so.6  libstdc++.so.6  libgcc_s.so.1  libcrypto.so.3
```

`libzstd.so.1` was ours, not transitive — reached through a corner of `devils_utils` the
stand never calls, and removed by `--as-needed`. `zstd`, `zlib` and brotli in the earlier
`ldd` output were the *host libcrypto's* dependencies, so they follow whatever OpenSSL the
target machine has. Size 5.1 MB → 6.2 MB.

**Statically linking the C++ runtime was tried, and it is a trap worth naming.** The
distribution's own copies of glibc/libstdc++ are built for the target's CPU baseline, which
is precisely why they should stay dynamic; a static libstdc++ carries the *build host's* ISA
choices instead. This host runs CachyOS, whose packages are built for x86-64-v3, so its
`libstdc++.a` holds ~18,000 unconditional AVX instructions with no runtime dispatch — and an
artifact linking it could not run on a CPU without AVX no matter what `DEVILS_ENGINE_ARCH`
said about our own code. That also explains why `qemu-x86_64` refuses every AVX-less model
below.

The price of keeping it dynamic is a libstdc++ floor of **GLIBCXX_3.4.36 (GCC 15)**, and
exactly **one symbol** sets it: `std::basic_format_arg<...>::_M_handle_unrecognized()`,
emitted by any use of `std::format`. Without it the floor would be GLIBCXX_3.4.32 — GCC 13,
i.e. Ubuntu 24.04 and Debian 13. One symbol costs two GCC generations, so that is the first
place to look if the target machines turn out to be older.

**OpenSSL stays dynamic and that is a real constraint.** GNS offers only OpenSSL or
libsodium for AES-GCM/SHA-256 — there is no bundled option for that pair, and the machine
has no static OpenSSL. We link 3.6.4 here, but the requirement is looser than that and it
was checked rather than assumed: the artifact's only OpenSSL symbol version is
`OPENSSL_3.0.0` and all 41 imported symbols are 3.0-era EVP/HMAC/RAND entry points, so
**any OpenSSL 3.0+** serves. A 1.1-era distribution (`libcrypto.so.1.1`) will not.

### The glibc floor is measured, and no code change can move it

**GLIBC_2.38** — roughly Ubuntu 23.10+, Debian 13, Fedora 39+, or a rolling distribution;
Ubuntu 22.04 (2.35) refuses it with a loader error. The floor comes from
`__isoc23_strtol`/`sscanf`, glibc's C23 redirects, which appear because everything is
compiled as C++23, plus `arc4random` at 2.36. Counted per source: GameNetworkingSockets 22
references, protobuf 10, Abseil 2, **the engine libraries 0**. So this project's own code
cannot lower it, and demoting the dependencies to C++17 would risk an ABI split with the
C++23 engine (Abseil's `string_view` aliasing). Building against an older glibc in a
container is the cure, and it is a packaging decision rather than a code one.

### The pinned toolchain, and the two defects reaching it exposed

The fleet is Debian 13: glibc 2.41, libstdc++ from GCC 14 (`GLIBCXX_3.4.33`), OpenSSL 3.5.7 —
and, decoded from its `OPENSSL_ia32cap=0x80202001479bfffd`, a CPU with **SSE2 and SSE3 and
nothing else**: no SSSE3, SSE4.1, SSE4.2, POPCNT, AES-NI, XSAVE or AVX. That makes the
`crc32c` fix above a runtime necessity rather than a build convenience — the SSE4.2 CRC32
instruction would have faulted on this machine.

The artifact needed `GLIBCXX_3.4.36` and the fleet has `3.4.33`, so the deployable build is
**pinned to GCC 14** (`gcc14`, 14.3.1). No container is required, and the reason is worth
recording: the libstdc++ floor comes from the *headers* that emit the references, not from
the host's runtime, and linking against a newer `libstdc++.so` is harmless because a symbol
keeps the version it was introduced with. The glibc floor *does* come from the host, but
2.38 ≤ 2.41 already. Docker would only be needed to lower the glibc floor below what this
host provides.

The pinned build is `-DCMAKE_CXX_COMPILER=g++-14 -DDEVILS_ENGINE_ARCH=OFF` with the runtime
dynamic. Result: `GLIBCXX_3.4.32` (a version to spare against the fleet), `CXXABI_1.3.15`,
`GLIBC_2.38`, `OPENSSL_3.0.0`, 5.8 MB, six dynamic dependencies, and above SSE3 only 2,160
`tzcnt` — which decodes as `bsf` without BMI1, so it cannot fault, and `__builtin_ctz(0)`
is undefined anyway so nothing depends on the difference.

**Defect: `utils::info`/`warn` named spdlog's format-string type instead of asking spdlog.**
They hard-coded `std::format_string<Args...>`, but spdlog uses that type only when the
standard library advertises `__cpp_lib_format >= 202207L` and falls back to a plain
`std::string_view` otherwise. Measured: GCC 16 reports 202304 and accepts it, GCC 14 reports
202110 and does not, so the wrapper compiled on one standard library and not on another.
Fixed by deferring to `spdlog::format_string_t<Args...>`. The other four `std::format_string`
uses (`utils::error`, `catalogue::log_line`, `trace_line`) format through `std::format`
themselves and hand spdlog a finished string, so they do not depend on its alias.

**Defect, and the serious one: a classic STL algorithm over a `views::transform` range.**
`checkpoint_ring::latest_at_or_before` ran `std::upper_bound` over `bounded_history::entries()`,
which is `views::iota | views::transform`. What `std::iterator_traits` makes of that view's
iterator is not the same answer on every standard library: **libstdc++ 14 answers
`output_iterator_tag`**, after which `std::upper_bound` still compiles and silently returns
the wrong element. The consequence in the stand was total and looked like something else
entirely — `assess_recovery` received no checkpoint, answered `no_checkpoint`, and *every*
reconnect was told recovery was impossible. `bounded_history::find_entry` had the same shape
and was merely lucky. Both now binary-search the **index space**, which needs no iterator
machinery, so the question cannot arise. NET-04's own test caught it: one assertion of 168
under GCC 14, green under GCC 16.

Closed a diagnostic gap while chasing it: the stand reported "recovery impossible" without
the reason, though three distinct faults reach that branch. The reason now travels in the
message and is printed.

**"CPU ISA level is lower than required" — the marker, not the code.** The first artifact was
refused by the target's `ld.so` with that message despite carrying no non-baseline
instruction. `ld.so` checks the ELF property `GNU_PROPERTY_X86_ISA_1_NEEDED`, and the linker
**propagates it from the shared libraries it links against** instead of deriving it from the
code — so on a host whose libc is built for x86-64-v3, every binary linked there inherits a
v3 requirement. Measured decisively: a plain `int main(){return 0;}` compiled on this host
reports `ISA needed: x86-64-baseline, x86-64-v2, x86-64-v3` with its own
`ISA used: x86-64-baseline`, and this host's `libc.so.6`/`libm.so.6` carry exactly that
marker. The requirement describes the build host; the target loads its own baseline libc.

`-Wl,-z,x86-64-baseline` is the intended fix and is unusable here — binutils 2.47 aborts
with an internal error in `_bfd_x86_elf_merge_gnu_properties` — so the target drops the note
after linking. Checked first: the note holds only ISA markers and `feature used: x86, x87,
XMM`, no CET properties, so nothing is lost; if CET is ever enabled the step must become
lowering the level rather than stripping.

With the marker gone the artifact's non-baseline instructions were re-attributed one more
time, and every owner is a zlib-ng function with an explicit ISA suffix (`adler32_avx512`,
`inflate_fast_avx2`, `crc32_fold_vpclmulqdq`, `adler32_ssse3`, …) chosen from CPUID, plus the
single `xgetbv` in zlib-ng's probe which its source guards behind the OSXSAVE bit the target
lacks. Ordinary code contributes only `tzcnt`, which decodes as `bsf` without BMI1.

### Both directions over a 5G link, and the asymmetry it exposed, 2026-09-08

Runs in both directions with reports, the remote side on a 5G connection. Pairing the four
reports by state root shows what they actually were, and it is more than was asked for:

```
run A   authority = remote (GCC 14, ISA baseline)   root 137152892415853530  @3000
        follower  = local  (GCC 16, ISA avx)        root 137152892415853530  @3000
run B   authority = local  (GCC 16, ISA avx)        root 8522686143933819458 @3000
        follower  = remote (GCC 14, ISA baseline)   root 8522686143933819458 @3000
```

Every compatibility fingerprint is identical across all four reports. So these are
**cross-build, cross-ISA, cross-machine exchanges in both directions with matching state
roots** — which is NET-LAB-02's central claim, arrived at as a side effect of pinning a
toolchain for the deployable artifact.

**The interesting part is what the reports disagree about.** The link was effectively the
same in both runs — ping p50 27–29 ms, p90 33–34 ms, max 38–45 ms, `quality_local` tenth
percentile 0.95–0.98 — yet the orders told a different story:

| | copies that never arrived | orders lost | forced by lateness alone |
| --- | ---: | ---: | ---: |
| run A (authority on 5G) | 26 of 2001 (1.3%) | 90 | **at least 64** |
| run B (follower on 5G) | 44 of 2001 (2.2%) | 22 | 0 |

Run B lost *more packets* while losing *four times fewer orders*. Since an order dies only
if every one of its three copies dies or arrives late, and run A lost only 26 copies, at
least 64 of its 90 lost orders were **purely late**. The difference is timing margin, not
link loss — and the run had no measurement of margin at all, which is why the cause had to
be reconstructed by arithmetic.

`copies.late` also behaved exactly opposite to intuition, which is the clearest possible
vindication of separating it from `orders.lost`: run A had 511 late copies and 90 lost
orders, run B had 294 late copies and 22 lost orders. More late copies meant *fewer* lost
orders, because a late copy is evidence that an earlier copy of the same order had already
been accepted.

### Three runs sized the proposal lead, 2026-09-08

The margin measurement paid for itself immediately. Three cross-build runs over the same 5G
link, each in both directions:

| run | lead | RTT mean | last copy's margin | late copies | orders lost | root reproducible across runs |
| ---: | ---: | ---: | ---: | ---: | ---: | --- |
| 1 | 4 | 28 ms | +0.6 | 511 / 294 | 90 / 22 | no |
| 3 | 4 | 16 ms | +1.2 | 57 / 58 | 6 / 9 | no |
| 2 | 8 | 17 ms | +5.2 | 3 / 11 | 0 / 2 | **yes** |

The second run changed two variables at once — the lead doubled *and* the link roughly halved
its round trip — so it could not settle anything by itself, and the third run was run purely
to separate them. Both mattered: same lead with a better link cut losses tenfold (90 → 6),
and same link with a bigger lead cut them fourfold again (6 → 0).

What explains all three is `margin_k ≈ lead − k − RTT/tick_period`, checked against the
histogram rather than assumed: run 3's nominal margins of +3.1/+2.1/+1.1 appear as buckets
+1 (456–470), +2 (654–663), +3 (589–611) and a tail at +4 (207–208), so each copy spreads
over about two tick buckets and the jitter is ±1 tick. The controlling quantity is the
**last** copy's margin and it has to exceed that jitter rather than merely be positive,
which is why a nominal +0.6 lost fourteen percent of orders while the link dropped only 1.3%
of packets. The rule `lead ≥ (w-1) + RTT/tick + jitter_ticks` ranks all three runs correctly
and is now recorded in `NETWORKING.md` as a decision, together with what it costs: the lead
*is* input delay, so this table is a latency-versus-lost-orders curve — 80 ms for about 1%,
160 ms for none, on a 16 ms link.

**And an integrity check fell out of it.** Cross-process agreement at a tick held in every
run, as it must. But *reproducibility across runs* held only in run 2, the one which lost no
orders: all four of its reports carry the identical root `152446983058193285` across both
directions and both builds. The runs which lost orders agreed within each run and differed
between them — correctly, because the input was not the same input. So a differing root
between two runs of the same schedule is a free, precise indicator that input delivery was
incomplete.

One measurement artifact recorded so it is not read as an event later: `in_pps == 0` appears
at exactly ticks 225 and 250 in two independent runs, and `quality_local == 0` on the first
sample of every run. A network does not repeat itself to the tick; these are the backend's
statistics intervals before they fill. "No data" and "no traffic" are different answers and
the report must not blur them.

### Margin is now measured, and it sizes the one knob that matters

The authority records, for every arriving copy, the target tick minus the tick it had
already committed: positive beat the seal, zero or negative was late. On loopback with the
default four-tick lead the three copies of an order land at exactly `+4, +3, +2` — copy *k*
has margin `lead - k` — and `margin.late_fraction` is 0. On a link with 30 ms round trip
that whole distribution shifts down by one and a half to two ticks, which puts the third
copy at the seal and the second one marginal. That is the mechanism behind run A's numbers,
and it is now visible rather than inferred.

So the proposal lead became a knob, `--intent-lead`, and it **travels in the grant** for the
same reason the pacing does: the authority is what seals the tick, and an operator who set
it on one side only would be running two different protocols without being told. Verified:
set on the authority alone, the follower reports the announced value and the margins move
with it — at a lead of eight every copy lands in the `plus5_or_better` bucket.

What the knob costs is the honest part: the lead **is** input delay. Sizing it from
`margin.late_fraction` is how to buy the least delay that keeps orders landing, instead of
guessing a number.

### The stand passed over a real network, 2026-09-08

Two machines, ping ~30 ms, 3000 ticks at 20 ms. Both runs agreed:

```
run 1 (authority = GCC 14 artifact)   authority tick=3000 root=14034182923505186525
                                      follower  tick=3000 root=14034182923505186525
run 2 (authority = GCC 16 artifact)   authority tick=3000 root=11665558108087288258
                                      follower  tick=3000 root=11665558108087288258
```

With a real reconnect on each (`transport_loss=1 reconnects=1 recoveries=1 replayed=5`) and,
for the first time, link statistics that mean something: `ping_ms=30`, `quality=0.988`,
`in_pps=50`. Note what is *not* reproducible here and should not be: the two runs have
different roots, because they had different inputs — 599 versus 609 accepted intents. The
root is a cross-process agreement at a tick, not a golden value, once the input depends on a
lossy network.

**And the run produced a number that needed explaining rather than celebrating: `late=383`,
against `late=1` on loopback.** Reconciling the counters shows why it is the wrong quantity
to look at: 648 orders proposed, each carried in three consecutive batches, of which 599
were accepted, 806 dropped as duplicates and 383 dropped as late. The late ones are
overwhelmingly *redundant copies* of orders that had already been accepted from an earlier
copy — the window working as designed. What the counters could not answer is the question
that matters: 599 of 648 is 92.4%, so **were 7.6% of the player's orders actually lost?**

So the stand now measures orders rather than copies. The follower keeps each proposal until
its tick is committed and checks whether the canonical bundle for that tick carried it:
`orders_landed`, `orders_lost`, and `orders_unobserved` for proposals whose tick was crossed
by a recovery replay rather than applied live — three answers instead of folding an
unobserved fate into either of the other two. The transport's `superseded` count is also
reported now: a redundant copy discarded by the backend because a newer frame in the same
unreliable lane had already arrived is where a copy goes on a jittery link, and it was
previously invisible. On loopback the accounting closes exactly: 133 proposed, 133 landed,
0 lost, 0 superseded.

This is the same lesson this project keeps relearning: an exact value of the wrong quantity
stays the wrong quantity. `late` was cheap to count and `orders_lost` is what a player would
notice.

### Driving it by hand across machines, and what that found

Ping ~30 ms between machines. The transport and the handshake worked on the first attempt —
the follower's refusals were session-layer answers, not connectivity — but the run exposed
three rough edges in driving the stand by hand across machines, one of them a real defect.

**Defect: the silence budget started before the traffic did.** The authority does not start
its clock until the whole roster is admitted, which across machines is however long the
operator takes to launch the others. A follower admitted first therefore sat in silence,
crossed `lost_after`, reconnected, and was told `no_checkpoint` — a failure whose every
symptom points somewhere other than "the run has not begun". The budget measures
*interrupted* traffic, so it cannot start before traffic does; it is now armed by the first
applied bundle. Reproduced locally with a twelve-second gap between two followers, which
now completes with both roots equal.

The other two: the wall deadline was 60 s, a number sized for the harness rather than for a
human starting processes on several machines — now 900 s in endpoint mode, with
`--deadline-s` to override — and the authority printed nothing while waiting, which is
indistinguishable from a hang, so it now reports `authority waiting: admitted M of N
followers` every five seconds. `--help` did not exist at all and an unknown argument was
fatal.

Not a defect but worth documenting, because it looks like one: re-running a follower with
the same `--index` within the hold window (20 s) is refused with `no_capacity`. The hold
belongs to that session's owner, who is expected to return with `--resume`; a fresh join
does not displace it.

**Identity now holds across three axes**: Debug/Release, AVX/baseline ISA, and GCC 16/GCC 14
all produce `8518737655127057956` at tick 70 and `13531786226404380819` at tick 42.

### Finding each other without a shared directory

`--listen HOST:PORT` and `--connect HOST:PORT` replace the file rendezvous, which stays for
the local harness only — the harness wants a port the operating system picked, and a
distributed run wants the port the operator declared, with no fallback: silently binding a
different port than the one the followers were told is worse than refusing.

The pacing now **travels in the grant**. A follower which took the tick period from its own
command line would declare a loss every tick against an authority pacing slower than it
assumed, so the tick period and both silence budgets are announced, and the budgets are
derived from the tick period rather than fixed at the 60/220 ms that suited a 4 ms
laboratory tick. Note what this is *not*: in this stand the wall pacing is not causal, so
it is announced rather than fingerprinted; a project with authored durations converts them
through the tick rate, which makes the rate causal and puts it in the compatibility
fingerprint instead.

### One class of defect, found three times by its own assertions

A scheduled failure names a roster position, and a reduced roster may not have that
position. With `--followers 2` the stand demanded a stale batch from a follower nobody
launched; with `--followers 1` the authority waited forever for a reconnect that could not
happen; and the cross-principal assertion fired because the followers' proposal cadences
were offset by `roster % 2`, so with two followers they never collided and the canonical
order across principals was never exercised at all. The schedule was wrong in each case,
not the assertion: `lab_schedule` now asks "does this failure hit anyone", and the
proposal cadence has one tick per window **shared** by every follower so a multi-principal
bundle is guaranteed for any roster size.

### No AVX on the target — the default build assumed it

The servers the artifact is meant for have no AVX, and the root `DEVILS_ENGINE_ARCH`
defaults to `AVX`: every engine target was getting `-mavx` and `GLM_FORCE_AVX`. The option
already supports `OFF`, so the portable artifact is a configure flag rather than a change,
but the result has to be *checked* rather than assumed, and the check needs two numbers to
be readable.

Measured, both builds side by side:

| | `ARCH=AVX` | `ARCH=OFF` |
| --- | ---: | ---: |
| VEX instructions, total | 3,971 | **957** |
| — zlib-ng, dispatched from CPUID | 957 | 957 |
| — **unconditional** | **3,014** | **0** |

The 957 are the expected residue and identical in both builds: zlib-ng compiles
`adler32_avx512`, `crc32_fold_vpclmulqdq`, `inflate_fast_avx2` and siblings as separate
functions and chooses between them from CPUID at runtime, so they exist at any baseline and
never execute on a CPU without the feature. A residual count made entirely of such symbols
is the success signature; a hit in the project's own code is not. In the `AVX` build the
3,014 unconditional ones are the stand's own loops plus inlined `std::filesystem` and
`std::string` — a `SIGILL` on the first one reached.

**And the state root is identical between the two builds** — `8518737655127057956` at tick
70, `13531786226404380819` at tick 42 — which is the integer-only causal state paying off
across a change of instruction set rather than merely across a change of optimizer. It is
the campaign's first cross-ISA evidence.

**`DEVILS_ENGINE_ARCH=OFF` did not build, and that was a defect rather than a configuration
mistake.** `libs/utils/src/utils/core.cpp` called `_mm_crc32_u64/u32/u8` unconditionally —
SSE4.2 intrinsics enabled only incidentally, because `-mavx` implies SSE4.2 — so the
baseline configuration failed to compile `devils_utils` at all and had evidently never been
exercised. `utils::crc32c` now has a portable byte-wise path under `#if !defined(__SSE4_2__)`,
verified bit-identical to the intrinsic one across 301 lengths (0..300): CRC32C is a
standard-defined value, so the two are one quantity computed two ways rather than a fast and
a slow answer. The function has no callers anywhere in the engine or the projects, so
nothing existing could shift. Two latent narrowings in the intrinsic path (a `uint64_t` crc
passed into `unsigned int` parameters) and a missing `_MSC_VER` case were fixed alongside —
on Windows `<immintrin.h>` was never included and the function relied on `<windows.h>`
happening to pull `<intrin.h>` in.

Recorded while auditing that: **the artifact links zlib-ng only because `devils_utils`
does**, and the stand compresses nothing — 59 deflate/inflate symbols and all those AVX512
paths are dead weight. Removing them means splitting `devils_utils`, an engine change
rather than a playground one.

**The emulator route was tried and does not work on this host, which is worth writing down
so it is not tried again.** `qemu-x86_64 -cpu max` runs fine, but every AVX-less model —
`qemu64`, `Nehalem`, `Westmere`, `core2duo`, `Opteron_G3/G5`, and `max` with the AVX bits
subtracted — dies with an illegal instruction on a *statically linked* `int main(){return
0;}`. The fault is in this host's own glibc, not in the tested binary, so qemu-user here
cannot answer the question at all. Static attribution of VEX instructions to symbols is the
available check; the real one is the target machines.

### Verification

The copied artifact was run from a directory with no build tree in sight, over an explicit
endpoint, at rosters of one, two and three followers — every follower agreeing with the
authority's state root each time — and once over a real non-loopback interface
(`192.168.122.1:41333`, 20 ms tick, 300 ticks) where all four processes reported
`root=11962137424109576394`. At that length the link statistics populate:
`quality=1, in_pps≈51`, one packet per tick. It leaves no files behind unless a scenario
persists a ticket. The registered local test is unchanged at 1.5 s, **41/41** harness
checks, and the focused networking set is **124/124** in GCC Debug and Release.

## NET-LAB-01 slice 2 — several followers, 2026-09-08

The stand now runs **one authority, three followers and one intruder** as separate
processes, and its criterion is unchanged but now says more: every independent process
agrees on the causal state root at the same tick. Golden values, stable in GCC Debug and
Release, across repeated runs, and across a loopback and a real non-loopback interface
(`192.168.122.1`):

```
continuous  tick=70  root=8518737655127057956    admissions=5  multi_principal=7
killed      tick=42  root=13531786226404380819    announced_final 30 -> 42
```

**Provenance is the whole content of "several followers"** — not N connections, but the
canonical order *across principals*. A sealed bundle is ordered by principal first, then
kind, then target, because two peers whose packets interleave differently must still seal
the same bytes, and only a total order keyed on something neither peer controls can promise
that. Correspondingly "a redundant copy" now means same principal, same kind, same tick;
the same kind from a different principal is a different order, not a duplicate. The
authority verifies the principal ordering of every bundle it seals, and `multi_principal`
counts the ticks which actually carried more than one — the followers' proposal cadences
are offset on purpose, because a schedule where they never collided would exercise none of
this.

The principal itself comes from the **credential**, never from a field the peer fills in: a
follower which could name its own principal could name someone else's, the same forgery as
naming its own actor. The follower asserts that the principal it was admitted under is the
one its roster position expects.

**One follower is deliberately left alone.** A stand where every peer is disturbed cannot
tell whether the undisturbed path survives a neighbour's recovery, so follower 2 reaches
the same tick and root with zero reconnects while 0 and 1 recover, and that is asserted
rather than observed. Each recovery is per-session: its own paced transfer, so one
follower's checkpoint cannot stall another's bundles.

### Two real defects the slice found

- **A refusal must be delivered, not merely sent.** The authority closed the connection
  immediately after handing the refusal to the transport, which discards it: the peer then
  learns only that the connection died and retries, spending exactly the deadline that
  SESSION-02's *terminal* refusal exists to protect. A refused connection is now held open
  for a declared grace. The client was wrong in the mirror image — a connection which dies
  during the handshake is now terminal for the attempt, because a client waiting for a
  refusal that is never coming waits forever.
- **The quantum is session identity, and the stand was not checking it.** Two peers with
  different quanta decode the same codes into different world values: a systematic, silent
  disagreement no digest can localize, because it appears in the state and not in the
  codec. The stand carried a literal `numeric_profile = 1`, so such a peer would have
  passed the handshake. It is now derived from the axis split, the code width and the
  causal step. (This surfaced from the question of whether a finer quantum is worth it —
  see the note on that below.)

### Two things now declared rather than assumed

- **The run's end belongs to the authority and can move.** A peer which rejoins at the very
  end recovers and then has nothing to do, which proves it caught up but not that it
  participates again. The grant therefore carries the final tick, a late rejoin extends the
  run by a declared tail, and the extension is re-announced to everyone still playing. The
  extension is a **fixed** end rather than "wherever we are plus a tail" — that is what
  keeps the run length, and so the state root, identical in every run instead of a function
  of how fast a process happened to start.
- **The capacity refusal is reachable only by a valid, already-used credential**, which is
  why a fourth process exists: an undeclared token is `identity_rejected`, a different
  answer to a different question. The intruder is refused with `no_capacity` (11) and the
  authority records it.

### Link statistics: measured, and the measurement is the result

The stand reads GNS's real-time status at the last tick — deliberately there rather than
after the linger, where the backend reports a connection that has been idle. At the
registered test's length it answers `quality=-1, in_pps=0`, which is the backend saying
**no data**, not no loss: its end-to-end statistics come from a periodic exchange whose
interval is longer than the whole run. A `--final-tick 6000` run (~25 s, 17,951 bundles)
populates them: `quality=1`, `in_pps≈244`, `ping_ms=0`. A 5 s run does not. So a LAN
measurement has to be **long, not merely remote** — a concrete requirement for the LAN
slice rather than a defect here.

`--address` is wired and proven on a real non-loopback interface, producing the identical
state root. Every process still runs on one host, so nothing has crossed a wire with
latency or loss yet; that and the Linux↔Windows exchange remain the gap.

Verification: `NETLAB01_multi_process_verify` stays a registered CTest at 1.5 s, five
consecutive Debug runs and three Release runs, **41/41 harness checks and ~1,350
in-process checks across 11 processes** per run. The focused networking set is
**124/124** in GCC Debug and Release.

**Harness rule, learned the hard way three times:** every long-lived child is spawned
before any child is collected. `collect` blocks until its child exits, so a spawn placed
after one runs against a session which has already ended — and each time the symptom
appeared somewhere else entirely (a follower stopping at the wrong tick, an intruder
"neither admitted nor refused"). The one exception is the replacement follower, whose spawn
is triggered by a death and is therefore deliberately late.

## NET-LAB-01 slice 1 — the same session as two real processes, 2026-09-08

`subprojects/playgrounds/NETLAB01_multi_process`. The first slice of the multi-process laboratory: one
authority and one follower as separate operating-system processes over real UDP, carrying SESSION-02's
handshake, SESSION-03's credential, SESSION-04's policy and HOT-01's intent class **on the same wire at once**.
This is the first place those six closed slices meet.

The criterion is one value, and it holds:

```
continuous  authority tick=70 root=10617983408498789030   follower tick=70 root=10617983408498789030
killed      authority tick=48 root=2915615940752639234    follower tick=48 root=2915615940752639234
```

Both roots are **identical in GCC Debug and Release and across repeated runs**, which is a property of the
state rather than luck: the causal state is integer-only and the tick is its only coordinate, so timing cannot
enter it. That is deliberate and it is an attribution rule, not a simplification — this stand exists to blame a
mismatch on the protocol, and a float world would let every failure be blamed on `libm` instead. The integers
are also exactly what the wire carries: `(key << 16) | code` from a hot intent *is* the stored count of quanta,
so the authority's decode seam performs no conversion, and the only floating-point step in the whole path is
the follower turning an authored target into a split.

### Three loss mechanisms are three code paths, and all three run

- **Explicit close** (tick 20): the transport reports a terminal state, so the follower's silence budget is
  skipped entirely and `session_hold_table` serves the resume. `from_hold=1`.
- **Quiet window, no close** (tick 45): nothing reports anything, so only the silence budget can notice
  (`warnings=1`, `silence_loss=1`). The resume then arrives **while the old connection is still nominally
  alive**, and the session migrates. `migrated=1`.
- **Process death**: the follower persists its ticket and leaves with `_exit(7)`; a brand-new process rejoins
  on a ticket read from disk, with no state of its own and no confirmed anchor. Its recovery target equals the
  retained checkpoint, so it is the documented zero-replayed-ticks case.

Also measured rather than asserted on paper: `late=1` from a scripted stale batch, `duplicate=7` from the
redundant intent window, `deferred=1..2` from bundles overtaking the checkpoint transfer, `chunks=8`.

### What the stand forced, and found

- **The authority's clock starts at the first admission.** The stand caught this in its first run: the follower
  refused a bundle for tick 21 while expecting tick 1. A follower joining fresh has *no state*, so a session
  which already advanced owes it a baseline transfer — a real requirement, and the next slice's. Pretending
  tick zero is wherever the authority happens to be would have hidden it.
- **A reconnect ticket's expiry is in the authority's clock, and that has to be said out loud.** Two machines'
  monotonic clocks share no origin, so a client using `expires_at` directly is using an unrelated number which
  happens to be milliseconds. The grant therefore carries the authority's issue instant and the follower
  anchors to it. Without that anchor the shared deadline SESSION-04 relies on is not shared, and nothing in
  `reconnect.h` can detect the difference — the library reads no clock, which is correct, and this is the cost.
- **A resume can arrive while the old connection is still alive**, and refusing it because the session is "not
  held" would strand a client whose path died in one direction only. The rule: the credential proves the
  principal; an attached session **migrates** and its stale connection is closed, and only an unattached
  session is looked up in the retention table. This is what the hold table's documented ordering means in
  practice, and both branches now have a test.
- **Pacing the bulk transfer is what makes the deferral path real.** One chunk per owner pass, because a sender
  which blasts a whole checkpoint into the lane has not used the budget the lane declares. With the unpaced
  blast the replay bundles and the chunks arrived together, `deferred` stayed 0, and the "bundles overtake the
  bulk transfer" property was a comment rather than a result.
- **The pinned GNS backend refuses a bind on port zero** (`backend_rejected`). `listen_any` therefore falls back
  to a scan and reports which path it took, so the stand records a backend property instead of hiding it. The
  rendezvous itself is a file: the authority publishes the port it actually got, which a fixed port cannot
  guarantee and a scan alone makes flaky.
- **A terminal handle stays owned until it is closed.** A reconnect which did not close it found the peer table
  full and spent its attempts on a capacity refusal instead of on the network. The adapter documents this; the
  stand is where it costs something.
- Own defect worth recording because the shape recurs: the harness decoded a child's exit status as
  `LAB_EXIT_CODE(LAB_PCLOSE(stream))`, and `WIFEXITED`/`WEXITSTATUS` are macros which evaluate their argument
  more than once — so `pclose` ran several times and the parent died on a double free. A macro argument with a
  side effect must be named first.

### Deliberately not proven yet

The checkpoint is about sixty bytes, so the "bulk" in bulk lane is exercised as multi-message assembly and lane
priority, **not as size**; a project-sized checkpoint belongs to the slice which attaches a real world. Several
followers are now done (slice 2 above); a second machine, recorded real RTT/jitter/loss and the Linux↔Windows
exchange are the remaining NET-LAB-01/02 work. The anchor-avoids-the-transfer optimization is not implemented: the stand carries the
confirmed anchor and the authority ignores it, exactly as the contract permits. The join credential is a shared
laboratory token, because SESSION-01/03 deliberately left join identity an injected policy.

Verification: `NETLAB01_multi_process_verify` is a registered CTest, 1.5 s, passing five consecutive Debug runs
and three Release runs. The focused networking set is **124/124** in GCC Debug and Release, and the complete
`devils_engine` label is **568/568** in Debug (196 s) — the first whole-suite run of this campaign, made
possible by the repaired verification target below. The per-process
check counts vary between runs because a check fires per message and the message count depends on scheduling;
the harness's own 14 checks are fixed.

## Verification target repaired — the list was the defect, 2026-09-08

`devils_engine_test` enumerated its `DEPENDS` by hand, and the hand had fallen **45 targets behind**: not only
the four newest session/hot-path tests but every `originator_*`, `painter_*`, `aesthetics_*` and `catalogue_*`
test added since the list was last touched. The failure mode is quiet in the worst way — the target runs
`ctest -L devils_engine`, which selects tests by label regardless of the list, so on a fresh tree those tests
are *selected but never built* and fail as missing executables, while on an already-built tree they pass and
hide the gap entirely. That is why the count could drift this far unnoticed.

Naming the four missing tests would have restored the same defect with a later expiry date, so the list is now
derived: `devils_add_doctest` appends each target to a global `DEVILS_ENGINE_TEST_TARGETS` property, and the
root target depends on that property plus an explicit remainder — the playgrounds and smokes which call
`add_test` directly and therefore pass through no helper. The conditional `add_dependencies` for the two GNS
tests is gone, because a conditionally registered doctest lands in the property exactly when it is registered.

Confirmed in the generated build graph rather than by reading the CMake: `devils_engine_test.dir/all` now
carries edges to `network_reconnect_test`, `originator_volume_test`, `GN05_constraint_collapse` and the GNS
pair. `ctest -R "network|NET0"` is **123/123** in GCC Debug afterwards.

## SESSION-01 — strict compatibility, identity and recovery, 2026-09-06

The first session-layer slice now lives in `libs/network/include/devils_engine/network/session.h`. It remains
transport- and project-neutral: no GNS type, file enumeration, wire codec, credential format or gameplay object
enters the library.

- `try_make_session_content_root` hashes product/version and complete resolved core/project/mod file bytes with
  canonical names and explicit mod load order. Invalid names, ambiguous mod positions and noncanonical entry
  order are refusals which leave the previous output untouched. The root is strict compatibility metadata,
  not authentication.
- `session_compatibility` separately names handshake/protocol, state schema, intent schema and numeric profile,
  giving a precise early refusal. Only a compatible peer reaches the injected identity verifier; its logical
  principal is independent of the current transport connection.
- Membership fixes session, logical local/authority peer, principal and authority epoch. Authority message
  stamps reject the wrong session/authority and both stale and unexpectedly future epochs. A fresh GNS peer ID
  after reconnect is deliberately absent from this identity.
- A recovery plan names checkpoint tick/root and target tick/root. `recover_session` preflights every required
  bundle before restore, checks the session/principal/authority epoch, restores and replays in detached staging
  with presentation suppressed, then calls one `noexcept` publish only after both roots match.
- The causal test carries a PRNG cursor, entity counter, timeline remainder and tick. Removing the PRNG cursor
  from the checkpoint is detected at the checkpoint root before replay and leaves the live state untouched.
  A missing middle tick is likewise refused before restore. The successful path publishes exactly the same
  causal state as uninterrupted ticks while preserving presentation state.
- The existing real `tile_frontier` proof was rerun in both configurations. Its actor snapshot includes the ECS,
  actor seeds, tick/game time, food-spawn sequence and authored scheduler/config values; after load it remains
  byte-identical for 120 ticks, and one/four-worker runs match for 45 ticks. Its `utils::timelines_causal_state`
  is currently restored beside the actor packet rather than inside it. Therefore a future network checkpoint
  envelope must compose both owners; sending `actor_world_slice::save()` alone would omit the clock remainder.
  The pacing test independently gives identical checkpoint bytes for one coarse and 100 fragmented frames.

Debug and Release verification passes **6/6 cases, 76/76 assertions**. The complete focused networking set,
including the existing real localhost UDP cases, passes **81/81** in both configurations. No sanitizer or
whole-project run was performed. Wire framing, challenge/response, credential lifecycle, automatic GNS
reconnect and replay across a real new connection remain the next integration layer.

## SESSION-04 — automatic reconnect policy and recovery feasibility, 2026-09-07

`libs/network/include/devils_engine/network/reconnect.h`. The slice deliberately stops short of the transport:
nothing here opens a socket, sends a byte or replays a tick. What the library owns is what must not be guessed.

- **Silence is not loss.** Two budgets, not one: a slow tick or a stalled frame must not tear down a session,
  so suspicion is separate from loss. The warning fires once per spike and rearms after traffic returns.
- **The deadline is the reconnect ticket's own `expires_at`.** Nothing new travels on the wire to arrange it,
  and the two sides therefore cannot disagree about how long a reconnect is worth attempting. The coordinator
  checks the deadline *before* spending an attempt, so an expired ticket abandons with `deadline_passed` and
  zero attempts.
- Backoff is deterministic doubling with a cap and **no jitter**: the library owns no randomness. A caller
  spreading a crowd of reconnecting clients adds its own. An incoherent policy answers `valid() == false`.
- Traffic resurrects a session from suspicion **and** from a declared loss the caller has not acted on yet —
  the spike which resolves itself, where reconnecting is pure cost. It does not resurrect from `attempting`
  onward: a fresh connection is in flight and bytes from the old handle are ambiguous, not reassuring.
- A transport which reported itself gone skips the silence budget entirely; that is direct evidence. A
  terminal refusal is not retried on the schedule, which would only spend the deadline.
- `session_hold_table` has **declared capacity**, because a table which grows with disappearing peers is an
  allocation a peer controls. Reaping is explicit, so the declared capacity is the capacity actually available.
  Consult it only after the credential verified: the credential proves the principal, so a stranger cannot
  enumerate sessions through resolution answers. Expiry and absence are different answers — "expired" tells a
  returning client its ticket is worthless, "unknown" may mean it reached the wrong authority.
- **`assess_recovery` is where the retention budget becomes visible.** A checkpoint at `K` is the committed
  state after `K`, so replay needs a sealed bundle for every tick `K+1..N`; the only sufficient history is one
  whose oldest retained bundle is at or before `K+1`. A history starting at `K+2` is `history_gap` — and that
  is precisely why the client has a `rejoin` action: "recovery is impossible, join fresh" is a normal answer,
  not a failure. A target equal to the checkpoint is recoverable with zero replayed ticks.
- The composition test runs the authority's side of one reconnect over a real `checkpoint_ring` and
  `bounded_history`: ticks which kept flowing while the peer was away evict the bundle after the retained
  checkpoint and make recovery impossible, while a **newer checkpoint restores feasibility without a larger
  history**. Retention is a checkpoint cadence and a bundle budget working together, not history alone.

**A test caught a real inconsistency, and the code was right.** The first run disagreed with
`session_hold_table::resolve` about epoch orientation. The implementation compares the epoch *presented*
against the one *recorded* — older than the record is stale — which is the same orientation as
`classify_authority_message` and `verify_reconnect_credential`. The test expectation was inverted, and a
comment in the header described the wrong mechanism (detecting that the authority itself migrated is the
credential's job, since only it knows the authority's current epoch). Both were corrected.

Verification: **7/7 cases, 173/173 assertions** under g++ and clang++. These were first measured by direct
compilation, because the CMake configure was broken at the time by the protobuf conflict below; the focused
set was rerun through ctest afterwards.

## Vendored protobuf for GNS — one cause, two failures, 2026-09-07

Building for Windows exposed that GameNetworkingSockets pulls in protobuf (it must: its wire messages —
certificates and connection setup — are protobuf), and that the Linux build had been resolving protobuf from
somewhere else entirely. The project vendors protobuf through FetchContent while GNS independently calls
`find_package(Protobuf)`, and that single conflict produced two different errors.

- **Before `OVERRIDE_FIND_PACKAGE`:** `find_package(Protobuf QUIET CONFIG)` found the *system*
  `/usr/lib/cmake/protobuf/protobuf-config.cmake` (36.0). GNS forces `protobuf_MODULE_COMPATIBLE=ON`, so that
  config included `protobuf-module.cmake`, which read the `LOCATION` property of the target
  `libprotobuf-lite` — a real target of the vendored protobuf, and CMake forbids reading `LOCATION` from a
  non-imported target.
- **After `OVERRIDE_FIND_PACKAGE`:** the right tool, and it removed the system protobuf from the search. But
  it works through a redirect config which only declares the package found, so `Protobuf_FOUND` became true,
  GNS never reached its module-mode fallback, and `protobuf_generate_cpp` was undefined. That command exists
  only in an *installed* protobuf's `protobuf-module.cmake` (generated from a `.in` at install time) and in
  CMake's own `FindProtobuf`; the vendored sources carry neither, only `protobuf-generate.cmake` with the
  modern `protobuf_generate`.

The redirect config documents the hook for exactly this case, so the one missing command is now written into
`${CMAKE_FIND_PACKAGE_REDIRECTS_DIR}/protobuf-extra.cmake` — no dependency patch and no system protobuf
leaking in. The **legacy flat layout is deliberate**: GNS includes generated headers flat
(`<steamnetworkingsockets_messages.pb.h>`) and adds its binary dir to the include path, while the modern
`protobuf_generate` preserves each proto's relative path, which those includes would not find. Import paths
are each proto's own directory, because these protos import one another by bare name.

A second error followed: `install(EXPORT protobuf-targets)` demanded that this project's `zlib` also belong to
an export set. Nothing here is installed, and the option protobuf 36 actually reads is `protobuf_INSTALL`,
now set OFF. Noted in passing: `protobuf_BUILD_EXPORT`, which the project sets, is **not an option in
protobuf 36** and has no effect.

Result: `GameNetworkingSockets_s` builds against the vendored protobuf on Linux, so both platforms now take
protobuf from the same place, and the focused set is **145/145** in Debug.

## SESSION-03 — reconnect credential, 2026-09-07

`libs/network/include/devils_engine/network/credential.h`. The scope decision came first and it is the
content of the slice: **of the two credentials, the engine must own exactly one.**

A join credential proves who a stranger is, and which authority vouches for that is policy — platform
identity, offline keystore, dedicated-server token — so it stays the injected verifier already in
`session.h`. A reconnect credential is different in kind: the authority mints it for itself at admission and
has to verify it **alone**, because the external identity service can be unreachable exactly when a reconnect
is needed. Nobody but the engine can own that.

- **Two independent proofs, and conflating them is the classic reconnect hole.** `ticket_mac` says what the
  bearer is entitled to, keyed by the authority's own key, checkable with no third party, and replayable on
  its own *by design* — it states an entitlement, not who is speaking. `presentation_mac` says the bearer is
  presenting it in **this** exchange: keyed by a secret only the authority and that client know, over the
  handshake transcript, which contains both nonces.
- The replay test is the one that matters: a passive observer holding **every byte** of a captured credential,
  presented against a different transcript, is refused with `presentation_mac_invalid`. Holding the ticket
  without the derived secret is likewise not enough, and a presentation made for one entitlement does not
  carry another ticket.
- The session secret is **derived**, `MAC(authority key, session, principal)`, not stored: the authority keeps
  no per-session secret table and therefore cannot lose one. Verified deterministic, and different per
  session and per principal.
- Three **domain tags** separate the entitlement tag, the secret derivation and the presentation. Without
  them all three are byte strings under one key and an attacker picks which is which; a test asserts that the
  secret and the ticket tag for the same session under the same key differ.
- **The library reads no clock.** Instants are caller-declared and only the authority's instant decides
  admission — a bearer's clock is not evidence. A clock which moved backwards answers `not_yet_valid`, a
  distinct status from `expired` because the operator's fix differs. Both boundaries are pinned: valid at the
  issue instant, refused at the expiry instant.
- **Check order is contract, and it is asserted with a counting policy**: a wrong session or an expired window
  costs zero MAC calls, and a bad entitlement tag stops before any secret is derived. An authority never does
  cryptographic work proportional to a stranger's claims.
- Tag comparison is `equal_in_constant_time`. A comparison whose duration depends on how many leading bytes
  matched turns an unforgeable tag into a few hundred guesses.
- Every tampered ticket field is refused **with the authority's state deliberately made to agree with the
  forgery** — the worst case, where the declared comparisons all pass and only the tag stands between the
  bearer and admission. That is the point: the field comparison is a diagnostic, the tag is the defence.
- The canonical credential is 109 bytes, inside the 256-byte handshake credential budget (a static assertion,
  not a hope). Wrong size, wrong format byte and an unprepared buffer are refusals.

**Written-down limitation:** reissuing a ticket does **not** revoke the previous one. Without per-session
state an authority cannot revoke, so expiry is its only revocation — which is why the window is short and a
ticket is reissued at every admission. A test asserts the older ticket still verifies until its own expiry,
so the property is recorded rather than discovered later.

The MAC primitive is injected through `credential_mac_policy`; no algorithm is chosen in the library. The
test supplies HMAC-SHA256 over the engine's own SHA-256, which is where a concrete primitive belongs. No key
storage, rotation schedule or join-credential format is added, and the authority key must not be stored
beside the tickets it signs.

Verification: **7/7 cases, 144/144 assertions** in GCC Debug, GCC Release and Clang Debug. Focused set
**138/138** in GCC Debug and Release.

## HOT-01 — hot-path intent class and fixed point, 2026-09-07

The measured handshake exchange is 394 bytes: `client_hello` 96, an
`authority_challenge` with a 48-byte challenge 148, a `client_response` with a 32-byte credential 98 and
`session_accepted` 52. Narrowing every identifier there (session and peer to 16 bits, epoch and tick to 32,
length to 16) would save 44 bytes, 11% of a once-per-connection exchange, and remove no packet: the pinned GNS
source allows 1248 bytes of encrypted payload per packet. Per packet the transport costs 28 bytes of IP/UDP
plus a 7-byte `UDPDataMsgHdr` and a 16-byte AES-GCM tag, so coalescing the four handshake messages into one
packet saves three tags — 48 bytes, more than the entire width optimization. **The handshake was therefore left
wide on purpose**, and the reason is recorded as a contract: the handshake establishes absolute values once so
that per-tick messages can carry relative ones, and a 16-bit session identifier would additionally let a stale
reconnect claim name a live session after 65536 sessions.

`network/fixed_point.h` and `network/intent_wire.h` implement the per-tick class where the same questions cost
ten times more.

- The project's `act::intent` measures 48 bytes (`act::vec3` is three doubles, because `act::real_t` is
  `double` with a comment promising fixed point "when determinism arrives"). Most of it must not travel at
  all, and for reasons of trust rather than size: the connection names the session and peer, the acting entity
  is implied (a client-supplied actor is the classic ownership forgery), and provenance is neither causal for
  the authority nor trustworthy from a peer. `network::intent` is a separate narrow untrusted type, translated
  at one seam which is exactly where ownership/legality/rate validation belongs.
- Registered identifiers travel as a dense index into the sorted registry rather than as 64-bit string hashes.
  Sorting is the agreement: peers need no protocol beyond registering the same things. The test confirms that
  registration order does not change the fingerprint, that adding one identifier changes it and shifts later
  indices, and that duplicate or empty registries are refused. Shifting is safe only because the fingerprint
  is `intent_schema_fingerprint` and a differing peer is refused before the first tick.
- The declared shape of a kind is what lets a batch omit both a count field and per-field presence flags. The
  kind and the offset into the tick window share one byte — the bit packing pays where it multiplies, per
  intent at 60 Hz, rather than once per packet.
- The quantum is declared as a negative power of two. That makes the code a deterministic function of its
  input on every conforming platform and makes decode-then-encode exact by construction rather than by
  floating-point luck. Rounding is half away from zero with no `<cmath>` call: product, truncation and
  remainder are each exact for the bounded range, so the comparison against one half compares exact
  quantities. Golden rows pin the mapping, including that one quantum past a cell boundary a million units
  from the origin still encodes as code 1 — the point of carrying a key.
- Because the only floating-point step lives in that one header, the batch codec moves integers and cannot
  contribute divergence at all.
- Measured budget, asserted in the test rather than derived on paper: one movement intent for one tick is
  **10 bytes**, three ticks of redundant movement **24 bytes**, a full eight-tick window **59 bytes**. Against
  51 bytes of per-packet overhead, resending two extra ticks costs 14 bytes and removes the need for any
  retransmit protocol. The lever for this class is packet count, not field width.
- Refusals proven: undeclared kind on both sides, tick delta outside the packed window, registry index beyond
  the frozen set on both sides, every truncated prefix (a header-only buffer is a legal empty batch, anything
  else partial is refused), a zero-filled buffer, an oversized batch, more intents than prepared storage, an
  unprepared output buffer, and a batch larger than one transport packet. `relative_cell` refuses a cell delta
  which does not fit its width instead of narrowing a distant or forged target.
- Tick reconstruction picks the window candidate nearest the receiver's own tick and refuses only the case
  with no answer — a delta reaching before the first tick.

**Correction to an earlier claim in this campaign:** quantization does not erase divergence, and it is not a
divergence sink. The *correction* erases divergence, by overwriting. Two peers which quantize their own
diverging values can disagree by a whole code at a boundary — a difference of two parts in ten million can flip
a code — so a digest must never be computed over quantized values, and `state_digest` and checkpoint roots stay
on canonical bytes. Worse, snapping a prediction onto a quantized authoritative value injects up to half a
quantum of error even with no divergence at all; a client must compare and ignore an error within one quantum
rather than snap. What the quantum buys is a *derived* correction threshold instead of an invented epsilon.
The cure for libm divergence itself remains fixed point inside the simulation (the NUM branch), not on the wire.

Verification: **10/10 cases, 325/325 assertions** in GCC Debug, GCC Release and Clang Debug. The whole focused
set is **131/131** in GCC Debug and Release. No transform frames, relevant set, cadence schedule, prediction or
correction policy is implemented by this slice.

## SESSION-02 — handshake wire format and ordered exchange, 2026-09-06

`libs/network/include/devils_engine/network/session_wire.h` freezes the handshake format above the neutral
SESSION-01 primitives. It is the first part of the library which deliberately abandons width neutrality: two
installations must agree on exact bytes, so session, peer, epoch and tick travel as fixed 64-bit values and a
project maps its own identifiers onto them. No socket, file, gameplay object or credential format enters it.

- The envelope is magic, envelope version, message type, a reserved byte which must be zero, and a payload
  length which must account for the whole buffer. A shorter buffer is `truncated`, a longer one is
  `trailing_bytes`; no prefix is ever accepted with a remainder ignored. Every truncated prefix of a valid
  hello is refused, and a buffer above the declared budget is refused before parsing.
- Budgets are declared by the library, not derived from the machine. A limit taken from local memory would let
  two installations disagree about what is a legal message, and the larger side would be refused with no useful
  reason. Encoders write into prepared capacity and never grow: an unprepared buffer returns `buffer_too_small`
  with capacity still zero.
- Canonical encoding is enforced on decode, not merely produced on encode. An absent optional must be zero, a
  presence flag accepts only zero or one, an anchor without a session is not a legal claim, and a refusal reason
  must be a named nonzero value. Otherwise two encoders could produce different bytes for one logical message.
- `session_transcript` hashes the exact bytes of the hello and the challenge with explicit length prefixes.
  This is what makes challenge/response more than decoration: the test records a complete exchange, replays its
  credential against an authority whose nonce differs, and the refusal is `identity_rejected` because the two
  transcripts differ. Length prefixing also keeps two split messages from hashing like one whole message.
- `authority_handshake`/`client_handshake` are ordered state machines over decoded messages. Compatibility is
  answered at the hello: each of the six fields produces its own refusal reason, and the policy is asked for
  neither a challenge nor a credential check (both counters stay zero). Identity is answered at the response.
  Out-of-order and repeated messages are `unexpected_message`, and a refusal is terminal — after being refused,
  a client ignores the challenge which arrives afterwards and cannot restart the exchange.
- A returned status describes the decode, not the decision: malformed input returns its wire status, while a
  well-formed compatibility/identity/ordering refusal returns `ok` and reports itself through `phase()`/
  `refusal()`. Both roles fill the reply buffer with exactly one `session_refused`, so a caller always has one
  thing to send and one place to stop.
- Nonces are caller-supplied. The library carries them and owns no randomness policy; a repeated authority
  nonce is a caller fault. The reconnect claim (`resumed_session` plus an optional proven anchor) is carried to
  the authority and observed there, and a granted session which differs from the claimed one refuses with
  `unknown_session` instead of silently rejoining a different session.

`network_session_wire_test` passes **9/9 cases, 329/329 assertions** in Debug and Release, and 9/9 under
Clang as well. The whole focused set is **121/121** in GCC Debug and Release. SESSION-02 adds no credential
issuer/storage, no automatic GNS reconnect, no multi-process execution and no transport binding: the exchange
is proven in-process over byte buffers, which is exactly what NET-LAB-01 then carries over a real connection.

## Toolchain matrix — GCC/libstdc++ and Clang/libc++, 2026-09-06

The verification matrix now has a second toolchain. Building the focused set with Clang 22.1.8 found three
portability defects which GCC 16.2.1 accepts; all three would also have blocked a Windows/MSVC build, which is
the platform the cross-libm work needs next.

| Defect | Nature | Status |
| --- | --- | --- |
| `utils/thread/stack_pool.h`, four `execute() const override` | overriding function laxer than the `noexcept` pure virtual base | fixed by adding `noexcept` |
| `aesthetics/common.h`, `offsetof(class_container<T>, class_container<T>::obj)` | qualified member designator is a GCC extension | fixed to the unqualified member |
| `devils_script` v1.2.1 `system_templates.h:1101` | static `get_user_function_type()` calls non-static `raise_error`; the error does not depend on the template parameters, so Clang diagnoses it at parse time | fixed upstream in v1.3.1, which the root `CMakeLists.txt` now pins: the call became a `static_assert`, which is what the position could legally do. Verified 2026-09-08 by a `clang++ -fsyntax-only` of `acumen_script_test.cpp` with the build's own flags — it parses clean, where before it failed inside the header. A complete Clang/libc++ build of the `tile_frontier` smokes is still not done |

`libs/visage/CMakeLists.txt` and `libs/bindings/CMakeLists.txt` hardcode `${FETCHCONTENT_BASE_DIR}/nuklear-src`
instead of `${nuklear_SOURCE_DIR}`. That is not a compiler defect, but it breaks any build tree which reuses
fetched sources through `FETCHCONTENT_SOURCE_DIR_*`. Closed 2026-09-08 — and the count was wrong: **the same
guess appeared in seven places, not two**, because the pattern `${FETCHCONTENT_BASE_DIR}/<name>-src` reproduces
by copy. `libs/painter` and `libs/sound` guessed `stb-src`/`dr_libs-src`, `subprojects/playgrounds/PF05` guessed
`nuklear-src`, and `libs/input` guessed `glfw3-src` for three imported-library properties on Windows, where a
wrong path is a link error rather than a missing header. All seven now read the variable FetchContent actually
publishes.

The override case was proved separately rather than assumed, because in the default tree both spellings resolve
to the same directory and prove nothing: an isolated probe declaring `nuklear` the same way, configured with
`FETCHCONTENT_SOURCE_DIR_NUKLEAR` pointed at a reused checkout, reports `nuklear_SOURCE_DIR` at the reused tree
while `${FETCHCONTENT_BASE_DIR}/nuklear-src` names a directory nothing populated. In the project's own tree the
generated include paths are byte-identical before and after, which is the intended result: only the overridden
tree changes behavior.

With libc++ actually installed, the native-float micro-corpus is unchanged: GCC/libstdc++ and Clang/**libc++**
both produce 21,505 bytes hashing to `77e13886fa5dd6706add0856195b81e18738d9750f75cc7410bc7f69b74e66e6`, with
`first_difference=none`. This is the expected result rather than evidence of portability: on Linux both standard
libraries call the same glibc `libm`, so the standard-library axis cannot produce a transition difference. The
corpus therefore still says nothing about a different `libm` implementation; only a non-glibc platform can.

Verification scope: GCC Debug and Release focused set 121/121 each, measured after reverting the temporary
`devils_script` stub. Clang Debug networking/serialization set 105/105 plus the new 9/9; Clang builds of the
`tile_frontier` smokes need the upstream `devils_script` fix first, and a Clang Release configuration was not
built. No sanitizer, no whole-project suite.

## NET-08C — common simulation/state proof over GNS, 2026-09-06

NET-08 is complete as a gameplay transport adapter. `network_backend_session_test` runs the same project/session
handlers over an in-memory byte backend and real localhost UDP through `gns_transport`. The backend owns only
opaque byte movement and delivery/lane policy; it does not know actors, baselines, ticks, checkpoints or digests.
The NET07 replication receiver was extracted into `NET07_replication_baselines/session.h` and templated on its
message link. The NET06 causal checkpoint types similarly live in `NET06_in_memory_transport/causal_fixture.h`,
so the old hot-path test and GNS proof exercise one schema instead of copied lookalikes.

### Proved composition

- A small laboratory codec writes an explicit little-endian envelope and bounded actor/delta arrays into
  prepared scratch. It round-trips full baseline, delta and recovery request, and rejects every truncated
  prefix, trailing bytes, unknown envelope type/version and prepared-capacity overflow. This is a fixture wire
  format, not a frozen engine session protocol.
- The common NET07 path loses `100 -> 101`, receives `101 -> 102` without its exact base, sends a reliable
  correlated request, installs full `102`, receives `103 -> 104` first, recovers full `104`, then rejects the
  delayed `102 -> 103` as stale. A duplicated final `104 -> 105` applies once. Both backends end at exactly the
  same project-owned entity set, with three full baselines, one delta, two missing-base detections, two requests,
  one stale frame, one duplicate and no project rejection.
- The common NET06 path sends the authoritative tick-2 float intent only after prediction has reached tick 6.
  It restores checkpoint 0 into staging, replaces the recorded input, replays ticks 1..6 with presentation
  suppressed, transactionally publishes the result and matches the authority's canonical Murmur64 digest.
  Presentation remains at six rather than being emitted again during replay.
- In-memory reliable messages lose their first attempt and exercise the neutral retry model. Each real-GNS
  common scenario begins with 100% native send loss for 40 ms; reliable traffic is accepted, retransmitted by
  GNS after loss ends and recovery converges. Logical stale/duplicate scheduling is explicit at the message
  boundary and is not presented as control over native packet ordering.
- Filling all four prepared reliable bulk slots makes the fifth send return count backpressure. A reliable
  control/recovery message still uses its own lane and arrives. Even after delivery, deliberately unobserved
  GNS payload-release completions retain all four reservations; polling releases reclaims them and permits the
  next bulk send. This pins memory ownership separately from delivery acknowledgement.

### Verification and limits

- Debug and Release focused networking sets: **75/75** registered tests pass in each configuration.
- NET-08C target: **4/4** cases pass in both builds; all four pass five consecutive Debug repetitions.
  Assertion counts depend slightly on the number of asynchronous empty polls and are intentionally not used as
  the stable result. Existing NET07 remains **58/58** and `network_hot_path_test` remains **11/11**.
- Builds used only focused networking targets and their dependencies, at most `-j4`; UDP tests ran outside the
  sandbox. No sanitizer run, allocator replacement, Originator target or whole-project suite was run.
- The test still deliberately allocates fake-project snapshots/deltas and GNS still allocates native message
  headers. Prepared scratch bounds serialization growth; it is not a claim that the whole session allocates
  nothing. Full replication baselines remain recovery data, not routine per-tick world checkpoints.
- NET-08C does not add protocol/schema/content handshake, authenticated player/session identity, authority
  assignment, automatic reconnect, replay across a new connection, multi-process execution or an engine worker.
  These are the next session/laboratory tasks above the now-proven opaque transport.

## GNS direct-IP connection, ports and bind behavior

For the pinned GNS v1.6.0 direct-IP path, one `HSteamNetConnection` is one logical bidirectional relationship
to a peer. A dedicated authority therefore normally owns one connection handle per connected client. It is not
a TCP socket and does not imply one server UDP socket per client: accepted connections share the listener's
underlying UDP socket and its fixed local port while keeping independent reliability, congestion, crypto,
statistics, lanes and remote endpoints.

`CreateListenSocketIP` requires a nonzero port and binds the supplied local address. A cleared/unspecified IP
means wildcard/all local addresses and attempts dual-stack operation; a concrete local IPv4/IPv6 address binds
that address and thereby its OS interface. The API does not accept an interface name or subnet prefix. Bind
chooses where packets arrive; admitting only a subnet requires application/firewall policy before `accept`.

In this pinned implementation, each outgoing `ConnectByIPAddress` currently opens its own UDP socket on an
OS-selected ephemeral local port. Its address family follows the remote address and its source address/interface
follows OS routing. The public API exposes the remote address, not a local-bind option for this path, and the
source explicitly calls possible socket sharing or a selected local address future work. Treat this as current
implementation behavior, not a permanent application invariant. `CreateListenSocketP2P`/`ConnectP2P` are a
different signaling/NAT/relay path with virtual ports; NET-08C exercised direct localhost IP only.

In a normal Docker bridge, the game should listen on the container address/wildcard and fixed UDP port; Docker
publishes/maps a selected host IP/UDP port. A management health/readiness endpoint needs its own independently
configured bind and must not consume a gameplay connection or peer slot. This follow-up adds that planned work
as **SERVER-02** in `NETWORKING.md`; no HTTP/probe implementation is part of NET-08C.

## NET-08B — endpoint lifecycle, 2026-09-05

`gns_transport` now owns IP listen/connect/accept in addition to the original adopt-only boundary. A prepared
`gns_dispatcher` is shared by all endpoint transports using one native interface. It borrows the runtime,
does not create a thread, and does not replace a process-global callback. All callback pumping for this
interface must use its `pump()` on the transports' owner thread. Dispatcher lifetime must cover registered
transports; explicit shutdown unregisters early. A second dispatcher/raw `RunCallbacks` on the same interface
is outside this contract.

### Contract and memory

- Listen/connect install a **static** callback as a creation option, before an event can be queued. Dispatch
  examines currently registered owners and native connection/listener handles. No transport pointer or
  callback-time `ConnectionUserData` snapshot is stored in GNS. This avoids retaining destroyed owners in
  delayed callbacks; routing slots can be reused after unregistering. Creation options use a fixed stack array
  (at most 64 caller options); overriding the routing callback is an ordinary refusal.
- Dispatcher registrations, listeners and peers have fixed preparation-time capacity. Incoming connections
  reserve peer slots and receive exact lane/buffer configuration **before** `accept`. Capacity/configuration
  failure closes the native handle and increments `refused_incoming_count`. A pending incoming connection
  consumes capacity too; it cannot bypass the limit by waiting for admission.
- Admission stays external: a `needs_accept` observation requires a prompt explicit `accept` or `close`.
  Native acceptance can fail if the peer has already disconnected; this is a return status, not an exception.
  No authentication relaxation is supplied by the wrapper. Local tests explicitly pass
  `IP_AllowWithoutAuth=2`; this checks lifecycle/encrypted transport, not authenticated public-session identity.
- Events remain **coalesced observations per peer**, not a lossless transition FIFO. Small/empty output does
  not consume an unreported current state. Terminal handles retain their peer slots until explicit close.
  No wrapper event vector grows in the pump. GNS's own `RunCallbacks` drains its native queue and has no
  exposed work budget: fixed wrapper storage is not a bound on native callback work or native allocations.
- `close_listener` also invalidates all its accepted/pending children, matching GNS's native contract.
  `shutdown` unregisters routing, closes connections/listeners/poll group without linger, and is final and
  idempotent. It emits no redundant local-close notifications. Existing received leases and send payload
  release storage can outlive shutdown; GNS runtime lifetime must still cover the leases.
- Reconnect is a fresh `connect`, reusing prepared tables/slabs with new generational IDs. It does not restore
  a player/session, choose authority or replay checkpoints. Those remain above opaque transport.
- GNS v1.6.0 refuses port zero for listeners. Tests select a free port from a bounded localhost range.
  Rebinding immediately after closing a used listener was also observed to refuse: native connection teardown
  can retain the underlying UDP socket. The adapter returns `backend_rejected`, not an exception or hidden retry.

### Reproduced verification

- Debug and Release focused networking sets: **71/71** registered tests pass in each configuration.
- Adapter target: **15/15** cases, **2074/2074** assertions in both builds. Its full set also passes **five
  consecutive Debug repetitions**. No new sanitizer run was performed, as requested.
- Six new cases cover: ten real UDP connect/accept/message/close/reconnect cycles; two simultaneous listeners
  routed through one dispatcher with single-element event output; pending-admission capacity and explicit
  rejection; twenty shutdowns before callback pumping followed by registration/listener reuse and a real new
  connection; late accept after remote close and invalid inbound budgets; dispatcher/listener/option limits.
  Foreign/stale IDs, terminal retention and listener-child invalidation are asserted directly.
- Existing nine ownership/lane/socket-pair cases still pass, including received leases outliving adapter
  destruction, out-of-order payload release, complete packet loss recovery and high-priority-vs-bulk delivery.
- Only `network_gns_transport_test` and dependencies were built, at most `-j4`; focused CTest runs used `-j4`.
  UDP tests ran outside the sandbox. No Originator source/target was changed/built, and no whole-project suite,
  Internet peer, P2P/signaling, authentication service or new allocator was exercised.

Build/test commands are the same focused commands recorded below for NET-08A. The implementation contract is
also documented in `libs/network/README.md` and the public header.

NET-08C is completed above. Engine-worker integration remains separate from this caller-driven lifecycle slice
and should reuse existing bounded FIFO channels. Native allocator profiling/replacement and a fresh sanitizer
pass are deferred.

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

- **NET-08B:** completed by the follow-up above. Later engine-worker integration should use existing bounded
  channels; it is not hidden in the endpoint wrapper.
- **NET-08C:** completed by the common backend/session proof above.
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
