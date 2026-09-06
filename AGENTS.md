# Project Memory

This repository is the author's experimental game engine / framework. It is a large WIP codebase where many engine ideas are being tested together, so prefer reading the nearby implementation and preserving existing exploratory structure over imposing a new architecture too quickly.

## Current Focus

- SESSION-02: THE HANDSHAKE BECOMES BYTES, AND A SECOND TOOLCHAIN (2026-09-06).
  `network/session_wire.h` freezes the handshake format and deliberately DROPS width neutrality: two
  installations must agree on exact bytes, so session/peer/epoch/tick are fixed 64-bit and a project maps
  its own ids onto them. Envelope = magic, version, type, reserved-must-be-zero, exact payload length; a
  declared length which does not account for the whole buffer is refused in BOTH directions, never a prefix
  with an ignored remainder. BUDGETS ARE DECLARED BY THE LIBRARY, not derived from the machine — a local
  limit would make two installations disagree about what is a legal message. Encoders write into prepared
  capacity and never grow. CANONICAL FORM IS ENFORCED ON DECODE, not just produced on encode (zeroed absent
  optionals, boolean presence flags, named nonzero refusal reasons), because otherwise two encoders could
  produce different bytes for one logical message. `session_transcript` hashes the exact hello and challenge
  bytes with length prefixes — that is what makes challenge/response more than decoration: a credential
  recorded from another exchange is refused as identity_rejected because the transcripts differ (proven).
  Both roles are ORDERED state machines: compatibility at the hello BEFORE any challenge or credential check
  (all six fields give their own reason, policy counters stay zero), identity at the response, everything
  else `unexpected_message`, and a refusal is TERMINAL — a refused client ignores the challenge that arrives
  afterwards. A returned status describes the DECODE, not the DECISION. Nonces are caller-supplied; the
  library owns no randomness policy. `9/9` cases, `329/329` assertions; focused set `121/121` in GCC Debug
  and Release. NOT included: credential issuer/storage, automatic GNS reconnect, multi-process execution.
  SECOND TOOLCHAIN: building with Clang 22.1.8 found THREE defects GCC accepts, all of which would also
  block a Windows/MSVC build — `stack_pool.h` overrides laxer than their `noexcept` pure virtual base,
  `offsetof(T, T::obj)` with a qualified member designator, and (open, upstream) a static devils_script
  template calling non-static `raise_error`. Also `libs/visage`/`libs/bindings` hardcode
  `${FETCHCONTENT_BASE_DIR}/nuklear-src` instead of `${nuklear_SOURCE_DIR}`. With libc++ finally installed
  the native-float corpus is UNCHANGED (`77e13886…`, first_difference=none) — expected, not reassuring: on
  Linux both standard libraries call the same glibc libm, so that axis CANNOT produce a difference. Only a
  non-glibc platform can answer the cross-libm question.

- GENERAL SERIALIZATION AND COMPOSITE ACTOR CHECKPOINTS (2026-09-06).
  Canonical byte codec/adapters now live in `utils/serialization.h`; section composition moved from
  `network/state_schema.h` to `utils/state_schema.h` (network names remain aliases). Generic
  checksum/zstd/preview packaging is `utils/serialization_sink.h`; aesthetics owns only the ECS
  projection and world wrappers. Serializer payload/envelope buffers use `std::byte`. Zpp and its build dependency
  are removed; Glaze stays because demiurg still uses JSON.
  `tile_frontier/core/actor_checkpoint.h` replaces actor_world_slice::save/load, without wrappers:
  timeline + world + actor-private causal counters. Tick/game time occur ONLY in the timeline
  section. Write at a committed boundary, with matching clocks and no pending intent; buffers are
  caller-owned and retain capacity. Growing `write` and byte-capacity-bounded `try_write` are
  distinct; map sorting, adapters and staging may still allocate.
  Restore builds a detached ECS world and stable-address `actor_brain_runtime` BEFORE publishing.
  The runtime owns interdependent act/GOAP/FSM/prefab registries; prefab callbacks capture that
  stable runtime, never the temporary staging actor. Publication moves ownership, resets lazy
  address-bound systems and emits the world event last. Subscribers must not throw. Failed decode
  or validation leaves live world and timelines unchanged. The new actor document deliberately
  breaks old actor save compatibility, while the inner ECS/envelope byte grammar is retained.
  Resume smoke checks nonzero timeline remainder (tick 61, scale 7:3), repeated restore, new prefab
  spawn and 120 subsequent ticks with intents against the WHOLE canonical document. Checkpoint
  audit now includes timeline/actor sections and tests refusal after staging every prefix as well
  as invalid rate/budget. Generic codec tests link utils only.
  Final parallel tests exposed an existing small-world tail race: kd_tree::build_parallel returns
  without a pool barrier on its synchronous path, while actor_batch was already submitted. The
  actor tick finalizer now explicitly drains/waits on BOTH paths before returning/checkpointing.
  Canonical traversal/write overloads now propagate section-writer failure; vector convenience
  returns empty on refusal, and one-shot make_state_digest refuses to return a prefix root.
  Writer failure is sticky even for backpatch. Utils-only tests check growth, capacity reuse,
  shrinking payloads without stale tails, alias refusal and failure propagation.
  Verification: 60/60 focused Debug tests (utils serialization, ECS serialization, network
  schema/digest/replay/session/hot-path/backend and five tile_frontier smokes); tile_frontier builds.

- SESSION-01, THE FIRST LAYER ABOVE THE CLOSED NET-08 TRANSPORT (2026-09-06). Focused networking set `81/81`
  in Debug and Release; new `network_session_test` `6/6`, `76/76`. `libs/network/session.h` fixes three
  borders. EXACT INSTALLATION COMPATIBILITY is one SHA-256 over a canonical manifest containing the
  product/version and COMPLETE resolved core, project and mod file bytes; filesystem order is erased, explicit
  mod load order is not. The hash is prepared before connect and is compatibility evidence, NEVER identity.
  Handshake compares protocol, content, state/intent schemas and numeric profile BEFORE calling an injected
  authenticator, whose principal is logical and outlives a `gns_peer`. Membership names session, local peer,
  authority peer, principal and authority epoch; stale/future epochs and wrong session/authority are refusals.
  RECONNECT IS A TRANSACTION: checkpoint `K` means committed state AFTER K, every sealed bundle `K+1..N`
  including empty ticks is required, roots at K/N are checked in detached staging, replay suppresses
  presentation, and only then one noexcept publish may touch the live world. The test checkpoint carries tick,
  PRNG cursor, entity counter and timeline remainder; deliberately omitting the PRNG cursor is caught at root K
  and publishes nothing. The real tile_frontier resume/time proofs were rerun in both builds: actor state stays
  byte-identical for 120 resumed ticks and wall-time partition does not change its checkpoint. ONE GAP IS NOW
  NAMED: `actor_world_slice::save()` and `utils::timelines_causal_state` are two checkpoint owners today; a
  network checkpoint envelope must compose both or it omits the clock remainder. This answers the join question:
  checkpoint+intents is sufficient IFF the checkpoint contains every cause of the next tick. Wire
  challenge/response, credential storage, automatic GNS reconnect and a real new-connection recovery exchange
  remain the next SESSION-01/NET-LAB-01 integration slice.

- ADJACENCY ON THE DEVICE: TWO REFUSALS, BOTH MEASURED, AND FOR DIFFERENT REASONS (2026-09-06).
  `531/531` project tests. The task was "write the bodies for `label_adjacency`/`sphere_adjacency`".
  THE MEASUREMENT ANSWERED BEFORE THE MECHANISM STARTED, and the two tools got different answers.
  WHAT THEY COST (GN02 profile, 262144 cells, `28.6` s total): `sphere_adjacency` `411.3` ms, `1.4%`,
  ONE call in step `topology`, with nothing to join in a run (it sits among `sequential` floods).
  `label_adjacency` DOES NOT APPEAR AT ALL — the whole `scatter` aperture together is `813.7` ms
  (`2.8%`) and half of that is the sphere. For scale, the `climate` step's `queue` is `2674.2` ms
  (`9.3%`) in 96 calls that are all `no_body`.
  `sphere_adjacency` IS A REFUSAL ON DISTANCE PRECISION. The neighbour order is canonical — distance,
  ties by cell index — so any other search structure gives THE SAME GRAPH as long as the SET of
  nearest matches. The host computes distances in `double`, a shader would use `float32`, and the only
  question is how far the boundary between the 6th and 7th neighbour sits from the last bit. Measured
  on the lattice itself (positions are identical on both paths — the field is `v3`, already float32,
  so only the distance arithmetic can differ): at `16384` cells, zero cells under one ULP and the
  narrowest gap is `116` ULP; at `65536`, one cell under one ULP and the narrowest gap is `0.81`.
  THE SETS MATCHED AT BOTH SIZES, BUT THE MARGIN VANISHED under a fourfold increase. At a million
  cells there will be several such cells and whether they agree is LUCK, not a property — i.e. "the
  graph may differ on a handful of cells and you don't know which", a different world under the same
  seed. `double` on the device needs `Float64`, which Vulkan does not guarantee. Same shape of refusal
  as `sphere_points`, different cause: there the spiral diverged, here it is the decision about who
  the sixth neighbour is.
  `label_adjacency` IS NOT A PROHIBITION BUT A COST OF ENTRY. Determinism is not involved — the work
  is integer and does not run into float32. The FORM does: a canonical CSR is count-with-duplicates,
  prefix sum, fill, dedup, and a second prefix sum with compaction — FIVE PASSES, TWO SCANS AND THREE
  SCRATCH BUFFERS that the call declaration does not have. A tool today has ONE body and ONE pass, and
  there is no device scan in the library at all. So this is not "write a body", it is "introduce the
  form of a TOOL MADE OF SEVERAL PASSES", and that should be built for whoever pays for it — this tool
  does not: it is invisible in the profile.
  BOTH REFUSALS ARE NOW WRITTEN NEXT TO THE TOOLS with their numbers, and the float32 measurement is a
  test, so the claim stays checkable rather than becoming folklore. WHAT THE MEASUREMENT SAYS IS NEXT:
  the `climate` step's `queue` at `9.3%` — six and a half times the adjacency, and not a new mechanism
  but a refusal reason to read.

- A DISK SPIR-V CACHE, A NAMED OWNER FOR THE COMPILER, AND WHAT UNEVEN CSR ACTUALLY COSTS
  (2026-09-06). `530/530` project tests. Three items off the GPGPU list, and two of them came back
  with a different answer than the list expected.
  THE DISK SPIR-V CACHE WAS UNDERVALUED BY ITS OWN ESTIMATE. §5 п.5 shelved it because, after the
  `shaderc` fix, compiling one shader costs `1`–`2.3` ms and there was little to save. That counted
  the wrong quantity: THE REAL SAVING IS THE 90 ms OF GLSLANG STARTUP THAT A WARM RUN NEVER PAYS.
  `shaderc::Compiler`'s constructor raises that state regardless of what is compiled, so if every
  program comes from the cache the compiler is never constructed at all — hence the design: the state
  is raised LAZILY, on the first real compilation, not in the constructor. Measured on GN04 (time to
  first result — context, six programs, first run): `143` ms without the cache, `144` cold, `37` warm.
  `3.9x`, and the world is unchanged (`22/22` on `--verify` either way).
  THE KEY DELIBERATELY EXCLUDES THE INCLUDED FILES, and that is the interesting part. Which files
  `#include` pulls in is known only AFTER preprocessing, i.e. after glslang is up — the very thing a
  warm run must not pay. So the entry stores the LIST of files it substituted with a content hash
  each, and a read re-reads and compares them: files are read, glslang is not started. Editing an
  included file MUST be a miss, or the engine runs a shader other than the one written and you cannot
  see it in the picture. Three smaller rules, all so the cache cannot become a failure: the key also
  lives INSIDE the entry (a hash collision must be rejected, not executed), the format version is in
  the header (a changed format is a miss, not garbage), and AN UNUSABLE DIRECTORY BEHAVES AS NO CACHE
  with one complaint per compiler — a cache is a speed-up, not a reason a shader fails to build.
  THE COMPILER GOT A NAMED OWNER INSTEAD OF A `thread_local`. It lived as a function-local
  `static thread_local` — that fixed the measured cost, but with hidden CACHING STATE whose owner and
  moment of death are unnamed. Now `painter::shader_compiler` is passed in from outside, owned by
  `graphics_base` (the render thread, next to the pipeline cache and for the same reason),
  `compute_context`, and the assets system — ONE compiler for a whole prefix batch, exactly where the
  win is. `shader_crafter` stays the one-shot setup of ONE compilation; these are two lifetimes of
  different length. THE OTHER THREE `thread_local`s WERE CHECKED AND KEPT, because they are a
  different kind: `gns_dispatcher::active_` is a trampoline for a third-party C callback with no user
  pointer, set and cleared around one `pump()`; `catalogue::current_record_context` is an RAII AMBIENT
  SCOPE, which is the whole point of it (otherwise it goes into every signature); the third is test
  scaffolding. Exactly one was the wrong kind.
  UNEVEN CSR COSTS `20`–`43%`, NOT MULTIPLES — the gap §5 п.5 named and never measured. Three layouts
  with the SAME arc total (262144 nodes, 1572864 arcs, 37449 of degree 24 against 3): even degree six
  `0.270` ms per pass, spikes CLUSTERED `0.330` (`1.20x`), spikes SPREAD `0.387` (`1.43x`). A 64-wide
  wave absorbs one spike; the catastrophe implied by `24:6` is not there. AND THE LAYOUT MATTERS MORE
  THAN THE UNEVENNESS: spread spikes cost twice the excess of clustered ones (`0.117` vs `0.060` ms)
  because they land in EVERY wave — and in the real planet graph they are clustered, i.e. the cheaper
  case. The cost of one pass is taken as a SLOPE (a queue of 1 pass against 81, both fields resident
  so transfer is identical): the full round trip here is almost entirely transfer — the arc buffer is
  six megabytes — and the first version of this measurement drowned in it and reported "unevenness
  costs nothing". CAUGHT BY A CHECK: that first version also printed "81 passes cost the same as one",
  which was TRUE — the call's range was never declared and NOT ONE pass ran. The check that caught it
  is "the field after the passes is not a constant". A measurement without one prints confident
  numbers about nothing — the same lesson as the buffer-copy defect.
  AND WHAT THE "FOUR MISSING BODIES" TURNED OUT TO BE. `value_noise`, `poisson_seeds`,
  `label_adjacency`, `sphere_adjacency` were listed as honest `no_body`. Checking the apertures says
  otherwise: `poisson_seeds` is `sequential` — REFUSED by construction, not missing; `sphere_adjacency`
  and `label_adjacency` are `scatter` and both BUILD A CSR (count, prefix sum, fill), which is three
  passes and wants a "tool made of several passes" form that does not exist. Only `value_noise` was
  a real missing body, and it is now written — closing the separate 64-bit PRNG item along the way.
  Its lattice hashes with splitmix64, and GLSL has no 64-bit integers, so they are assembled from a
  pair of `uint` via `umulExtended`; a different hash would be a DIFFERENT FIELD that looks like the
  same noise. It needed one declaration: the RAW call seed in the header (`raw_seed_lo`/`raw_seed_hi`)
  — the folded 32-bit `seed` is irreversible and cannot be repurposed, because every existing body and
  the whole `ds` translation read it and changing it would change their randomness, i.e. the world.
  CHECKED WHERE THE HASH IS WHAT IS ACTUALLY CHECKED: at frequency exactly 1 the sample coordinate is
  an integer, interpolation degenerates, and the field value EQUALS the lattice value — no float
  operation left between splitmix64 and the result. `0 of 262144` differ there. With interpolation it
  is `1.79e-07` (one ULP), four octaves `2.38e-07`. `26 of 46` tools now have a body.

- THE PUSH-CONSTANT BUDGET IS THE LIBRARY'S DECLARATION, NOT A QUESTION TO THE DEVICE (2026-09-06).
  Originator tests `142/142`, translator `12/12`. `maxPushConstantsSize` was never queried or checked
  anywhere — the last cheap gap the GPGPU list still named. THE FIX IS NOT TO QUERY IT. The limit now
  lives as a constant (`device_push_limit = 128`, Vulkan's GUARANTEED minimum) and the device's own
  value is asked for NOWHERE, for the same reason the work-group limits next to it are held that way:
  something clamped to the author's machine fits for the author and refuses for the player — and for a
  generator a refusal is not "slower", it is A DIFFERENT WORLD UNDER THE SAME SEED, because a queue
  that does not reach the device is computed on the CPU. What fits the guaranteed minimum fits
  EVERYWHERE, so "will this queue run on a device" has ONE answer on all machines.
  The layout is the header (`count, begin, extent_x, extent_y, seed`, 20 bytes) plus one `float` per
  parameter, i.e. `27` parameters (`device_param_limit`, computed from the header rather than written
  as a number). CHECKED IN ONE PLACE — `build_device_shader`, where the push constant is declared —
  so native bodies and translations get the same limit; the translator additionally names its OWN
  reason (that many `ctx:arg:`) and treats it as a REFUSAL, so the queue stays on the CPU and computes
  the same thing. The test pins the BOUNDARY from both sides — a limit that never fires promises
  nothing — plus the `128 == header + 27 * 4` identity.
  THE HEADROOM IS MEASURED, NOT HOPED: the most parameterised tool in the library declares `5`
  (`remap`, `blend`), five times under the ceiling. Which retires half of §7.1's argument: the uniform
  buffer it proposed is NOT needed for parameter delivery. It stays wanted for the other thing — a
  shared queue context (step seed, chunk key, the salts of every call site) — which does not fit 128
  bytes and should not: a push constant carries what changes per call. And the sensor for when that
  moment arrives now exists, because the refusal names the number. FIXED WHILE THERE: §7.1 still said
  "31 arguments fit", from before the header grew.

- FOUR TOOLS, AND WHAT "INTEGERS IN THE SHADER" TURNED OUT TO MEAN (2026-09-06). Originator tests
  `182/182`. Three bodies written and one REFUSED WITH A NUMBER, plus the question the user asked
  alongside them.
  `graph_slope` and `graph_vote` are the `graph_blur` shape again — buffers and one CSR loop — with one
  thing that had to travel and nearly did not: the vote's TIE-BREAK ON THE SMALLER LABEL. With equal
  weights the answer must not depend on the order the arcs are visited, and that rule belongs to BOTH
  paths, not to a hope that the device happens to agree. Slope agrees within `1e-6` (a float sum), vote
  agrees BITWISE (it is a choice, not arithmetic).
  `polyline_distance` needed two mechanisms that did not exist. THE HOST'S PREPARED INDEX IS NOT ON THE
  DEVICE — only declared bindings go there — so the body REBUILDS the segment in place from the same two
  buffers the preparation reads, and the bounding-box cull stays a CONDITION OF APPLICABILITY rather
  than an optimisation. And a WORD-VALUED parameter (`metric = "chebyshev"`) does not fit a push
  constant, which carries `float`; a numeric synonym would be two ways of saying one thing, so
  `device_param` grew `resolve`, and the tool itself turns the name into a number, once, where its CPU
  body already lives. Worst deviation `5.7e-06` (euclidean) and `3.8e-06` (chebyshev) at 262144
  elements.
  `sphere_points` IS REFUSED, AND THE REFUSAL IS MEASURED. The Fibonacci spiral's angle comes from the
  fractional part of `i * φ`, of which `float32` keeps four bits: at a million points the angle is off
  by `0.9955` OF A TURN, i.e. it is noise. Fixed point would fix it and CHANGE THE LATTICE ON BOTH PATHS
  — a different world for everyone who computes it — and buy nothing, because `sphere_adjacency`
  (`scatter`) comes next and there is no run to join. `25 of 46` tools now have a body.
  THEN: "can integers from `ds` be written as integers in the shader?" — and the answer is neither yes
  nor no. IN AN ORIGINATOR PROGRAM EVERY FIELD READ IN `ds` IS A `double`: the accessor is registered
  that way whatever the field's kind. So integer arithmetic in the shader would DIVERGE from `ds`, not
  converge — `a / b` would become integer division, and a sum would wrap at 2^32 where `double` is exact
  to 2^53. The naive "yes" makes it worse. BUT `float32` HAS A 2^24 CEILING, and above it adjacent
  integers merge: an identifier field wider than sixteen million COLLIDES on the device, and that is a
  real disagreement with a `ds` that reads exactly. So the line is drawn by semantics: THE LEAF STAYS
  INTEGER — an integer literal or a read of an integer-kind field, in COMPARISONS (no epsilon) and in an
  IDENTITY STORE (no round trip through `float32`); everything arithmetic stays floating, as `ds` has
  it. Comparing two leaves as integers cannot change an answer — in `ds` it compares two EXACT doubles,
  and an epsilon on integers is equality — but it stops lying above 2^24. CHECKED WHERE IT SHOWS: 4096
  pairs of adjacent integers starting at 2^24; `ds` says "not equal" for every pair and the device now
  does too, where before the fix it would have said "equal" for ALL of them. Full typing was refused for
  the standing reason: it would mean reproducing `ds`'s overload resolution (`5 / 2` is integer or
  double division there depending on the expected type), i.e. a SECOND type system to silently drift
  from the first.

- REDUCTIONS, AND A SILENT DEFECT THE SUM CAUGHT (2026-09-06). Device-queue tests `15/15`. Item 2 of
  the GPGPU list: §5 п.6 had measured that GN04's histogram cost MORE than the labelling it summarised
  (`7.43` vs `4.65` ms) although it does incomparably less work — the cause being CONTENTION, a quarter
  million pixels fighting over dozens of counters. The named form is now built: a group accumulates in
  SHARED memory and one atomic per bucket per group reaches the global buffer.
  IT NEEDED A DECLARATION THAT DID NOT EXIST — `device_whole_group`. A barrier not reached by every
  invocation is undefined behaviour, and the generated `main` cut the extra ones off with an EARLY
  RETURN; the last, partial group of a range would hang or count garbage, and only at some data sizes.
  A body declaring this now gets an `in_range` flag and decides for itself. Not named `active` — that is
  a GLSL reserved word, and glslc caught it, which is exactly what type checking was delegated for.
  MEASURED (isolated one-call queue, 262144 elements, median of three): narrow histogram `6.57` ->
  `5.16` ms (`~21%`, and that is the WHOLE round trip including a megabyte of transfer, so the dispatch
  share is larger); wide histogram `5.77` -> `5.48` ms, i.e. unchanged as it must be — the fallback path
  runs the same code, and the match inside noise confirms the measurement measures what changed. ON
  GN04'S REAL QUEUE THE DIFFERENCE IS INVISIBLE (`5.31` vs `5.35`–`5.40` ms at `512²`): there the
  histogram is a small part and the Voronoi labelling dominates.
  AND THE SUM CAUGHT A SILENT DEFECT OLDER THAN THIS TASK. The histogram check verified not only "CPU
  equals GPU" but also "every element was counted" — and the second one fired: the total came out
  exactly twice the element count. A BUFFER COPY SHARED MEMORY WITH ITS ORIGINAL: `buffer` holds both a
  byte vector and a RAW pointer to the aligned start INSIDE it, and the implicit copy duplicated the
  vector and then copied the pointer, which still looked into the source. Both buffers stayed valid and
  the copy wrote into the original. WORST OF ALL: a two-path comparison done through a buffer copy was
  comparing memory WITH ITSELF and always agreed — that is where this session's "the noise matches
  bitwise" came from. Three checks were verifying nothing; after the fix two of them failed at once, one
  on a real disagreement. The fix is not just recomputing the pointer: a vector copy keeps bytes at
  their old OFFSETS while the new allocation's aligned start lands elsewhere, so the data is copied
  START TO START. THE LESSON: a two-path comparison needs a check that FAILS when the wrong things are
  being compared — here it was the sum, a quantity independent of whose bytes get read.
  CORRECTED, THEREFORE: our noise does NOT match bitwise across paths. Measured properly it is one last
  bit typically, `3.1e-06`/`3.4e-06`/`4.25e-06` worst over four octaves, with `8`–`15%` of elements
  exact. Both hypotheses were tested and neither explained it (the `mix` formula, and `-ffp-contract=off`
  on the host), so the divergence lives in the driver — which is precisely what §4.2 declares.

- MEMORY: DECLARED AGAINST OCCUPIED (2026-09-06). Originator tests `143/143`. Closes the audit's main
  open finding (§10.5): the library promises a generator can name its memory cost BEFORE the run, and
  the promise rested on the sum of declared buffers while the machine needed a third more.
  FIRST THING FOUND: NOBODY MEASURED THE OCCUPIED SIDE. Every number in every report was a sum of
  DECLARATIONS; the process peak was asked nowhere, so the promise had nothing to be compared against.
  Hence `utils::peak_resident_bytes()` — Linux reads `VmHWM` from `/proc/self/status` as a FILE rather
  than through `getrusage` (whose `ru_maxrss` means different units on different systems, exactly the
  class of drift this project hunts), Windows uses `GetProcessMemoryInfo`, and everything else returns
  ZERO meaning "not measured here", never "no memory used".
  SECOND: A TOOL NOW DECLARES ITS TEMPORARY COST (`tool_description::footprint`), as a function OF THE
  CALL, because table sizes depend on element count and parameters and only the tool knows them —
  exactly like the aperture.
  THIRD, AND THIS IS THE DECISION THAT MATTERED: "NOTHING" AND "UNKNOWN" ARE DIFFERENT ANSWERS. The
  first report said "78% of the hours undeclared" while almost all of those calls allocate not one
  byte — a tool's silence read as a gap. So a tool with nothing to declare declares it EXPLICITLY
  (`no_temporary_memory`), and the report shows only the real gap and names it, like the missing-body
  list.
  NUMBERS ON THE SAME MILLION CELLS THE AUDIT FLAGGED: declared buffers `432` MB -> `452.8` MiB, the
  largest temporary table `not declared by anyone` -> `168.0` MiB (`graph_flood`), process peak `562`
  MiB measured externally -> `580.0` MiB measured by the generator itself, and the unexplained THIRD ->
  ZERO: declared now covers the peak with a `40.9` MiB MARGIN. The margin says what it should — the
  estimates are UPPER bounds (arc count for adjacency, queue length for the flood) — so the report
  prints the relation BOTH WAYS: "not accounted for" when declared is below the peak, "declared with a
  margin" when above. Staying silent in one direction would hide the very thing the number is for.
  THE REMAINING GAP IS NAMED AND IS NOT IN THE ENGINE: all `21` undeclared calls are GN02's own tools
  (`hotspot_tracks`, `insolation`, `plate_velocity`, `wind_field`, `plate_interaction`,
  `axis_component`) plus the FastNoise2 wrapper — the playground author's territory, and the report now
  names them instead of leaving one to guess.

- NOISE OF OUR OWN, CALLED OUR OWN (2026-09-06). Originator tests `142/142`. §11 ended on "what is left
  is noise"; FastNoise2 itself cannot move — its node tree arrives as an ENCODED STRING and is evaluated
  by its own library, so porting it would mean porting a graph interpreter. So the core got its own:
  `noise_value` (lattice value), `noise_perlin` (gradient), `noise_cellular` (Worley F1), all with
  octaves, written TWICE (C++ and GLSL) and declared as ours — the field they compute equals FastNoise2
  at no seed, and never will. `position_grid` got a body too, so the device builds the lattice itself
  and positions never cross the boundary.
  ONE HASH ON BOTH PATHS (`utils::shared::prng2`, already in the shader preamble) and BOTH PATHS COMPUTE
  IN float32 — deliberately, because computing the noise in double on the host would produce different
  numbers exactly where identical ones were available. (THIS ENTRY ORIGINALLY CLAIMED BITWISE AGREEMENT,
  `worst deviation 0`; that was an ARTEFACT of the buffer-copy defect found the same day — see
  "REDUCTIONS, AND A SILENT DEFECT THE SUM CAUGHT" above for the honest numbers.)
  THE LATTICE IS ALWAYS THREE-DIMENSIONAL and a two-component position field means the `z = 0` SLICE:
  one implementation instead of two that would drift. The cost is named — a flat field pays eight hashes
  where four would do.
  THE PRICE DOES NOT FLATTER, and that is the honest half: on the CPU our noise is NINE TIMES slower
  than FastNoise2 at equal threads (`28.1` vs `3.2` ms on 1024², four octaves) — SIMD there, a scalar
  pass through the general accessor here, plus the 3D lattice. ON THE DEVICE IT PAYS: lattice plus three
  noise fields on 262144 elements is `30.6` ms INCLUDING transfer against `223.3` ms of one CPU thread.
  So the rule for a config author is: take our noise where the queue is declared for a device; where
  everything runs on the CPU, FastNoise2 stays faster. Named and NOT done: a separate 2D path (four
  corners, two hash rounds) should give about `3x` on flat fields — but it is a DIFFERENT field, not the
  same one faster, so it would have to be declared out loud.
  TWO GENERAL MECHANISMS FELL OUT: `tool_description::device_prelude` (functions a body calls — noise
  does not fit in an expression, and `main` cannot declare them) and per-binding component defines
  (`ORIGINATOR_IN_0_COMPONENTS`), because a body working over both a plane and a volume must ASK rather
  than assume the component accessor exists.

- THE TWO BLOCKERS THE MEASUREMENT NAMED, AND WHAT THEY TURNED OUT TO BE (2026-09-06). Originator tests
  `178/178` (two new device cases); the two `tile_frontier` failures in the full run belong to the
  parallel checkpoint work editing `libs/aesthetics/serialization`, not here.
  THE FIELD KIND WAS NOT A CONFIG PROBLEM. Widening GN01's labels by hand worked (`4.6%` -> `63.8%`
  ready) but was the wrong fix: a narrow kind exists FOR COMPACT STORAGE ON THE HOST, and making the
  author give that up for someone else's device pays memory exactly where it was wanted. §6.1 had
  already declared the right thing — the device copy is a CACHE, and a cache need not repeat the
  layout of the truth. So a narrow field now lives on the device in the 32-bit kind of the same MEANING
  (`ub`/`us` -> uint, `ib`/`is` -> int, `sf`/`c` -> float), and the conversion at the border is done by
  the SAME `load_component`/`store_component` the host accessor uses — the two paths agree BY
  CONSTRUCTION, clamping and rounding included. Three consequences had to be said out loud: the SHADER
  clamps (by the ORIGINAL kind's range, because a `resident` field is never downloaded and a `ub` that
  became `uint` would happily hold 300); `device_integer_ready` is no longer needed for narrow integers
  (float32 is exact to 2^24, and `ub`/`us`/`ib`/`is` fit in 16 bits — so the BINDING decides, not the
  tool); and a latent silent defect closed itself — a `ui1` field read through a filter used to become
  an `r32f` image with its bits copied verbatim, i.e. read as something else entirely. Result WITHOUT A
  SINGLE CONFIG EDIT: narrow `74.6%` -> `0%` on GN01, `26.5%` -> `0.1%` on GN02.
  BODIES ARE MEASURED BY THE RUN THEY UNBREAK, and the profiler now prints the work list by name.
  Written: `classify`, `decay`, `ratio`, `index`, `lookup`, `graph_blur`, `graph_frontier` — 18 of 43
  tools now have one. The graph ones are three buffers and one loop (a raster window is meaningless on
  a sphere); checked against the CPU path in a new test — the blur agrees within `1e-6` (a float sum)
  while the frontier and the lookup agree BITWISE, because those are integer decisions and a difference
  there would mean reading the wrong neighbours. Named cost: CSR occupancy is UNEVEN (degree up to 24
  against six on average), and that wants measuring on a real graph rather than assuming.
  TOGETHER (the only row that promises anything is the last one — work that can actually leave whole):
  ready `4.6% -> 82.1%` / `4.7% -> 24.4%` / `50.2% -> 51.1%`, and IN RUNS >= 2 `0% -> 20.3%` (GN01),
  `1.8% -> 22.4%` (GN02), `51%` (GN03). WHAT IS LEFT IS NOISE: FastNoise2 is the main remaining
  `no_body` in all three generators, and it is the only work that would still move the number much.

- THE DEVICE SESSION WAS CANCELLED BY THE MEASUREMENT (2026-09-06, same day, after the profiler).
  `516/516` project tests. THE UNIT OF RESIDENCY IS THE QUEUE, and making the region bigger means
  DECLARING more work in one queue, not inventing a new concept. The profiler prints what a run is made
  of, and GN03's most expensive run read `fill fill fill queue[17] queue[17] queue[17] queue[17]
  queue[17]` — eight calls with NOTHING between them: no read-back, no call the queue refuses. The body
  simply flushed a queue per biome. Residency INSIDE a queue already exists (a field written by one
  element and read by the next never crosses the border), so the whole measured saving was available by
  declaring ONE queue. PROVED BY EDIT: all biome rules now build one queue in `S01_field.lua`, the
  profile shows exactly the prediction (`88 passes in 1 call, transfer 2.45 -> 2.45 MB` against
  `12.76 -> 2.45` before), the world is BITWISE identical (fingerprint `ea836d9f19da9946` of the twelve
  reference chunks, `104/104` checks), and CPU throughput is unchanged (`263.7` vs `263.0` chunks/s —
  noise; a chunk is computed single-threaded and fusion in one thread gives zero, as measured earlier).
  The profiler now names this case itself ("N of them declared as SEVERAL calls where one queue would
  do"). A session would only be needed where something the queue refuses (`reduce`, `scatter`,
  `sequential`) sits BETWEEN two device runs — and no such case exists among the measured ones: GN03's
  runs are separated by the climate reductions, i.e. by a genuine read-back where the region has to end
  anyway. ALSO FIXED while measuring the profiler's own cost: the queue classification ran even with
  profiling off (two vectors of bindings per queue), and a `run_script` whose output field is `ub1`
  logged an error-level line on EVERY ordinary run — a declared refusal printed as a failure, i.e. the
  noise that teaches you not to read messages. The kind is now checked before the translation is asked
  for. Off-cost measured: `5563` vs `5499` ms median on GN02 — inside the noise.

- WHERE THE GENERATOR'S HOURS ACTUALLY GO (2026-09-06). `510/510` project tests (5 new). Closes
  `ORIGINATOR_GPGPU.md` §5 п.0, the measurement the plan demanded FIRST and that was never taken.
  New `originator::execution_profile` (core) + `script_host::set_profile`, `--profile` on GN01/GN02/GN03.
  THE QUANTITY IS WALL CLOCK, AND THE REASON IS NAMED. Counting "how many calls fit the aperture" is
  meaningless — a hundred cheap `fill`s and one `graph_flood` give the same count and the opposite
  answer. So a record is the wall clock of ONE call, and four classes say what it is blocked BY:
  `ready` / `no_body` (write the body — 11 tools of 43 have one) / `narrow` (the field's kind has no
  device type: `ub`, `us`, `sf`, `c`) / `refused` (the aperture, i.e. never). Step time is measured
  whole, and the difference from the sum of its calls IS the lua composition.
  THE THREE REAL GENERATORS BLOCK ON THREE DIFFERENT THINGS, and none of it was obvious in advance:
  GN01 (map) `74.6%` narrow, GN02 (planet) `44.8%` refused-by-aperture (`39%` of it `sequential`
  floods) plus `19.3%` lua composition, GN03 (streaming volume) `50.2%` ALREADY READY with zero
  narrow. For map generators THE FIELD KIND IS THE BLOCKER, NOT THE MISSING BODIES — GN02's whole
  96-call `climate` queue is narrow to the last element. MEASURED BY EXPERIMENT: re-declaring GN01's
  four label fields from `ub1`/`us1` to `ui1` moves readiness from `4.6%` to `63.8%` and pipeline
  memory from `21.0` to `32.0` MB (+52%), with wall time unchanged. An honest trade of memory for
  portability, and it belongs to the author of the config, not to the engine.
  READINESS IS NOT PORTABILITY, and that is the finding that changes the roadmap. After widening
  GN01's kinds, `63.8%` is ready and `0%` sits in runs of two or more passes: the ready calls stand
  ALONE, separated by calls without bodies, and a lone pointwise call loses to the transfer round trip
  (§5 п.3). So a missing body costs not its own share but the RUN IT BREAKS, and the profile prints
  both: "today in runs >= 2" against "if the missing bodies were written" — GN01 `0%` -> `10.5%`,
  GN02 `1.8%` -> `9.2%`, GN03 `50.2%` -> `87.8%` (longest run 103 passes).
  AND THE PRICE OF RESIDENCY LASTING EXACTLY ONE QUEUE IS NOW A NUMBER. Every `run()` re-uploads its
  inputs and downloads its outputs, so adjacent calls pay for the same field once each; the union over
  a run is what a shared session would pay: GN02 `734` -> `353` MB (`51.9%`), GN03 over 25 chunks
  `287.9` -> `81.8` MB (`71.6%`), one chunk's `field` step `12.76` -> `2.45` MB (`5.2x`). This meets
  §5 п.3 from the other side: there transfer was `70%` of the round, here residency removes `72%` of
  the transfer. That is the justification for the device session, and it is measured, not assumed.
  TRANSLATION IS NOT WORTH PRE-COMPUTING: `ds` -> GLSL costs `0.71` ms per GN02 run, `0.25` ms for
  GN01, zero for GN03 (no programs on that path). After the `shaderc` fix there is nothing to save.

- THE TRANSLATOR'S SECOND HALF, AND THE LIBRARY'S DOCUMENTATION PASS (2026-09-06). `505/505` project
  tests, `originator_translate_test` 10/10 (4 of them new). `libs/originator` only.
  RANDOMNESS ON THE DEVICE IS A DECLARED SECOND STREAM, NOT A COPY OF THE CPU ONE. `chance`, `random`,
  `rndmix`, `rndmix1` now translate: the run seed rides in the SHARED PUSH HEADER (`device_call_header`
  gained `seed`, folded 64 -> 32 by `fold_seed`), the per-call-site salt is DERIVED FROM THE ORDER OF
  TRANSLATION, and the hash is the engine's own `utils::shared::prng2`. What is kept is the property
  that mattered — the value is a function of (seed, element, call site) and so does not depend on how
  the work was split — and what is NOT kept is bit equality with the CPU: ds generates its salt in the
  EMITTER at compile time, so it is not in the AST at all, and its PRNG state is 64-bit. The engine
  hash is written out in the shader preamble because a compute program has no `#include`, and a test
  checks the copy against `utils::shared::prng2` BITWISE (it also checks `rndmix`, which hashes the
  VALUE and therefore does match a host formula exactly).
  BRANCH BLOCKS ARE NESTED TERNARIES, BUT THEY ARE TRANSLATED FORWARD. `select`, `sequence`, `switch`
  and `random` are now translated, and the ORDER is the trap: translation is not a pure function — it
  numbers push-constant arguments and randomness salts as it goes, so sub-expressions are emitted in
  explicit forward order and only the TEXT is assembled backwards. Equality became `abs(a-b) < 1e-6`,
  because that is what ds's `raweqd` computes; a bitwise `==` would have disagreed exactly at the
  border where the rule decides. Two divergences are declared rather than hidden: a `switch` with no
  matching case yields zero (ds pushes nothing there, i.e. undefined), and `random`'s last branch is
  unconditional (`pick` is below the full weight sum by construction).
  `ctx_save`/`ctx_set` BECAME SHADER LOCALS, AND THE OLD REFUSAL BECAME A NARROWER ONE. The expression
  stays an expression; everything with a name (saved slots, overridden arguments, `random` weights, the
  `switch` subject) lives in a PRELUDE before the store. That is exactly why a write from INSIDE a
  branch is refused: a local is computed before the branch is chosen, so the write would happen even
  when the branch is not taken. The translator has no dominance proof, so the border is drawn by
  SYNTAX and said out loud, rather than guessed.
  MEASURED WHERE MEASUREMENT WAS POSSIBLE: `select`/`sequence`/`switch`/`ctx_save`/`ctx_set` agree with
  the CPU path BITWISE on integer programs (4096 elements each), weights 1:3 gave the heavy branch
  0.7539, and device `chance` matches the host formula on all 4096 elements. NB found along the way: in
  a NUMBER-typed ds program a bare comparison cannot be a `condition` at all (the expected type leaks
  into it and bool does not convert), so conditions written as comparisons only compile in a PREDICATE
  program — the tests are split accordingly, and that is a ds property, not a translator one.
  DOCUMENTATION PASS OVER THE WHOLE LIBRARY, on the author's rule: every file carries a block after the
  includes and before the namespace (what it is, what to watch for, implementation specifics), long
  rationale moved THERE from individual declarations, and in-body comments cut back to an algorithm
  description above a function plus short implementation notes. Two comments had already drifted off
  their declarations (`parse_field_type` in `common.h`, `remap` in `standard_tools.cpp`) and were found
  by this pass. GENERATED GLSL IS NOW ENGLISH — it leaves the project (glslc, dumps, debuggers) — while
  the library's own comments stay Russian. README rewritten as a technical reference (1232 -> 751
  lines): working notes dropped, decisions collapsed into one-line facts, and a new `libs/originator/
  AGENTS.md` holds what an AGENT needs instead — a question -> file map, twelve invariants, recipes for
  the typical edits (new tool, device form, new translator construct, new push header field), the test
  matrix and the known traps.

- LEARNING, GRAPHS AND BACKTRACKING FOR THE CONSTRAINT SOLVER (2026-09-05). `501/501` project tests,
  GN05's own `19/19`. Three additions to `libs/originator/.../constraint_tools.cpp` and three modes in
  `subprojects/playgrounds/GN05_constraint_collapse`.
  ONE CORE, THREE TOOLS. `collapse` (raster), `graph_collapse` (CSR) and `learn_rules` (rules off a
  sample) share ONE implementation of wave, observation, propagation and rollback: the neighbourhood
  arrives as a function, because a raster and a graph differ by exactly that. A second copy of this
  logic would have drifted silently — both versions would keep producing "some" layout.
  GRAPH RULES MUST BE SYMMETRIC, AND ASYMMETRY IS A REFUSAL. A graph arc has no canonical direction, so
  "a next to b" and "b next to a" are the SAME statement and the matrix is ONE (`allowed[a * tiles + b]`
  instead of two axes). An asymmetric table is refused by naming the pair: silent symmetrisation (by OR
  or by AND) would generate rules nobody wrote.
  LEARNING: ADMITTED IFF OBSERVED. A `window x window` window walks the drawn sample, distinct windows
  become an alphabet of PATTERNS, and observed adjacencies of windows become the table. The classic
  overlapping model instead admits a pair whose windows AGREE on the overlap even if nobody saw them
  side by side; that gives more variety but no promise one can present. Observation gives a checkable
  one — NO WINDOW IN THE RESULT THAT WAS NOT IN THE SAMPLE — and the playground verifies it against the
  SAMPLE itself, not against the table derived from it. At window 1 both models coincide and it is
  exactly the simple tiled model. The solver then works in the alphabet of PATTERNS while the map is
  needed in tiles: the translation is a plain `lookup` over pattern representatives, and no second
  mechanism was needed.
  ROLLBACKS ARE TWO QUANTITIES, NOT ONE. `rollbacks` is PATIENCE (how many undos per attempt),
  `history` is MEMORY (how many observations back). What is kept is a change JOURNAL, not a wave
  snapshot: a snapshot per observation would cost a copy of the whole wave per choice, i.e. quadratic
  in cells. The undone choice is then FORBIDDEN — without that the solver returns to the same point and
  the search never narrows — and the forbid is journalled into the ENCLOSING level, because it is only
  valid in the state it was derived in.
  DECLARED CAPACITY NO LONGER COSTS TIME. The pattern alphabet's capacity must be declared (nobody
  knows the count before the run) and it used to double as the solver's WIDTH — a generously declared
  spare silently made solving 4x slower. Now the tail of the alphabet that weighs nothing and relates
  to nothing is not part of the alphabet: capacity 96/512/2048 give the SAME map (checked by hash) in
  556/570/690 ms, and the residual is parsing the declared matrix, i.e. the memory the author asked
  for.
  ZERO WEIGHT MEANS "NEVER CHOSEN" AND IS EXCLUDED FROM THE INITIAL WAVE: otherwise the option count —
  the solver's CRITERION — would lie. A pre-taken cell is a condition, not a choice, so its tile is set
  regardless of weight; that is how "deep water only where I drew it" is expressed.
  MEASURED, AND THE ANSWERS CUT BOTH WAYS: on tight learned rules a handful of rollbacks replaces half
  a dozen restarts and wins up to 3x in time (seeds 2/3/4 at `96x96`: 7/5/6 attempts and 3838/3342/3494
  ms became 1 attempt with 4/1/2 rollbacks and 1272/1236/1251 ms). On a SYNTHETIC hard instance
  (3-colouring an odd torus) restarts win on cost while rollbacks win on reach (side 15, one attempt,
  40 seeds: 4/40 without, 24/40 with) — and at side 45 NOTHING works, neither 64 restarts (330 ms) nor
  65536 rollbacks with history 64 (46 s). A rollback rescues a LOCAL mistake; against a constraint
  whose consequence is global it is as powerless as a restart. What is cheaper depends on how much the
  thrown-away work was worth.
  THE JOURNAL COSTS ~18% WHEN NO ROLLBACK HAPPENS (optimised build, `192x192`, 7 tiles: 37.5 → 44.5 ms
  median) and is lost in the noise on a wide alphabet. The layout is BITWISE IDENTICAL with and without
  rollbacks when none are used — checked by hash at four sizes. NB: the Debug build showed ~50%, which
  is an artefact of unoptimised `push_back`, not a design fact — worth remembering before publishing a
  Debug ratio as a property.
  THE WINDOW IS THE PRICE KNOB: sample `48x48` → raster `64x64` gives 7 patterns / 26 ms / no structure
  at window 1, 85 / 556 ms / islands with beaches at window 2, and 435 / 80 SECONDS at window 3.
  Declared pairs constrain WHAT touches WHAT; learned windows constrain the SHAPE — the difference is
  visible by eye (grainy lawful speckle versus real coastlines).
  THE SAME LADDER ON A DEGREE-6 GRAPH IS NOT THE SAME RULE SET. On a free sphere it goes 55% water with
  no mountains at all; with 24 snow peaks pinned, not a drop of water. The "neighbour one step away"
  rule has no restoring force, so the whole ball drifts wherever the first condition pushed it, and the
  more neighbours a cell has the stronger this is. A rule set tuned on one neighbourhood does not
  transfer to another — the graph mode therefore pins conditions from BOTH ends of the ladder.
  LEARNING REPRODUCES THE DRAWING'S SLIPS, caught here on a live example: the first sample wobbled the
  coast by ANGLE, and near the centre — where the angle between adjacent cells changes fast — the
  ladder skipped a step, so in a couple of places water touched grass ON THE PICTURE. The rules carried
  that faithfully and the playground check caught not the solver but the DRAWING. The wobble is now a
  function of position, and the ladder holds on the sample BY CONSTRUCTION.

- CONSTRAINT SOLVER (WAVE FUNCTION COLLAPSE) AND GN05 (2026-09-05). `480/480` project tests, GN05's own
  `8/8`. New tool `collapse` (`libs/originator/.../constraint_tools.cpp`) plus the lab
  `subprojects/playgrounds/GN05_constraint_collapse`.
  WHY IT EARNS ITS PLACE: a HARD LOCAL PROHIBITION. Noise gives smoothness, Voronoi gives regions,
  graph flood gives reachability — none of them can say "water NEVER touches grass". That is the one
  thing the solver promises, so that is the one thing the checks verify.
  INPUT/OUTPUT: tile weights + a MATRIX of allowed adjacency + optionally pre-taken cells → one tile
  index per cell + optionally the number of attempts. The matrix is declared as
  `allowed[(axis * tiles + a) * tiles + b]`; there are two axes and THE OPPOSITE DIRECTIONS ARE DERIVED
  by transposition — two lists of one truth would drift silently, because an asymmetric table looks
  like an ordinary one and merely contradicts more often. The bitsets the solver works with are its own
  business: word layout written down twice would be the same duplication.
  APERTURE `sequential`, AND THAT IS NOT AN IMPLEMENTATION DETAIL. Which cell to observe next is
  decided by the field left behind by the previous propagation. So the solver will never enter a queue
  (the queue's `sequential` refusal says exactly this) and will never reach a device — parallel WFC
  variants exist and produce DIFFERENT results. It is also the first tool in the library that can FAIL
  TO FIND AN ANSWER: a contradiction is a normal outcome of arc consistency, hence a declared attempt
  count and a LOUD refusal when they run out — a half-filled grid is a different world under the same
  seed and invisible in the result.
  DETERMINISM BOUGHT BY DROPPING ENTROPY: the classic compares Shannon entropy in float, so the choice
  of cell depends on comparing floats — the very class the project already burned on with FastNoise2.
  The criterion here is INTEGER (fewest remaining options) with ties broken by a hash of cell and seed,
  the same trick `graph_flood` already uses. Entropy stays a HEURISTIC, not a definition of the task.
  MEMORY NAMED UP FRONT (the GEN-15 lesson applied immediately): the wave is `cells x ceil(tiles/32)`
  words and nothing else. The classic also keeps support counters per (cell, tile, direction) — 64 MiB
  at `512x512` with 64 tiles — and they are deliberately absent: support is recomputed as a bitset
  union. A named time-for-memory trade, to be reversed only after measurement.
  TWO DISPATCH RULES GENERALISED ALONG THE WAY, both needed by a solver whose inputs are a rule table
  rather than per-element data: `optional_inputs` (symmetric to `optional_outputs`) so pre-taken cells
  need not be a buffer of zeros; and for `sequential` the range now covers only the FIRST output while
  inputs are read WHOLESALE — a solver's inputs are a few dozen words of rules while its range is a
  million cells, and what is ordered about it is the WRITES, not the reads.
  MEASURED: cost is LINEAR in cells (4.60/3.81/3.54/3.30 µs per cell at 64/128/192/256 square) — the
  observation queue with stale-entry discarding does not blow up quadratically. But three MICROseconds
  per cell against three NANOseconds for a native pointwise pass: that is the price of solving a
  problem, not of walking a field, and the two should not be confused.
  UNEXPECTED NUMBER: weights are declared `grass 2.4`, `forest 1.8`, `water 1.0`, and the layout comes
  out `water 32%`, `grass 20%`, `forest 11%`. A weight is a preference AT THE MOMENT OF CHOICE, not a
  frequency in the result — what ends up on the map is decided mostly by the constraints and the
  pre-taken frame. Tuning proportions by weights works only approximately, and that must be known in
  advance.
  NAMED AS ABSENT AT THE TIME, ALL THREE DONE IN THE NEXT SESSION (see the entry above): learning
  rules from a sample, a graph-neighbourhood solver, and backtracking instead of restart. Still absent:
  chunked generation (constraints cross chunk borders, so `key_support` is `global`).
- WHERE DEVICE CODE COMES FROM, GN04 ON CONFIG, AND AN AUDIT (2026-09-05). `458/458` project tests,
  GN04's own `22/22`. Recorded as `ORIGINATOR_GPGPU.md` §10.
  THE DIVIDING LINE IS NOT "GLSL OR NOT", IT IS WHO DECIDES WHAT IS COMPUTED. A native tool's device
  body is written AHEAD OF TIME and lives in the library (`remap`, `graph_flood`, `plate_velocity`):
  that is project source, compiled with the engine and read at review, so keeping ready GLSL there is
  fine and needs no text checking — nobody substituted the text. `run_script` is the other case: what
  it computes is decided by the AUTHOR OF THE DATA, so it brings a `devils_script` PROGRAM, never a
  shader, and the engine translates it by the AST `ds` itself hands over. A brought-in shader would
  pass no check at all and get direct access to device memory.
  THE RULE IS NOW BACKED BY A TYPE, not only a comment: a foreign body's form is `translated_form`,
  and it cannot be built from a string — the constructor is sealed by a key the core hands to exactly
  the translator. The previous `queue_call::device_body` was a plain string, and the first consumer
  (GN04) put raw GLSL in it BECAUSE IT COULD. Full enforcement is impossible without owning the
  project — C++ goes around anything — but it no longer happens by accident.
  `run_script` INSIDE A QUEUE IS NOW ACTUALLY TRANSLATED: `script_host` attaches the form, and an
  untranslatable construct yields a declared REFUSAL carrying the translator's reason, on which the
  queue stays on the CPU.
  WHERE A QUEUE RUNS IS A CONFIG DECLARATION (`device = true`) — §6.4 in full: the decision lives in
  the manifest, so every machine takes one branch, and a machine that cannot take it REFUSES LOUDLY.
  The executor is installed by whoever assembled the application (`set_device_executor`); the
  generator core still knows nothing about Vulkan. THE PLAN IS BUILT ONCE and reused BY THE QUEUE'S
  SIGNATURE — parameter values are not part of the signature and are rewritten into the push constant
  before each submission, so changing a threshold does not build a second plan with its own device
  buffers. Checked by counters, not assumed.
  THE BODIES MOVED FROM THE LAB INTO THE LIBRARY, and the honest answer to "general or GN04-specific?"
  was "general, and one of them already existed": `voronoi_label` was being REIMPLEMENTED by the lab.
  It now has a device form (brute force over the site list — a kd-tree does not travel, and scanning
  costs exactly as many sites as there are) and an OPTIONAL second output, border closeness, computed
  in the SAME traversal (`optional_outputs`, `tool_call::has_output`, `ORIGINATOR_OUTPUTS` in the
  shader) — optional because someone who only wants labels should not declare a buffer for a result
  nobody reads. New: `filtered_blur` (the engine's ONLY tool declaring a filtered input, hence the
  only reason a field ever becomes an image), `label_colour` (colour DERIVED from the label by hash,
  packed RGBA8; the shading curve stays outside, in a plain `remap`), and `count_by` (the first tool
  with `order_free_writes`; it does NOT clear its target, because an atomic can only add and one
  declaration with two behaviours is worse than an extra call — so the clearing is a `fill` INSIDE the
  queue, alive precisely because an accumulator reads what it writes into).
  TWO REFUSALS TURNED OUT TO BE UNNECESSARY: multi-component fields (in `soa` the components lie
  contiguously inside the element, so only an accessor with a component and an element-counting length
  were missing — without this the `v2` site positions could not reach a device at all), and integer
  fields under a native tool, which is still refused BY DEFAULT but can be opted into with
  `device_integer_ready`, whereupon converting accessor overloads are emitted. The price is named:
  conversion goes through `float32`, exact to 2^24, while the host reads the same field through double.
  GN04 IS NOW A LAB LIKE THE OTHERS: the queue moved out of code into `buffers.tavl`, `values.tavl`,
  `texture.tavl` and `S01_texture.lua`; `main.cpp` holds NOT ONE line of GLSL and not one Vulkan
  command. The two boundaries are declared by a config number, and that immediately caught its author:
  the first verify run showed 258069 differences of 262144 because the check read fields not named in
  `output`. They had not arrived — the device computed them and left them there, exactly as promised.
  Numbers: 6 calls, 4 barriers, 1 image; uploads only `sites.position` (512 B), downloads
  `summary.count` (256 B) against 16777472 B at the inspection boundary — `65537:1` at `1024x1024`.
  Labelling matches the CPU path on EVERY pixel; edge closeness `1.29e-05`, filtered smoothing
  `7.15e-07`.
  AUDIT. Silent defects found and closed this round: `float data[]` over a field of ANY kind (a native
  tool over `ui1` read its BITS while the CPU read the VALUE); the translator's OWN push header
  against the tool one (harmless only while translations never entered a device queue); an indirect
  range silently dispatching ZERO elements on a device (a pass that did nothing is distinguishable
  from one that did only by the field staying unchanged — and it stays unchanged anyway); a
  `compute_context` barrier covering only the compute stage while one submission has three
  neighbouring pairs.
  THE OPEN FINDING IS MEMORY, AND IT IS MEASURED. The library's own rule says a generator must be able
  to name its memory cost before it runs. GN02 at a million cells names **432 MB** (the sum of
  declared buffers) while the machine needs **562 MiB** of peak RSS — a third on top that nobody
  declares, and it is the tools' TRANSIENT tables: `sphere_adjacency` holds `nearest` and `filled`
  (every arc twice before canonicalisation) for about a hundred megabytes; the scatter partial-sum
  table is capped by `maximum_counter_table` at 8M cells, i.e. **64 MiB** nothing accounts for;
  `label_adjacency` piles neighbours into a vector of vectors and dedups only at the end. The first
  step is deliberately small and honest: `sphere_adjacency` no longer keeps a separate counter table
  and computes 32-bit offsets (12 MiB at a million cells) — but the peak moved 563 -> 562 MiB, so the
  bottleneck is NOT there and must be found by phase measurement, not by reading. The right shape of
  the report: a pipeline must name its PEAK including transient tables, and a tool must declare its
  transient cost the way it declares its aperture.
- GN04 NOW RECEIVES A PIPELINE COMPOSED BY libs/originator; IMAGES, A SECOND BOUNDARY AND SCATTER
  (2026-09-05). `450/450` project tests, GN04's own `22/22`. Recorded as `ORIGINATOR_GPGPU.md` §9.
  BINDINGS ARE NOW GENERATED, BECAUSE THE RESOURCE KIND IS DERIVED. §6.2 declared that buffer-vs-image
  is DERIVED from usage — but while a tool wrote its whole shader text, the kind was written INTO that
  text by hand (`layout(std430 ...) buffer`), so "derived" was untrue. A tool now declares a BODY
  written against accessors (`in_<i>_at`, `in_<i>_length`, `in_<i>_sample`, `out_<j>_set`,
  `out_<j>_add`) and `build_device_shader` (`libs/originator/.../device_form.h`) assembles the preamble
  knowing the derived kinds. The same assembler now builds the translated `ds` program too, which
  closed a drifted convention BEFORE it fired: the translator had its OWN push header (`uint count`)
  against the tool one (`count, begin, extent_x, extent_y`) — harmless only while translations never
  entered a device queue.
  A LATENT DEFECT CLOSED WITH IT: the old text declared `float data[]` for ANY field, while the queue
  admits kinds `v`, `ui` and `i`. A native tool over a `ui1` field therefore read its BITS as float —
  exactly what `as_span<T>` catches on the host — while the CPU path read the same field through an
  accessor and got the VALUE. Two paths silently computing differently. Accessors are now typed by the
  declared kind, and a native tool over a non-float field is REFUSED with its reason: its body was
  written once for every future binding and cannot know the kind. A foreign body was written for THESE
  fields, so it gets the integer accessors.
  WHAT MAKES A FIELD AN IMAGE: exactly ONE declaration, and not "make this an image" — the only thing
  invisible from the bindings, namely THIS INPUT IS READ FILTERED (`device_filtered_inputs`). The plan
  then decides: a field ANY call reads filtered becomes an image, everything else stays a buffer. That
  is §6.3's criterion verbatim, and in GN04 it does real work — of four raster fields exactly ONE
  (`edge`) becomes an image, while `region` (read by its own index) and `colour` (not read at all) stay
  buffers though they live in the same raster of the same shape.
  FILTER REFUSALS, each its own: a filter over integers (the average of two region labels is not a
  label — a refusal by MEANING, not by type), a filter over a buffer with no `extent` (reading BETWEEN
  elements needs to know between which), and an image of another shape addressed BY INDEX (the index
  folds by the CALL's shape, so it would read the wrong texel and not complain). Hence a filtered input
  has no index accessor at all — its shape need not match the call's, which is exactly what upsampling
  a coarse field into a fine chunk needs.
  A painter BARRIER BUG FIXED ON THE WAY: `compute_context::recorder::barrier` covered the compute
  stage only, but one submission has three neighbouring pairs — upload→pass (transfer→compute),
  pass→pass, and pass→download. Two of three were a race that "works" on integrated memory. Same class
  as works-for-the-author-breaks-for-the-player.
  TWO BOUNDARIES: `output` AND `resident`. `output` meant two things at once — "live" and "goes back to
  the host" — and the first real consumer split them: GN04's visible texture stays on the device and is
  read LATER by a post-process the queue cannot see. Naming it in `output` would download a megabyte
  just to keep the compose pass from being called dead; not naming it would refuse honest work. The
  dead-work check treats both lists alike (it asks whether ANYONE reads this, not where the reader
  lives); they differ only in the transfer plan. A field in both lists is refused: it either comes back
  or it does not, and two answers mean one of them is untrue.
  SCATTER IN A QUEUE: WHAT WAS REFUSED WAS THE ORDER, NOT THE FOREIGN INDICES. Re-reading §4.4's own
  wording says so. Integer accumulation has no order at all — GN04 MEASURED that last round — so a tool
  whose writes do not depend on order declares it (`order_free_writes`) and is admitted. Floating
  accumulation cannot declare it (§4.3), and an image cannot either (no atomics without an extension).
  From that one declaration three things follow, all by the same single question — AN ACCUMULATOR READS
  WHAT IT WRITES INTO: a `fill` with zeros before it is honest work rather than dead; its output is a
  queue INPUT, so the initial value is UPLOADED (otherwise the atomic would add to allocator garbage
  and the summary would come out plausible and wrong); and a barrier is needed before it. The upload
  trap fired TWICE while writing the lab — a second run without zeroing gives exactly double.
  GN04 ON THE PLAN: four passes, not one Vulkan command from the lab. `1024x1024`, 64 regions: 4 calls,
  3 barriers, 1 image, uploads 768 B, downloads 256 B; the SAME calls with the boundary "everything
  out" download `16777472` B — `65537:1`. The second plan is never run: transfer is derived at
  composition, so it is PLANS that get compared. The lab used to state that ratio; now the plan does.
  HOW FAR THE PATHS AGREE on the same queue (it has a CPU path — §4.6 requires one): Voronoi labelling
  `0` of `1048576` pixels decided differently (the decision is INTEGER, so arithmetic differences would
  only move pixels exactly on a border, and there were none); edge closeness `2.98e-08`; FILTERED
  smoothing `2.98e-08`; an image round trip through the boundary exactly `0`; a filtered read at the
  TEXEL CENTRE exactly `0`. The smoothing figure is an OBSERVATION, not a promise — §6.3 declared
  filter precision implementation-defined, and this machine's bilinear happened to match the textbook
  formula to two ULP.
  THE REDUCTION IS NOW CHECKED HARDER: the summary is compared not against the other path's histogram
  but against a histogram of THE SAME LABELS computed on the host. Otherwise the check would be empty
  exactly where it matters most — border pixels legitimately disagree, and any summary difference would
  be written off to them.
  QUEUES THAT WERE ALREADY WRITTEN (§1's claim retested), six of them: GN02 step `climate` — the
  moisture transport loop, pairs of `moisture_step`, a dozen or more passes with NOT ONE intermediate
  lua value; the longest queue in the planet generator, and `moisture_next` staying out of `output`
  names out loud that it lives only between two neighbouring passes. GN02 step `landforms` — two
  prepare-then-blur-in-pairs chains, plus LANDFORM IDENTIFICATION, where `devils_script` programs and
  native tools stand SIDE BY SIDE in one queue (two `run_script` rules, then four tools reassembling
  them by the land mask): the queue does not distinguish the two cases anywhere but the call site, and
  here that is literally visible. GN03 step `field` — one biome's rule, 14-16 consecutive `pointwise`
  passes over one range (the fusion case), plus the air-island chain of six.
  MEASURED HONESTLY: GN03's single-thread chunk cost is UNCHANGED (`143.1` ms before, `143.2` ms
  after), which agrees with the earlier measurement that fusion buys 0% on one thread and 14-25% on
  eleven. The conversion buys naming and a boundary, not speed, and saying otherwise would be untrue.
  ONE CONVERSION REFUSED AND EXPLAINED ITSELF: `fill{rain}` before the transport loop. `moisture_step`
  ACCUMULATES rain into its own element but declares `rain` as an output only — and the queue sees
  declarations, not bodies. Inside the queue that `fill` would look overwritten-unread, and the refusal
  would be honest by declaration and wrong in fact. The fill stayed outside. (The mechanism to say it
  honestly now exists — `order_free_writes` — but it needs the TOOL to declare its accumulation, which
  is an edit to `moisture_step`, not to the step body.)
- ORIGINATOR NOW COMPOSES THE DEVICE PIPELINE FROM ITS OWN QUEUE (2026-09-05). `436/436` project
  tests. `libs/originator/.../device_queue.h` in a new target `devils_engine::originator_device`
  (separate for the same reason lua and `devils_script` are: the generator core must not know about
  Vulkan, and a CPU-only consumer must not link painter). This is what `ORIGINATOR_GPGPU.md` §3.3
  promised from the start and what did not exist until now — the device used to be driven by hand while
  the queue stayed a CPU notion. Recorded as §8.
  NO SECOND SCHEDULER APPEARED. Everything in the plan is DERIVED from the same queue `check_queue`
  validates: TRANSFER from the queue's boundaries (upload the fields a call reads that nobody in the
  queue wrote; download exactly `output`) and BARRIERS from the same question as the dead-work check
  ("does the next pass read what the previous one wrote") — literally one question asked twice. Tested:
  a `remap`→`blend` chain uploads `height` and `moisture`, does NOT upload `smoothed` (the first pass
  writes it), downloads only `mixed`, and `smoothed` on the host stays UNTOUCHED — computed on the
  device and left there. One barrier for the dependent pair, ZERO for a lone `box_blur`; both numbers
  are checked. Plan validation needs no device at all: it is derived from the declaration, not asked of
  the driver.
  DESCRIPTORS BELONG TO THE CALL, NOT THE PROGRAM, and that is not convenience. Descriptors take effect
  at SUBMISSION, not at record time, so two calls of one program with different bindings recorded into
  one submission would both read the LAST bindings — silently different numbers, not a refusal. And a
  queue does exactly that: two `remap`s in a row are one program over different fields. Hence
  `compute_context::create_binding_set` per call, with the pipeline still per program.
  ONE SUBMISSION: §5 STEP 3's 70% IS NOW RECOMPUTED. Same work both ways (`262144` elements, 2 passes,
  3 MB moved, best of seven, variants interleaved): **one submission `1.442` ms against `3.506` ms step
  by step = `2.43x`**. Five submissions instead of one cost `+2.06` ms, i.e. about `0.5` ms per
  submission — which matches the constant term §5 step 3 found in its slope, and means MOST of that 70%
  was submission cost, not transfer. So the real transfer share is nearer `40%` and falls further as
  the queue lengthens, and step 3's break-even ("from the second or third pass") moves in the device's
  favour. `run_step_by_step` stays in the code and is admitted as measurement-only: comparing one
  estimate with another requires running both on the same work.
  REFUSALS, each for its own reason: a tool with no device form (a legal answer, not a defect — the
  caller falls back to `run_queue`; forms exist for `remap`, `blend`, `modulate`, `maximum`, `minimum`,
  `box_blur`), `aos` layout (a strided field cannot be transferred as a block, and stride costs
  LINEARLY on a device per §5 step 5), narrow or multi-component field kinds, and everything the CPU
  path refuses — the device plan runs `check_queue` in full first.
  DUPLICATION NAMED OUT LOUD: device-form parameter defaults are declared (`device_param::fallback`)
  while the tool body writes them as literals. A drifted default would mean a call WITHOUT that
  parameter computing differently on the two paths, visible only by comparison — so the comparison
  exists, and a chain with no optional parameters at all diverges by EXACTLY ZERO. Removing the
  duplication means every tool body reading its values from the declaration; that waits for a second
  consumer.
  HOW FAR THE PATHS AGREE: `remap`+`blend` exactly `0`, the same without optional parameters exactly
  `0`, `box_blur` radius 2 `2.98e-07` (two ULP). The zeros are an observation, not a promise — the CPU
  computes in double but the result lands in the same `float32`; `box_blur` shows why the promise
  cannot be made once there are many terms.
  A GLSL RESERVED WORD BIT ONCE: `remap`'s device form declared `float sample`, and `sample` is an
  interpolation qualifier. Caught by glslc — which is exactly what type checking was handed to it for.
- GN04 OPENED: BACKGROUND TEXTURES ON THE DEVICE (2026-09-04). `430/430` project tests, GN04's own
  `11/11`. `ORIGINATOR_GPGPU.md` §5 step 5 (textures) done; the queue is now used end to end on a
  device for the first time. Fourth generator-campaign lab
  (`subprojects/playgrounds/GN04_texture_generation`), headless BY NATURE — it proves the picture does
  NOT leave the device, and a window would substitute for that proof rather than support it.
  THE CASE WAS NOT PICKED AT RANDOM: it is the one place where NONE of the queue's weaknesses
  interfere — the result stays on the device (so transfer is not paid), determinism is not required
  (presentation, not simulation), the reduction is INTEGER (so group arrival order does not matter),
  and the chain is short. Checking a construction belongs where its weaknesses are irrelevant;
  otherwise the measurement measures the weaknesses.
  FOUR PASSES: Voronoi labelling (`gather` — region id and border proximity into two images; sites are
  placed by the HOST, which the library's rule allows since the set is small and enumerated by the host
  itself), smoothing BY FILTERED SAMPLING, colour composition, histogram via atomics.
  THE HEADLINE NUMBER IS `4096:1`. 512x512, 64 regions: `13.81` ms on the device (labelling `4.65`,
  smoothing `0.87`, composition `0.85`, histogram `7.43`), `256` bytes of summary out in `0.58` ms, and
  a megabyte of picture stayed. The round trip that ate 70% of `remap`'s time in step 3 is not paid at
  all here.
  IMAGE-VS-BUFFER CONFIRMED IN THE FORM §6.3 PREDICTED: the smoothing pass reads with a linear filter
  at a NON-INTEGER coordinate — four taps at half a texel, each already averaged over four neighbours
  by hardware bilinear. A buffer has no such read; there one would fetch sixteen elements by hand. The
  check "the filter actually ran" (the smoothed field differs from the source, and differs
  measurably) is its own item: without it the sampling pass would be expensive copying, and that is
  invisible in the picture.
  AN INTEGER REDUCTION VIA ATOMICS IS REPRODUCIBLE, verified by running the chain twice: group arrival
  order is unpinned, but integer addition does not depend on order — so §4.3 hits FLOAT reductions, not
  reductions as such. Mitigation 2 of §4.3 is now working code rather than an argument.
  UNEXPECTED NUMBER: THE HISTOGRAM COSTS MORE THAN THE LABELLING ITSELF (`7.43` against `4.65` ms),
  though it does incomparably less work — one `atomicAdd` per pixel against a loop over 64 sites. The
  cause is CONTENTION: `262144` pixels fight over `64` counters. The right shape is known and
  deliberately not built until a reduction has a real consumer: accumulate in the group's shared
  memory and issue ONE atomic per group instead of one per pixel. Practical consequence for §3.2: on a
  device `reduce`'s bottleneck is not the two-stage tree but contention in the first stage.
  `compute_context` GAINED IMAGES for this: `create_image` (r32f / rgba8, format features QUERIED from
  the device rather than taken on faith), a linear sampler, typed bindings (storage buffer / storage
  image / sampled image — a descriptor is typed, and confusing the kinds silently would mean reading an
  image as a buffer), `dispatch_2d`, and `read_image`. Images live in `GENERAL` for their whole life —
  a lab simplification, named as such: the right path is deriving transitions from bindings, and
  painter already has it in the render graph, so a second one must not appear here. No barriers between
  passes because each submission waits on its fence; a real one-submission queue will need them, and
  they follow from the same question as the dead-work check (§6.1).
  WHAT THE LAB DOES NOT CLAIM: the queue is still not COMPOSED from config — the passes are wired by
  hand, while §3.3 says originator should compose the device pipeline from its own config. That is the
  next step and it is bigger than this one.
- QUEUE BEFORE THE DEVICE: GATHER, LAYOUTS, SHADER CACHE (2026-09-04). `429/429` project tests.
  `ORIGINATOR_GPGPU.md` §5 step 5 done, and THE BIGGEST FIND IS AN ENGINE-WIDE ONE, not a queue one.
  (1) **`shaderc::Compiler` WAS BEING CONSTRUCTED PER COMPILE** inside `shader_crafter::compile`, so
  every shader in the ENGINE paid glslang's one-time initialisation. Measured: `100`–`124` ms per
  program, the SECOND program no cheaper than the first (so not warm-up); the SPIR-V optimizer is not
  the cost (`99`–`105` ms without it); the driver is not the cost (a disk pipeline cache gave `111.8`
  cold against `107.1` warm). With one compiler per thread: first compile `92` ms (the one-time
  glslang init), second **`2.3` ms**, `1.1` ms unoptimized. So **98% of compile time went into standing
  a compiler up and throwing it away.** Fixed as `static thread_local` in `shader_crafter` — a member
  would not have helped, because nearly every consumer creates a crafter per call
  (`glsl_source_file::prepare_spirv`, the compute context); glslang state is process-natured and one
  compiler per thread is its honest model, and a single `shaderc::Compiler` is not thread-safe, hence
  thread_local rather than plain static.
  (2) SECOND PAINTER GAP FOUND ALONGSIDE: `compute_pipeline_maker::create` called
  `createComputePipeline` with `nullptr` instead of the cache, i.e. painter's COMPUTE path never used
  its pipeline cache at all — and that is invisible in behaviour, since an uncached pipeline works
  identically and only takes longer to create. The graphics maker did pass it. Added the cache-taking
  overload and the compute context uses it.
  (3) GATHER ON THE DEVICE IS THE BEST CASE MEASURED SO FAR, and it inverts step 3's conclusion.
  `box_blur` radius 2 (a 5x5 window) over 1 M cells: CPU one thread `251`–`261` ms, dispatch
  `1.8`–`2.0` ms = **135–141x**; deviation from the native tool `2.4e-07` (two ULP on a 25-term sum).
  Counting transfer (`3.7` ms) the round trip is ~`5.7` ms against `251` ms — `44x` WITH transfer,
  where `remap` lost to eleven threads. THE RULE: what pays for itself is not the aperture but the
  DENSITY OF WORK PER ELEMENT, and gather is dense by construction. Raster windows are balanced by
  construction; the imbalanced case is the adjacency CSR (degree up to 24 against a mean of six in
  GN02) and stays a named gap needing the graph tools' data.
  (4) LAYOUT COSTS MUCH MORE ON THE DEVICE THAN ON THE CPU, as §3.2 suspected: same pass over 1 M
  elements, `soa` (stride 1) `0.60`–`1.03` ms, `aos` stride 4 `2.4`–`3.0x`, stride 8 `4.4`–`6.0x` —
  roughly LINEAR in the stride, because fetches come in cache lines and a stride of N uses 1/N of each.
  On the CPU the same switch cost 2% single-thread and 17% on eleven. `layout = aos|soa` stays one line
  of config, but its price is now named.
  (5) The shader cache is still wanted but no longer urgent: it saves `1`–`2.3` ms on a repeat, not a
  hundred. Implemented in `compute_context` keyed by the TEXT plus binding shape (the text, not a hash:
  a hash collision would mean running a different program than the one asked for, undetectable from the
  result), plus the declared `pipeline_cache_path` is now actually WRITTEN on destruction — reading a
  cache file and never writing it is a path that looks like a cache and is not one.
  (6) TRANSLATOR DEBUG OUTPUT, as asked: the emitted shader carries its own origin in a header comment
  — the ds source, the binding table, the push-constant layout byte by byte, and the final expression.
  `GN01 --translate` prints the translation of the lab's real rule. The comment costs nothing (glslc
  strips it) and it is what makes the host side checkable instead of remembered.
  (7) TWO DESIGN ANSWERS RECORDED, both deferred on purpose (§7). Randomness: a uniform buffer is the
  right DELIVERY and needed anyway, because `maxPushConstantsSize` is guaranteed at only 128 bytes (31
  float arguments); but OBTAINING the salt is the separate half — either the translator derives its own
  deterministic salt (paths then differ, which §4.2 already accepts) or it reads ds's salt out of the
  compiled container. Lists: the first answer ("no dynamic memory") was incomplete — a shader has no
  GROWTH but does have fixed local arrays, and IN A POINTWISE PROGRAM THE LIST LENGTH IS BOUNDED BY THE
  NUMBER OF `add_to` SITES IN THE TEXT, because there is no loop (no iterators are registered). So the
  capacity is DERIVED, not declared, and the pipeline ops become unrolled loops. A list filled by an
  ITERATOR stays a refusal, and it should be solved together with neighbour access, not before.
- TRANSLATOR `ds` -> GLSL (2026-09-04). `426/426` project tests.
  `libs/originator/.../script_translate.{h,cpp}` + `originator_translate_test`.
  `ORIGINATOR_GPGPU.md` §5 step 4 is done except randomness, and the reason randomness is missing is
  NOT the one the design predicted.
  THE DECISION THAT MATTERS: THE TRANSLATION WALKS THE AST `ds` ITSELF PRODUCES. `devils_script`
  exposes `make_script_ast(parser, src)` returning the flat prefix `tavl::node` tree its own semantic
  compiler consumes, and `system::configure_parser` declares the operators with their precedence and
  fixity. So the translator is a second CONSUMER of one parse, not a second parse — a second rule set
  would have drifted silently, and drift here means the two paths compute different things, which is
  exactly what a translation must avoid. TYPE CHECKING IS glslc's JOB: our own type inference would be
  a third type system in the project (after ds and Vulkan) and would drift from both; a shader that
  does not compile is a refusal carrying the compiler's message.
  RANDOMNESS DOES NOT TRANSLATE, AND HASH WIDTH HAS NOTHING TO DO WITH IT. §3.1 treated 64 bits as the
  blocker and removed it by choosing 32; the real reason is that the PER-CALL-SITE SALT is generated by
  ds's emitter at compile time (`ctx->gen_value()` in `system_builtins.cpp`) and is not in the AST at
  all. Translating `chance`/`random`/`rndmix` needs the compiled container or the salt from ds — that
  is a separate job, and it is about access to the salt, not about a PRNG. The PRNG itself needs no
  writing: `utils::shared::prng`/`prng2`/`prng_normalize` in `libs/utils/.../shared.h` are ALREADY
  shared between C++ and GLSL, so one hash on both sides comes for free.
  TRANSLATED: arithmetic, comparisons, ds's word operators (`and`/`or`/`not` -> `&&`/`||`/`!`), the
  whole GLSL-named vocabulary, `inv` -> `(1.0 / x)`, three-argument `value_or` -> a ternary, reading a
  field of one's own element, `ctx:arg:` -> push constant, and a combinator block (sum for a numeric
  program, AND for a predicate). REFUSED, EACH WITH ITS OWN REASON rather than a shared "unsupported":
  randomness (no salt in the AST), lists (dynamic memory per element), `ctx_save` (needs the dominance
  check of §6.8), diagnostics, other block forms, `execute`, unknown name, unbound name, wrong arity,
  narrow storage kind. Refusals name the position in the ds text.
  BITWISE AGREEMENT ON AN INTEGER PROGRAM: `65536` of `65536` elements identical. Nested `value_or`
  with integer thresholds and integer fields (`i1` in, `ui1` out), the same text run by
  `script_program` on the CPU and by the translated shader on the device, all three classes non-empty.
  The translation computes in `float32`, which represents integers up to 2^24 exactly — which is
  precisely why the integer program is the one that checks the translator.
  ON GN01'S ACTUAL (FLOAT) RULE THE PATHS DIVERGE, AND IT IS NOW MEASURED: `325` of `65536` cells
  (`0.50%`) get a DIFFERENT CLASS. The rule is taken word for word from `biome_rule.ds` and the data is
  deliberately placed right at the thresholds. Not a translation defect — §4.2 seen in numbers: `ds`
  computes in `double`, the shader in `float32`, and classification AMPLIFIES the difference into a
  changed class, the same lesson as GN03's "float is dangerous only where the decision amplifies". The
  test asserts RARITY, not zero; zero cannot be promised here and nobody will promise it.
  SIDE FIND, CAUGHT BY THE TEST: the program's argument list was being built in the order
  `std::format`'s arguments happened to be EVALUATED, which C++ leaves unspecified — gcc produced it
  reversed. The push-constant layout would then depend on the compiler, i.e. the shader would read
  someone else's numbers. Branch translation is now sequenced through named locals and a test pins the
  order.
- COMPUTE CONTEXT: VULKAN WITHOUT A WINDOW AND WITHOUT A RENDER GRAPH (2026-09-04). `421/421` project
  tests. `painter::compute_context` (`libs/painter/.../compute_context.{h,cpp}`) plus the guarded
  `painter_compute_context_test`. `ORIGINATOR_GPGPU.md` §5 step 3 is done; the translator is next.
  WHERE IT IS ASSEMBLED WAS THE ACTUAL WORK, exactly as the design said. Instance and logical-device
  assembly is now `painter::create_instance` / `painter::create_device` in `auxiliary.h`, and
  `simul::render_runtime`'s templates CALL them — those thirty lines had two consumers differing by one
  flag, so a second copy would have drifted. Choosing the PHYSICAL device stayed in the runtime
  deliberately: it differs with and without a window (surface capability, on-disk cache, postponed
  creation), and folding two different questions into one function would have been a lie. The context
  takes allocator, descriptor pool and pipeline cache from `graphics_base` but keeps its OWN command
  pool on the COMPUTE queue, because `graphics_base::create_command_pool` takes the graphics family and
  a separate `device_queues::compute` is planned precisely for this. No surface, no swapchain, no
  frames in flight, no graph. GLSL text goes through the same `shader_crafter` the engine uses for
  material shaders, so no second GLSL compiler appears.
  MEASURED (Iris Xe, `remap` = `clamp(x*scale+offset)`, the same arithmetic as the native tool; split
  host→device→compute→back, best of five): 1 MB `0.765`/`0.686`/`0.759` = `2.210` ms; 4 MB
  `1.936`/`1.352`/`1.804` = `5.093` ms; 16 MB `4.749`/`3.652`/`4.608` = `13.009` ms. **TRANSFER IS 70%
  OF THE ROUND TRIP AT EVERY SIZE.** Three conclusions, all measured:
  (1) THE COMPUTE BEATS THE CPU BUT THE ROUND TRIP DOES NOT. One `remap` pass over 1 M elements:
  dispatch `1.35` ms against `3.67` ms on eleven threads (from the fusion bench: three fused passes =
  `11.0` ms), so the compute alone is `2.7x` faster — while the full round trip costs `5.09` ms and
  LOSES to eleven threads. §4.5's rule "a queue longer than one instrument, otherwise pointless" is
  therefore a fact, not caution.
  (2) THE THRESHOLD IS COMPUTABLE AND IS TWO-TO-THREE PASSES. Transfer costs `3.74` ms, each pass saves
  `3.67 - 1.35 = 2.32` ms, so the device pays off from the SECOND pass and wins visibly from the third
  (`11.0` against `7.8` ms). Against ONE thread (`19.7` ms per pass) it wins from the first. This is
  exactly the number §6.4 says belongs in the manifest: a STRUCTURAL criterion ("shorter than N passes
  runs on the CPU") now has a measured N.
  (3) DIVERGENCE FROM THE CPU IS ONE ULP (`5.96e-08`). §4.2 still holds (two paths, two results), but
  on a multiply-add the magnitude is negligible — so §4.1's real risk lives in transcendentals and
  reductions, where the decision AMPLIFIES the difference, not in arithmetic.
  The measurement splits into three submissions ON PURPOSE, to attribute cost per phase. A real queue
  goes to the device in ONE submission (§1's "executes as a whole, without returning to the
  orchestrator" is true at the Vulkan level too), so `70%` is an UPPER bound that the next slice must
  recompute.
- ORIGINATOR TOOLING REFINEMENT AHEAD OF THE TRANSLATOR (2026-09-04). `418/418` project tests, GN01
  `24/24` (`+2`). ALL THREE declarations from `ORIGINATOR_GPGPU.md` §5 step 2 are in; the translator
  stays behind them by decision.
  (0) CALL RANGE FROM A FIELD (`range = { count = state:field("used") }`), in both namespaces. The
  mechanism already existed and already worked on the host — GN03 declares capacity as the buffer size
  and delivers the USED length in an ordinary one-element buffer; what is new is that a range can
  REFERENCE such a field, and on a device that same buffer IS the `dispatchIndirect` argument. Rules,
  each following from the same fact: the counter is read at EXECUTION time (an earlier element of the
  same queue writes it, so a value read at declaration would be the previous run's); the field must be
  a single-component integer (a fractional element count is a silent truncation); NO element may write
  the counter LATER than the one reading it, and the refusal names both positions; the counter counts
  as a READ of the field, so the pass that computed the length is alive and the barrier between them
  falls out of the same question as the dead-work check; a counted range starts at zero and explicit
  bounds beside it are refused; and CAPACITY is what gets checked before execution, since the range is
  unknown while the capacity is exactly the bound the overflow clamps to.
  OVERFLOW CLAMPS RATHER THAN REFUSES, deliberately on BOTH paths: nothing to throw in a shader, and a
  CPU refusal against a GPU clamp would give one declaration two behaviours. The clamp is reported
  (`report.clamped`, visible in lua) because a silently truncated pass cannot be traced from the result
  — GN03 paid for that lesson with a hole in the world. Fusion follows: counted calls fuse only when
  they reference the SAME counter field, so equal ranges hold by construction; two counters holding the
  same number today do NOT fuse, because tomorrow they diverge.
  (1) BUFFER SHAPE (`extent`) REPLACES SIZE where a buffer is addressed as a raster: `extent = [ width,
  width ]` names the axes with the same size CONSTANTS, at most three, and the element count is DERIVED
  as their product — `size` next to `extent` is a loud refusal because it is a second way to name one
  number. This was not GPU work: the shape used to arrive as a PARAMETER to every tool that reads it,
  in THREE spellings (`width` for value_noise/noise_grid/box_blur, `size_x`/`size_y` for
  position_grid, `width`/`height` for voronoi_label) — one truth duplicated once per reader. Now the
  tool takes it from the BINDING (`resolve_extent`) and a step body from the buffer
  (`cells:extent()`). Both spellings stay valid while documents are translated, but NEVER on the same
  call: a shape parameter next to a declared extent is refused by buffer and parameter name, and a test
  proves the two spellings give a bit-identical field, so translating a config does not change the
  world. GN01 is translated; GN02/GN03 still ride the legacy path untouched.
  The translation itself found a conflation worth keeping apart: `voronoi_adjacency` and
  `voronoi_polygons` take `width`/`height` as WORLD bounds, not as the target raster — the numbers
  coincide in GN01 by accident, so those stayed parameters. Folding them into one shape would have
  declared two different quantities equal.
  (2) `ds` SAVED SLOTS ARE NOW RESET PER ELEMENT, closing the latent defect below, and it came out
  STRICTER than expected: reading a slot not written on this element is a LOUD refusal (`Saved value #0
  has type '', expected 'double'`), not a defined zero — so a program that is not pointwise never runs,
  instead of answering differently at different thread counts. Done the way ds itself does it for a
  fresh SUB-script frame (the `execute` opcode clears its saved frame's types); paid only by programs
  that actually use `ctx_save`, since the others declare zero slots. A compile-time dominance check is
  still wanted, but now as the condition for translating `ctx_save` into a shader local rather than as
  the fix.
  (3) §6.4 WAS CORRECTED BY THE AUTHOR AND THE CORRECTION IS THE BETTER IDEA. I had written "runtime
  path choice is incompatible with world determinism, so `auto` is only for presentation". What is
  incompatible is not the choice but a choice made from MACHINE properties. An ALGORITHMIC criterion
  ("a queue shorter than four passes runs on the CPU") is decided from values the config already
  declares, so every machine takes the same branch. The author's analogy is the right one: AVX vs SSE2
  FIXED IN THE BUILD MANIFEST — and the project already lived this, since FastNoise2 picked its SIMD
  set at runtime and one seed produced different artefacts per machine, closed not by a smarter choice
  but by `FASTNOISE2_STRICT_FP`, i.e. by removing the runtime dependency. So: the criterion reads only
  declared values (passes after fusion grouping, element count, footprint, presence of untranslatable
  calls); thresholds live in the MANIFEST and measurement says what number to write there, not which
  branch to take now; `auto` as a runtime notion is not needed at all; and a machine that cannot run the
  decided path REFUSES LOUDLY rather than substituting the other one — substitution is precisely the
  runtime dependency, and it is the same rule by which GN03 POSTPONES a chunk instead of generating a
  wrong one. Structural criteria are preferred over size-based ones, because a size threshold would
  have to enter the world's identity.
- ORIGINATOR GPGPU DESIGN PASS (2026-09-04, `ORIGINATOR_GPGPU.md` §6, design only — nothing built).
  THE HEADLINE: almost all of it is ALREADY IN PAINTER, so the work is what to DECLARE, not what to
  build. Verified in code: `compute_dispatch_constant` takes X/Y/Z from a constant (literally a
  `VkDispatchIndirectCommand` in constant data), `compute_dispatch_indirect` reads them FROM A BUFFER
  (`task.dispatchIndirect(res.buf, offset)`), barriers are already derived and stored on the step
  (`make_barriers1`), a transfer step exists (`transfer_copy_buffer`), and `resource_container` already
  carries `extent{x,y,z}`, `format`, `mips`, `usage_mask`, `mem_ptr`, `is_image()`, `host_visible()`.
  DECISIONS: (1) the device copy is a CACHE — the host owns the truth; the transfer plan is DERIVED
  from the queue's existing boundaries (upload = fields read before anyone in the queue wrote them,
  download = exactly `output`), and BARRIER DERIVATION IS THE SAME QUESTION AS THE DEAD-WORK CHECK, so
  it must not grow a second answer. First version has no residency at all, because invalidation is the
  only part that cannot be derived (a host `field:set` after a queue silently staleness the copy; the
  only cheap place to mark dirty is where a WRITABLE accessor is handed out, once per call).
  (2) Buffer-vs-image KIND is derived from usage exactly as painter already does (`is_image()` is a
  function of usage); what gets declared is the SHAPE, `extent`. That declaration is needed TODAY
  without any GPU: `width` currently arrives as a PARAMETER to `noise_grid`/`box_blur`/`position_grid`,
  and in GN01 `step.params.width` is written in two step bodies about the same buffer — the very
  duplicated truth the library forbids itself. (3) An image is worth it exactly when reads land on a
  NON-INTEGER coordinate (LUTs, upsampling a coarse field into a fine chunk — GN03's two-scale case is
  literally hardware bilinear); and FILTERED SAMPLING IS NOT DETERMINISTIC (Vulkan filter precision is
  implementation-defined), so it is strictly the presentation class and never enters chunked
  generation. (4) RUNTIME PATH CHOICE AND WORLD DETERMINISM ARE INCOMPATIBLE — §4.2 accepts CPU/GPU
  divergence only because the path is chosen ONCE; if the runtime picks, a machine with a GPU grows a
  different planet from the same seed. So `auto` is legal only for the presentation class, and a
  world-facing queue declares its path, which becomes part of the world's identity (same shape as
  `key_support`: silence means the safe answer, CPU).
  (5) FOLDING THE LINEAR INDEX INTO 2D/3D IS MANDATORY, NOT AN OPTIMISATION. Measured limits on this
  box: `maxComputeWorkGroupCount = [2147483647, 65535, 65535]`, `maxComputeWorkGroupSize =
  [1024,1024,1024]`, `maxComputeWorkGroupInvocations = 1024`, `maxComputeSharedMemorySize = 49152`,
  `maxStorageBufferRange = 4294967295`, `maxImageDimension2D/3D = 16384/2048`. Intel's X is effectively
  unbounded, but Vulkan's GUARANTEED minimum for X is `65535`, i.e. `4 194 240` elements at group size
  64 — and GN01 at `--size=4096` runs `16 777 216`. So a 1D dispatch already fails the portable limit,
  and worst of all it would WORK on this machine and break on someone else's: same family as the
  FastNoise2 SIMD-set divergence. Group SIZE stays a specialization constant; group SHAPE (`64x1` vs
  `8x8`) is a hint the author gives, and the engine clamps it against the limits OUT LOUD.
  (6) INDIRECT CLOSES CONDITIONAL WORK WITHOUT LEAVING THE QUEUE, and the mechanism already runs on the
  CPU: GN03's variable-length output declares capacity as the buffer size and delivers the USED length
  in an ordinary one-element buffer (`state.vertex_count`) — on the device that same buffer IS the
  `dispatchIndirect` argument. So a call's range may come FROM A FIELD (`range = { count =
  state:field("vertex_count") }`): one declaration, two paths. Two consequences: such calls fuse only
  when they reference the SAME counter field (equal ranges then hold by construction), and OVERFLOW ON
  A DEVICE CANNOT BE A LOUD REFUSAL — nothing to throw in a shader, so it must CLAMP and record the
  clamp in the output, which means a variable-length buffer carries two numbers, length and "hit the
  cap". A silently truncated surface is a hole in the world that cannot be traced from the picture, and
  that is literally GN03's lesson.
  (7) THREE OF THE NEXT FOUR ITEMS ARE CPU WORK that pays on its own and each removes one GPU blocker:
  `extent`, range-from-field, and the ds saved-slot check. The translator moves behind them.
  (8) LATENT DEFECT FOUND BY THE GPU QUESTION: `ctx_save` is a language construct, not a registered
  function, and `ds::context::clear()` resets the operand stack and the base offsets but NOT the
  `saved_stack` CONTENTS, while `run_chunk` builds the context per CHUNK and calls `clear()` per
  element. So a slot saved on element i is physically readable on element i+1, and a program that reads
  a slot it did not save on this element (a `ctx_save` inside an untaken `value_or` branch) picks up the
  previous element's value — with the chunk boundary depending on the thread count, which breaks
  "parallel == serial bit for bit". No live bug (GN01–GN03 rules are simple expressions where the write
  dominates the read), but the README's structural guarantee currently rests on the shape of the
  programs rather than on a check. Fix options in §6.8; the compile-time dominance check is preferred
  because the translator needs it anyway — one check, two consumers.
- ORIGINATOR COMPUTATION QUEUE, FIRST SLICE (2026-09-04, construction in `a7a73b7`, fusion after).
  Whole suite green at `402/402`; the queue's own targets are `originator_queue_test` and
  `originator_queue_lua_test` (the rest of the count growth is the concurrent NET work). GN01
  `22/22` with the queue in production.
  `ORIGINATOR_GPGPU.md` step 1 is now done except fusion; the design doc and
  `libs/originator/README.md` ("Очередь вычислений") carry the contract.
  THE CONSTRUCTION: a queue is a NAMED chain of pre-declared calls executed as a whole. It is split by
  NAMESPACE, not by a flag — `originator.box_blur{...}` computes now, `originator.queue.box_blur{...}`
  DECLARES, and `originator.queue{ ..., output = {...} }` runs the lot. Argument tables are identical
  in both namespaces; only the moment of execution differs. `originator.queue.run_script{...}` puts a
  `devils_script` program in as an ordinary element (the core `computation_queue.h` knows nothing of
  lua or ds — a foreign element carries its own body).
  THE RULE WORTH KEEPING: INPUTS ARE DERIVED, THE OUTPUT IS DECLARED. A queue sees every reader inside
  itself, so a field read before anyone in the queue wrote it IS an input and can be computed; who
  reads a result AFTER the queue it cannot know, so `output` is said out loud. From that same boundary
  follows the dead-work check: an element whose output nobody downstream reads and which `output` does
  not name is dead, and so is one whose result a later element overwrites without reading. Overwrite is
  judged by range COVERAGE — two calls writing different halves of one field are both alive, and a
  false refusal there would cost more than a missed warning.
  Apertures that fit are exactly those writing their own element into a declared output (`pointwise`,
  `gather`); the other three each get their OWN reason, not a shared "does not fit". `reduce`'s refusal
  turned out not to be a choice among mitigations but a consequence: a reduction is computed in order to
  READ its value back in the conductor (that is what `terrain.lua`'s normalisation does), and reading
  back in lua is the one thing a queue does not do. A declared call must reach a queue EXACTLY once —
  declarations are counted, and both a dropped and a twice-used element fail the step, because neither
  is visible in the result. The report returns `passes` (real data traversals, today == `calls`) so the
  fusion win will be MEASURED rather than assumed.
  FUSION LANDED. Adjacent `pointwise` calls over the SAME range now run in one traversal: the queue
  reorders the walk from "call, then its chunks" to "tile, then every call of the group", so the
  intermediate field stays in cache. Legal exactly for `pointwise` and exactly at equal ranges
  (element i of the second call reads ONLY element i, and the first call of that same tile has already
  written it); `gather` needs the whole previous pass, and a different range means one call's tile is
  not the other's. Bit-identity holds for the same reason it holds for a single pointwise call, and it
  is tested at 1/3/7 threads. Tile size comes from the group's footprint — bytes per element over
  DISTINCT fields, the shared intermediate counted once — targeting L1d as a starting number.
  MEASURED (`--bench-queue`, `remap`→`blend`→`modulate`, Release, i7 / 11 threads; variants
  interleaved, read the median): **zero on one thread at any size** (`+0.7%` / `-1.0%` / `-0.4%` at
  `1.0` / `4.2` / `16.8` M elements) and **14–25% on eleven** (`+25.4%` / `+14.0%` / `+14.1%`). The
  explanation follows from the same numbers rather than being guessed: a pass costs `18.8` ns per
  element, so ONE thread pushes about `0.6` GB/s — orders of magnitude below bandwidth, nothing there
  to save. Eleven threads bring the pass to `4.0` ns per element for a speedup of only `4.7x`, i.e.
  the parallel pass is ALREADY bandwidth-limited, and only then does the saved traffic show.
  SO THE REAL SINGLE-THREAD LEVER IS NOT FUSION: `18.8` ns for one multiply-add is the price of the
  GENERIC ACCESSOR (`get`/`set` with bounds checks and a field-kind switch per element). The
  `as_span<float>` fast path exists in the library but only the noise and volume tools use it — the
  arithmetic tools do not. Fusion is not thereby pointless: it removes exactly the cost that will
  remain after the accessor, and on a GPU that cost is the main one.
  MEASUREMENT LESSON, same family as GN03's "measure without pacing": at a few percent, one run on a
  laptop measures thermal state — in the general `--bench`, which runs the whole pipeline four times
  before this line, the SIGN of the result changed between runs. Hence a separate mode where the two
  variants INTERLEAVE and both minimum and median are printed. The first pair's minimum means nothing
  (at `1024` it read `-72%` purely because the variant that went first got a hot buffer); read the
  median, and use the minimum only to judge whether to believe it.
  A `ds` program does not join a fusion group yet: its execution lives in `originator_script`, and
  entering it per tile would rebuild the VM context each time — it needs a prepared call of its own,
  like `prepared_call` for a native tool.
  TRANSACTION-WITH-CALLBACK (a coroutine collecting intermediate values, DB-transaction style) WAS
  CONSIDERED AND REJECTED, reasons recorded in `ORIGINATOR_GPGPU.md` §4.5a: the analogy carries the
  word but not the mechanism (no concurrent writers, no log, no rollback); it un-declares the call set
  and kills both "one refusal before execution" and the dead-work check; on a device it means a sync
  point and a round trip INSIDE the queue, i.e. exactly the cost the queue exists to remove; and an
  observable intermediate must materialise, which disables fusion. The shape that IS wanted already
  exists and is called a queue boundary — two queues with ordinary lua between them, as
  `terrain.lua`'s normalisation already does.
  **NO LAMBDAS IN THE BINDINGS.** Every function reaching lua from `script_host` is now a member
  function pointer (`&script_host::run_tool`, …) or a free function (`field_get`, `field_set`, …), and
  the per-tool shortcuts — the one thing needing a bound name — are LUA closures built by a two-line
  factory over the bound `run`, so they cost no C++ type. Named functions have unique type names by
  construction, which makes the sol2 bug below UNEXPRESSIBLE rather than merely unlikely. Reserved
  names (`run`, `run_script`, `tool_exists`, `queue`) are refused loudly at host construction, because
  a tool with such a name would silently shadow the function.
  QUEUE ELEMENT LIST IS READ BY HIGHEST INTEGER KEY, NOT BY `#`: on a table with a hole lua's length
  is undefined (`{a, nil, b}` may legally give 1 or 3), so by `#` a missed element would simply
  vanish, and a queue one pass short differs from the right one only in a result nobody expected. A
  hole, a plain value, and an unknown key (`outputs =` for `output =`) all fail loudly and by name.
  **SOL2 LANDMINE FOUND AND FIXED, and it is not queue-specific — see Build/Layout Notes.** Two
  DIFFERENT lambdas declared in the SAME function with the SAME parameter list share sol2's `__gc`
  metatable (gcc prints a lambda as `<lambda(args)>`, with no ordinal, and sol2 keys the functor
  destructor by the demangled name), so one type's destructor destroys the other's functor at the
  wrong layout. It surfaced as `free(): invalid size` at `lua_close`, arbitrarily far from the cause.
- GN03 CLOSED BY A CLOSING AUDIT (2026-09-04). GN03's own `104/104` property checks (`+17`);
  `359/367` project tests — the eight failures are `GameNetworkingSockets`' own tests, and that
  dependency does not LINK in this environment (the system `abseil` was upgraded, the cached build
  still references the old `.so` versions); nothing in the audit touches it.
  THE AUDIT'S BEST FIND IS ABOUT THE LAB'S OWN SUBJECT: the spin of an entity around its axis was
  hashed IN THE SHADER FROM ITS POSITION, and the position a frame sees is relative to the CAMERA'S
  CHUNK — so entities turned in place every time the observer stepped into the next chunk. The rule
  worth naming: everything derived about an entity is derived from its NAME (chunk key plus attempt
  number — the very identity the world remembers it by), never from the frame's numbers. The spin now
  comes from the CPU in eight bits of the kind word, and the check reproduces the OLD way too, so the
  difference is visible in the report. Other defects: entity columns were drawn from nodes
  `2..cells-1`, so they kept two cells from the far border and one from the near one — a band without
  entities along every seam (the check measures both bands: `103` against `0` before, `94` against
  `83` after); surface time accumulated with `+=` into a REUSED mesh that the streamer does not reset
  when the window discards a chunk, so the reported chunk cost lied exactly when the window moves
  fast; the skeleton PACKAGE was read without the CSR invariants that `build` checks (a plausible size
  with one corrupted offset indexed points PAST THE END of the array — and a file is foreign data);
  in the engine, `polyline_distance` read `max_distance` TWICE with DIFFERENT defaults (`0` in prepare
  as the pruning box, `1e9` in the body as the answer where there is no polyline — a call without the
  parameter produced a field with a discontinuity, which reads as a wall in the middle of the world),
  and `ratio`'s `minimum_divisor` was DOCUMENTED as mandatory while having a default, i.e. the engine
  decided which weight sum counts as zero; both are now required, with tests for both refusals. Two
  silent truncations where the whole lab refuses loudly: the entity buffer overflow broke the loop
  quietly (and the declared capacity `128` did not cover the declared `prop_attempts` range of `200`),
  and "the biome weights never sum to zero because the base biome spans the climate plane" was a
  property of the TABLE, which the config author edits. Plus the arena's slot discipline — the thing
  that once broke — is now three checks instead of a comment.
- GN03 SCENE OPTIMISATION: THE FRAME'S PRICE IS THE DRAWN RANGE, NOT WHAT IS VISIBLE (2026-09-04).
  The question came from looking at the screen ("frames drop when flying with shift"), so the lab
  gained a RECORDED FLIGHT (`--flight=<m/s>`, fixed route, step capped so runs stay comparable) and a
  per-phase frame report plus device passes through the engine's existing `gpu_timestamp_profiler`.
  (1) MEASURE WITHOUT PACING. The same frame with the same geometry costs `5.1` ms when the loop runs
  flat out and `10.6` ms when paced to sixty: the integrated GPU sits at `633` MHz of `1250` and never
  clocks up in a leisurely loop. A paced measurement measures power management — same family as PF03's
  "compare dumps only at fixed exposure".
  (2) The scene pass is proportional to the vertices IN THE DRAWN RANGE (`1.33`/`2.80`/`4.83` M →
  `1.81`/`2.99`/`4.21` ms; marginally about `0.65` ns per vertex), and the resolution sweep
  (`3.69`/`2.55`/`2.16` ms at 1280x720 / 640x360 / 320x180) splits that into ~`2.1` ms of vertex
  stream and ~`1.5` ms of fragments.
  (3) PER-CHUNK FRUSTUM CULLING BUYS NOTHING, AND THAT IS MEASURED: `29`–`30%` of the window's chunks
  are visible and the pass does not move (`4.65` against `4.74` ms hot, `10.49` against `10.72`
  paced), because an off-screen triangle produces no fragments anyway while the vertex stream is still
  read in full — a vertex learns its slot only from its own bytes. Kept behind `--cull` to reproduce
  the measurement, off by default. NAMED ENGINE GAP: an invisible chunk becomes free only with a
  MULTI-draw indirect (one draw per chunk from a list), and painter's `draw_indirect` step hardcodes
  `drawCount = 1`; at 30% visible that is the difference between `2.1` and `0.6` ms of vertex stream.
  The second lever lying next to it is welding vertices inside a chunk (an index buffer), which
  marching cubes deliberately does not do.
  (4) THE HITCH WHILE MOVING WAS THE EVICTION SLAB, and it is fixed. A sideways window step evicts
  `45` chunks at once (`81` upwards), and eviction zeroed the chunk's whole arena span: measured
  `36.6` M of the `78.8` M uploaded vertices were ZEROS (half of all upload traffic), the worst frame
  carried `9.66` MiB, and its evict and upload phases cost `5.25` and `5.13` ms on top of a `6.6` ms
  frame. Now A CHUNK'S DEATH IS A ROW IN THE OFFSET TABLE (the shader collapses a dead row's vertex to
  a point), so eviction costs ZERO arena bytes; the zero-fill remains only so a slot and its span can
  return to circulation (while foreign vertices with that slot still lie in the arena, handing the
  slot out would gift the new owner the previous chunk's triangles) and is paid on a declared per-frame
  BUDGET (`65536` vertices = 768 KiB, `--scrub`). Worst frame: `2.3`–`2.9` MiB instead of `9.66`.
  (5) The number that answers the original question: near the ground with the default window the paced
  frame costs `15.4` ms STANDING STILL (`10.0` ms scene pass, `1.0` ms the lab's overlay, `0.16` ms
  ALL of the streaming work), so `137` of `600` frames are already over a sixty-per-second budget. The
  drop while moving is the price of the WINDOW, to which movement adds the last few percent; the only
  lever today is a smaller window (`--radius=3`) or a coarser chunk.
- GN03 STREAMING VOLUME OPENED: THE GENERATOR NOW EMITS GEOMETRY (2026-09-03). `367/367` project tests
  (`+13`), GN03's own `87/87` property checks, GN02 unchanged. Third generator-campaign lab
  (`subprojects/playgrounds/GN03_streaming_volume`): a free camera flies while marching-cubes surface is
  built around it from a chunked density field — hills, overhangs, arches and tunnels.
  (1) THE REAL QUESTION WAS VARIABLE-LENGTH OUTPUT, and the answer is that NO NEW MECHANISM was
  needed. Every originator buffer so far had a known length (as many heights as cells); how many
  triangles a density field yields is unknown until it runs. Three things that already existed sufficed:
  capacity is the buffer's declared size, the USED length arrives in an ordinary one-element buffer
  (`state.vertex_count` — "state between steps is a normal buffer with size = 1"), and overflow is a
  loud refusal naming the buffer and both numbers. Not truncation: a truncated surface reads as a hole
  in the world and cannot be traced from the picture.
  (2) `tool_registry::add_volume_tools()` + `marching_cubes` in the engine: aperture `scatter`, and the
  project's first genuine `chunk_local` key support (the group is one pass's vertex list, and the chunk
  finishes it itself). Three fixed-partition phases exactly like `group_by`: cube case per cell → integer
  prefix sum in cell order → each cell writes its own precomputed span, so the result is bit-identical at
  any thread count.
  (3) THE 256-CASE TABLE IS DERIVED, NOT TRANSCRIBED. Canonical tables are data one can copy with a typo,
  and a single wrong row is a hole that shows up once in a hundred chunks. It is built from the face rule
  instead, and the rule is a function of ONLY that face's four signs — so two neighbouring cubes read the
  shared face identically and surface continuity holds BY CONSTRUCTION. Orientation follows from the same
  rule (solid stays on the right seen from outside), so triangle winding agrees with `-grad(density)`.
  (4) FOUR FINDINGS, EACH AN ERROR BEFORE MEASUREMENT. "Watertight" is about DIRECTED edges: the
  criterion "every edge in exactly two triangles" produced 74 false failures out of 20752 because on an
  ambiguous face the surface legitimately TOUCHES ITSELF along a segment; the right test is forward count
  == backward count. Metres were not metres: the engine's shared noise tree has its own scale (period
  `91` units at frequency 1, range `±1.35`, mean `|v|` `0.385`), so "a 190 m feature" was a 17 km feature,
  a 32 m chunk saw a CONSTANT field, and the world came out empty — found only because the report prints
  field ranges, since zero vertices cannot distinguish "empty" from "no surface here". A postponed chunk
  must WAIT for arena room rather than go back into the queue (requeuing thrashed 717 regenerations
  instead of 324 chunks). And the camera started inside a hill, because relief reaches 1.35 amplitude.
  (5) MEASURED (Release, Iris Xe): chunk of 32 cells = `42875` samples costs `4.15` ms and `8524`
  vertices single-threaded; `216` chunks/s on one thread against `870`–`970` on eleven (`4.0`–`4.5x` — memory bound,
  each worker drags its own 1.4 MiB sample buffer); default window `9x9` chunks × 4 layers = 324 chunks
  and `4.67` M vertices; frame `4.44` ms in ONE draw call. The arena is one 96 MiB buffer drawn as
  `[0, high_water)`: holes are filled with degenerate triangles, spans are granular in multiples of
  three, and a freed span waits `frames_in_flight` frames before reuse (so an unloaded chunk lingers a
  frame or two instead of tearing).
  (6) Not in scope, deliberately: LOD and cross-level stitching, editable fields, collisions, occlusion
  (cave walls underground are meshed and drawn though nobody sees them — that is why the arena is large),
  textures/materials, chunk persistence.
  (7) SAME DAY, TWO MORE SLICES. The world now goes IN EVERY DIRECTION: the chunk window became a CUBE
  around the observer (radii on all three axes instead of absolute vertical bounds, which meant "the
  world is flat and has a top"), and above ground level there are floating islands — a rare noise
  excursion with a height gate and a fade, composed by `maximum` rather than a sum (a sum makes an
  island thicker the lower it hangs, and at ground level it becomes a second relief layer).
  (8) EVERYTHING IS NOW RELATIVE TO THE CAMERA'S CHUNK, and no large world coordinate exists in the
  vertex, the shader or the camera. A vertex is TWELVE bytes: three `uint16` of fixed point inside its
  own chunk (half a millimetre step), a `uint16` chunk slot, three `int8` of normal. Where that chunk
  sits relative to the camera comes from an offsets table of 4096 slots — 64 KiB, rewritten whole every
  frame. Consequences: the camera leaving its chunk costs 64 KiB of writes instead of 72 MiB of arena,
  frame precision no longer depends on how far the observer flew, and the vertex shrank by a quarter.
  `local_frame`/`rebase`/`chunk_offset` are the whole mechanism; the floor division is deliberate,
  since truncation toward zero puts −0.5 and +0.5 in the same chunk.
  (9) THREE MORE FINDINGS. A CHUNK SLOT MUST NOT BE RECYCLED BEFORE ITS BYTES: while the arena kept a
  removed chunk's bytes ("don't write what the device is reading") but released the slot at once, the
  lingering triangles kept drawing with their old slot and jumped to A DIFFERENT PLACE in the world for
  a frame or two — the correct order is the reverse, zero the span immediately (a degenerate triangle
  draws nothing whatever its slot) and delay only the REUSE of the space. A FLOATING ORIGIN FIXES
  RESOLUTION, NOT ACCUMULATION: measured over 370 km of flight, a naive `float32` position drifts
  `2783.78` m and the framed one `0.36` m — but not to zero, because the local offset is still summed in
  `float`; the scheme's promise is constant resolution (`3.8` µm against `3` cm), and the check now
  states that instead of absence of drift. THE FIELD HAS ITS OWN, NEARER LIMIT: lattice positions reach
  the noise as `float32`, so up to `10^7` m the lattice is still distinct while at `10^8` m 88% of
  neighbouring nodes collapse onto one position — with `--start=1000000:2:-500000` the rendering is
  perfect and the islands are cubes. The world's working radius is therefore a stated number, and only
  different arithmetic inside the noise would move it.
  (10) UNITS: NOISE FEATURE SIZE IS NOW A WORLD LENGTH, AND THE TREE'S SCALE IS MEASURED. The lesson
  from the "metres were not metres" bug was not "multiply by 91" but "never assume the scale of SOMEONE
  ELSE'S data", so the conversion moved out of the config and into the engine: `noise_at`/`noise_grid`
  take `feature` in world units, and `prepare` measures the tree's period itself (sign changes along a
  DIAGONAL line — along one axis a lattice noise passes through its nodes and reads the wrong scale;
  seed ALWAYS zero, because the period must be a property of the TREE, not of the world, or the same
  `feature = 190` would mean different metres in different worlds; cached by tree text, since 8192
  samples per chunk would be a quarter of the noise's own cost). `frequency` stays with its own meaning
  (noise units per world unit) for fields where a world length is meaningless, e.g. the planet's unit
  sphere; giving both is a loud refusal. GN03's config now speaks only metres and carries no conversion
  factor at all. The library README also states which of the OTHER tools carry length: `radius` on the
  blurs is LATTICE STEPS, not metres.
  (11) TWO STREAMING BUGS WITH ONE SYMPTOM ("a chunk never appears until you fly away and come back"),
  both in the queue rather than the generator. LOST QUEUE: `set_window` rebuilt the queue and enqueued
  only keys absent from the state table — but a chunk that had been WAITING in the previous window was
  already in that table, so it stayed "queued" precisely nowhere; flying away dropped it from the table
  and coming back re-created it, which is exactly the reported symptom. NO WAKEUP: `forget` (a chunk
  that did not fit the arena, or went stale) put the chunk back in the queue without notifying the
  condition variable, so with all workers asleep it lay unnoticed until the next window change. The
  first is pinned by a regression check that FAILS with the bug restored: the window moves three times
  while the queue is still full, and every chunk of the final window must arrive (one worker
  deliberately — with eleven the queue drains faster than the window moves and the bug hides).
  (12) DETERMINISM BOUNDARY, STATED. A chunk's CONTENT is fully deterministic (four checks: order,
  threads, cleared buffers, a second pipeline), and a fingerprint of twelve reference chunks (`178545`
  vertices) is printed as one line — measured on this machine, Debug and Release produce it BIT
  IDENTICAL, so neither FMA contraction nor reassociation moved anything on this pipeline. It is one
  free data point, not a proof of cross-platform equality. WHEN a chunk appears is NOT deterministic,
  and that is a stated property: the mesh is a PRESENTATION of the field, not world state, so per
  `NETWORKING.md`'s split it belongs to the non-deterministic class — hence checks assert "every chunk
  of the window arrives", never "in which frame". Inside a chunk the geometry is ALREADY integer
  (positions in fixed point, normals in bytes), which makes float disagreement rare and bounded rather
  than absent; only an integer FIELD removes it, the same conclusion `ORIGINATOR_GPGPU.md` reaches for
  the GPU. The boundary moves the day the surface becomes collision or navigation — then it stops being
  presentation and has to be computed in fixed point end to end.
  (13) ACCUMULATION IS A SECOND DISEASE AND IT IS NOW CURED. A floating origin fixes RESOLUTION (what a
  number can express at all); ACCUMULATION (a million roundings summed) does not depend on the origin at
  all — it depends on the ACCUMULATOR'S WIDTH, because every addition rounds by half a float step of the
  current magnitude and a session is millions of frames. Measured over 370 km of travel: a world
  position in `float32` drifts `2783.78` m, in-chunk with a `float32` accumulator `0.364759` m, in-chunk
  with a `double` accumulator `2.9e-10` m. The first number argues for the floating origin, the second
  against calling it sufficient — `0.36` m accrues in a couple of play sessions, which is an obstacle,
  not a footnote (the first write-up wrongly filed it as an accepted limit). The observer position is
  now `double` (`local_frame::position`), converted to float once per frame for the view matrix and
  never fed back; `playground::free_camera` gained separate `look`/`displacement` so the ACCUMULATOR
  BELONGS TO THE CONSUMER — a world larger than `float32` needs it in `double`, a one-room lab does not.
  A FOURTH SCHEME closes the question: key + offset in FIXED POINT (1/65536 m) drifts EXACTLY ZERO, and
  zero BY CONSTRUCTION rather than by magnitude — offset and chunk span are integers, so both the
  addition and the wrap are integer, and integer addition does not round. The only price is a one-time
  quantisation of the step itself (`4.9e-6` m): a fixed bias, identical on every machine, that does not
  grow with time. Hence the split by purpose: the camera is PRESENTATION and needs a float for the view
  matrix anyway, so a `double` accumulator costs twelve bytes and settles it; ENTITY positions, once
  they exist in the simulation, must be "key + fixed point" rather than "key + double", because there
  what is needed is not small drift but its ABSENCE plus bit equality across machines — the same
  conclusion the project already reached choosing fixed point as the simulation language, and the same
  representation GN03's VERTICES already live in. Also worth recording: the drift has no observer at
  all while a path is computed ONCE (the world is `f(key, seed)`, geometry is relative, chunk choice is
  by key — nothing observable depends on the "ideal" path); it becomes observable only when the same
  path is computed TWICE, i.e. lockstep, replay, a saved waypoint — which is exactly the project's
  stated direction, hence worth curing.
  (14) "IS CROSS-PLATFORM FLOAT MATH WORTH WORRYING ABOUT" NOW HAS A NUMBER. A last-bit field
  disagreement becomes different geometry only through DECISIONS, and marching cubes has two: the sign
  of a node against the iso level (which picks the cube case, i.e. topology) and the fraction along an
  edge (which moves a vertex — but the vertex is quantised to fixed point with a `0.5` mm step, so a
  difference below one quantum vanishes entirely). Measured over `257250` nodes: within `1e-7` of the
  iso level (the scale of FastNoise2's SIMD divergence) NOT ONE node, within `1e-5` none, within `1e-3`
  (far larger than any arithmetic disagreement) three — one flipped node per two chunks, and a flipped
  node changes the geometry of at most the eight cells around it. Hence the rule beyond this lab: float
  divergence is dangerous exactly where a decision AMPLIFIES it. GN02's sea level, found by bisection
  against a land-share target, is such a decision (a last bit moves the threshold, the threshold moves
  which cells are land, and it is a different planet); GN03's decisions are local, so there is no
  amplification — and that is measured rather than assumed.
  (15) FIRST ENTITIES: KEY PLUS OFFSET, DERIVED FROM THE FIELD. Markers with a place, a tilt and a kind,
  and their mechanism is the geometry's: position is an integer chunk key plus an offset (no absolute
  position anywhere), the CHUNK OWNS them (unload the chunk and they go with it — no separate registry
  of world objects was needed), and what stands in a chunk is a third generator step, i.e. `f(key,
  seed)` exactly like the field. Variable-length output again, same mechanism again: declared capacity
  plus a counter. THE SECOND SEED FINALLY EARNED ITS KEEP: the field must be CONTINUOUS across a seam
  (`step.seed`), while entity scatter must be INDEPENDENT per chunk (`step.chunk_seed`) — GN01's
  two-seed contract existed from the start and no consumer had used both until the volume. Placement:
  forty attempts per chunk, the column chosen BY HASH (a lattice scan would show regularity at seams
  because candidates would start from each chunk's own origin), then FLOORS are found in that column —
  there can be several, because a cave floor is a floor too, and that is exactly what separates a volume
  from a height map. Everything else comes from the field: exact height is the linear crossing fraction
  (the same one marching cubes uses, so the marker stands ON the surface rather than on the nearest
  node), tilt is minus the density gradient, a wall does not count as a floor, and the kind is chosen by
  whether there is a ROOF overhead — pole in the open, boulder on a slope, glowing crystal under a roof.
  An entity is not placed onto the world, it is DERIVED from it. Rendering: instances, and the buffer is
  rewritten WHOLE every frame rather than living in an arena — the difference is quantity, not
  principle: millions of vertices against a dozen entities per chunk (measured `1072` in the window
  above ground, `2631` below), half a megabyte per frame is what the overlay writes anyway, and a second
  arena with slots and deferred release would cost more in every way. Seven checks, all about the world:
  reproducibility by the same three routes, "stands exactly where the field crosses the iso level",
  "lies inside its own chunk", "never on a slope steeper than the declared limit", capacity against the
  most populated chunk of the sample.
  (16) A LATENT BUG THE DEBUG BUILD CAUGHT: the entity step computed a lattice index as
  `x + side * (y + side * z)` where `side = cells + 3`, and config numbers reach lua as `double` — so
  `side` was `35.0` and the index came out fractional. Release accepted it silently (sol2's safety
  checks are on only in debug), Debug threw "number maybe has significant decimals". The rule: cast lua
  indices to integers EXPLICITLY (`math.tointeger`), because a "number" from the config is not an
  integer. After the fix the world is unchanged (same 84 entities, same fingerprint) — it was a typing
  bug, not a world bug.
  (17) WORLD MEMORY: DERIVED PLUS DELTA. While an entity is derived it is FREE — unload the chunk and it
  is gone, come back and it is recomputed bit-identically. Memory begins where the world stopped being a
  function, and what is stored is not the world but the DIFFERENCE: `world = derived + delta`. Identity
  is (chunk key, generator ATTEMPT NUMBER), not the slot in the output array: the slot depends on how
  many attempts before it were rejected, so nudging the slope threshold would shift every slot and make
  the memory of one entity the memory of another. The store is sparse (it grows with what was touched,
  not with what exists: `760` entries = `30416` bytes, 40 bytes per record), it IS the save file (record
  order is fixed by sorting rather than by hash-table iteration, so two saves compare byte for byte),
  and the join of derived with the store is a FUNCTION — verified by the very scenario that motivated
  it: a chunk recomputed from scratch after other chunks joins into the same world.
  THE FINDING is the CRITERION for what enters a save: "THE WORLD DIFFERS", not "something happened".
  The first version counted a touch counter in "the record is empty", so unmarking left a record forever
  — a player who touched and reverted a thousand markers paid a thousand records. A quantity nothing in
  the world depends on is a STATISTIC, not a difference; the moment something depends on it, it becomes
  a difference and enters the condition by itself. A delta with nothing to match (the world changed
  under the save: another seed, other settings, another rule version) is NOT dropped but COUNTED — the
  entity may come back, yet "half the player's memory no longer refers to anything" must be visible as a
  number. Deliberately absent: entities the player CREATES, which the derived world never had — those
  need an identity not tied to the generator, i.e. a REGISTRY rather than a delta.
  (18) THE TWO SCALES FINALLY MET: SKELETON AND CHUNK META. The library has declared them since GN01
  ("a coarse world pass, once, and a fine chunked one, on demand") but they had never met — GN03's field
  is pure noise and needs no skeleton. The skeleton is a SEPARATE generator (`generator/skeleton`):
  gameplay nodes on a jittered lattice, a nearest-neighbour tour, Catmull-Rom smoothing FLATTENED INTO A
  POLYLINE; it runs once (`1.2` ms), covers `4096` m against the ~300 m visible window, and lives as a
  package on disk (a package from another seed is not silently accepted — that is a different world).
  THE QUESTION AT THE SEAM is how a chunk gets its share of the skeleton, and the answer is NOT THE
  SKELETON BUT A QUERY OVER ITS AREA, whose result arrives as an ordinary buffer with declared capacity.
  Three rules: the query must be COMPLETE (if it returns "whatever happened to be resident", the same key
  yields a different world depending on load history — incomplete meta is not "slightly worse", it is a
  DIFFERENT WORLD, so the spatial index is checked against an exhaustive scan); the query margin is
  DERIVED from the radius of influence DECLARED BY THE SKELETON (a segment passing NEAR a chunk bends the
  field inside it, and the query adds the margin itself so it cannot be forgotten outside); overflowing
  the capacity REFUSES rather than truncates (a truncated route gives a corridor that ends inside a
  mountain). Measured: the route reaches 7 of 147 sampled chunks, the densest needing 3 points of the
  declared 96.
  TWO ENGINE ADDITIONS came from it. `polyline_distance` (gather; points plus CSR chain offsets; bbox
  rejection; projection CLAMPED to the segment ends, since "distance to the line" continues the corridor
  past the end of the route, i.e. builds a tunnel to nowhere) is the quantity that was missing to turn a
  coarse route into a field — the engine deliberately knows no curves, because Bézier/Catmull-Rom/arcs
  are many and all flatten into a polyline ONCE at the coarse scale, where the flattening density IS the
  smoothness. And `pipeline_description::inputs` — PIPELINE INPUTS: the meta buffer is filled by the
  HOST, so no step writes it and none should, yet "reading before anything wrote" would otherwise reject
  the whole pipeline. The input is DECLARED by the author rather than inferred from "nobody writes it":
  that is exactly what the library must treat as an error, and only a declaration distinguishes "brought
  from outside" from "mistyped the name".
  The corridor is cut by OVERLAYING A MINIMUM rather than subtracting (inside the tube the field must go
  negative no matter how much rock was there, and outside it must not be touched at all — subtracting a
  constant would punch a pit at the surface and nothing deep in a mountain), and it is cut LAST: while it
  ran before the islands, an island landing on the route honestly filled the tunnel back in. The property
  checked at the seam: two neighbouring chunks have DIFFERENT meta yet agree exactly on their shared face.
  (19) BIOMES: RULES ARE BLENDED, NOT PICTURES. A biome is a set of NUMBERS (amplitudes, gradient, cave
  width/strength, shade) plus a place in a climate plane; one rule serves all of them, and density is
  computed per biome PRESENT in the chunk and summed with its weight. Five biomes: plateau, mountains,
  karst (the world becomes a network of passages), steppe, badlands. Two decisions make it work.
  A WEIGHT WITH COMPACT SUPPORT — `(1-r²/reach²)²` reaches EXACTLY ZERO at its radius rather than "almost
  zero" like an exponential, and only that makes "compute just the present biomes" an EXACT equality:
  verified bit-identical against computing all five. With an exponential one would have to pick a
  "negligible" threshold, and the world would depend on where that threshold sits. AND THE COST IS THE
  NUMBER OF BIOMES IN THE CHUNK, NOT IN THE WORLD: presence comes from the chunk's climate RANGE (two
  reductions) and is tested conservatively; measured `2.5` biomes per chunk of five, `15.9` ms with
  pruning against `21.9` ms without. Noise layers are computed ONCE for all biomes, and that is about
  cost: feature size enters the frequency, so a biome with a different SIZE would force a noise pass per
  biome, whereas a different amplitude costs one multiply — hence the rule that amplitudes and
  thresholds are what to vary first. Normalisation required one new engine tool, `ratio`, with a
  MANDATORY minimum divisor: a zero weight sum means "no rule acts here", and a silent zero density
  would be a SURFACE, i.e. a wall inside a coverage hole. Colour comes FROM THE GENERATOR (the shade
  accumulates with the same weights, rides in the vertex's fourth byte, and the shader expands it into a
  ramp) — computing the biome in the shader would be a second copy of the rule that eventually disagrees
  with the first, painting the colour of a biome that was not the one computed.
  (20) TUNNELS: A STYLE IS GEOMETRY, NOT A COEFFICIENT. The skeleton route carries styles, and a style
  belongs to a CHAIN rather than a point (it changes at nodes, or the cross-section would jump mid-
  passage); it is chosen per LINK, and consecutive links of one style become one chain, so the world
  gets stretches instead of alternation every step. A natural cave is a round cross-section (euclidean
  metric) with walls broken by the detail layer and a Catmull-Rom path; a bunker tunnel is angular (the
  chebyshev metric, whose level surface is a cube), with even walls and straight segments turning at
  nodes. Hence TWO CALLS with different metrics instead of one with a coefficient, and TWO PAIRS of meta
  buffers instead of one with a "style" field: the distance tool works over one set of chains with one
  metric and cannot be given a subset of chains — so the host splits them while filling the input, where
  it is one style test per copied chain.
  (21) A measurement metric must not bottom out on a plateau: the field limit was first measured as the
  share of neighbours sharing a DENSITY value and read 32% at the origin, because in the air density
  saturates on the islands' constant floor (`max` with a constant) — the metric was measuring the
  plateau. Position is what loses precision, so position is what to measure.

- ORIGINATOR THROUGH DEMIURG: THE GENERATOR IS NOW ONE NAME (2026-09-03). `354/354` project tests
  (`+9`), GN02 `71/71` unchanged and bit-identical output. The ask was an ENTRY POINT: a generator
  should be addressable by one id, and that id should name its own parts, instead of the host knowing
  the names of three files — which is the same as the host knowing the internals of somebody else's
  generator, and which leaves a mod no way to lay its own generator out differently.
  (1) THE LINES THE AUTHOR HAD ALREADY WRITTEN WERE A LIVE BUG. `values = values.tavl` and
  `buffers = buffers.tavl` sat at the top of GN02's `steps.tavl` since `848d179`, and they did not
  "do nothing": a top-level ROW next to top-level `{ ... }` BLOCKS makes tavl read the document as one
  aggregate whose blocks are excess values, and with the step mirror that turned into unbounded
  allocation — the generator was killed by the OOM killer before printing a line, at ANY resolution.
  Measured and confirmed by deleting the two lines. Both confusions are now classified BEFORE parsing
  (`document_is_list`) and refused by name: a row inside a steps list, and a bare list of blocks handed
  to `parse_entry`.
  (2) THE ENTRY IS ONE DOCUMENT: `name`, `values`, `buffers` and `steps = [ ... ]`. The steps had to
  move INSIDE the document rather than stay top-level blocks — not for looks, but because the two
  shapes cannot coexist in one tavl document at all (see (1)).
  (3) PATHS ARE DEMIURG IDS: root-relative, extension-free, `./` relative to the entry's own folder.
  `absolute_resource_path`/`resource_parent_path` MOVED from `simul` (where they sat next to the lua
  bindings) into `demiurg/resource_path.h`: the rule belongs to resource addressing, not to lua, and
  the second consumer would otherwise have grown a second opinion about what `../common/values.tavl`
  means.
  (4) NEW TARGET `devils_engine::originator_config` — `generator_source` (a text carrier exactly like
  `painter::render_config_source`) plus `load_generator(resources, entry_id)` returning a
  `generator_config`: pipeline description, value ranges, and every script text keyed by absolute id.
  The core `originator` still knows nothing about demiurg, as it knows nothing about lua.
  (5) TWO SILENT DEFECTS IN DEMIURG'S `//---` SPLITTER, both found by writing the config: the separator
  was matched as a SUBSTRING (so a file that merely MENTIONS `//---` in a comment was split — and the
  entry file's own comment says exactly that), and the separator's TITLE (`//--- имя документа`, which the playground configs
  happen to be written with) was left in the following section, where it became that section's first
  value. Both are now line-anchored/consumed-to-end-of-line, with regression tests.
  (6) BOTH PLAYGROUNDS CONVERTED: generator files moved into `resources/gn02/generator/` and
  `resources/gn01/generator/` — real demiurg MODULES — and both `main.cpp` lost `read_file` entirely.
  Output is bit-identical (409 land cells of 1024 at the same seed before and after), GN02 `--verify`
  still `71/71`, GN01 `22/22` plus its chunked comparison, `354/354` overall. A lua chunk name is now
  the demiurg id, so a script error points at the address the script actually lives at
  (`[string "generator/scripts/regions"]:39`). GN01 needed one extra seam: `read_generator_source`,
  because its perf bench runs a lua TWIN of a `ds` rule that is not a step, so the entry does not name
  it. Third rule of the same family, stated in both READMEs: an entry FILE must carry no `//---`, or
  its base id stops existing and the failure reads like a typo in a path that is in fact correct.
  (7) DESIGN ONLY, NOT SCHEDULED: `ORIGINATOR_GPGPU.md` (root) writes down the computation-queue /
  GPGPU task — goal, what it buys (GPU obvious; on CPU honestly small, and the only real speedup is
  FUSION of adjacent pointwise passes, not removing lua calls), what it needs (`ds`→GLSL, whose math
  vocabulary ALREADY IS GLSL's names; GPU specifics incl. dispatch groups; a small headless context),
  and its limits with mitigations. The author's decisions, not to be re-litigated: the unit of
  transfer is a QUEUE not a step; there is NO float determinism on GPU, so the queue is unusable for
  chunked generation unless integer; a CPU/GPU result mismatch is a STATED PROPERTY, not a defect
  (the generator needs no bit-exactness between paths, and `ds` can be taught `float`); the PRNG
  inside originator can be 32-bit (a `uvec2` splitmix64 is nice-to-have, not a blocker); painter
  ALREADY does headless (`presentation_engine_type::no_present`, `choose_physical_device_headless`,
  a dedicated compute queue) — what needs care is WHERE the compute context is assembled, since the
  path lives in `simul::render_runtime` and demands a render config with graphs; the queue must have
  a CPU implementation; no second scheduler. The one genuinely sharp limit is REDUCTION ORDER: a
  `reduce` result feeds back into the world (GN02 picks sea level from a land-share target), so a
  hardware-ordered GPU reduction breaks "same seed = same world" BETWEEN RUNS ON ONE MACHINE — a
  different and worse thing than CPU≠GPU. Step 0 of the proposed
  order is a MEASUREMENT that can close the task: what share of a generator's wall time is even in
  GPU-able apertures.

- GN02 AFTER-AUDIT PASS: ENGINE BASE FUNCTIONS, STEP-NUMBERED SCRIPTS, BUFFER FIELDS AS OPERATOR ROWS
  (2026-09-02). `71/71` planet checks on five seeds plus the check resolution and a million cells;
  `340/340` and `345/345` project tests; tavl's own suite `109/109`.
  (1) ONE HASH FOR THE WHOLE ENGINE. `bindings::basic_functions` now lands in the generator's step
  environment as the `base` table, and the step bodies moved off their hand-written splitmix onto
  `base.prng64_2` / `base.prng64_normalize`. While every body carried its own copy, "the same seed"
  meant "the same seed inside this file", and the generator hashed differently from the rest of the
  engine. This required splitting the bindings library: `env.cpp` lived in one target with the nuklear
  bindings, so any consumer of `base.prng64` dragged in visage — unacceptable for a headless generator
  — hence `devils_engine::bindings_base` (which also breaks the visage↔bindings link cycle). Two
  functions of that table are off-limits inside a step body: `base.perf` (wall-clock) and
  `base.script_stack`, because a decision that depends on time breaks both reproducibility and
  "parallel == sequential". CONSEQUENCE, stated plainly: a different hash is A DIFFERENT WORLD for the
  same seed. Every reported number moved (land pieces 139→158, sea zones 635→559, largest realm 81→199
  counties) and NOT ONE property broke — the argument for property checks over golden snapshots.
  (2) The "shared lua prelude" gap is WITHDRAWN as mis-named: sharing a helper between step bodies is
  impossible BY DESIGN, because `require` would drag `demiurg` in, so duplication inside a playground is
  the norm; the same decision is why there is no `os`, why `math.random` is nilled, and why `sin`/`cos`
  are expected to move to host functions later — generator scripts get no OS access and no source of
  nondeterminism.
  (3) SCRIPT NAMES CARRY THE STEP: `S01_topology.lua`, `S02_convergent.ds`, `S08_regions.lua` — the
  prefix is the step number, and for a `ds` rule it is the step where the rule FIRST appears
  (`S03_land.ds` is read by both surface and landforms). The pipeline is sequential, so the file name
  answers the first question one asks of the folder.
  (4) A BUFFER FIELD IS AN OPERATOR ROW: `position = v3` instead of the `(position, v3)` tuple. Parsing
  is unchanged, and that is a property rather than a coincidence: a row with an operator IS the same
  PAIR as a row in parens, because reading a pair skips operator tokens between slots — so both
  spellings stay valid and the format migrates file by file. The wanted spelling was `position : v3`,
  but `:` was not in tavl's `operator_chars`, so registering the operator aborted on the validity check.
  THE TAVL CHANGE IS DONE IN THE LOCAL CLONE and left uncommitted for the author's release: one
  character in `detail.h` plus three tests (an unspaced `position:v3` splits; datetime keeps priority
  over a registered `:`, which is the interaction that makes it safe; a list of pairs reads identically
  in all spellings) and a README note. After that release the format flips with one line —
  `p.add_operator(":", binary, 2)` in `parse_buffers`.
  A FIFTH FINDING came from the new world: historical-region names could clash inside a continent (seed
  99). There are five compass directions and up to twenty regions in a continent, so most regions of a
  large continent fall back to their OWN word, and nobody checked that fallback for uniqueness. Fixed by
  construction like the continents (the name seed advances on a clash). Same family as the audit's
  finding, found by the same five-seed check — the new draw simply presented a different pair.

- GN02 PLANET GENERATOR: CLOSED BY A CLOSING AUDIT (2026-09-02). `71/71` planet checks on five seeds,
  at the check resolution and at a million cells; `340/340` and `345/345` project tests. Ten findings,
  four of them real defects, two named as ENGINE GAPS rather than fixed here.
  (1) CLICKING SELECTED A DIFFERENT AREA THAN THE ONE DRAWN: picking took the NEAREST cell while the map
  fills the coverage winner, and near a border those differ — a click at the edge selected the neighbour
  and the highlight appeared beside the click. Same family as "the border is not where the colours meet",
  and the same cure: the map rule became ONE function called by the fill (its GLSL twin), the verify
  check and the picking, which also removed a third copy of the rule that lived inside the check.
  (2) `--smoothing=W` was silently pulled to the nearest button step, because the viewer stored the step
  INDEX instead of the number — and the overlay then reported the pulled value, i.e. lied about what was
  drawn; `atof` also became `stof`, since `atof` returns a silent zero on garbage, which here means "the
  most angular border".
  (3) A COMMENT ASSERTED A FALSEHOOD AND COST THE MOST EXPENSIVE STEP FIVE DENSE LUA PASSES: "there is no
  inverse label → cell mapping in the data" — the mapping is built by the SAME `group_by`, applied to the
  SEED field, where each label owns exactly one cell. Five passes over every cell became five native
  groupings plus loops over LABELS (thousands, not hundreds of thousands): `regions` `1334 → 1204 ms`
  with a bit-identical result, `+4 MB`. The cost of one dense Lua pass was measured in a way that changes
  no result — by DUPLICATING the pass: `35..105 ms` at 262144 cells.
  (4) THE EIGHT-NEIGHBOUR CUT WAS NEVER CHECKED, though graph degree reaches 24 (symmetrised kNN adds
  arcs where the lattice is uneven). A dropped neighbour is a dropped WEIGHT, and if it falls inside the
  kernel it is cut ASYMMETRICALLY about a border — precisely the defect for which the kernel was chosen
  compact rather than Gaussian. The check now measures not "degree is small" but "every dropped arc sits
  beyond the widest allowed kernel"; it holds at both resolutions on five seeds.
  Also: rule classes are now tied to their names and palette (the palette clamps, so a new class would
  silently take the previous class's colour — `static_assert` ties names to palette, a check ties the rule
  to the names); five hand-written copies of the camera block became one included file (the engine already
  had the mechanism, the playground was not using it, and one omission had already cost the whole
  overlay); a dead varying went away; the hidden coupling of `--frames` to frozen rotation is documented;
  continent names are unique by construction.
  DELIBERATELY NOT FIXED: one dense pass of the same family remains in `S02_tectonics.lua` at `35..105 ms` of
  its 1067 ms, because copying twelve lines to win one percent would create a second copy to keep in
  sync, and sharing a helper between step bodies is IMPOSSIBLE BY DESIGN, not by omission — `require`
  would drag `demiurg` along, so inside a playground duplicated code is the norm (per the engine's
  author). The same decision is why the host opens only base/coroutine/math/string/table/utf8 with no
  `os` (which is how the attempt to time from inside Lua failed) and why `math.random` is explicitly
  nilled: generator scripts get no OS access and no source of nondeterminism. NAMED ENGINE GAP: MAP FILL
  BY LATTICE COVERAGE as part of painter — the rule
  irreducibly has two implementations (GLSL on the GPU, C++ for the check and the picking) until it moves
  into the engine, which by project rule waits for a SECOND consumer.
  What the campaign gave the engine: painter's present-mode choice with a desirable and a fallback (vsync
  policy lives where it is asked for, not where modes are enumerated), and originator's
  `connected_components` and `label_adjacency` (a connected piece is a property of the GRAPH, and the CSR
  BY LABELS is what the whole named-place hierarchy is grown from).

- GN02 MAP RENDERING: THE FRAGMENT DECIDES THE AREA (2026-09-02). `69/69` planet checks, `339/339`
  project tests. Three asks about borders, and the third one — "the bands exist but they sit at a
  distance from the borders themselves; the outlined border and the surface under it do not agree on how
  they are drawn" — described the defect literally. THE WATERSHED MOVED: the vertex decides SHAPE
  (displacement has nowhere else to live), the fragment decides everything else — colour, area, coast,
  border, selection. Choosing an area PER VERTEX is impossible in principle, not merely awkward: a label
  is discrete, so any interpolated quantity mixes two different labels, and the workaround of handing
  over a PAIR of colours plus a signed field between them breaks at vertices deep inside an area — they
  have no pair, so their sign is not consistent with their neighbours, and the map grows OUTLINED SLIVERS
  inside provinces. Making the sign globally consistent would require the area adjacency graph to be
  bipartite, and it is not. So the pixel finds its own cell: a graph DESCENT from the cell the vertex
  named (Voronoi cells are convex, so a step to the nearest neighbour cannot stall, and one step suffices
  because a triangle is finer than a cell), then AREA COVERAGE over the cell and its ring with a COMPACT
  kernel. Compact and not Gaussian for a reason: a Gaussian has to be truncated at the ring, and
  truncation is asymmetric about a border — more is cut from the far area than from the near one, so the
  border drifts outward and the lattice reappears. The kernel width is DERIVED, not tuned: a one-cell
  area must win at its own centre against the worst possible ring of eight neighbours at the minimum
  distance, hence `8*(1 - 1/R^2)^2 < 1`, so `R < 1.244`, and 1.2 leaves a quarter of margin. The first
  attempt set 1.5 by eye and was ARITHMETICALLY wrong (a lone cell's share is 0.35, not the 0.54 I
  computed); measured, 1.5 hides 154 land-mass cells and 91 province cells — exactly the one-cell islands
  that physics produced and the generator named. A verify check now mirrors the shader rule and reports
  that number, because the eye cannot see it: an island eaten by the kernel looks like it was never there.
  General rule that fell out: A LABEL NEEDS A BORDER, A NUMBER NEEDS INTERPOLATION. A class is a label
  too, so climate and landform regions are filled FLAT like provinces — while they were blended "softly"
  with a power on the weight, both maps came out as BEADS, because the power gives almost everything to
  the nearest cell. Numbers (height, temperature) interpolate linearly, and the blend-sharpness parameter
  disappeared entirely: nothing between those two cases was ever needed. The border LINE now lies on the
  ZERO OF THE SAME MARGIN that picks the fill, so it cannot land anywhere else; there were four wrong
  attempts before, and the instructive one is the blurred frontier — an unsigned quantity has no side, so
  its 0.5 level yields TWO lines, each one cell away from the real border. Whether a line is drawn is a
  SEPARATE flag from whether the field has areas (the answer to "some views need no border at all"): a
  hash colour is a name and needs the line, a semantic palette names the class itself and a black stroke
  between desert and steppe would assert a boundary nature does not have. Line thickness must be measured
  in PIXELS (distance to the zero over the field's screen slope), and the slope as a SUM OF ABSOLUTE
  derivatives rather than a vector length — on a quad edge one derivative vanishes and the line breaks
  into dashes. One SILENT bug cost three screenshots: the neighbour count is stored as an INTEGER in the
  record's fourth word and must be read with `floatBitsToUint`, not a type cast — integer 6 in float bits
  is a denormal, i.e. zero; the neighbourhood came out empty, the descent never stepped, and the picture
  silently fell back to the Voronoi lattice. Neither the compiler nor the validation layer says anything;
  the only symptom is that an edit which must change everything changes nothing, which is why the
  diagnostic was to triple the kernel width and see that the picture still did not move. Cost: `254 → 207`
  FPS uncapped at 262144 cells (a pixel reads up to nine cells instead of four per vertex); with vsync it
  is still exactly 60. Direction recorded, not a task: transitions between climate zones will also be
  built by MORE CLASSES — where Earth puts semi-desert and savanna between desert and forest we have a
  single border, and rendering neither can nor should fix that.
  A FOLLOW-UP PASS fixed "holey borders" — at some distance the line broke into dashes — and both causes
  were about SAMPLING, not about the field. (a) The field's slope must be computed ANALYTICALLY, never by
  a screen derivative: the candidate neighbourhood changes as the pixel crosses a Voronoi edge, so
  coverage takes a tiny step — the border itself does not move, but the derivative spikes, and the line
  width and its fade were both computed from it. A compact kernel has an analytic derivative
  (`-4u(1-u^2)/R`), it is continuous, and the coverage gradient is a sum of it over an area's cells (with
  a quotient-rule term for the normalisation, and the radial part projected out because the pixel travels
  ON THE SPHERE). (b) The FILL EDGE must be antialiased by PIXEL AREA COVERAGE: a one-pixel staircase
  under a thin line is what made the line holey. The decisive measurement was rendering the same frame at
  double resolution and downsampling — the line came out clean, so the defect lived in sampling. Blending
  by pixel coverage is NOT the forbidden mixing of labels: what mixes is not the area values but their
  shares of the pixel's area, exactly as in a glyph. General rule: THE FILL, ITS EDGE AND ITS BORDER MUST
  BE COMPUTED FROM ONE FIELD. The smoothing scale is now a knob (`B` in the window, `--smoothing=`)
  because it is taste; the range ends are derived, and the verify check measures the WIDEST allowed width
  so it covers the whole knob. Cost `207 → 150` FPS, run-to-run spread `136..171` on an integrated GPU, so
  read it as "about a third of a frame at 60 Hz" rather than as a precise number. A second SILENT bug of
  the same family as the denormal read: key-latch slots are hand-numbered per call site, the new key's
  slot ran past the array, and the out-of-bounds write changed the VIEW MODE — the window showed relief
  instead of what `--mode` asked for, with nothing in any output about it. The symptom of this family is
  always "an edit that must change everything changes nothing, or changes the wrong thing". A bounds check
  now lives inside the latch. Also found by the five-seed verify and fixed BY CONSTRUCTION: two continents
  could draw the same synthesised name (seed 1); on a clash the name seed now advances by the same mixing
  used inside synthesis, which keeps "the name is fully determined by the data" because the traversal
  order is the same for every consumer of the package.

- GN02 ISLAND CLUSTERS, TACTILITY AND SMOOTHING: THE TASK QUEUE IS CLOSED (2026-09-02). `67/67` planet
  checks at the time, `321/321` project tests. (1) A THIRD ISLAND MECHANISM, because neither of the first two can
  produce a CLUSTER: a hotspot track and a volcanic front are both LINES. The Aegean is a drowned
  highland on continental crust — the back-arc crust is stretched, the region subsided, and the peaks of
  the old fold belt stand out of the water. So the mechanism is not "raise islands" but LOWER A MOUNTAIN
  COUNTRY, and it took four measured corrections. Conditions must be thresholded ONE BY ONE: a product
  of four quantities near one half is one quarter, and after a single saturating gate that reads as
  "fully extended" everywhere any one of them was nonzero — continents turned into a sieve. The shape
  must be a band the size of a real archipelago, not a broad decay: distance to a convergent junction is
  small almost everywhere, because there are many junctions. The orogen must be SUPPRESSED by the basin
  mask, or the tectonic uplift raises the lowered crust straight back (459 enclosed water bodies against
  13 islands). And the elevation inside the basin must be REPLACED with a shelf elevation, not reduced by
  a fixed amount — a fixed subsidence does not know where the crust stood, and the interior of a
  continent stands a kilometre higher than its edge. The quantity to measure was not the island count but
  the LAND FRACTION INSIDE THE BASIN: at 45% it percolates into one mass and grows peninsulas onto the
  mainland, at 15% there are too few islands; 23% gives 49 pieces with 25 in clusters. (2) SMOOTHING was reworked in a second
  pass (see the entry above) and the account here is kept only for the lessons that survived: the coast
  is decided per FRAGMENT at the 0.5 level of a smooth LAND FRACTION, and that fraction must be its own
  field rather than the sign of the height, because height in the record is CLAMPED at zero (water is a
  smooth sphere; depth is shown by colour, not by shape), so "height above zero" on a blend meant "there
  is land nearby", not "this is land". Everything the first pass did with per-vertex colour, blend
  sharpness and a blurred border field was later found wrong in principle. (3) TACTILITY is surface labels plus ray picking. Labels are
  DECALS: every corner of every glyph sits ON THE SPHERE (the corner direction is the anchor shifted
  along a tangent basis, then normalised), so the text bends with the planet, foreshortens toward the
  horizon and is occluded by a mountain. The first attempt drew a BILLBOARD sized in screen fractions —
  always readable, but hanging ABOVE the planet rather than lying on it, and the difference shows the
  moment the globe turns. Size is therefore in RADIANS: a decal has a place on the surface, so it has a
  size on the surface, and readability comes from three thresholds (screen size, which doubles as LOD;
  the horizon; and an occupancy RECTANGLE — a point is not enough, because a long name has a different
  width). The lift above the surface is measured from the HIGHEST POSSIBLE mountain: at 1.5 km the text
  sank into slopes and read as clipped glyphs rather than as occlusion. The property that matters is that
  WHAT IS SELECTED AND LABELLED IS DECIDED BY THE CHOSEN VIEW — the level is declared next to the view
  itself, not derived by a second formula in the label code. (4) Two loud
  bugs found: a camera block repeated in a shader WITHOUT one field silently shifts every later field to
  a wrong offset (that is how the whole overlay vanished — no error, no warning), and sea-zone capacity
  depended only on the requested zone count, so at a million cells the label reached 1302 against 1232
  declared buckets — the number of enclosed water bodies grows as the lattice gets finer.

- GN02 NAMED-PLACE HIERARCHY, CK-STYLE TITLES, ISLAND ARCS, VSYNC AND STARS (2026-09-02). Five of the
  seven queued tasks; `66/66` planet checks, `320/320` project tests. (1) A LEVEL GROWN OVER AREAS MUST
  BE GROWN OVER THE AREA GRAPH, not over cells. The task's hard rule was "every border coincides with a
  province border", and growing a historical region by flooding cells would cut provinces in half. Two
  new engine tools make it structural instead of hoped-for: `connected_components` (a connected piece
  is a property of the GRAPH, not a value of a field — the label "land" is on every land cell, but the
  fact that Eurasia and America are two pieces is not in it) and `label_adjacency` (a CSR over LABELS
  built from the CSR over elements). A cell then learns its level by `lookup` on its province number,
  so the borders coincide by construction. (2) A LEVEL MUST BE GROWN INSIDE EACH PARENT SEPARATELY.
  Grown over the whole set at once, the flood cannot cross water, so the count stops being a function
  of the level's size and becomes a function of how many pieces of land the planet has: three ocean
  seeds covered 57 sea zones of 94 and the leftovers produced 35 "oceans", each one enclosed lake.
  (3) THE SMALL PIECES MUST BE ATTACHED, NOT PROMOTED. Japan is an island but its continent is Asia;
  without a threshold a hundred islands give a hundred "continents". Empires needed the same fix — 77
  empires on 862 counties against a requested hundred counties per empire, and the whole difference was
  islands declaring themselves empires. Symmetrically, a lake is not an ocean. (4) DE JURE POLITICS IS
  THE SAME PARTITION AT OTHER SCALES, and the de jure KINGDOM *is* the historical region — that is what
  "the political hierarchy follows from the geographic one" actually means. De facto realms are a
  different PROCESS, not a different scale: a title is a right and divides area evenly, a realm is
  force, so it grows by `graph_vote` weighted by population; 83% of a realm's counties come out of its
  own culture although the vote never looks at culture. (5) A NAME IS A SEED, NOT A STRING: the buffers
  are numeric and the name is fully determined by the seed, so the record stores `name_seed` plus the
  naming culture and synthesis is deterministic. (6) FOUND A REAL CONVENTION BUG IN ALL FOUR RECORD
  BUFFERS: `group_by`/`accumulate` bucket by the RAW key (bucket 0 = "no label"), while the scripts
  wrote centre and size at "label minus one", so one record mixed TWO DIFFERENT areas —
  `provinces[0].height_sum = -3.9e7`, the summed depth of all water, next to `cells = 19` from the
  first province. One effect was live: merging small provinces read a NEIGHBOUR's frontier flag. Only
  addition catches it, and that check is now in `--verify`. (7) ISLAND ARCS: the mask that chops
  volcanoes decided nothing while the back-arc platform stayed continuous and lifted the whole arc above
  water. Both halves of the fix are physical — the arc front WANDERS relative to the trench (the slab
  dip varies along the junction, which is also the answer to "a strip of REGULAR shape": distance to the
  junction is a graph flood and therefore smooth), and the arc is cut by TRANSVERSE straits. Pieces
  `18 → 87`, median `254 → 28` cells, elongation `4.9 → 2.3`. (8) TWO FRAME LIMITERS, and measurement
  said so: with the producer-loop pacer removed the rate stayed 63 FPS because the present mode held it.
  `choose_swapchain_present_mode` now takes a desired and a fallback mode, defaulting to `Fifo`.
  (9) Stars on the skybox re-taught the PF07 lesson — count the RIGHT quantity: the star count came out
  right (315 on half a screen against 950 predicted for the whole) and the sky still did not read,
  because the median star was at 20 of 255. It also exposed a real geometry error: a cube lattice in
  VOLUME is not bijective with directions, so a star drifts into a neighbouring cell and nobody draws
  it — a cube UNWRAP is needed. (10) The new step was quadratic (8.7 s of 33, the most expensive step
  with the least work): the per-parent loop walked the whole area-buffer CAPACITY each time. Bucketing
  nodes by parent once: `8.7 → 3.5 s` Debug, `915 ms` Release, same result.

- GN02 PHYSICAL ISLANDS, LANDFORM REGIONS, MORE LAND (2026-09-02). Three asks — islands from physics
  rather than from a count, physical REGIONS that a later step amplifies, and less water area without
  losing the water — and each landed a transferable rule. (1) ISLANDS ARE NOW A MECHANISM: a mantle
  plume stands still while the plate rides over it, so the chain, its direction, its age gradient and
  its length all fall out of the Euler-pole geometry instead of being parameters (new `hotspot_tracks`
  tool; new engine tool `graph_slope`). The second mechanism is the island arc, and it only works as
  TWO parts — a wide low back-arc platform plus narrow volcanoes on it: without the platform a volcano
  has to climb 6 km from the abyss and comes out either a 4 km mountain or nothing (measured 19 land
  masses instead of 36). Corollary: physically-born islands have a RESOLUTION FLOOR — a 200 km edifice
  is 1.5 cells on a 16k lattice, so the contract test moved to 49152 cells and the default to 262144.
  (2) REGIONS ARE CLASSIFIED FROM MEASURED PHYSICS AND THEN AMPLIFIED: the per-kind gain is applied to
  the deviation from the smoothed neighbourhood, which makes plains flatter and mountains sharper —
  27x between coastal plain and mountains in the measured per-kind slope. Two rules there: the
  descriptor must be the true local SLOPE, not deviation-from-blur (a tilted plane barely deviates
  from its own blur, so a flat plateau and a smooth slope were indistinguishable — 122 against 121);
  and the gain field must be BLURRED before use, because a hard class boundary with different gains
  makes a step where the relief has none (a "flattened" abyssal plain came out rougher than with no
  amplification at all). (3) LESS WATER AREA, SAME WATER: land 40% instead of 29% with the ocean
  deepened to keep the volume at 1.0 Earth, and the report prints the volume so it stays a
  measurement. The cost is visible and irreducible: with the water forced deeper, 14% of the surface
  sits below 6 km against Earth's 2%. (4) Also fixed: the two-pass shelf construction oscillated
  (raising the shelf break drops a shallow band to the ocean floor, which removes land, which lowers
  the sea level) — both conditions collapse into ONE bisection, "cells above shelf_break + shelf_drop
  equals the land target".
- GN02 RELIEF REWORK: HYPSOMETRY, BELTS, FRACTAL DETAIL (2026-09-01). A pass for "more realistic and
  more pronounced relief" that turned into six real modelling errors, all of the same kind — a quantity
  computed in the wrong place, invisible because the result still looked plausible. (1) BOUNDARY SPEED
  WAS LOCAL: `plate_interaction` writes convergence only on cells whose neighbour is in another plate,
  i.e. a ONE-CELL ribbon, so `uplift * drive * decay(distance)` was nonzero only where `decay` is
  already 1 — every belt width in the config (orogen, trench, ridge, rift) did nothing, mountains were
  a one-cell ribbon smeared by smoothing, and uplift had been inflated to compensate. Fixed by flooding
  the boundary CELL ID instead of a 0/1 flag and looking up its convergence and subduction side, which
  is what the new engine tool `index` exists for. (2) ALL NOISE FREQUENCIES WERE ~100x TOO LOW: the
  encoded FastNoise2 tree carries its own frequency (~0.01 cycle per radian per frequency unit), so
  "fine relief detail" at frequency 11 had a pattern several radians wide — a smooth planetary gradient.
  Neither detail amplitude nor frequency moved the measured land slope until frequency grew fiftyfold.
  Noise sizes are now declared in RADIANS with the tree's scale declared next to the tree. (3) TWO
  MECHANISMS FOUGHT OVER THE CONTINENTAL MARGIN: seafloor depth was subtracted in the divergent rule via
  `(1 - crust)`, so the drop to the abyss was made by the CRUST WIDTH (3 km over 0.12 rad) and was
  steeper than the constructed slope; the shelf collapsed to 1.7% of the surface against Earth's 5%.
  Depth now belongs to the single height curve. (4) ISLAND CONES MUST OVERLAY, NOT ADD: summing a
  descending continental cone with an island cone sank mid-ocean islands the further they were from a
  continent (playable landmasses 46 instead of 130) — hence the new `maximum`/`minimum` tools. (5)
  FRACTAL NORMALISATION BY THE SUM OF AMPLITUDES makes the fractal WEAKER than one octave, because
  octave peaks never align; normalise by power (root of the sum of squares) instead. (6) AGE-DEPTH WAS
  LINEAR IN FLOOD STEPS: resolution-dependent (a finer lattice drowned the ocean twice as deep) and it
  made ridge flanks steeper than land. A saturating curve is both the plate-cooling model and the fix.
  Transferable rule: measure the SHAPE of a field (hypsometry, per-class neighbour slope), not only its
  range — a plausible min/max hid every one of these. Use the MEDIAN for "typical roughness": one
  trench arc carries 3 km and 1.5% of such arcs double the mean.
- GN02 PLAYABILITY PASS, SETTINGS AND CAMERA LIMIT (2026-08-31). Three deliberate departures from
  "whatever physics gives" toward "what a game needs", each measured rather than tasted. (1) CONTINENTS
  ARE SEEDED, NOT DRAWN FROM PLATES: declaring a whole plate continental produced either one
  supercontinent or all land in one hemisphere — real, but half the map is then empty water. Continent
  centres are now spread by poisson selection and mass falls off from them, so continentality is a CELL
  property, which is also truer: a real plate carries both continent and ocean floor. The falloff needs a
  PLATEAU inside plus a shelf-wide ramp — a linear falloff from the centre left land only at the very
  middle, the sea-level search then dropped the water and exposed mid-ocean ridges, and land came out as
  RIBBONS along plate seams. (2) MANY MEDIUM ISLANDS at `0.045` rad with their OWN score field, because a
  shared score made the greedy seeding put island centres exactly where continent centres already were.
  (3) COASTAL EROSION removes cells with almost no land neighbours — physically wave erosion, in practice
  the specks in which no province fits: at 16384 cells the world went from `183` land masses with `113`
  specks to `53` masses with `1`; at 65536 it is `108` masses, `0` specks, `81` able to hold a province.
  The pass runs INSIDE the sea-level bisection so the declared land share still holds exactly. PROVINCE
  SIZE IS NOW BOUNDED BOTH WAYS — from above by flood capacity (a full label stops growing and the patch
  wave splits it), from below by merging (the seed of a small province is removed and the flood hands its
  cells to neighbours), both expressed as shares of the average so they follow neither resolution nor the
  requested province count: `3..45` cells within bounds `8..62`, none over, `32` under (all islands).
  Two more real defects surfaced: the patch wave must START at the normal area spacing and halve per pass
  (going straight to neighbour spacing re-seeded a merged region every 1.5 cells and produced `54`
  one-cell provinces), and area buffers must be CLEARED before the final summary or a label that vanished
  in merging leaves its old numbers in the record — a nonexistent area shipped in the package, and no
  connectivity check can catch it because the buffer is valid. VIEWER: camera is clamped by the ROTATION
  AXIS component to `±0.94` (~70 deg) because that is what degenerates in `lookAt`, all program text is
  now Latin (the playground font atlas has no Cyrillic), `G` regenerates with a fresh hashed seed, and
  `Tab`/`-`/`=`/`Enter` drive simple generator settings whose bounds and step are declared next to the
  value itself in `values.tavl` (`ranges = { name = [min, max, step] }`, parsed by the new engine call
  `originator::parse_value_ranges`); `--set name=value` gives the same override headlessly. `--verify` is
  `45/45` on four seeds, `312/312` tests pass, `713 ms` for 65536 cells and `3.49 s` for 262144.
- GN02 PLANET GENERATOR: FIRST VERTICAL SLICE CLOSED (2026-08-31). New playground
  `GN02_planet_generator` computes a whole planet downward by CAUSE in seven config-declared steps:
  topology, tectonics, surface, climate, seasons, peoples, regions. Surface address is a DIRECTION, not
  a coordinate pair: cells lie on a Fibonacci lattice and adjacency is a symmetrised CSR, so there is no
  seam and per-cell sums need no area weights. Sea level is not a number but a bisection under a declared
  land fraction (`29.0%` hit exactly at both resolutions). Five engine additions are generic and now live
  in `libs/originator`: `add_graph_tools` (`sphere_points`, `sphere_adjacency`, `graph_blur`,
  `graph_frontier`, `graph_flood`, `graph_vote`, `poisson_seeds`, `lookup`) plus field arithmetic
  `blend`/`modulate`/`decay`. `graph_flood` takes PASSABILITY separately from cost, because an expensive
  price is not a ban: a province flooded across a strait stays two pieces after water is masked out, and
  that shows up only when a border is drawn. Six findings were each a mistake before measurement:
  (1) precipitation must be normalised by the LAND mean — normalising by the planet made 28% of the
  surface desert, because it always rains over ocean; (2) convective rain must be non-linear in
  temperature (exponent 2.5), otherwise the equator comes out DRIER than mid-latitudes and the planet has
  no tropics; (3) the equatorial bulge at 800 m stopped being a correction and became the cause — three
  seeds in a row put 78-88% land in polar bands against 15-20% in the tropics, because continental crust
  floats only 420 m above oceanic, so it was reduced to 150 m; (4) relief smoothing with self-weight 2.4
  erased mountains entirely (a peak fell to a quarter per pass over ~7 neighbours; global maximum was
  1132 m); (5) `poisson_seeds` blanks candidates BY STRAIGHT LINE while flood passability follows the
  GRAPH, so a landlocked sea can lose its seed to one across an isthmus — fixed by a neighbour-step patch
  wave looped to zero uncovered cells; (6) widths must be declared in RADIANS and divided by the lattice
  step, or changing resolution silently changes the world. THREE devils_script properties reshaped the
  rules: a function call is not an expression operand; the context has EIGHT argument slots (so at most
  seven `ctx:arg`); and A LINE BREAK ENDS AN EXPRESSION — `ds` separates elements by `,` or `\n`, so a
  formula split across lines is legitimately two formulas. The last is a LANGUAGE RULE, not a defect
  (confirmed by the author), but easy to step on: the same population-growth formula gave `0..0.49` on
  one line and `0.00013..1` split before a `*`, with no error because there is no error. Config rule
  that follows: one expression per line, break only after a comma, and check a rule by the RANGE of its
  output (`--stats`), because the text looks the same either way. Release on 11 threads: `479 ms` for
  65536 cells, `1929 ms` for 262144, package `19.0`/`75.7 MB`; parallelism only `1.8x` because floods,
  seed selection and bisection are sequential by nature. `--verify` is `41/41` on four seeds and
  `312/312` tests pass. NEXT: rivers/erosion, reading the package in PF10, states and trade.
- GN02 VIEWER AND PER-STEP INSPECTION (2026-08-31). `--view` opens a PF10-style planet window from the
  same binary: painter render graph in `tavl`, orbit camera, twelve fields on the digits (`climate`,
  `relief`, `plates`, `tectonics`, `temperature`, `seasonality`, `precipitation`, `habitability`,
  `population`, `cultures`, `provinces`, `sea zones`), and `[`/`]` stepping through the SEVEN GENERATION
  STEPS — which re-runs the generator up to step N rather than switching a display, so later fields stay
  zero and the overlay says so. The mesh deliberately does NOT match the generator: cells are a Fibonacci
  lattice with no polygons, so an icosphere is drawn and each vertex takes the value of its NEAREST cell —
  the same question a package consumer has to ask. Subdivision is auto-picked at about four triangles per
  cell; `628 FPS` at 65536 cells and `615` at 262144 (both `327 680` triangles) on Iris Xe at 1280x720,
  and a step regeneration costs `505 ms`. Four presentation rules came out of the first frames: displace
  only land and only above sea level (absolute height made the shelf a sawtooth of 4 km steps), flat-shade
  categorical fields and interpolate continuous ones (interpolating two random label colours invents a
  third that belongs to no region), colour land by the SQUARE ROOT of height (almost all land is under a
  kilometre and went uniformly green), and overlay text must be ASCII because the playground font atlas
  has no Cyrillic. THE WINDOW ALSO FOUND TWO REAL DEFECTS in minutes. In the world: culture spread was
  counted in TICKS, i.e. graph hops, so cultures reached half as far on the 262144 lattice — the same
  mistake as relief widths and moisture travel, now fixed by declaring `culture_reach` in radians (mean
  culture label `3.47` vs `3.59` across resolutions instead of a 2x gap). In the drawing itself: unlabelled
  LAND and water were painted the same dark colour, so the culture map read as a speck on an empty planet
  while the measurement said `87%` of habitable land was claimed — the picture lied, not the model. The
  report now prints the habitable-land share, and `--verify` grew four mesh checks including sphere
  closure: the sum of spherical triangle areas equals `4π` within `5e-05`, a tolerance chosen between the
  measured float error `5.2e-06` and one missing triangle out of 5120 (`1.95e-04`).
- ORIGINATOR LUA ENVIRONMENT AND STEP BUDGET (2026-08-31). The generator's lua state was already its
  own (`sol::state` + environment, no `math.random`, no io/os, separate build target from `visage`), but
  two real defects sat under it. First, the whitelist DID NOT WORK: `std::string_view` keys silently
  missed in sol2 proxying, so every base function and standard library landed in the environment as
  `nil` — GN01 bodies never noticed because they only touch `step`, `originator` and arithmetic, and the
  single `error()` in `terrain.lua` sat on a path that never fired, so the error report itself would have
  crashed. Keys are now `const char*`, and a missing MANDATORY global (`assert`, `error`, `pcall`,
  `type`, `tostring`, `tonumber`, `pairs`, `ipairs`) or an empty standard library is a loud failure at
  host construction. Second, the default budget was unlimited, so a looping step body hung forever.
  Default is now 200M lua INSTRUCTIONS with wall time off: instructions do not count time spent inside a
  native tool, so they catch exactly what must be caught, while a clock cannot tell a loop from an hour
  of honest world noise. Hook interval is derived from the limit (a 500-instruction limit with a
  10000-instruction hook would never fire), and budget exhaustion sets a flag that fails the step EVEN IF
  `pcall` inside the body swallowed the error — otherwise `pcall(function() while true do end end)` would
  hang while the pipeline counted the step as done. The perf bench opts out out loud. GN01 `--verify`
  grew four checks for this and is `22/22`.
- PF10 FORMALLY CLOSED WITH A STRUCTURAL CLOSING AUDIT (2026-08-31). The audit deliberately has no frozen
  image gallery: unlike the weather labs there is no meaningful on/off reference contract to protect. It
  strengthened `--verify` from `39` to `47` checks instead: every one of the 1.58M atlas-512 texels now
  round-trips through R16 and agrees with canonical kind/state/CSR metadata; playable CSR is explicitly one
  component; state ribbons and hydrology are bit-identical across repeated builds; every rendered ribbon half
  samples its recorded physical state; complete river centrelines and 16-sample lake rims remain playable;
  and 256 planet-wide radial rays return the front displaced owner. This found TWO real data bugs. Two of
  `1719` atlas-512 state segments sat at ambiguous three-way junctions and used guessed side IDs; they are now
  omitted instead of lying, with the thin province line retained as coverage fallback (`1717` honest segments,
  `3944` at runtime atlas-1024). Two lake rims crossed non-playable terrain despite land centres; lake radius
  now shrinks against canonical rim samples, leaving `959` feature primitives. Debug/Release are `47/47`;
  min/default mesh, near/far, both layer A/Bs and all three border-debug modes launch. Near/far/exact-debug
  Vulkan validation is VUID/API clean. Iris Xe 1280x720 Release is `4.324 ms / 231.3 FPS` at `1.16R` and
  `2.998 / 333.5` at far LOD. PF10 is closed; water port graphs, persistence, deeper hierarchical LOD and
  heraldic billboards require future consumer-owned scope.
- PF10 GIANT STATE LABELS + NORTH-UP PROVINCE LABELS CLOSED (2026-08-31). Far LOD state names now use the
  same PF05-derived MSDF surface decals at genuinely cartographic scale: up to `.110 rad` high, with a small
  `.035` longitudinal margin over almost the full presentation-side state extent. A 512-sample canonical pass
  trims the PCA Bézier to its longest continuous centreline inside the connected state. The broad glyph itself
  is deliberately not pixel-clipped at every coast/frontier because that split letters into unreadable pieces;
  ownership constrains the path, while province labels remain strictly owner-clipped. Province Béziers are
  reversed as whole curves when necessary so `cross(surface normal, text tangent)` stays in the local-north
  half-plane; glyph order remains intact, including near-vertical names. The new verification checks this at
  nine points along every curve. Release is `4.337 ms / 230.6 FPS` at the `1.16R` camera floor and
  `2.993 ms / 334 FPS` at far LOD, including about `.11 ms` for 31 giant glyph volumes. `--verify` is `39/39`;
  Debug/Release build and both near/far Vulkan validation are clean.
- GN01 GENERATOR CONTRACT CLOSED WITH A CLOSING AUDIT (2026-08-31). New `libs/originator` in four build
  targets: core (buffers/tools/pipeline, no lua and no devils_script), `originator_script`,
  `originator_lua` and `originator_primitives` (FastNoise2 + jc_voronoi). The pipeline lives in `tavl`
  and lua is the step BODY, occupying the place a graphics `command` holds; a graphics pipeline can fix
  its command set because the conveyor is fixed, and a generator has no conveyor. Parallelism is never
  declared by a script: it is derived from the tool's aperture
  (`pointwise`/`gather`/`sequential`/`scatter`/`reduce`) plus the actual bindings, and a violating call
  does not assemble — a read binding has no write operation, and a gather whose source equals its
  destination is refused before execution. Three execution tiers measured on ONE rule over `4194304`
  elements: native kernel `3.44` ns/element (`1.10` on 11 threads), `devils_script` `90.05` (`17.15`),
  per-element lua `120.13` with no parallel path at all. The surprise: ds beats lua by only 2x in a
  single thread — its real advantages are that it parallelises and that it type-checks at parse time.
  Both scatter tools (`group_by` CSR, `accumulate`) are bit-identical at 1/4/9 threads through fixed
  chunking, and parallelise 4.4x/4.5x. `voronoi_polygons` returns a shared-vertex planar mesh whose
  polygon areas sum EXACTLY to the map area. Four rules that were each a mistake before measurement:
  regular noise must generate whole rows (`GenUniformGrid2D` accumulates positions inside a row, so a
  run starting mid-row drifts `3e-07`), `FASTNOISE2_STRICT_FP` instead of picking a SIMD feature set at
  runtime (`max` vs `SSE2` drifted `1.8e-07`, now exactly `0`), TWO seeds instead of one under chunking
  (`step.seed` chunk-independent for fields continuous across a seam, `step.chunk_seed` for what must be
  independent per chunk — one seed made noise drift `1.99` at the seams), and self-calibration by a
  measured range is incompatible with chunking. Chunking measured per aperture on 2x2/4x4/8x8:
  pointwise exact `0` over 262144 cells, gather differs ONLY inside a `radius`-wide border band and
  exactly `0` inside, global scatter refused by declared key support. The closing audit raised eight
  items including one real bug: the field fast path compared only byte size, so `span<float>` over a
  field declared as raw `uint32` passed and silently wrote float bits (`1061158912` read back instead of
  `0.75`); fixed by requiring an exact storage match, with a test that failed before the fix. Cross-chunk
  scatter merging is deliberately absent: a float accumulator changes `816/1024` group sums with chunk
  arrival order while fixed point changes none. `311/311` tests; package sealing stays a project
  decision, not an engine format.
- PF10 CANONICAL PROVINCE ID COLLISIONS FIXED (2026-08-31). The apparent missing contours and zero-sized
  names on `0x2EFD4C17`/`0x1A154983` had one structural cause: `(hash_cell & 0x3fffffff)|1` was treated as
  identity, and `524` active IDs each owned 2–4 distinct 3D Voronoi cell keys. The two reported IDs were four
  remote cells; area moments joined their locations and collapsed both Beziers, while state assignment made
  remote pieces false enclaves. The fragment then suppressed the thin province contour on a state frontier,
  and the presentation filter removed its short closed state trail, leaving no line. Canonical land identity is
  now an affine permutation of the already unique 18-bit `cell_key` modulo `2^30`; its odd multiplier is a
  bijection and hashing is only presentation entropy. The working atlas has `4602` playable nodes, `12945`
  undirected CSR edges and `4702` compact exact records. The four former components now own independent IDs
  `2B17612A/0B6E5E30/2890AAB3/18FD74A7`, with atlas-1024 label spans `.0324/.0090/.0415/.0440 rad` rather than
  zero. Every ordinary province line is also retained beneath the later state ribbon as a coverage fallback;
  discarding a short state symbol can no longer erase the boundary. A clipped-cell label fallback grows and
  shrinks a symmetric tangent corridor from its verified interior anchor. Atlas-512 verification explicitly
  proves ID↔cell-key bijection and no collapse among labelable provinces; it records `288` genuine sub-eight-
  texel fixture slivers which cannot physically contain a five-character decal and must be merged/min-area
  filtered by future production generation. Unique ownership removes false state topology: atlas-1024 now
  materializes `3946` state segments in four trails instead of about `17.4k`. Pixel-stable seam-aware province
  AA, connected three-state CSR ownership, two-sided patterned ribbons, PF05 decals, world-Y orbit, hydrology
  and 4x focus remain intact. Iris Xe 1280x720 Release: near `1.16R` frame 120 `4.642 ms / 215 FPS`, far frame
  80 `2.993 / 334`; `--verify` is `38/38`, Debug/Release build and close Vulkan validation are clean. NEXT:
  water port graph/content naming and hierarchical LOD; heraldic billboards remain later and anomalous weather
  stays parked.
- PF08 FORMALLY CLOSED — SLICES 0–7 PLUS CLOSING AUDIT (2026-08-30). Eight permanent 1280x720 frame-80
  PNGs now cover clear noon/sunset/night, overcast, rain, snow, universal magic lightning and aurora. The
  dedicated `verify_audit_frames.sh` launches ONLY PF08, writes temporary PPMs and matched all eight frozen
  files byte-for-pixel (`AE=0`); it never launches PF07 or rewrites references. Five Vulkan-validation runs
  covered clear, rain, snow, downpour+distant-lightning and aurora+overcast with zero VUID/API warnings/errors.
  Release build and `117/117` pass. Iris Xe 1280x720 steady total minima across two deterministic runs:
  clear noon `5.814`, double sunset `6.421`, clear night `4.256`, overcast `7.229`, rain `9.396`, snow
  `10.038`, magic lightning `7.547`, aurora `6.260 ms`; the worst authored frame retains `6.63 ms` of a
  60-Hz budget but makes no 120-Hz claim. Transition tests lock 1/4 progress after one second of a four-second
  blend, interruption continuity and the two-frame atmosphere-cache rebuild cadence; the prior dynamic resize
  test remains the fullscreen gate for fixed rain-memory. WEATHER HORIZON: ice-crystal halos/sundogs/pillars
  and refractive mirages are genuinely new mechanisms and belong in another optical lab; glory/fogbow reuse
  rainbow+fog, virga reuses precipitation volume, noctilucent clouds reuse the aurora shell with scattering,
  sprites reuse lightning+aurora, and unusual cloud shapes are density authoring. PF08 is closed because the
  mechanism families are covered, not because every named terrestrial phenomenon received a preset.
- PF08 SLICE 7 PLANET-ANCHORED AURORA CLOSED (2026-08-30). Aurora is upper-atmosphere emission, not cloud
  fog and not a post effect. It occupies its own `90–240 km` spherical shell instead of stretching the closed
  `100 km` scattering-atmosphere LUT through near-vacuum. Eight ray samples build emission around a magnetic
  oval at colatitude `18±4°`; the planet-anchored pole tilts `16°` toward azimuth `330°`. A longitude field is
  constant down radial columns, producing vertical curtains: 72 incommensurate green/violet folds drift at
  `.12°/s` in real time. Atmosphere transmittance is applied before the existing weather volume, so terrain and
  fixture depth occlude normally and `overcast` hides the aurora without a special branch. Artistic controls
  expose intensity/saturation/density/daylight visibility, shell, oval, pole, folds and speed. Weak I=1 under
  normal scotopic vision is nearly grey by design; self-contained `aurora` preset uses I=4, scotopic=0 and a
  side-on magnetic-oval view for a readable green/violet corona, while CLI overrides retain both modes. Speed
  zero paused frame 8/80 is byte-identical; authored motion MAE is `.000157413`. Authored versus intensity-zero
  MAE is `.0910081`. Replacing per-sample acos with the local cosine coordinate and 12 samples with 8 reduced
  cost to sky minima `1.710/.807 ms`, total `6.395/5.362` at 1280x720. Vulkan validation is clean and verify is
  `117/117`. Slices 0–7 are closed; closing audit is next.
- PF08 SLICE 6 UNIVERSAL LIGHTNING CLOSED (2026-08-29). Lightning is not a weather preset: new pure
  `lightning_event` stores a world-metre channel, colour, separate channel luminance and luminous intensity,
  cloud-glow radius, start/duration, deterministic return-stroke count/seed and author
  (`weather|magic|scripted`). The author does not change evaluation, so magic and storms share mechanics.
  `sample_lightning` emits a short channel envelope and longer flash envelope; segment-distance inverse-square
  illuminance remains active even when channel geometry is culled. The geometric gate is numeric: a 4.5 cm
  channel at ~900 m is only ~0.06 px at 720p/65 degrees and should illuminate its cloud without an unstable
  ribbon, while a near channel is resolvable. Thunder is a separate `distance/343 m/s` consumer. Twelve headless
  checks cover lifetime, profiles, envelopes, segment distance, falloff, near/far gate, shared magic evaluation
  and sound delay. Four explicit GPU vectors feed shared `pf08_lightning.glsl`: nearest-segment inverse-square
  light reaches surfaces and every froxel medium, while glow radius bounds the source without thickening the
  channel. Fixed downpour phase `.03`, strength 1 vs 0 is normalized MAE `.0120665`; the cold cloud column and
  ground response remain visible with no line. `distant|close|magic` share one event; L retriggers, `storm`
  schedules deterministic distant events and composes with `--weather=downpour`; fixed phase makes A/B stable.
  A shared fullscreen HDR consumer draws a 12-segment world-space path plus three branches only when projected
  diameter reaches `.75 px`; distant bolts exit before the loop and retain only their flash. Close/magic paths
  test opaque depth and sample froxel transmittance to channel distance. A conservative screen rectangle cuts
  the frozen active magic channel's fog-apply minimum to `.725 ms`; total without foliage is `5.445 ms` on Iris
  Xe. Distant flash and close/magic channel are Vulkan-validation clean; `--verify` is `113/113`. Honest limit:
  local lightning has no separate cube shadow map, though the visible channel is camera-depth occluded. Strength
  zero scales both flash and line and is byte-identical to lightning off, preserving the clear early-out. Aurora
  follows as slice 7.
- PF08 SLICE 5 SURFACE MODEL REVISED — cheap world memory, after-effects and precipitation performance
  (2026-08-29, author rejected the artificial scalar wetness/procedural patches/PBR film/displacement model).
  The previous 5A/5B surface claims are SUPERSEDED: PF08 has no biome/material data, asphalt depressions,
  leaf-water representation or SSR, so pretending to infer puddles and a universal wet BRDF from rain rate
  was the wrong abstraction. New `surface_precipitation_memory` is a fixed world-space `64x64` map over
  `512x512 m` (`8 m` cells, one `vec4`/cell): recent rain reservoir, snow water-equivalent and filtered
  current rain/snow rates. A 4096-invocation compute integrates `rate * duration` through the SAME advected
  precipitation field, dries rain exponentially, melts snow separately, accelerates melt in rain and moves
  meltwater into the rain reservoir. The moving front therefore leaves a stationary trace. Cost is
  `0.004–0.007 ms`; each buffered copy is `64 KiB`; debug 11 shows rain red and snow cyan. Material response
  is deliberately tiny: terrain darkens at most 14% from rain and receives a restrained pale snow mix,
  fixture reacts less, foliage almost not at all, while the real trajectory-aware roof remains dry. Removed:
  random puddle masks, wet GGX/sky reflection, second light loop and all snow vertex/shadow displacement.
  120-frame clear/recent-rain/recent-snow scene minima are `1.336/1.377/1.353 ms`, totals
  `5.783/5.845/5.799`. AFTER-EFFECTS consume the map: a primary rainbow is angular geometry around the
  anti-solar direction, requires direct low sun and recent rain, and is suppressed by strong current rain.
  The original restrained bow was visible only analytically, so the shipped artistic version widens RGB
  bands to about `0.7°`, adds a soft `1.55°` luminous veil and explicitly uses visibility scale `0.0065`.
  It is now a project-controlled art system: intensity/saturation/width/sharpness, veil, Alexander-band
  contrast, memory persistence, current-rain cutoff, source mode, source balance, source separation and
  secondary-order strength occupy three explicit GPU vectors. `primary|brightest|all` selects stellar
  sources independently; default `all` makes both suns produce their own physically centred primary bow,
  while balance can lift dim Ember without changing world illumination and separation can exaggerate only
  bow centres. New `double_rainbow` preset uses g1 d0 16:00, where the stars are ~`13.7°` apart: Aurin's bow
  is upper and Ember's lower. Artistic equal-source frame versus dry is `MAE 0.00714818`; all versus primary
  is `0.00104737`. Iris Xe 120-frame dry/double sky minima `0.989/1.278 ms`, total `5.802/6.224`; dry exits
  before the source loop. Geometry/conditions remain honest unless the explicit separation knob says otherwise.
  FULLSCREEN BUG: the rainbow itself was resolution-independent and rendered correctly on a direct 1920x1080
  launch; runtime resize instead erased the fixed 64 KiB world-map that authorises the after-effect. Painter's
  resize path recreated only screensize resources but called `initialize_temporal_resources` for EVERY history,
  silently zeroing fixed-size world simulation as collateral damage. Resize initialization is now filtered to
  screensize histories; fixed temporal buffers/images retain both contents and existing layout. An injected
  frame-20 `1280x720 -> 1600x900` resize preserved both bows through frame 80. This is an engine lifetime fix,
  not a rainbow reseed: genuinely accumulated local rain remains intact across fullscreen.
  Snow uses one stable random microfacet per 12.5 cm world cell and the already computed primary direct light
  for rare sunlight sparkle, with no extra particles or shadow loop. All authored presets now verify rain XOR
  snow; continuous rain↔snow interpolation may briefly mix as sleet and to avoid a pop. A real production
  phase choice belongs to wet-bulb temperature, season and biome, none of which PF08 fabricates. Precipitation
  still has near 4096-slot particles, stateless 8192-candidate mid LOD and 160x90x96 far froxels driven by one
  field; the snow XZ torus fix keeps slow flakes around the camera. Lighting now defaults to stride 3 while
  retaining all density slices: downpour stride `2 -> 3` volume `2.883 -> 2.405 ms`, total
  `9.257 -> 8.622`, MAE `0.00203176`, PSNR `47.15 dB`. Release, runtime variants, downpour Vulkan validation
  and `98/98` pass; six checks belonging only to the deleted CPU scalar integrator were removed, while the
  authored rain-XOR-snow contract was added. PF07 and `compare_pf07_baseline.sh` were not launched; frozen
  frames were not recaptured.
- PF08 SLICE 5 ARTISTIC SNOW CLOSED (2026-08-29). Snow sparkle is now a project-authored presentation
  contract rather than three shader literals: intensity `6`, active-cell density `.70`, angular sharpness
  `.50`, stellar balance `.65`, all exposed on CLI and packed in an explicit UBO vector. One world-locked
  microfacet per 12.5 cm cell uses a cheap derivative-AA threshold instead of `pow`; unresolved random signal
  fades only once a pixel spans 8–24 cells. Nearly vertical crystal normals avoid collapsing low-sun sparkle
  into one horizon stripe. Both stars own independent facet families and retain their colours; `snow_glint`
  puts Aurin below the horizon and leaves Ember at +8.3°, yet warm glints remain, proving this is not a primary
  index effect. Source balance may artistically lift Ember without changing scene illuminance. Paused frame
  8 vs 80 is byte-identical, authored vs intensity-zero normalized MAE is `0.0000938067`; full-foliage scene
  minima `2.751/2.678 ms` make the surcharge about `.073 ms`, total minima `7.099/7.109` are noise. Vulkan
  validation is clean and `--verify` is `101/101`. NEXT ROADMAP: slice 6 is a universal short-lived lightning
  event shared by distant cloud flashes, close storm bolts and authored magic; slice 7 is planet-anchored
  upper-atmosphere auroral curtains; slice 8 is the closing audit.
- PF08 SLICE 4B CLOSED — snow plus real visible shelter (2026-08-29). `snow` is the eighth continuous
  weather preset and a second type in the existing 4096-slot persistent precipitation pool, not recoloured
  rain or a new pipeline enum. Its water-equivalent rate, `1.6 m/s` fall, shared-wind `3.2 m/s` drift,
  `4.5 cm` flake size, `22 m` near radius and `0.00085 1/m` far extinction are independently authored;
  a fourth particle `vec4` stores kind/phase, flakes billboard and flutter, and rain↔snow transitions can
  occupy deterministic fractions of one pool. Snow contacts disappear rather than emit rain rings;
  accumulation belongs to the wet-world slice. THE SHELTER IS VISIBLE: one roof plus four posts are part
  of the fixture, and `make_fixture_scene` returns their instances with the roof AABB from the same box.
  Near segments test that AABB before current-depth collision; far rain/snow tests whether a ray upward
  against its actual falling velocity crosses the roof underside, so slanted precipitation has a downwind
  dry boundary and can blow in through open sides. `--no-shelter-occlusion` retains visible geometry for
  the A/B. Snow becomes subpixel early, so its froxel handover is `0.25R` start / `0.45R` width; rain stays
  `0.72R/0.45R`. Fixed-noon MAE: near pool `0.000349426`, far snow `0.0167005`, shelter `0.000073345`, with
  the shelter diff localized under the roof/downwind edge. Iris Xe minima: simulation `0.026 ms`, volume
  `3.703`, apply `0.331`, draw `0.045`, total `10.330`. Release/runtime GLSL, Vulkan validation and `93/93`
  pass. PF07 was not launched and its images were not recaptured. The permanent shelter intentionally ends
  byte-identical PF07 clear after slice 4A; weather-dependent geometry would be the less honest contract.
- PF08 RUNTIME WEATHER TRANSITION CRASH FIXED (2026-08-29). Pressing `T` from clear to haze changed turbidity every frame and PF08 called `inc_counter(atmosphere_cache)` on adjacent submitted frames. Painter correctly aborted: `multiscatter_lut` is a conditional doublebuffer, so a generation may still be read by an in-flight frame for two frames. New pure `atmosphere_cache_gate` allows rebuilds only at the configured two-frame cadence using wrap-safe unsigned frame differences. Weather state, transmittance and every noncached consumer still update each frame; multiscatter can lag by at most one frame, and a final dirty value remains pending until the next legal rebuild rather than snapping or being lost. Four headless checks lock `build/skip/build/skip`; `--verify` is now `86/86`.
- PF08 SLICE 4A CLOSED — rain near/mid/far and current-depth impacts (2026-08-28). `rain` is one continuous authored weather state, not a screen filter. A fixed 4096-slot GPU pool uses one invocation per stable slot and per-frame history; rate in mm/h controls active occupancy, fall and shared-wind drift are in m/s, streak length/radius in metres. A coordinate bug caught by the first image mattered: shared wind XY encodes world XZ, so constructing `vec3(vec2,-fall)` sent gravity down Z and made almost horizontal rain; components are now explicit. Simulation runs after opaque depth and turns a crossing drop into a 0.22 s expanding impact ring; streaks/rings manually test reverse-Z and cannot show through geometry. Beyond the near radius, cubic handover (`0.72R` start, `0.45R` width) feeds rain extinction/scattering into the existing `160x90x96` froxel volume from ground to cloud base; the same stars, moons and sky ambient illuminate it, and the same analytic rain column attenuates direct surface/froxel-source light. Rain draws into HDR after fog apply and before metering. Fixed-noon A/B: near particles MAE `0.000300492`, depth collision `0.0000308103` with differences localized to ground rings, far extinction `0.0115107`. Iris Xe steady minima: simulation `0.036 ms`, volume `3.772`, apply `0.334`, rain draw `0.038`, total `9.750`. `--verify` is `86/86`, authored rain is Vulkan-validation clean, and the ONE final clear set has `AE=0` against all four frozen PF07 PNGs without launching PF07. Invisible shelter boxes were rejected: the inherited fixture has no physical roof, so snow plus a visible canopy/shelter volume are slice 4B.
- PF08 SLICE 3 CLOSED — finite 3D cloud layer and frozen-reference baseline (2026-08-28, author explicitly stopped repeated PF07 recaptures). `compare_pf07_baseline.sh` now launches ONLY PF08 and compares against PF07's four already-authored PNGs; PF07 is run again only by its explicit capture script. The cloud is not elevated exponential fog: `cloudy|overcast` author finite base/top, coverage, extinction, albedo and HG anisotropy. Three-octave 3D value noise lives in world space, advects along the shared weather wind with its own m/s speed, and is multiplied by a smooth `sin²` vertical envelope. The same `pf08_clouds.glsl` supplies froxel density, self-shadowing and surface cloud transmittance; the vertical `sin²` column is analytic and samples the same 3D field at the light-path midpoint, so cloud and ground shadow cannot be independently moving cookies. Debug 9 isolates density and debug 10 isolates the primary's cloud transmittance on geometry. Paused frame 8→80 changes `cloudy` density by MAE `0.00239` and the `overcast` ground shadow by `0.000842`; `--cloud-speed=0` makes both pairs byte-identical. Distance depth increased from 48 to 96 after the former showed horizontal slice stairs at kilometre range. Iris Xe 1280x720 steady minima for cloudy/overcast are volume `1.235/1.248 ms`, apply `0.363/0.369`, total `7.404/7.569`; the optimized empty clear volume is `0.009 ms`. `cloudy` is Vulkan-validation clean and headless verification is `72/72`. Clear still matches all four frozen PF07 frames without launching PF07. Precipitation is next.
- PF08 SLICES 0–2 CLOSED — baseline, weather state, height-shaped froxel fog and advected density cells (2026-08-28, follow-up driven by the author's observation that dense fog must not leave dry sharp ground shadows). PF08 is an independent copy of the closed PF07 scene and never links it; `compare_pf07_baseline.sh` still returns byte-identical `MATCH` for `noon/double_sunset/night/eclipse`. The continuous weather state currently has real consumers only: aerosol turbidity invalidates atmosphere LUTs, one wind field drives foliage plus its shadow caster, and local-medium extinction/albedo/HG anisotropy/base/scale-height/variation/cell/advection feed a real `160x90x32` RGBA16F froxel volume. Each XY compute thread walks 32 quadratic distance slices once and stores cumulative `in_scattering.rgb` plus scalar `transmittance.a`; scene depth limits the lookup, and `L=L_scene*T+S` is composed into HDR `weather_color` BEFORE metering, so exposure sees the actual fogged frame. Both stars and all moons use the existing sky data, atmospheric transmittance and the same two shadow-map slots; volume shadowing uses a single comparison because trilinear froxel sampling already filters it. THE SHADOW CORRECTION: a shadow map answers only source visibility and must not imply that visible direct light survived the medium. Shared `pf08_local_medium.glsl` now supplies the same exponential density `exp(-max(y-base,0)/H)` and world-space column modulation to the froxel march and surface lighting; surface direct is `light * fog_light_transmittance * shadow_visibility`, with the light-path optical column integrated analytically. Thus dense fog removes ground-shadow contrast while shadowed in-scattering can remain as a weak volumetric shaft. Measured on the lower half of the fixed noon frame, A/B two versus zero shadow sources: clear MAE `0.00203056`, authored fog (`sigma_t=0.018 1/m`, `H=240 m`, variation `0.35`) `0.00022983`, an `88.7%` suppression. TWO low-frequency value-noise octaves live in world XZ, are extruded into analytic vertical columns, and advect in the shared wind direction at an independently dimensioned speed in m/s; column modulation is calculated once per froxel/fragment and reused by all sources. On paused debug frames without foliage, frame 8 versus 80 is byte-identical at speed zero and changes by MAE `0.000631` at authored speed; one sky row has transmittance standard deviation `0.01891` against `0.00193` for the smooth profile. `--debug=8` on the smooth A/B at a horizontal 220 m ray from 2 m gives `5/255=0.01961` against analytic `0.01970`; `--verify` is `64/64`. Iris Xe steady minimum at 1280x720: clear volume/apply `0.127/0.224 ms`, active cells `0.619/0.279 ms`; the separate noise surcharge is below run-to-run measurement spread. Active fog and debug view are Vulkan-validation clean. This is the fog substrate reused and extended by slice 3; precipitation remains next.
- PF07 FORMALLY CLOSED (2026-08-27, follow-up to the audit entry immediately below). The four closing gates are now permanent assets rather than promises. (1) `observe_events` in `survey.*` is the single observability classifier used by both the printable `--events` calendar and `calculate_event_budget`; no duplicated horizon/night/moon-occultation/parade rules remain, and the budget correctly treats the tightest (MINIMUM spread) parade as strongest. (2) `--verify` is now `41/41` and scans the exact two-minute authored interval from calendar `y1 d0 00:00`, locking category counts `15/12/32/5/2/0` and `66` unique episodes; a five-minute shortcut was tested and rejected because it briefly dropped below threshold and split one mutual eclipse into a false extra event. The reusable observation vectors retain capacity across samples, avoiding millions of allocations in the seven-year scan. (3) Vulkan validation is now completely quiet: PF07's depth-only shader consumes the shared `p3n3` normal through the runtime-zero reserved UBO field, so location 1 survives reflection without a second vertex layout. (4) `capture_reference_frames.sh` produces `noon/double_sunset/night/eclipse` at `1280x720`, fixed frame 8, preset camera+EV, and `--no-overlay`; ImageMagick metadata is stripped, and two independent runs produced bit-identical PNG SHA-256 values recorded in the README. Those four PNGs live in `reference_frames/` and total about 628 KiB. PF07 is closed; new environmental work starts in PF08.
- PF07 AUDIT CLOSED — event calendar, eclipse data and night foliage (2026-08-27, author decided even minimal weather belongs in the next playground). THE CALENDAR CORRECTION: seven years is only the best short integer BEAT of the binary, not a cycle of the system. It misses by `0.64 d` and ignores all three moon phases, so `--year` and presets now use an ABSOLUTE 1-based year; the overlay separately annotates `beat N/7`, and year 8 no longer aliases year 1. `--events` now applies the same observability thresholds as the survey, starts at the requested absolute date, excludes eclipses below the horizon and lunar eclipses while either star is up, and includes moon-over-moon occultations plus tight parades. The seven-year (`2555 d`) budget starts at calendar year 1 day 0 midnight and counts a UNION of intervals rather than a harmonic sum of rates: `15` mutual stellar + `12` star-by-moon + `32` lunar + `5` moon-over-moon + `2` parades give `66` unique visible episodes — one per `39 d`, or `15.5 real h` at one game minute per real second. ECLIPSE DATA FIXES: unoccluded star illuminance is now stored explicitly instead of reconstructed as `actual/(1-occlusion)`, which failed exactly at totality and erased the corona; moon state/GPU data preserves separate Aurin/Ember visibility, so covering one source leaves the other source's colour instead of applying one grey lunar-eclipse multiplier. NIGHT GRASS was three problems, not an ambient knob: albedo fell `(0.20,0.30,0.13)->(0.13,0.20,0.08)`, foliage gained height-graded local direct/sky occlusion (`0.45/0.25` at the base), and its normal receiver bias fell from `1.5` to `0.25` texel because the old offset was wider than a `10 cm` blade and removed self-shadow. Cascade caster height now includes the `16.5 m` valley and shrub tops, not fixture boxes alone; at the low-sun eclipse preset the release validation run measured shadow `1.595 ms` minimum and whole 1080p frame `7.834 ms`. Before/after fixed-EV frames show a restrained change rather than a repaint (lower night crop `-1.7%` mean; bases visibly grounded at sunset/eclipse). `--verify` is `34/34`; release build, 7-year survey, shader compilation and Vulkan validation pass (one pre-existing unused vertex-attribute warning). PF07 is large enough and closes here; weather/froxel/screen effects move to PF08 on this baseline.
- PF07 solar corona, and why this system can never look like an Earth eclipse (2026-08-27, author: the disc is occluded now "но луна все еще сильно сливается с окружением" and eclipses should have a "золотой ореол"). The missing piece was the CORONA — the star's outer atmosphere, literally the thing people watch eclipses for, and it simply was not drawn. It is a MILLION times fainter than the photosphere, which is why it is invisible while the disc is open; falloff `r^-2.5` from the limb gives `2000 / 354 / 62 nits` at `1 / 2 / 4` radii. The moon is larger than the star and hides everything inside `1.76` radii, so the ring starts there exactly as in a terrestrial totality, and the warm tint is spread out to three radii rather than hugged to the limb — a rim pressed against the disc would sit entirely behind the moon. Corona reveal is tied to how COVERED the disc is, and that is a PERCEPTION model, not physics: the corona never goes away, but next to an open disc it drowns in glare (scatter in the eye and in optics) and glare is not modelled here at all, so without the factor an un-eclipsed star grew a halo it does not have in life; the covered fraction is derived from two values already present, illuminance with and without the eclipse. Strength is a documented exaggeration knob (`--corona=`, default `6` chosen by picture: `1` is indistinguishable from sky, `25` is a blur). THE STRUCTURAL FINDING, worth more than the feature: the classic black-sun-in-a-golden-corona look is IMPOSSIBLE in this system and never will be. It depends on the sky going a thousand times darker at totality; here THE COMPANION SITS `8-14°` AWAY AND KEEPS SHINING. Checked across all six total eclipses of `Aurin` in the seven-year cycle: in every case visible above the horizon `Ember` is not merely up but HIGHER than the eclipsed star (`1.28/10.15`, `10.73/16.84`, `14.00/19.70` degrees); the other three peak at night. The bright halo around the moon in frame is mostly the COMPANION's aureole, not the corona. What an eclipse actually looks like here, measured at the same hour on adjacent days with the same star altitude and fixed exposure: ground `(178,157,144)` luminance `160.5` `R/B 1.236` without, against `(80,62,48)` luminance `64.8` `R/B 1.667` with — the world dims `2.5x` and swings hard into orange because the white star is covered and only the ginger companion lights it. Not darkness, but a CHANGE OF LIGHT. General rule already recorded and reconfirmed here: an effect expressed only as a drop in brightness gets erased by auto-exposure; it must have a geometric or chromatic expression to survive.
- PF07 eclipses were invisible, and the cause was three-layered (2026-08-27, author: "не могу визуально заметить затмения" plus "луны не темнеют когда стоят между планетой и светилами" — one illness, not two). (1) DISCS WERE ADDED, NOT COMPOSITED: `color += disc`, so a dark moon added zero and could not occlude a star, a star's disc, or anything else — a new moon rendered as the ABSENCE of a moon rather than a black disc, which is exactly the "moons do not darken" report. The only trace an eclipse left was the star's disc dimming uniformly (its brightness came from illuminance, which already included the occlusion) — AND THE AUTO-EXPOSURE FAITHFULLY UNDID THAT DROP, so nothing remained. Everything beyond the atmosphere is now accumulated separately and the moon REPLACES what is behind it, while the air's in-scattering stays in front of it; the star's disc uses the UN-occluded illuminance since the moon now covers it geometrically, and dimming it too would count the eclipse twice. Moon disc radiance is now physical — a Lambertian surface of known albedo under starlight — instead of being derived from the illuminance the moon sends US, which collapses to zero at new moon along with the entire disc including the part that is still visible. (2) THE DECISIVE ONE — DISC EXAGGERATION TURNED PARTIAL PHASES TOTAL: discs are drawn 3x for readability but the angular SEPARATION between bodies was not scaled, so at a true `0.457` occlusion (separation `0.425°`, radii `0.249/0.439`) the drawn moon (`0.75/1.32`) covered the sun completely. The viewer saw not a bitten sun but a vanished one, and a vanished sun reads as "there was never one there". My first fix was simple and wrong — relax the exaggeration near contact: geometry became exact but the sun shrank to four pixels exactly when you want to look at it, and at peak the crescent fell BELOW one pixel. The right fix scales THE SEPARATION TOO: with the same factor on radii and on separation the overlap fraction is preserved exactly while discs stay large; outside contact the moon is pinned at the tangency point, otherwise drawn discs would overlap where real ones have already parted. Now legible at every phase — full disc, crescent, thin arc — with the sky darkening alongside at fixed EV. Honest remaining lie: `disc_visual_scale` on the small moons (`Kolo` `1.6`, `Iskra` `3.0`) still inflates them beyond physics so they can be found at all, so eclipses BY those moons are geometrically distorted; `Selen`, which makes the total eclipses, has it at `1.0` and is exact. Block gained `star_disc_illuminance` (un-occluded, un-horizoned, for discs only) and `moon_distance_km` (so the nearer of two overlapping moons wins); moon order in the array is deliberately NOT sorted, because the shadow system indexes moons by that order and sorting here would silently divorce lighting from shadowing.
- PF07 time became a CALENDAR (2026-08-26, author asked for a normal 24-hour day, whole-day time frames and a way to start in a chosen year of the 7-year cycle): three connected fixes. (1) THE UNIT: game time counted SIDEREAL days of exactly `86400 s`, which is convenient for orbits but drifts from the sun by four minutes a day, exactly as on Earth — and hour zero fell on an arbitrary rotation phase, so the report printed SUNSET AT 05:52 AND SUNRISE AT 19:23. The unit is now the SOLAR day and hours count from midnight, i.e. from the primary's lower culmination: sunrise `06:44`, sunset `17:15`, noon exactly between them. The midnight epoch is found NUMERICALLY by minimising the primary's altitude rather than derived in closed form — altitude already folds in rotation, orbital motion and axial tilt, and re-deriving that would be a second source of truth; it is searched in `[-1, 0)` so t=0 lands inside day 0 rather than in the previous year. (2) WHOLE DAYS: the year is fixed by orbit and masses and cannot be tuned, but DAY LENGTH is the planet's only genuinely free parameter, so the config now states `solar_days_per_year = 365` as an integer and the day length is the year divided by it. Year is exactly `365.00` instead of `365.26`; every other period shifted by the same `0.07%` (binary `46.48 -> 46.44`, `Selen` `15.271 -> 15.260`). Ordering in the constructor is now mandatory: year from Kepler in SECONDS first, then day length, then all other periods in days. (3) THE 7-YEAR CYCLE is a real property, not an invention: `7` years is `55.01` binary orbits, off by `0.42 days` (14 -> `110.02`, 21 -> `165.03`). Cycle length is SEARCHED at load rather than written in the config, since moving an orbit would silently invalidate a hard-coded number. The year of the cycle is substantive: at day 100 noon the star separation runs `1.21° / 0.05° / 0.76° / 0.85° / 0.70° / 0.96° / 1.87°` across years 1-7 and year 8 repeats year 1. Only SIX total eclipses of `Aurin` exist in the whole cycle and almost all peak while it is below the horizon — the one daytime case is year 5 day 279, which is now the `eclipse` preset. Calendar conversion lives in `celestial_system` (it owns the epoch and the year length) and all three formatters — window, report, survey — go through it; `--year/--day/--hour` work in every mode, and `--events` gained a start time, which it previously ignored entirely so "show me year 3" scanned year 1 while printing year 3. Presets now carry `cycle_year/day/hour` instead of a raw day count, and re-measured EVs are noon `+12.47`, double sunset `+9.28`, night `+0.99` (a genuinely full `Selen`, phase `0.995` at `63.7°`, `0.64 lx`), eclipse `+10.49`. Overlay subtitle now names the slices instead of claiming "Slice 2".
- PF07 slice 5 CLOSED — proxy valley + instanced foliage with a shared wind field (2026-08-26): new `src/terrain.*` and `src/foliage.*`, shared `pf07_wind.glsl`. THE COORDINATE DECISION came first: the ground disc follows the camera, so terrain computed from a camera-following mesh's local coordinates WOULD SLIDE WITH THE OBSERVER. The honest alternative is world-space height in the vertex shader, but then the CPU needs the same function to plant foliage, which means a shared C++/GLSL dialect header. Sidestepped entirely by making the valley patch STATIC: a valley is a PLACE at the origin, not a property of the world. Height is computed once on the CPU and the same function seats shrubs and the feet of the fixture boxes. `400x400 m` at `3 m` spacing, `106134` vertices, fading to zero over `130..175 m` so it lies flush on the far disc, lifted `2 cm` so the two coplanar surfaces do not fight for depth. FOLIAGE is geometry, not an alpha texture — the playground has no leaf atlas and no alpha-to-coverage, and a cross of opaque quads reads as cardboard; five tapering blades give a plant silhouette with no texture at all, each emitted twice with reversed winding because the scene material is shared with the boxes and culls back faces. THE WIND MUST LIVE IN A SHARED INCLUDE: sway is computed by TWO shaders, the main pass and the shadow-map build, and if they diverge by a single term the shrub's shadow slides off the shrub with no compiler or validation-layer complaint — the same lesson as the shared medium and surface models, now about motion. Verified by measurement, not by eye: switching the wind on changes `4.78%` of GROUND pixels (pixels that are not shrubs); ground is not shrub, so only the shadows could have moved there, and a wind-unaware shadow map would have left it untouched. One wind field for the whole world (wind is weather, not a property of a plant); only the phase is per-shrub, else the whole valley sways as one object; the wave TRAVELS along the wind direction, else the valley pulses in unison; and wind time is REAL, not game time, so fast-forwarding days must not speed up the branches. LOD is measured structurally as well as in time: `6000` shrubs, near mesh `60` vertices, far `24`, and the overlay reports the split — `584` full + `4112` reduced, `1304` culled at `130 m`. FOLIAGE SHADOW REACH, reported by the author ("тени от травы видны только на совсем близком расстоянии"): TWO limits, both of them mine, both taken for economy at the time. (1) Foliage was written only into cascade 0 — but with the classic split lambda `0.85` cascade 0 reaches only `3.73 m`. (2) Only the NEAR LOD pair was listed as a caster, i.e. `584` shrubs of `4696` drawn, so even a farther cascade would have had nothing to draw. Both pairs now cast into all three cascades: shadow pass `1.04 -> 1.64 ms` and grass shadows reach `60 m` instead of `3.7`. THE SPLIT ITSELF was then re-chosen by measurement rather than tradition: `0.85` spent cascade 0 (`1.0 cm` texel) on the first three metres and dumped everything else into cascade 2 at `15.9 cm` against a `10 cm` blade; the far cascade's texel barely depends on lambda (its sphere radius at 60 m dominates) while the near two stretch threefold. Chosen by the fraction of SHADOWED GROUND — literally how much grass actually casts — measured per screen band: `0.85` gives `3.7 / 2.4 / 6.0 %`, `0.40` gives `4.4 / 3.3 / 6.1 %`, `0.25` gives `4.4 / 3.2 / 6.6 %`. The classic value lost in ALL THREE bands including the nearest, because cascade 0 did not even reach the near grass. Shipped `0.40`, exposed as `--cascade-split=`; near field re-checked for acne and shadow detachment, clean. COST at 1080p, minimum over 140 frames x 3 runs: shadow `1.03`, sky `0.92`, scene `3.96`, TOTAL `7.87 ms`; foliage alone is `0.43 ms` with LOD against `0.74` with the full mesh everywhere, so LOD removes `42%` of it. MEASUREMENT NOTE worth keeping: between runs the numbers wandered enough that foliage came out costing NEGATIVE time — the machine was heating and the spread swallowed the difference whole. Usable numbers needed minimum-over-140-frames, interleaved configurations and a cooldown. A quantity smaller than the measurement spread has not been measured, however confident the average looks. ENGINE GAP FOUND AND FIXED IN `painter`: a draw group's indirect buffer was sized `32 * 32` bytes and IGNORED the declared `draw_capacity`, even though the capacity constant is assigned into the resource one line earlier — the buffer was always 1 KB, i.e. 32 commands. Harmless while pairs were small, but `register_pair` reserves indirect space per INSTANCE rather than per command, so the first 6000-instance pair ran off the end and the validation layer caught it as a drawIndirect overrun. The fix takes the max of the old size and the declared capacity, so no existing config shrinks and anyone who asks for capacity gets it; PF06 re-checked validation-clean afterwards.
- PF07 slice 4 CLOSED — geometry fixture + two-sun cascaded shadows + ground as geometry (2026-08-26): slice 3 is closed. New `src/fixture.*` (one unit cube instanced into pillars and slabs — deliberately primitive, its job is CASTERS, not landscape) and `src/shadows.*` (source selection + cascade math), plus `scene.vert/frag`, `shadow.vert`, `depth.frag`, shared `pf07_surface.glsl` (the ONE surface-lighting model, shared by scene geometry and the analytic ground for the same reason the medium model is shared) and `pf07_shadow_sample.glsl` (bindings fixed at `set = 1` in BOTH consumers — shared sampling code is impossible while each shader picks its own set index). THE DESIGN DECISION: the shadow system does not know about "suns" and "moons" — it takes the TWO BRIGHTEST SOURCES in the sky, whatever they are (day: `Aurin`+`Ember`; night: `Selen`). One rule instead of three modes, and handover comes free: while both slots are filled and each contribution is scaled by its light share, swapping bodies between slots changes nothing at all; a click is only possible when a body ENTERS or LEAVES the pair, so strength fades near the threshold. Threshold `0.03 lx` is physical — a full moon gives `~0.3 lx` and its shadow is visible, so the cutoff sits an order below. Atlas is six `1024²` tiles (2 sources x 3 cascades) drawn in ONE pass via `draw_regions`; layout travels as DATA so the shader never knows it, and an empty slot gets zero instances rather than six idle tile passes. TWO COORDINATE SYSTEMS, and they must not be confused: atmosphere in KILOMETRES from the planet centre, scene in METRES near the camera; planet curvature inside the scene is `0.8 mm` over `100 m` and deliberately ignored. First bug from that: the camera sat at scene `y=0` while the atmosphere put the eye at 2 m, so object bases landed exactly on the horizon line and everything looked infinitely far; camera height now drives the atmosphere, so climbing actually pushes the horizon out. VALIDATION OF THE DOUBLE SHADOW, the point of the slice: `--shadow-sources=N` renders with one source instead of two, and the frame difference IS the second shadow. Same ground point, fixed exposure: unshadowed `1158 nits`, `Aurin`-only shadow `264`, both `141` — so `Aurin` contributes `895` and `Ember` `123`, ratio `7.27`, against `5.87` from pure celestial mechanics; atmospheric transmittance, different for two spectra and two altitudes, supplies `1.19x`, giving `6.99`. Two independent computations agree to `4%`. Colour follows: `R/B` `1.224 -> 0.833 -> 0.381`, the double umbra lit by sky alone. THIRD ENCOUNTER WITH THE SAME PRECISION BUG, and it should now be reflex: shadow edges shredded into speckle on the analytic ground ONLY (boxes were clean, which rules out depth bias and filter width — neither can tell receivers apart). The sphere intersection computes `|o|² - r²`, which at 2 m eye height is `25.5 km²` out of `~4e7` where the float LSB is `4.8` — a 19% error for free — and `-b - sqrt(b*b - c)` then subtracts two values differing in the fifth digit. Both fixed by identities, not by more precision: `(|o|-r)(|o|+r) = h(2r+h)`, and the stable quadratic form computing one root directly and the other through the root product `c`. Clean edges, zero extra samples. COST, honestly: shadow pass `1.48 ms` and scene `1.06` are fair, but `pf07_sky` went `4.03 -> 13.0 ms` and the whole 1080p frame `7.36 -> 25 ms`. Measurement shows it is NOT the taps: with `PF07_SURFACE_NO_SHADOWS` the same frame costs `7.4` instead of `10.6`, and looking UP with no ground on screen `4.9` instead of `6.9` — two thirds of the surcharge is paid merely because the shadow code SITS in the sky shader and raises register pressure, so even the branch that never executes costs. DONE the same session: the analytic surface moved OUT of the sky fragment shader into the geometry pass as a camera-following `6 km` disc with curvature BAKED INTO THE VERTICES (a second pair in the same draw group — the draw command iterates all pairs, so it renders itself, while the shadow pass uses explicit spans that name only the caster pair, which is exactly right since ground must RECEIVE shadows and has nothing to cast). Curvature is not decoration: without it a plane runs to infinity and there is no horizon at all. Drop `d²/(R + sqrt(R² - d²))` — the same identity that saved the shadow edge, since naive `R - sqrt(R² - d²)` again subtracts near-equal millions — is exactly `2 m` at `5048 m`, i.e. eye height, so THE HORIZON EMERGES FROM GEOMETRY: measured `-0.051°` against a theoretical `-0.045°`, under one pixel. The move is transparent in both image and numbers: the double-shadow measurement afterwards returns THE SAME `7.27` to two decimals, which is the real proof that the surface-lighting model is genuinely shared — a completely different shader now computes the ground and the result did not budge. COST at 1080p, minimum over five runs (minimum, not average — extra load can only add time): day looking down shadow `0.48` / sky `1.13` / scene `2.48` / TOTAL `6.54 ms`; looking up `6.14`; night `5.90`. Slice 3 with NO geometry and NO shadows cost `7.36 ms`, so the frame now carries a fixture, six cascades from two suns and a ground mesh and is CHEAPER; the sky pass fell `4.03 -> 1.13 ms` because it no longer touches the surface at all. The intermediate state with the ground still inside the sky shader measured `19.4 ms` by the same minimum — a 3x difference, the price of one branch living in the wrong shader. Gotcha found on the way: a pair reserves indirect-draw space per INSTANCE, so an oversized `max_count` on the first pair pushes the next pair past the buffer (registering 64 for a 12-instance fixture overflowed at the ground pair); tighten reservations rather than growing the capacity constant. HORIZON BEHAVIOUR, the last item of the slice, and its bug was invisible by eye: a cascade is stretched along the light so casters OUTSIDE the camera slice can still shadow into it, and that margin was a constant `20 m` — correct only down to 23° of elevation. The requirement is `h / sin(elevation)`: `16 m` at 30°, `46` at 10°, `92` at 5°, `458` at 1°. The failure is silent — nothing flickers or tears, the far part of every shadow simply CEASES TO EXIST. At `5.02°` there was no shadow in frame at all; with the margin computed from the tallest caster it came back whole, changing `3.1%` of the frame. A sine floor caps the margin at the horizon, where light grazes the surface and contributes almost nothing directly. Lesson: a constant that happens to be right for the common case hides a whole regime. HANDOVER measured rather than eyeballed: through `Aurin` set at fixed exposure, frame-to-frame change reads `10.9 / 11.1 / 6.3 / 5.3 / 9.8` across altitudes `+1.22 -> -1.33°` — no spike anywhere, and it actually DROPS at the crossing, which is exactly where `Ember` overtakes `Aurin` and the slots swap; invisible because each source is scaled by its light share and at equal shares a swap is the identity. Also worth recording: at grazing light shadows on flat ground physically VANISH (the normal-light cosine goes to zero with the direct term), so what remains visible at the horizon is shadowing on VERTICAL faces, which holds up while the faces take on the sunset orange. Final cost at 1080p, minimum of five runs: day `0.47 / 1.10 / 2.43` TOTAL `6.59 ms`, low sun `6.69`, night `5.97` — still under the slice-3 baseline of `7.36 ms` which had neither geometry nor shadows. Double-shadow ratio re-measured after both the ground move and the margin fix: `7.27` unchanged to two decimals each time.
- PF07 exposure re-derived: meter LUMINANCE, not illuminance (2026-08-26, author reported "рассвет все еще выжигает глаза"): the fix took two passes and the second one matters. FIRST PASS (superseded): metering saw only the direct beam, so at twilight — both suns below the horizon, direct `0.018 lx` — exposure concluded night and opened up 4x while the sky was still glowing. I added a diffuse-skylight term whose CURVE was physical (anchored to `3.4 lx` at `-6°`, `0.008` at `-12°`) but whose horizon MAGNITUDE was deliberately ~6x above physics, justified as compensating incident metering. Dawn still burned. THE REAL DIAGNOSIS: exposure was `+2.20` where the eye wants about `+5`, and the fudge was useless in BOTH directions — physical twilight illuminance `3.4 lx` gives `+0.73` and the inflated `16.8 lx` gives `+2.11`, i.e. REMOVING the fudge makes the image BRIGHTER, because less measured light means lower EV and a wider aperture. When both ends of the knob over-expose, the measured QUANTITY is wrong, not its scale. Illuminance is incident light and is correct for a scene that light ILLUMINATES; the sky is not illuminated, it IS the source and at twilight it fills the frame — metering it by illuminance on a horizontal plane is like judging a lamp by how it lights the floor. The tempting trap was that this playground knows illuminance EXACTLY (the celestial module reports lux), so exposure could be analytic with no readback and no latency: an exact value of the wrong quantity is still the wrong quantity, and its exactness lends it unearned trust. SECOND PASS (shipped): new `meter.comp.glsl` reduces the HDR `sky_color` over a sparse `64x64` grid to one number in a `readback`-role buffer; the host reads the CURRENT copy, which the GPU filled `buffering` frames ago and long since finished, so there is NO fence wait, and there is no feedback loop because `sky_color` holds pre-exposure radiance. The average is over the LOGARITHM of luminance and that is not cosmetic: a star disc is ~`2e9 nits` and would outweigh the entire sky in an arithmetic mean, while in the log it shifts the result by hundredths of a stop. Reflected metering at ISO 100 / `C=12.5` gives `EV100 = log2(L * 100 / 12.5)`. Pass costs `0.06 ms` minimum, `0.19` average. SECOND FIX, and the actual cause of the blinding dawn: the old `night knee + compensation` pair broke the scale in half — it left bright states untouched and dragged dark ones toward its threshold. Measured deviation from mid-grey exposed the kink directly: noon/eclipse/double-sunset all sat at `-1.90` stops, NIGHT at `+1.88`, but DAWN at `-1.05` — i.e. night was pushed 1.9 stops into darkness while dawn was pushed 1.05 stops into LIGHT. Replaced by ONE honest parameter, `adaptation_strength` (how completely exposure follows scene luminance; 1 = everything mid-grey, 0 = fixed exposure), anchored at the measured noon frame. THE VALUE WAS THEN CHOSEN BY TWO MEASURABLE THRESHOLDS, not taste — my first guess of `0.75` still burned and the author said so. At the top end: FRACTION OF THE FRAME CLIPPED TO PURE WHITE at dawn, which matters because approaching white destroys colour and the whole sunrise washes into a pale band. At the bottom end: MOONLIT GROUND, which must stay readable under a full moon. Ladder measured at dawn (EV / horizon sky / upper sky / clipped) and night (EV / sky / ground): `0.75` -> `+4.41 / 218 / 89 / 9.34%` and `-0.66 / 25 / 23`; `0.65` -> `+5.48 / 179 / 53 / 0.06%` and `+1.09 / 12 / 11`; `0.55` -> `+6.54 / 126 / 31 / 0.02%` and `+2.83 / 6 / 6`; `0.45` -> `+7.61 / 78 / 19 / 0.00%` and `+4.57 / 3 / 3`. Clipping collapses 150x between `0.75` and `0.65`; moonlit ground stops reading between `0.65` and `0.55`. There is almost no room between the two thresholds and that is not a coincidence — both are the same scene dynamic range. Shipped `0.65`, exposed as `--adaptation=`. Final ladder: noon `+12.42`, eclipse `+10.55`, double sunset `+10.16`, dawn `+5.48`, night `+1.09`; sunset and eclipse were over-exposed too, just less offensively. Also confirmed the sky PHYSICS was never at fault: measured twilight sky luminance at `-6.2°` is `0.49-0.64 nits` at `60°` altitude rising to `7.58 nits` at `15°` toward the star, against published `1-3 nits` at zenith and `10-30` near the horizon — if anything slightly dim. Stability checked: the same instant at 6/12/30/60 frames gives `+4.41 / +4.41 / +4.42 / +4.47`, the drift being the sun actually rising. Preset EVs are now measured by each preset's OWN camera aim (noon `+12.42`, double sunset `+9.81`, night `-0.66`, eclipse `+10.26`) — aim stopped being decoration, since the same instant looking at the rising star versus away from it asks for different exposure.
- PF07 time acceleration must compress eye adaptation too (2026-08-26, author: dawn still burned out even at `--adaptation=0.55`, "это происходит как только солнце пытается выйти из горизонта, после чего через некоторое время все стабилизируется"). NOT the same bug as the two before it, and the report said so without touching a pixel: `EV100 +6.41 · target +10.33` — the adaptation was lagging 3.9 STOPS behind, an aperture 15x wider than the meter asked for, while `metered frame luminance 185.3 nits` would sit exactly at mid-grey at the target. Diagnostic tell that separates lag from genuine over-exposure: the SKY blew to white while the GROUND stayed normal; a scene that is actually too bright drags both down together, since the exposure is one scalar. The cause was one line higher in the same overlay: `0.0222` game days per real second, a day in `45 s`, `32x` the nominal rate — at which a star crosses its own diameter in `0.06 s` and the whole `-1°..+1°` dawn takes `0.25 s`, against a `1.2 s` light-adaptation constant. The constants are in REAL seconds because it is the player's eye adapting, not the planet, and that is right while time runs nominally; under fast-forward the premise breaks, because accelerated time IS time-lapse photography and the exposure in a time-lapse must track the scene or the frame stops carrying an image at all. Constants are now compressed in proportion to the acceleration, never stretched on slowdown, and the PAUSED rate is used rather than the configured one (otherwise time stopped during fast-forward leaves an instant eye and a camera pan snaps the exposure). Measured on the night->sunrise ramp at `0.0222`: peak lag `4.07 -> 0.29` stops, with `-0.6°` reading `3.89` against the author's screenshot `3.92` — reproduced number for number. Peak lag by rate after the fix: nominal `0.36`, `0.005` `0.35`, `0.0222` `0.29`, `0.1` `0.10`; nominal behaviour is untouched by construction (multiplier is exactly 1) and that `0.36` is the eye lag the feature exists for. FOUND ON THE WAY, and important on its own: in fixed-frame mode game time advanced by a constant `1/60 s` per frame (for reproducible dumps) while adaptation used the REAL `dt`, so a dump on a fast machine crossed sunrise at a completely different exposure than the 60-fps window — and the complaint could not be reproduced headless at all. One step value now feeds both. Hence `--trace-exposure` (per-frame target/current/lag): a single dump cannot show a transient, it shows the result. Same lesson as `--debug=7` for the galaxy band — a quantity that drives a decision must be displayable separately from its consequences.
- PF07 galaxy band: a soft light ribbon has a visible edge, a scatter of stars does not (2026-08-26, author: "светлая зона галактики ночью ... есть у нее видимая граница которая прям выбивает из погружения"). The band was drawn as diffuse luminance along a great circle, cut off by `band < 0.002`; a smooth gradient on near-black sky shows such a cutoff as a hard border, because the eye finds an edge where the maths considers it vanishingly weak — a general hazard of any soft addition over darkness, not a bad constant. Luminance removed entirely; the band is now expressed ONLY as star DENSITY (cell occupancy `0.11 -> 0.385` in the galactic plane), so no edge can exist by construction, and widened ~6x because the old `3.5°` half-width read as a seam. Star-cloud/dust drawing deferred: it needs a texture or noise, not another formula. DEBUGGING LESSON: the concentration measured as barely present and I nearly blamed the uniform not reaching the shader; in fact the band simply was not in the frame regions where I was counting stars — an aggregate over the frame cannot find a narrow structure. Added `--debug=7`, which draws the band VALUE itself, and the answer was immediate. A quantity that drives a decision must be displayable separately from its consequences. Also factored `sky_inertial_direction()` so the star field and the band cannot disagree about the sky-fixed basis.
- PF07 slice 3 started — physical exposure, night adaptation, presets, colour script (2026-08-26): slice 2 is closed and the exposure slice is running. Exposure is DERIVED, not tuned: this playground knows scene illuminance exactly (the celestial module reports lux), so `EV100 = log2(E / 2.5) + bias` from incident-light metering at ISO 100 / C=250, then the standard `1.2 * 2^EV100 -> 1.0` normalisation. No histogram, no readback latency, no dependence on where the camera points; the only free number is a `-1.9` stop bias that shows in the report. Measured: noon `+12.37`, eclipse `+11.31`, double sunset `+8.51`, full moon `-1.18`, moonless `-5.71`. THE KEY MECHANISM of the slice is the night compensation knee: a photographically correct night exposure is ALWAYS brighter than the eye sees (a long moonlit exposure gives a blue day), and full compensation would be exactly what PF06 forbade — the renderer inventing visibility. Below `EV100 = 4` exposure follows illuminance only `60%` of the way (full moon asks `-4.64`, gets `-1.18`), with a hard floor at `-6` for real darkness. Adaptation is asymmetric and in REAL seconds (`1.2 s` brightening, `6 s` darkening) because it is the player's eye adapting, not the planet. Four presets live in `resources/celestial/presets.tavl`, each fixing time, camera AND EV — without the last one a dump compares the adaptation's work rather than the scene; times were found by scanning the mechanics (`eclipse` is `d114 08:51`, `Selen` totally covering `Aurin` at `65°` altitude with `Ember` beside it at `67°`). CORRECTION to an earlier README claim: the eclipse does NOT turn the whole scene orange. Honest A/B at one camera and one fixed EV shows the SKY goes COOLER (`R/B 0.675 -> 0.510`) because Rayleigh stays blue under any source and removing white `Aurin` removes what made it paler, while the GROUND warms `+30%` (`R/B 1.045 -> 1.359`) at `61%` brightness — source colour lives on lit surfaces, not in the sky. Companion share `22.9% -> 100%` is the real content of the event. Added scotopic (Purkinje) response keyed off PRE-exposure luminance (cones fail at scene brightness, not at whatever the tonemapper did): moonlit ground `B/R 1.020 -> 1.230`. Colour script lives in `resources/celestial/colour_script.tavl`, keyed by primary-star altitude, deliberately bounded — identity at noon, percent-level warm shift at the horizon, saturation `0.55` only in deep night. Compose order is not arbitrary: exposure first on physical nits, night vision on the original luminance, colour script on light before the curve. REAL GAP FOUND: moons did not light the SKY, only the ground — an absolutely black sky over moonlit ground cannot happen, since the light reaching the ground had to cross the same air. Moons are now sources in the sky march (no multiscatter term for them: five orders below the suns, would double march cost for an invisible addition); full moon gives `0.03 nits` of scattered sky against `0.019` on the ground it lights, so the night sky is correctly BRIGHTER than the ground. `pf07_sky_view` `0.54 -> 1.07 ms`, whole 1080p frame `8.44 ms`. New CLI: `--ev`, `--ev-bias`, `--preset`, `--night-vision`.
- PF07 aerial perspective LUT + multiscatter caching (2026-08-26): the four-table atmosphere chain is complete. Added `aerial.comp.glsl` — a `32x32x32` froxel volume (two screen axes, one distance axis, quadratic so slices bunch near the camera where scattering changes fastest) built with a SINGLE running march per screen cell that writes each slice as it passes, not a separate march per slice, which would be quadratic work. It replaces the per-pixel 8-step ground march with one 3D fetch and agrees with the march to within `1/255`. Its real purpose is slices 4-5: it knows nothing about where depth comes from, so terrain and foliage can use it unchanged. Demonstrated from `400 m` altitude where the ground runs `(126,113,112)` underfoot to `(139,135,150)` at the horizon — warm near, blue far, blending into the sky; at the shipped 2 m eye height the whole far ground is compressed into a few pixels so the effect is correct but has nowhere to show. Also moved the multiscatter LUT onto a host-driven conditional counter (`atmosphere_cache`, same mechanism as PF03's grade-LUT cache, RND-50): the LUT depends ONLY on medium parameters — the sun zenith angle is one of its axes, not an argument, so it does not change with time of day — and the host bumps the counter on startup and when turbidity changes, i.e. with weather later. The timestamp report shows it directly: `pf07_multiscatter` minimum `0.001 ms` in frames where the counter did not move. New CLI: `--aerial-range=KM` (default 8, covering the 5 km horizon from 2 m) and `--camera-height=KM`; `--light-steps` is gone with the nested march it configured. Frame cost at `1920x1080` day: transmittance `0.12`, multiscatter `0.60` avg / `0.002` cached, sky-view `0.54`, aerial `0.70`, sky `4.03`, compose `1.04`, TOTAL `7.36 ms`; night `7.33 ms`. Whole-slice arc: `78.6 ms` for a single direct-march sky pass at 1080p down to `7.4 ms` for the entire frame, with none of the four tables scaling with screen resolution.
- PF07 multiple scattering + star sizing + sky irradiance on the ground (2026-08-26): added `multiscatter.comp.glsl` (Hillaire's method, `32x32` over height x sun-zenith, 64 sphere directions x 20 steps, ground bounce included). Higher orders are assumed isotropic and the medium locally homogeneous, so the infinite order sum collapses to a geometric series and only second-order light plus the re-scatter fraction are needed. Measured zenith gain `+14/+21/+28%` in R/G/B — strongest in blue where optical depth is highest, as it must be. MY OWN BUG worth remembering: the first version doubled the sky because I dropped the `1/(4pi)` isotropic phase. Two similar-looking quantities come out of the same along-ray integral and they are NOT the same kind: second-order light is a RADIANCE and carries the isotropic phase (the sampling solid angle `4pi/N` cancels against averaging over directions, but the phase cancels against nothing), while the re-scatter fraction is a PROBABILITY, dimensionless, phase-free, and stays a plain average. Second gap found from the author's "moonlight on the ground does not show": the moonlight was in fact correct and measurable (`(43,38,36)` at full moon, matching the hand calculation) — the real hole was that the GROUND HAD NO SKY LIGHT AT ALL, day or night, only the direct beam, which is why it looked cut out of another picture. Now approximated with five sky-view samples and `E = pi * L_mean`, taken only on ground pixels. Star flicker fixed by size, not filtering: a star drawn SMALLER than a pixel hops between neighbours as the sky rotates. The radius is now screen-aware with a `1.5 px` floor (the same reasoning as PF02's screen-aware shadow filter width), brightness holds constant FLUX rather than constant surface brightness so an inflated star does not also get brighter, and the count dropped to ~1050 stars (40 cells per cube-face edge at 11% occupancy) because three thousand two-pixel grains read as noise. Measured mean bright-blob width `2.7 px`. Frame cost at 1920x1080 day: transmittance `0.15`, multiscatter `2.33`, sky-view `0.44`, sky `5.42`, compose `0.88`, TOTAL `9.78 ms`; night `8.29 ms`. Next obvious saving: the multiscatter LUT depends ONLY on medium parameters — the sun zenith angle is one of its axes, so it does not change with time of day — and therefore needs rebuilding only when turbidity changes, i.e. with weather; the conditional-pass-as-cache mechanism is already proven in PF03 (RND-50).
- PF07 sky-view LUT + presentation knobs (2026-08-26): the sky is now resolution-independent. Added `sky_view.comp.glsl` (`192x108`, in-scattering in rgb and view-transmittance luma in a) plus a shared `pf07_atmosphere.glsl` holding the ONE medium model, phases, disc solid angle and the transmittance lookup — previously the medium constants were duplicated between the sky march and the transmittance builder, which is the same class of hazard as PF02's buffer layouts. `1920x1080` day: sky pass `26.8 -> 4.0 ms`, whole frame `28.1 -> 5.8 ms`; night `7.3 ms`. Both LUTs cost `0.08` and `0.25 ms` and do not scale with screen size, so 1080p is no longer more expensive than 720p except in the final disc/star pass. Ground rays are NOT in the LUT and must not be: from 2 m up they are shorter than five km even at the horizon, so they run an 8-step march in the fragment shader where they also need surface shading. The sky-view axis is WORLD azimuth, not sun-relative as in the usual implementations, because two stars have no single reference azimuth. Limitation recorded: the LUT is built for one observer altitude. Presentation knobs added on the author's request: per-moon `disc_boost` (drawn brightness) and `disc_visual_scale` (drawn size) in the config — physics of lighting, eclipses and the event budget untouched. Size turned out to be the decisive one: `Iskra`'s `0.126°` disc is SIX PIXELS at 1080p and no amount of brightness makes six pixels findable. Moon terminator now sums BOTH stars weighted by illuminance, so the crescent carries the narrow companion-only band along its edge (width = the stars' angular separation, brightness ratio ~1:5). Added a first galaxy band (density falling off the disc plane, tilted away from the ecliptic so it does not coincide with the stars' path) at `0.35 nits` — the real thing is `1.7e-4 nits`, four orders below a full moon and invisible at any moon-visible exposure, so this is a deliberate exaggeration. Star field gained `--star-rotation` (default `0.15`): a full sky revolution takes one game day = 24 real minutes, so honest rotation looks like a helicopter; suns and moons keep moving physically and only the constellation drawing is slowed. Also caught a real inconsistency: the window's default `time_scale` was `0.02` game days per real second, i.e. a DAY EVERY FIFTY SECONDS, 29x faster than the rate the whole event budget was computed at; default is now `1/1440`. Hour-long game days would stretch large events to one per `47` real hours instead of `19` (measured with `--survey --game-minutes=0.4`); restoring the old cadence would need inclinations about 2.5x lower.
- PF07 transmittance LUT + author questions answered (2026-08-26): the reported fullscreen FPS drop was measured, not guessed — the nested light march is cheap while a star is BELOW the horizon (one sphere test then early-out) and expensive once it rises (8 steps x 32 primary samples x 2 stars = 512 medium samples per pixel): `8.2 ms` vs `36.5 ms` at 720p, and `78.6 ms` at 1080p. Added a Bruneton-parameterised transmittance LUT (`256x64`, `resources/shaders/transmittance.comp.glsl`, compute pass `pf07_transmittance` rebuilt every frame at `0.055 ms`, shared uv<->(r,mu) mapping in `pf07_records.glsl` so write and sample cannot diverge). Result: 720p day `36.5 -> 12.4 ms`, 1080p day `78.6 -> 26.8 ms`, night unchanged at `8 ms` (it had no inner loop to remove). Accuracy improved too: 8 uniform steps along a slanted exponential profile systematically UNDER-estimated optical depth, so the old sky was too bright. PAINTER CONFIG GOTCHA worth remembering: `type = fixed` in `declare_values` builds a SQUARE image `x by x` (it is meant for buffer element counts) and silently ignores the second component — the LUT came out `257x257` while the compute wrote a `256x64` grid, upper rows stayed zero and sampling was shifted, which showed up as a darker sky with a curved seam. Rectangular images need `fixed_2d`. Third debugging lesson: when the formula inputs also look sane, print the RESOURCE SIZE next — `imageSize` answering `257x257` where the config asked `256x64` settled it in one run. Also verified for the author: stars ARE already sky-fixed (0% bright-pixel overlap both when the camera turns 20 degrees and when the sky rotates 29 minutes, so they are locked to neither screen nor horizon), and `Kolo`/`Iskra` were invisible in the reported screenshot for physical reasons — `Kolo` at `-11.4°` altitude and both moons at phase `0.09`/`0.04`, i.e. thin crescents of `~11` and `~34` nits against a `5000`-nit day sky; a gibbous moon at day `8.2` renders as a normal pale daytime moon.
- PF07 slice 2 started — physical sky renders (2026-08-25): `PF07_party_environment` is now a graphical playground too. New files `src/sky_frame.*` (pure celestial-state -> GPU block packing, the single ENU->world coordinate change) and `src/sky_view.*` (window/Vulkan/loop), plus `resources/render_config/` (graph `pf07_party_sky`: sky -> compose -> ui -> present) and `resources/shaders/` (`pf07_records.glsl` as the single buffer-layout declaration, `sky.frag.glsl`, `compose.frag.glsl`). `--render` is the default action; slice-1 headless modes stay explicit flags and still link no Vulkan path at runtime. The sky is a DIRECT atmosphere march in the fragment shader (Rayleigh + Mie + ozone, 32 primary x 8 light steps) chosen deliberately so the LUT step has a measured baseline rather than a citation — same method PF03 used for Hi-Z. Two stars cost almost nothing extra because single scattering is LINEAR in the source: contributions are summed over the same samples. Measured on Intel Iris Xe at 1280x720: `pf07_sky` `35.4/31.2/64.0 ms`, compose `.39`, ui `.14`, present `.21` — the sky alone is 5x the whole frame budget, which is the justification for LUTs next. Working: day sky with horizon haze and forward-Mie aureole, star and moon discs at honest angular sizes with limb darkening, and the double sunset (`d0 06:07`: `Aurin` set, `Ember` at `9.4°` giving `20 921 lx`, horizontal `3413 lx`, golden sky). Missing: multiple scattering (sky away from the sun is too dark/grey), aerial perspective, star field, physical exposure (`--exposure` is one manual knob for now). Three bugs fixed and worth remembering: (1) tan(half-FOV) was read from the VIEW matrix instead of the projection — `view[1][1]` is a rotation component, so ray directions depended on camera orientation; it is now passed explicitly in the free `viewport_near.w` channel; (2) surface self-intersection — a ground point testing its own sphere has `dot(o,o)-r² == 0` to float precision, the sign flips per pixel and lit ground gets ragged black patches, fixed by lifting the point 50 m before the test; (3) half-float overflow — a star disc's radiance is illuminance/solid-angle ≈ 2e9 nits, which is +inf in `sf4`, and inf through the tonemap curve yields NaN and a BLACK disc, fixed by clamping at the format boundary, not in the tonemapper. Process lesson: half the debugging time went to a "ragged horizon" that did not exist — the dark regions were debug UI panels, including a stray PF06 panel that arrived with the copied Lua script. Look at the image before reading pixels by coordinate. Vulkan validation clean, strict warnings clean, slice-1 verify still 27/27.
- PF07 slice 2 — night, twilight and discs (2026-08-26): three author complaints fixed, two of which were real bugs. (1) ABRUPT DARKENING AT SUNSET: the CPU packing multiplied a star's illuminance by its above-horizon disc fraction, so the light snapped off the moment the disc set. Twilight exists precisely because the upper atmosphere still sees the star when the observer does not, and only the per-sample march can decide that — `body_view` gained `space_illuminance_lx` (eclipse applied, horizon NOT) and the shader consumes that. Measured sky brightness through sunset is now `224 → 197 → 143 → 112 → 67 → 8.5 → 0.4` over two game hours, no cliff. (2) EVERY DISC RENDERED UNIFORMLY WHITE: `2*pi*(1 - cos(theta))` for `theta ≈ 0.004 rad` is catastrophic cancellation and this GPU's `cos` returns EXACTLY `1.0` there, so the solid angle was zero, illuminance/0 was inf, inf hit the half-float clamp — terminator, limb darkening and the size difference between the two stars all vanished although computed correctly. Replaced by the stable identity `4*pi*sin(theta/2)^2`. (3) BLACK NIGHT: added a star field plus moon contribution to ground lighting. Stars use CUBE-FACE cells, not a cubic lattice in direction space — the lattice put neighbouring cells at different distances from the origin, so after normalisation their stars collected into visible shells; `48` cells per face edge at `18%` occupancy gives ~2500 stars against ~3000 naked-eye. Stars live in the inertial frame (`sky_state` now exports the ENU basis) so they rotate with the planet, and fade by sky luminance because a real star occupies a negligible fraction of a pixel while ours occupies a whole one. Moon terminator is geometric: the surface normal is reconstructed from the point's position on the visible hemisphere and must point TOWARD the observer — the opposite sign lights the far side and every crescent renders as a full disc. `--disc-scale` (default `3.0`) is an explicit readability cheat that does NOT change surface radiance (the denominator stays the physical solid angle) and does NOT touch the angular radii that drive eclipses; `--disc-scale=1` is physically true. Process lesson worth keeping: reasoning about the output value nearly found the white disc but blamed the wrong term — only dumping the FORMULA INPUTS through a debug mode settled it.
- PF07 celestial mechanics (2026-08-25): `PF07_party_environment` is the active playground and its first slice is CLOSED. New headless target `PF07_party_environment` (links only `devils_engine::utils`, `glm`, `tavl` — no Vulkan) owns `src/celestial.*` and `resources/celestial/system.tavl`. It evaluates a P-type binary analytically from Keplerian elements: `Aurin` (1.00 M☉ / 5772 K / 125 310 lx at 1 AU) and `Ember` (0.79 M☉ / 0.75 R☉ / 4800 K / 28 288 lx), binary `a=0.27 AU` period `38.30 d`, planet `a=1.1265 AU` year `326.41 d`, three moons (`8.796`/`31.352`/`3.373 d`). Star colour AND luminous efficacy come from one Planck × CIE-1931 integral (`92.1` vs `77.3 lm/W`), which is why the visible companion ratio is `0.226`, not the bolometric `0.269`. Positions are topocentric (moon parallax reaches two degrees), eclipses are analytic disk-overlap, and the same primitive evaluated from a moon's position produces lunar eclipses. `--report` prints ephemerides, `--events` a rise/set/eclipse calendar, `--verify` runs 23 numeric invariants and returns non-zero on failure. Measured: second-shadow contrast breathes `12.6%`–`27.3%` over the binary period; mutual stellar eclipses every `21.70 d` alternate between `Ember` covering `0.705–0.898` of `Aurin` and `Aurin` hiding `Ember` entirely for ~10 h; moon eclipse seasons emerge from the orbital inclinations (none before `d92`, dense from `d110` to `d191`); a total solar eclipse in daylight drops horizontal illuminance `74 803 → 18 241 lx` and leaves the world lit by one orange star rather than in twilight. One real bug was found and fixed by the continuity check: at FULL occlusion of one star the moon's lost-illumination fraction was recovered by dividing by `(1 - fraction)` and collapsed to zero, deleting the lunar eclipse exactly at its deepest phase; the check is a step-refinement test (a discontinuity keeps the same jump when the step is quartered) because a close moon legitimately changes eclipse depth at `0.15`/minute. Build and `-Wall -Wextra -Wpedantic` are clean.
- PF07 system survey (2026-08-25): added `src/survey.*` and `--survey` to answer structural questions about the configured system by brute-force sampling rather than on paper, plus `holman_wiegert_critical_ratio` in `celestial.*` and four more checks (27 total). Measured for the shipped config: seasons swing noon altitude `27°–70°`, daylight `9.5–15.1 h` and daily insolation threefold from the `21.5°` tilt at latitude `42°`, with the `38.30 d` binary cycle superimposed so a higher sun can still mean less light; within that cycle star separation runs `0.86°–14.28°`, the sunset gap `1–73 min`, hours lit by a single star `0.03–1.77`, and the companion share `12.4%–27.2%`; the binary turns `8.52` times per year so the pattern nearly repeats every two years. Stability ties two questions together: angular separation and a slow binary are the same requirement, so for these masses the floor is `4.15` revolutions per year and the ceiling `21.2°` of separation — four revolutions per year is exactly the destruction boundary and a binary period longer than the year is impossible for P-type. All three moons are visible in broad daylight (star above `30°`): `Selen` `1.07` and `Iskra` `2.02` of sky luminance against Earth's moon `0.3–0.7`. All five bodies are above the horizon `5.3%` of the time and gather within `60°/45°/30°/20°` once per `48/98/490/979` days. One moon covering BOTH stars at once is a once-a-decade graze with the shipped config; measured levers make it total and roughly annual — `Selen` at `92 000 km` gives one per `272 d`, a tighter binary `a_b=0.16 AU` one per `466 d` at the cost of shrinking separation to `8.6°`. Slice 2 (atmosphere/sky LUTs) is next.
- PF07 event budget (2026-08-25): the shipped `system.tavl` was RETUNED for a spectacle budget and the old numbers in earlier notes no longer apply. `--survey` gained an event-budget section counting only events VISIBLE above the horizon and converting to real hours at a configurable time compression (`--game-minutes`, default 1 game minute per real second; `--lunar-threshold`, `--star-threshold`, `--budget-days`). The design rule found: orbital INCLINATION is the frequency knob — near-coplanar orbits eclipse at every conjunction, inclination confines eclipses to node seasons. Baseline before tuning: any event once per 3 game days (1.1 real hours), dominated by `Iskra` lunar eclipses every 4 days. After: once per 36 game days (14.3 real hours), with lunar eclipse `74 d`, star-by-moon `163 d`, mutual stellar `172 d`, moon-over-moon `466 d`, tight parade `3264 d`, one moon over both suns never in 10 years. Config changes: planet orbital inclination `0.35° → 7.0°`, axial tilt `21.5° → 15.5°` (the two ADD, keeping effective obliquity ~22.5° and Earth-like `9.9–15.2 h` days), moons moved out and inclined — `Selen` `180→260k km`/`i=12°`, `Kolo` `420→430k km`/`i=12°`, `Iskra` `95→160k km`/`i=30°` — with radii rescaled to preserve disk sizes `0.90/0.30/0.35°`, so peak moonlight is unchanged at `0.63 lx`. Three rules worth keeping: a close-in moon CANNOT give rare lunar eclipses (too many full moons, too wide a shadow cone — `Iskra` at `95k km` would need ~50° inclination); moon-over-moon occultation needs a SMALL mutual inclination (`Selen`/`Kolo` nodes `17°` apart → mutual `3.5°` → once per `466 d`); and rarity costs depth — with `i=7°` most mutual stellar eclipses are now grazing (`0.22–0.51`), total ones a few per decade. New daylight showcase: day `915`, `Selen` totally covers `Aurin` at `29°` altitude while `Ember` stands beside it — companion share jumps `21.2% → 100%`, the world lit by one orange star at a quarter brightness. Verify still 27/27, build and strict warnings clean.
- PF07 small/large split + atlas alignment (2026-08-25): the config was retuned AGAIN and earlier PF07 numbers are superseded. Effects are now classified by the ONE quantity the player actually perceives — fraction of illuminance lost (daytime from stars, night from moons) — not by geometry kind: `small` = 2-15% loss, `large` = >=15% loss OR one star occulted >=90% (losing a shadow caster changes the frame more than its light share implies). `sky_state` gained `unocculted_star_illuminance_lx`/`unocculted_moon_illuminance_lx`; the moon path computes lit and unlit branches independently because at total lunar eclipse the actual value is zero and a ratio would lose exactly that case. New CLI: `--small-dimming`, `--large-dimming`. Result: LARGE once per 47 game days (18.9 real hours, in the author's 15-20 h target), SMALL once per 21 days (8.2 h); deepest daylight dimming 92.7%, night 100%. System realigned to `circumbinary_earth_atlas.md` (author-supplied) archetype "золотое + оранжевое, 0.30 AU": `Aurin` `1.05 M☉/1.04 R☉/5926 K`, `Ember` `0.79/0.76/4900 K`, binary `a=0.31 AU` period `46.48 d`, planet `a=1.2254 AU` — year is now exactly `365.26 d` at `1.000 S⊕`, since those two requirements force `L_total = M_total^(2/3)`. Planet inclination `7° → 5°`, stability margin `1.53x`. `Iskra` shrunk to `R=168 km` (disk `0.12°`, quarter of Earth's Moon): it covers at most `3.9%` of a star's disk over 1500 days, contributes nothing to the light budget, and exists purely as fast-moving texture at `30°` inclination. Cross-validation against the atlas: conjunction intervals match to hundredths (`12.82/25.02/25.90/28.19 d`). Correction TO the atlas: it uses bolometric `L~M⁴`, but luminous efficacy falls with temperature, so the cool companion's VISIBLE share is lower — gold+orange `0.197 → 0.156` (−21%), sun+red `0.032 → 0.013` (−59%), meaning the M-dwarf archetype gives a second-shadow contrast of `1.1%` and thus NO second shadow at all.
- PF06 closing / per-pass GPU timestamps (2026-08-25): `PF06_submarine_light_room` is CLOSED. It now attaches Painter's existing `gpu_timestamp_profiler` to its single graphics context after graph selection, consumes delayed results only after the corresponding frame-slot fence, accumulates average/minimum/maximum per graph pass, and reports on both bounded `--frames` runs and Escape. A representative 1280x720 exploration rail with flashlight, shadows, medium and helmet enabled collected 356 samples: window shadow `.330/.204/.703 ms`, flashlight shadow `.241/.164/.563`, lighting `2.366/1.502/5.817`, medium `6.013/3.709/14.932`, compose `.624/.418/1.517`, motes `.028/.018/.069`, helmet `.351/.215/.956`, empty UI `.002/.001/.005`, present `.130/.084/.321`; total GPU average/minimum `10.137/6.393 ms`. Wall time was pinned to `16.666 ms` by present/60 Hz, validating why GPU timestamps were required; minimum identifies medium as the real optimization candidate while both shadow maps total only `.368 ms`. Build, runtime report and Vulkan validation are clean. Dedicated animated-caster bias/aliasing and outdoor atmosphere remain future real-content fixtures rather than PF06 closing gates; the next bounded painter result is PF07.
- PF06 camera-centred shadow wall / evolving pressure fields (2026-08-25): arbitrary sparse ellipsoidal pockets were removed because they looked like objects without a clear artistic job. Volume shadow now implements a noisy visibility horizon: the default keeps a `4.5 m` camera-centred region readable, then every ray enters a `2.4 m` transition where density rises, source scattering falls and accumulated occupancy adds Beer–Lambert optical depth. A slow world-space signed field, the existing broad density noise and filaments only deform the boundary/rate; they never create standalone blobs or a screen-space vignette. Both pressure layers now evolve rather than merely translate: surface pressure cross-fades unrelated tangent-plane warps while slowly changing ridge scale/branch weight, and the volume uses independent multi-minute phases for boundary curvature and filament openings. Nearby props remain orientation anchors while the back room/corridor dissolve behind a continually changing nonuniform shadow wall. `Shadow wall density` (`--volume-shadow=0..2`, default `1`) and the new `Shadow wall radius` (`--shadow-radius=1..8`, default `4.5`) are separate runtime axes; this expands the lighting UBO to 256 bytes / 16 `v4`s and the UI to seven sliders. `medium_transmittance.a` carries the bounded visibility response into depth-aware compose. Fixed frame 180/motes off, wall on versus off gives `AE=10026.6`, `RMSE=1026.11`; safe and blackout remain bit-identical for strength `0→2` (`AE=0`). With a fixed camera, frame 180→900 gives `AE=929.661` for surface-only and `AE=2117.31` for volume-only, proving independent slow temporal evolution. Surface pressure projects one coherent world flow into the actual normal's tangent plane. The uncapped 1280×720 exploration rail remains around `6.1–6.5 ms / 154–163 FPS`. Build, runtime shader compilation, fixed dumps and Vulkan validation are clean.
- PF06 exploration-visibility response (2026-08-24): the intended exploration look is low perceived illumination with readable, continuously changing irritants, not near-black navigation. Physical room GI and medium density therefore no longer have to be raised merely to survive output mapping. A sixth UI/CLI axis, `Low-light visibility` / `--low-light-visibility=0..1` (default `.8`), applies observer-facing dark adaptation after the filmic curve: a square-root toe expands only weak existing display signal, retains exact black, avoids bright direct light, and is multiplied by powered-source presence plus inverse safe gate. It is perception/output response, not invented irradiance. `room_irradiance.w` now carries this response strength; surfaces use RGB directly so GI semantics are unchanged. At fixed frame 180 with defaults and motes off, surface-only versus both pressure layers off gives `AE=390.644`, volume-only `193.621`, and combined `504.068`; the pre-response combined result was only `68.6484`. With authored GI zero, combined pressure still differs by `AE=327.072`. Surface-only without medium differs by `787.795`, and its fixed-camera frame 180→360 change is `141.413`, proving a slow non-static visual landscape. Exploration response `0→1` is bit-identical in both blackout and safe (`AE=0`). UI layout, build, dumps and Vulkan validation are clean.
- PF06 layered indoor-pressure revision (2026-08-24): the volumetric-only experiment was visually strong but triple-coupled positive ridges to higher density, lower scattering and a second optical-depth term, producing almost black random clouds. Indoor baseline is now three deliberately separate layers. Exploration source presence guarantees a `.085` surface orientation floor even if authored bounce is zero; blackout still has zero source presence and remains black. A dominant-plane two-layer value-noise `Surface pressure` modulates only indirect, fades under direct light, grows toward the periphery and is capped at 30% darkening. Medium `Volume shadow` uses a separate cheap smooth signed field, not the ordinary broad/filament density noise; in `0..1` it changes density by at most ±15% and scattering by 8%, with default `.55` giving about ±8%/4.4%. The striking heavy optical-depth behavior is retained only through `smoothstep(1,2,strength)`, so the upper slider half is a special horror/illness range rather than room baseline. This revision added the fifth pressure axis and `--volume-shadow=`; the observer-response follow-up above adds the sixth. Before that response, both effects off versus default surface-only was `AE=33.2575`, volume-only `47.6405`, combined `68.6484`; heavy `2.0` gave `476.273`. Safe max versus off was `AE=0`. Build, fixed dumps and Vulkan validation are clean.
- PF06 volumetric shadow-pattern follow-up (2026-08-23): the pattern no longer multiplies surface indirect, which previously made lowering the GI slider erase the effect. `scene.frag` now contains only direct + room irradiance. The half-resolution medium reuses its existing broad-noise and anisotropic-filament samples to form ridges without extra value-noise fetches; ridges moderately raise density and suppress local in-scattering, while the nearest coherent ridge is retained as extra optical depth so twenty ray samples do not average the silhouette into uniform grey fog. Pattern strength remains independent of surface bounce, retains the peripheral/safe gates, and naturally vanishes when medium density is zero. At surface GI 0, pattern on/off still differs by `AE=837.933`; default GI gives `1265.27`; `--no-medium` and safe pattern on/off are both bit-identical (`AE=0`). This deliberately makes the atmosphere a separate low-light fill carrier; it is not full GI and does not illuminate surfaces behind opaque geometry.
- PF06 runtime tuning UI slice (2026-08-23): PF06 now owns a local interactive Lua/Nuklear panel using the existing single common Visage context/convert, not a second UI system. Four sliders round-trip canonical host values every frame: exploration-only room GI `0..1`, left situational source energy multiplier `0..3` (therefore affecting both its surface and volume contribution), medium density `0..0.8`, and shadow-pattern contrast `0..2`; reset restores `.23/1/.14/1.55`. The common overlay gained explicit pointer input plus small numeric/bool environment exchange while its old empty-input update remains unchanged for every other lab. `I` switches GLFW between captured camera and normal cursor with cursor rebasing; `U` and the in-window hide button remove the UI and recapture the mouse. Hidden frames explicitly write command count zero into every rotating `ui_commands` buffer; `--no-ui` starts there. `--left-source=` complements the existing deterministic CLI axes. Build, fixed dump and Vulkan validation are clean; default→high values changes `3467.01` pixel-equivalent outside UI. The later pressure/timestamp slices expanded the panel and completed PF06 closing.
- PF06 shadow/tonemap/helmet slice (2026-08-23): PF06 now renders two independent `1024²` reverse-Z maps before lighting: a camera-following flashlight deliberately offset `22 cm` right/`16 cm` down so its shadow does not hide exactly behind visible silhouettes, and a fixed side/window spotlight. The same instanced cube stream feeds both. Surfaces use hardware comparison `3×3` PCF plus world-texel normal offset; half-resolution medium tests both maps so shafts stop behind opaque casters, but reuses one compare for two adjacent midpoint samples and skips reads outside an inactive source/cone. That approximation changes the lower fixed crop by only `RMSE .025/255` relative to all-20 checks. `K`/`--no-shadows` is visibility A/B; production off should omit map passes through a graph generation. Compose now has fixed/constrained output controls (`T`, `--tonemap=aces|hable|reinhard`, contrast, saturation, black crush) and still never auto-lifts blackout. LDR output ping-pongs through `scene_ldr`: depth-tested motes blend there, then a separate helmet pass writes `final_color`, before UI. Helmet is a screen superellipse with edge-only refraction/tint, dark shell, inner rim, two restrained reflection arcs and edge-only dirt; center stays nearly unchanged and strength zero is exact passthrough (`H`, `--no-helmet`, `--helmet=`). Full fixed-lighting runtime remains validation-clean; a representative uncapped 1280×720 run with flashlight/shadows/helmet was `7.795 ms`, but wall-time present jitter means per-pass decisions still need GPU timestamps.
- PF06 GI/propagation/medium slice (2026-08-23): direct light and room GI now have deliberately different aggregation. Direct sources remain local/additive; room irradiance uses maximum smooth source presence, so one flashlight gives the whole volume a fixed minimum orientation level and additional lights cannot raise ambient energy. Normalized source-presence weights may still shift GI chroma. Exploration/safe bounce was reduced to `.23/.30`, leaving enough geometry for orientation but less certainty outside direct light. Indoor palette is intentionally dirty gray/brown: desaturated cold weak/work lights, warm tungsten safe light, neutral-cold flashlight and surface-reflected bounce rather than clean saturated cyan. Spatial reach uses quadratic ease-out `1-(1-t)^2`; flashlight front is `2.5 m` at frame 12 and `9.6 m` at 60. Source envelopes are asymmetric: flashlight on takes `1.8 s`, off only `.14 s`/9 fixed frames. The rejected pattern-level experiment posterized values without usefully changing its shape. Current continuous pattern combines two independently flowing narrow warped ridges into slowly splitting elongated strips; a screen-eccentricity gate scales it from 48% in the center to 108% at the edges, creating peripheral motion while retaining world-space attachment. Direct radiance still erases it smoothly. Medium moved from 20 full-resolution midpoint samples to a half-resolution scattering+transmittance MRT; full-resolution compose depth-aware reconstructs only those coefficients and retains exact surface color. Uncapped 1280×720/Intel measurements after warmup improved from `14.462 ms` to `7.840 ms`; isolated medium overhead fell by roughly 78%. A later depth-tested pass draws 1536 ~2 px room-bound motes: 58% dark and 42% varying cold gray-green→rare dirty-amber scattering so exploration remains legible against near-black, while powered-source/safe gates prevent emissive-looking dust in blackout. Indoor/outdoor remain planned as data profiles of the same technology; outdoor tuning waits for an exterior fixture. SS-decal pattern caching is not the primary fix; if desired use low-res cross-faded world/triplanar modulation textures rather than losing surface attachment. Validation is clean.
- PF06 first lighting-state baseline (2026-08-23): the new independent `PF06_submarine_light_room` executable established an 18-instance room/opening/corridor fixture and the initial runtime lighting block (expanded by the medium slice above). `L` cycles blackout/exploration/safe and `F` adds the camera cone; CLI exposes exact lighting mode, room bounce, exposure and low-light pattern parameters. Opaque fragments calculate weak/safe/flashlight direct light per pixel plus a project-owned room-local irradiance proxy whose energy comes only from powered sources, so fixed-exposure blackout is exactly black outside the separate overlay. HDR surface lighting is followed by a separate fixed-exposure/ACES-like compose pass, leaving a stable insertion point for medium and helmet. Two setup defects were caught: one-step graphics passes need attachment states before, during and after the subpass (omitting active made normal/depth read-only during writes and crashed Intel `vkCreateGraphicsPipeline`), and fullscreen-triangle UV already interpolate `0..2` vertices to visible `0..1`, so an extra `*0.5` sampled/stretched only one quarter of the camera image. Fixed-step/fixed-camera dumps are deterministic outside overlay, and Vulkan validation is clean. Current pattern/GI semantics and the completed medium baseline are recorded in the entry above.
- PF05 native-Nuklear world-UI closing slice (2026-08-23): small object panels use a separate C++ Nuklear context, never Lua. A deliberately bounded config supplies stable id, world anchor, name, health, up to 3 short colored label/value rows and 3 ordinary bindless image slots; shared style fixes the base window at `196×92 px`, Crimson at `18 px`, anchor gap and limits. All N windows go through ONE `nk_convert`. Standard 20-byte Nuklear vertices gain a `uint window_id` derived from command userdata; raw matrix pointers do not cross convert/upload. A 48-byte/window SSBO maps the dense GPU id to `{anchor,fade-end}`, `{pixel offset,size}` and a distance policy. Scale is `clamp(reference_distance/view_depth,min,max)` (`5.5 m`, `.45..1.35` default) and alpha fades `13..16 m`, combining spherical/world-size approach with a readable screen-size clamp while preserving anchor reverse-Z. CPU repeats the exact projection/scale for hit rectangles and nearest-view-depth overlap choice; `I` switches GLFW to normal cursor, click selects the stable string id, empty click clears, and selected panel gets a mild cyan highlight. Fixture ids map back to two box instances and the sphere, where a material bit highlights the selected scene object too. Cursor rebasing prevents a synthetic camera jump on recapture. One Nuklear trap remains documented: automatic background is emitted before new userdata, so it is transparent and the visible panel is emitted after `nk_begin`. Fragment solid/MSDF/image discards zero alpha before `greater_or_equal` depth write. `U`/off differs by `3954.75`; selected sentinel changes `2481.53` pixel-equivalent in the combined panel/object crop; behind-wall rail still matches off (`AE=0`); full validation is clean. CPU rectangles do not know scene occlusion: exact invisible-window rejection needs a shared GPU ID/readback or scene ray query. Promotion candidates after a second consumer are pure projection policy, generic userdata-preserving native Nuklear conversion, font face/atlas split out of Visage, and renderer-wide picking; health/status schema, colors and thresholds remain project-owned.
- PF05 cel-shading slice (2026-08-22): cel lighting and outline are independent runtime policies in a project-owned 48-byte UBO, not pipeline variants. Opaque directional Lambert optionally quantizes N.L to 2..8 evenly spaced levels, then applies ambient; a cell-relative smoothstep width (default `.08`) avoids forcing infinitely hard shimmering boundaries. A dedicated smooth UV sphere makes multiple bands observable where boxes cannot. `G` toggles lighting, `B` cycles 2..5 bands, `O` cycles outline `off→silhouette→feature`; CLI exposes exact bands/softness/policy. The fullscreen outline consumes visible opaque depth+normal before decals/particles/text: silhouette uses geometry/background plus relative linear-depth discontinuity, feature also uses normal creases, and neither is a PF04-style through-wall selector. Runtime off currently discards in the retained pass; a production graph generation should omit it. Outside overlay, smooth→2-band `AE=17417`, 2→5 `20338`, silhouette→feature `493.542`, repeated feature `AE=0`; build and Vulkan validation are clean.
- PF05 current-depth collision and weather slice (2026-08-22): particle compute moved after the opaque scene so every slot can use CURRENT `scene_depth + scene_normal`, avoiding previous-frame camera lag. Collision projects the candidate position, reconstructs the nearest world hit, and accepts a bounded previous-camera-side→candidate-surface-side crossing only when velocity enters the receiver normal. Sparks reflect with restitution; weather particles terminate and respawn. This remains honestly screen-space: only the first visible surface exists, offscreen/occluded geometry is absent, and one candidate sample plus thickness is not arbitrary-speed CCD. A separate explicit shelter AABB fills that semantic gap for precipitation: weather inside the volume is killed before rendering, including scattered reset spawns, so an offscreen roof need not be inferred from the G-buffer. The room fixture is a roofed dry volume with an open entrance; `H`/`--no-weather-shelter` is the A/B. With depth collision disabled in both frames, shelter ON/OFF still differs by `415.191` pixel-equivalent in the entrance/room crop; repeated shelter output is deterministic (`AE=0`) outside overlay. The pool is 3072 stable slots: the original 2048 spark partition plus 1024 camera-local weather slots. `T` cycles rain→snow→clear; rain is fast wind-driven blue streaks whose long axis is locked to world velocity while only their millimetre-scale width rotates around that axis toward the camera (a cylindrical ribbon), so camera pitch produces physical foreshortening instead of screen-up streaks. Snow is slow soft flakes with time-varying lateral drift. Weather uses ordinary unsorted alpha, sparks remain order-independent additive, all test opaque reverse-Z and write no depth. Full rain still needs separate mid/far atmospheric density, wet surfaces and impacts outside this particle slice. `C`/`--no-particle-collision` is the depth A/B; fixed rain frame 180 differs by `2510.13` pixel-equivalent, rain/snow by `5648.02`, and repeated rain is deterministic (`AE=0`) outside overlay. Dense snow exposed ordering ambiguity with the diagnostic overlay, so UI is now a guaranteed separate final pass. Vulkan validation is clean.
- PF05 GPU particles/emitter lifecycle slice (2026-08-22): the first particle slice established a 2048-slot persistent spark partition with no CPU state round-trip. A compute pass writes the current per-frame `particle_state` while reading the same resource at `history=1`; one invocation owns one stable slot, so bounded spawn/update needs no freelist atomics. CPU emitter state sends only parameters and a contiguous `{first_serial,count}` batch; serial modulo capacity chooses the slot and seeds deterministic cone velocity/lifetime. Lifecycle is explicit: `P` changes emitting→draining and restarts emission, maximum lifetime turns draining into guaranteed stopped, and `R` sends a one-frame history reset and restarts. Semi-implicit Euler applies gravity plus exponential drag, then analytic room planes clamp/reflect position and velocity with restitution. The spark baseline remains a fixed 2048-instance procedural six-vertex spherical billboard draw; dead slots leave clip space. Soft emissive sparks use order-independent additive blending, test opaque reverse-Z depth and do not write it. This deliberately avoids hiding alpha-sort/OIT and compaction problems: smoke needs an explicit transparency policy, arbitrary world collision still needs colliders/SDF/BVH, and compaction+indirect count waits for measurements/larger pools. `--fixed-step --weather=clear --emitter-stop-frame=60` is the deterministic lifecycle rail: frame 180 still differs from an empty pool, frame 360 after max lifetime is identical outside overlay (`AE=0`); repeated emitting frame-180 crops are also identical (`AE=0`). `--no-particles` is only a runtime debug switch; production off should remove both graph steps. Build and Vulkan validation are clean.
- PF05 screen-space decal and text-depth slice (2026-08-22): PF05 now has a genuine deferred decal pass rather than coplanar text. The opaque scene writes full-resolution reverse-Z depth and world normal; each decal glyph rasterizes the back faces of an oriented unit box, reconstructs the selected scene surface through inverse VP, clips it with a precomputed `world_to_decal`, gates adjacent faces at normal cosine `0.55`, maps local XY into the shared Crimson atlas and alpha-blends only scene color. Back and right wall fixtures prove two orientations; `F`/`--no-decals` provides A/B. This is intentionally an opaque-receiver/color-only slice: transparent receivers, normal/roughness writes and camera-inside-volume culling remain separate policies. The graph explicitly transitions depth from the decal pass's read-only layout back to attachment layout. World and billboard MSDF materials now write reverse-Z depth so labels occlude one another and disappear behind scene geometry; near-zero MSDF coverage is discarded so transparent quad area never becomes an invisible depth occluder. Semi-transparent label intersections still need a later sorted-alpha/OIT policy. Build, fixed-camera dump and Vulkan validation are clean.
- PF05 first world-MSDF/billboard slice (2026-08-22): the new independent `PF05_scene_effects` executable reuses the exact Crimson MTSDF atlas and `font_t` metrics already owned by the common Visage overlay; `font_metrics()` exposes them read-only, avoiding a second atlas or a Nuklear-dependent world vertex stream. One instanced unit-quad renderer carries per-glyph world matrix, atlas rect, fill/outline colours and UI-compatible boldness/outline/softness. Fixed `font_height + max_length` accepts only whole glyph advances inside the segment and clips the suffix; `max_length` alone derives `font_height = length/natural_width`, with a startup invariant that the result consumes the requested length. A 64-segment arc-length LUT maps pen distance to quadratic Bézier `t`, giving every glyph an individual tangent frame; quadratic/cubic derivative functions were promoted to CPU/GLSL `utils/shared.h`. Billboard placement now has three explicit modes: spherical camera right/up in world units; cylindrical/Y-locked with only the horizontal axis facing the camera; and world-anchored screen-size, which projects the anchor then adds pixel offsets multiplied by clip W, retaining anchor reverse-Z depth. A weathered-stone asset texture can modulate glyph fill without changing MSDF coverage/outline; atlas/detail slots plus quantized mix share one packed 32-bit instance word, so the feature adds no glyph bytes. Production direction is NOT raw runes in vertex shaders: CPU language shaping must output glyph ids/advances, a static GPU font table stores metrics, a ~16-byte shaped-glyph stream stores id/pen/line/flags, and vertex code performs curve/billboard placement. Pure CPU/GPU math/packing belongs in `utils/shared.h`; font face/metrics/atlas should eventually split from the Nuklear-bearing `visage::font_t`, while descriptor/pipeline contracts remain Painter-owned. The initial coplanar wall fixture was deliberately not called a decal and has since been replaced by the genuine depth-reconstructed slice above. The same atlas simultaneously renders the Visage diagnostic overlay. PF03/PF04-style fixed-camera dump and Vulkan validation are clean.
- PF04 depth-aware screen-space outline and separate overlay selector (2026-08-22, `RND-23`): the geometry shell is gone, but constant pixel width alone was not enough — a plain post-effect drew above every object. The mask stores `{coverage, reverse-Z device depth}` in one four-byte-per-pixel `sf2`; no separate selection depth texture is needed. The mask draw has depth disabled and uses fixed-function `VK_BLEND_OP_MAX` on both channels: coverage remains one and, under reverse-Z, maximum depth is the nearest rasterized selected-object surface independent of triangle order. The MAX result is bit-identical to the earlier D32-assisted fixed-view frame. A round three-pixel dilation propagates the depth of the nearest covered texel, writes it through `gl_FragDepth`, and the base orange pipeline uses reverse-Z `greater_or_equal` plus depth write. Foreground geometry therefore occludes the outline like it occludes the selected object, and the later portal respects that propagated boundary. The old always-on-top semantics remain honestly separate: `O`/`--outline-overlay` enables a yellow variant after the portal and all green/red wallhack draws with depth disabled, suitable as a debug or second wallhack selector. A fixed-view brown occluder makes the A/B explicit: orange disappears behind it, yellow reappears through it and is not overwritten by the green target. The direct dilation costs 37 taps; many outlined objects would want separable dilation or a distance transform. Vulkan validation is clean.
- PF04 closing dynamic/front-back slice (2026-08-22, `RND-23`): the stencil laboratory is CLOSED. Painter materials still author `dynamic = [stencil_reference, stencil_compare_mask, stencil_write_mask]`, but `material` no longer grows one bool per state: it owns `std::array<uint32_t,16> dynamic`, initialized to `UINT32_MAX`, and every occupied value is a Vulkan dynamic-state enum. `DEVILS_ENGINE_PAINTER_DYNAMIC_STATE_LIST` in `common.h` is the single X-macro mapping for the four human names and `VK_DYNAMIC_STATE_*`; it generates the dense `dynamic_state::values`, string conversion and `to_vulkan()` mapping in `common.cpp`, including compile-time assertions. Parsing rejects duplicates and overflow, while pipeline/runtime paths query the stored Vulkan values. The selection writer and stencil debug share one runtime `{reference,compare_mask,write_mask}` constant; `R/C/X` change it without pipeline/graph rebuild, while the screen-space outline intentionally reads its independent coverage+depth semantic mask. The final asymmetric fixture submits two quads with opposite framebuffer winding in one draw: front `replace` writes `0x10` and appears green, back `invert` produces `0x30` and appears yellow; mask `0x30` preserves the existing `0x01/0x02/0x04` effects. `F`/`--no-face-fixture` removes both regions without residue. Measured earlier: moving selection `0x01→0x08` is bit-identical outside overlay (`AE=0`); write mask 0 removes debug tint; compare mask 0 gives fullscreen `equal`. Parser coverage is 14 cases/157 assertions; final dumps are Vulkan-validation clean. PF05 scene effects is the next laboratory.
- PF04 spatial-window camera correction (2026-08-22, `RND-23`): a world-space aperture passes main depth and writes only bit `0x04`; a restricted fullscreen fragment clears color plus reverse-Z depth inside it, and remote geometry redraws there with coherent private depth. The first version cropped a FIXED alternate camera's fullscreen image, so main-camera rotation moved the aperture in one clip space while content remained in another and visibly slid under the frame. `spatial_window_link` now owns explicit source/destination plane transforms, aperture half-extent and clip offset; the source draw consumes the same full matrix. The rigid transition is `M=T_destination*Ry(pi)*inverse(T_source)` and every frame uses `V_remote=V_main*inverse(M)`, hence `P*V_remote*M*X=P*V_main*X`: rotation is applied once and translation produces physical parallax. The destination +Z plane is sent to the remote camera block and `scene.vert` writes `gl_ClipDistance`, so geometry on the camera side of the destination is removed; main view receives an always-positive plane. Symmetric projection remains correct for direct-to-framebuffer stencil rendering; shader clip distance was chosen over oblique near-plane projection, which is only a possible early-clipping optimization here. Destination frame geometry and recursion remain deferred. `P`/`--no-window`, PF03-style frame-exact dumps, cursor release under `--fixed-camera`, and Vulkan validation remain clean.
- PF04 independent stencil-bit slice (2026-08-22, `RND-23`): selection now owns only bit `0x01`; an invisible world-space rectangle passes reversed-Z depth, writes only bit `0x02` with color writes disabled, and a later fullscreen pass tests only `0x02` to alpha-blend a cyan local effect. Pixels in the overlap hold `3`, while outline/debug still test only bit 0, proving that concurrent effects preserve and ignore unrelated bits. `L`/`--no-local-effect` controls the consumer; `V` remains the bit-0 debug view. This exposed two Painter blend-parser defects: an authored color mask was OR-ed into default RGBA and therefore never narrowed writes, and a mask-only declaration copied absent-expression sentinels into Vulkan blend enums. `mask = none` and partial `rgba` masks now replace the default, omitted factors retain valid defaults, and `painter_shader_prepare_test` covers both (13 cases/147 assertions). Fixed-camera visual inspection and an eight-second run with both masks plus Vulkan validation are clean. The local effect is deliberately blend-only because sampling and writing the same color attachment would be feedback; sampled desaturation would require an input attachment/subpass or ping-pong image. This is the independent-bit baseline extended by the spatial window above.
- PF04 first stencil slice (2026-08-22, `RND-23`): the new independent executable uses one per-frame `D24S8` attachment. Ordinary geometry draws first; the selected blue cube passes reversed-Z depth and replaces stencil with reference 1; an expanded-normal back-face shell tests `stencil != 1` and leaves an orange visible-silhouette outline while retaining depth occlusion; a fullscreen triangle tests `stencil == 1` and applies an optional magenta debug tint without sampling the stencil image. `--fixed-camera --stencil-debug` provides the deterministic inspection view; `V` toggles it interactively. Visual inspection matches the selected geometry and the first validation run is clean. Static front/back fail/pass/depth-fail ops, compare/read/write masks and reference flow from TAVL into `VkStencilOpState`. This is the outline/debug baseline extended by the later slices above.
- Independent render-settings direction after PF03 (2026-08-21, `RND-31`): production selects a renderer/graph recipe and technology set FIRST; that recipe exposes project-defined independent axes such as shadows/AO/reflections/DoF. A global performance/balanced/quality preset is only a macro that writes several choices; the concrete variant is the canonical resolver output, not a complete authored profile. Externally there are two costs: project-owned UBO/SSBO runtime data next frame, or a full graph-generation build for specialization, defines, pipeline state, resource shape or topology. One axis may contain both: DoF gameplay/cinematic can be runtime lens data, while DoF off/on builds a generation without/with the chain. MVP: `$fixed_declared_value[.component]` references in step `shader_constants` and material `definitions`; typed partial `values/materials/steps` patches per choice; graph-specific choices/defaults/preset; ownership/conflict checks; full validation before transactional commit. Optional patch structs are required because current mirror defaults cannot distinguish absent from explicit false/0. Structural follow-up: constrained named graph slots/fragments with explicit resource contracts, avoiding both arbitrary pass deletion and hand-authored 2^N graphs. Design: `PF03_post_processing/RENDER_PROFILES.md`.
- PF03 native-TAA moving-edge retune (2026-08-21): the author's visible staircase on the far wall under the slightest camera turn is primarily the single-history algorithmic tradeoff, but the defaults amplified it. The targeted rail freezes objects, moves the camera at `0.2 rad/s`, and compares frame 48 against 32-frame accumulation at the EXACT same frozen viewpoint, so lag/blur cannot score as AA. RMSE in 0..255 units: no TAA `4.57`, old minmax/weight `.92`/sharpen `.35` `4.10`, variance/.92/no sharpen `3.62`, selected variance/.88/no sharpen `3.41`; frame-to-frame RMSE at weights `.88/.90/.92` is essentially flat (`5.72/5.70/5.70`). Defaults are therefore variance clamp, history `.88`, sharpen `0`; sharpen remains an opt-in taste control. A geometry-history experiment was rejected rather than shipped: previous jitter-corrected depth plus normal, with the current world point projected by `previous_view_projection`, can identify disocclusion but cannot identify subpixel COVERAGE of the silhouette; even the strict version left multipixel trails and worsened RMSE to `5.14`. Full cure still requires richer history/coverage or more current-frame samples, not merely looser rejection.
- PF03 TAAU continuous coverage follow-up (2026-08-21): binary coverage was numerically sharp on a static camera but visibly dirty under the slightest turn, because a display pixel switched between a full real sample and history-only weight 1. TAAU now uses a tent footprint one render texel wide and stores accumulated EFFECTIVE sample weight; blending is `n/(n+w)`, with a jitter-aligned Catmull-Rom spatial estimate between sample centers. At scale 0.5 static RMSE versus native TAA improved `3.76 → 2.98/255`. On deterministic `0.2 rad/s` camera motion, frame-to-frame RMSE fell `6.65 → 5.22` (22%) while average error to native TAA at the same two viewpoints fell `5.29 → 4.02` (24%), so the smoothness is not merely lag/blur. Full-resolution TAA and zero-weight passthrough are unchanged.
- PF03 closing shader audit and TAAU (2026-08-21, `RND-46`): TAAU is now CLOSED with a positive reconstruction result. The hidden root defect was coordinate-space, not filtering: `gbuffer.frag` divided low-resolution `gl_FragCoord` by DISPLAY extent, so at scale 0.5 current UV covered `0..0.5` while previous UV covered `0..1`; render extent now travels explicitly in `blur_params.yz`. Resolve again jitters in RENDER pixels and stores temporal state in separate full-resolution `taa_meta` (`.r` accepted source-sample count, `.g` rejection), while `taa_color.a` has one meaning again — fog transmittance. A source sample is accumulated only into the display pixel containing its measured `center - jitter` position; Catmull-Rom is only the spatial fallback for uncovered pixels. Count drives `n/(n+1)` until the configured fixed-weight equivalent, preventing the old 0.99 non-convergence; disocclusion reduces count. Measured at scale 0.5, static camera, fixed exposure, 32 frames with bloom/shafts disabled: simple upscale `7.40/255` RMSE against native TAA, wrong coverage sign `6.87`, final TAAU `3.76`. Other audit fixes: fractional-scale motion tiles cover remainder rows/columns; bloom 13-tap weights now sum to one and use the intended outer/inner sample pattern; DoF neighbourhood CoC reads the coarsest mip rather than implicit level 0; hierarchical SSR avoids `0/0` on axis-aligned rays; `--taa=0` and `--taa-weight=0` now bypass coverage identically and emit bit-identical frames; dead/over-wide descriptors were removed. Every PF03 shader now starts with a human algorithm summary. Render-settings design is documented in `PF03_post_processing/RENDER_PROFILES.md`: graph-specific independent choices resolve declared-value references, named build patches and later constrained topology fragments, while a global preset is only a convenience macro. Externally there are only two change costs — next-frame runtime data or a full graph-generation build.
- Active development campaign (updated 2026-08-25): work is selected by one bounded playground/project result rather than by walking small engine backlog items bottom-up. The Painter capability galleries `PF01_forward_plus` through `PF05_scene_effects` and the first project-look scene `PF06_submarine_light_room` are CLOSED. PF06 proved one coherent cramped underwater room where lighting state controls visibility/safety, local irradiance, shadowing, suspended particles, volumetric shafts, low-light pressure fields, exposure and helmet presentation without linking earlier playground targets. Active work can now move to `PF07_party_environment`. `ROADMAP.md` is a dependency catalog, `PLAYGROUNDS.md` owns active sequence/DoD, and `ROADMAP_ULT.md` remains the long horizon.
- Playground layout (2026-08-22): every lab lives in `subprojects/playgrounds/<CODE>_<human_name>/` with its own README/executable/resources; `common/` receives only proven repeated shell code. Labs never link to another lab target. Later labs may selectively copy/freeze an earlier baseline or consume promoted `libs/painter`/`common` contracts, so continued gallery experimentation cannot silently change project scenes. Painter sequence is `PF01` Forward+ → `PF02` shadows → `PF03` post processing → `PF04` stencil effects → `PF05` scene effects (3D SDF, screen-space decals, particles/weather, cel shading, billboards, world-space UI) → `PF06` submarine light room → `PF07` party environment.
- PF01 first live slice (2026-08-15): `PF01_forward_plus` renders the inside of a cube with a checker wall, free WASD/QE/mouse camera and 96 visible-marker point lights in an animated 8×4×3 volume grid. Zero ambient and hard radius cutoff keep illumination attributable to local lights. Its TAVL graph is depth prepass → depth-aware 16x16 tile compute assignment (capacity 96 lights/tile) → HDR Blinn-Phong Forward+ → Visage overlay → present. One 8×8 compute workgroup handles each tile; positive-float atomic depth reduction, stable light-id packing, conservative camera/near-plane sphere handling and direct tests against four view-space tile-frustum planes replace the false-negative-prone projected-bounds path. The frustum scale comes from the pure projection matrix; using the diagonal of `projection * view` previously made 16-pixel cutoff bands depend on yaw/pitch. Capacity covers every current lab light until explicit overflow diagnostics exist. Dispatch follows the actual viewport within the configured 120×68 capacity. PF01 uses mailbox-first presentation plus a separate common absolute-deadline/`sleep_until` 60 FPS limiter; `--uncapped` disables only pacing. The main runtime already follows this separation through per-worker `simul::advancer`; `RND-24` retains overrun-resync, actual-present-mode metrics and explicit MAILBOX/FIFO/IMMEDIATE fallback policy. `RND-25` is complete: named descriptor `sets` contribute layouts to step barriers/read-write masks with dedup/conflict checks, while pass/subpass attachment transitions remain explicit; PF01 uses separate read/write SSBO descriptor views and no repeated manual barriers. `painter_shader_prepare_test` covers inference. Playground common now owns a non-interactive Visage Lua/Nuklear overlay, shared Crimson MSDF font atlas and smoothed FPS/frame time, while PF01 sends its POD output through ordinary config-defined `draw_ui` resources. `painter` also owns compute-pipeline creation, material shader definitions with stage+definition SPIR-V variants, standalone shader filesystem roots, pre-render-pass graphics barriers, color-only blend attachment counting, initialized material stencil data and correct per-frame image-view naming. Parsed config constants are active immediately; runtime writes remain staged until `update_event()`, preventing silent `dispatch(0,0,0)` and covered by `painter_shader_prepare_test`. Next PF01 slice is naive-forward A/B plus heatmap/overflow, repeatable rail and timings.
- PF02 live shadow slice (2026-08-15): independent `PF02_shadows` owns a config-driven `2048² reverse-Z 2×2 directional CSM atlas + 2048² 2×2 spot atlas → camera depth → half-res contact compute → shadowed forward → debug/present` graph. An open floor/wall stage, five cube instances, a sloped receiver and thin contact caster expose acne/peter-panning; four colored spot lights occupy fixed 1024² regions and one moves. Generic Painter `draw_regions gpu_data host_commands` drives both atlases. Its versioned host stream stores viewport/scissor, dynamic depth bias, GPU `data_index` and `{pair,first_instance,count}` spans; main owns cone/range culling and packed instance lanes while the graph remains layout/resource manager. Directional CSM uses four practical splits (lambda .68, far 40), rotation-independent spheres, light-space texel snapping and 12% blend bands; runtime tint plus full atlas depth expose selection. Raw Vulkan raster bias remains adjustable, but receiver normal offset is now derived from the world size of one shadow texel per cascade/spot depth and receiver-plane derivatives correct every filter tap. Edge AA independently selects hard/3×3 PCF/rotated-Poisson plus radius; spot PCSS separately uses a world-unit emitter radius. A depth prepass feeds an 8-step half-resolution screen-space pass producing one directional and four spot contact channels. Signed receiver-plane, N·L/cone/range gates and conservative depth-discontinuity rejection remove invalid contributions, but a single camera depth layer cannot reconstruct a blocker behind its visible silhouette; contact is therefore opt-in (`F`/`--contact`) and disabled by default. Painter gained the generic image role `shadow_mask`. HZB/history/temporal reconstruction belongs to PF03. Common Visage publishes cascades/splits/tint, modes, bias, occupancy and six pass timings. Validation runs are clean. Next quality slice is repeatable camera-rail stabilization/world-bias validation plus directional caster culling; atlas lifetime follows measurements.
- PF02 technique boundary and two painter primitives (2026-08-17): the lab explicitly targets *smoothed* (edge-AA) shadows, not physically soft ones — temporal accumulation, stochastic sampling, HZB, full PCSS and area lights belong to `PF03`/a later PBR slice. Plausibility comes from correct bias, non-black shadow interiors and contact darkening; distance-dependent softening is allowed only as a single-center-tap penumbra estimate. Contact masks must combine with `min` rather than multiplication and be upsampled depth-aware. Two engine primitives shipped for this: `painter::sampler` accepts `compare = <compare_op>`, turning it into a comparison sampler for hardware `samplerXDShadow` PCF (reverse-Z needs `greater_or_equal`), and a step accepts `shader_constants = [ name = "value" ]` specialization constants. `constant_id`, type and size come from SPIR-V reflection, so config values are never type-guessed; `id_<N>` addresses constants that carry no `OpName`; because `spirv-opt` strips `OpName`, the name→id map is taken from a separate debug-info compile of the same source while the pipeline still uses the optimized module; an unknown name is a loud error listing the available constants. GOTCHA: a derived `const int x = <spec constant>` takes over the `SpecId` name, so reference specialization constants directly. PF02 quality tiers now live there (`pcf_radius`, `contact_ray_steps`, `contact_refine_steps`) instead of material defines; `painter_shader_prepare_test` covers reflection, blob typing/ordering, name merge and config parsing.
- PF02 smoothed-shadow slice (2026-08-17): shadow atlases are now sampled through a comparison sampler (`shadow_compare`, `greater_or_equal` under reverse-Z), so one `sampler2DShadow` tap returns the bilinear fraction of passing texels and a separable-tent-weighted `3×3` reaches a `6×6` footprint; rotated Poisson uses the same compare taps. The same atlases stay bound through an ordinary nearest sampler in the same set because hard-mode A/B and the PCSS blocker search need raw stored depth. Receiver-plane correction survives because the per-tap reference depth is what the compare receives. Contact masks now bound the map result with `min` instead of multiplying it, and `contact_directional` became `sf2` (`.r` mask, `.g` source linear view depth, `.g == 0` = no geometry) so the forward pass picks the nearest-depth half-res texel instead of bilinear-bleeding across silhouettes. Contact strength fades along three axes: ray length, camera depth (`8..18 m`, beyond which no rays are traced at all) and hit proximity to the frame border. `scene_buffer` gained a `contact_params` vec4 (fade start/end, edge-fade width), so every shader declaring the block carries it. Playground common overlay cap rose from 10 to 12 detail lines (C++ and `lab_overlay.lua` both). Every preset (`hard/pcf/poisson/pcss/contact/spot-only/cascade-debug/zero-bias`) runs validation-clean.
- PF02 near-cascade density tuning (2026-08-17): the world size of one cascade-0 texel is the single multiplier behind three separate visual complaints, because both the filter tap spacing and the normal offset are expressed in texels. At `lambda = 0.68` cascade 0 reached `3.53 m` and measured `10.1 mm/texel`, which produced ~5-8 px edge stairs, a ~40 mm filter footprint (read as heavy blur) and up to ~5 mm shadow detachment at the object base. `lambda = 0.88` puts cascade 0 at `0.1..1.60 m` and `4.64 mm/texel`; cascades 1/2 also get denser (`11.5`/`31.7` mm) while the far cascade is unchanged (`≈111 mm` either way), so the redistribution costs nothing at distance. Bias/filter defaults were trimmed on top: raster constant/slope `-1.25/-1.75 → -0.60/-1.00`, normal base/slope `0.20/1.10 → 0.15/0.85`, AA spacing `1.0 → 0.85` texels — hardware compare filters the transition itself and receiver-plane derivatives fix each tap, so the old margins were paying for a problem that no longer exists. Net: footprint `40.5 → 16.5 mm`, normal offset at `N·L=0.7` `5.4 → 1.9 mm`. The overlay now prints per-cascade texel size and the resulting filter width in millimetres, and reads `pcf_radius` back from the step's `shader_constants` so the diagnostic cannot drift from the shader. Residual stepwise edge motion is expected: snapping keeps the map grid world-stationary (static scenes stay stable under camera motion), so what steps is a *moving* caster's silhouette crossing texel boundaries; a full cure needs temporal accumulation (`PF03`).
- PF02 closing slice (2026-08-18): directional shadows now fade into ambient over the last `--shadow-fade` fraction of the final cascade (default `0.18`), because beyond it there are no shadows at all and a hard boundary reads as a cut — the far cascade is coarse, so losing it gradually is cheaper than pushing quality there. Measured to touch only the far band (max difference 120/255 there, exactly zero elsewhere). PF02's README was rewritten from a working log into a subproject description: the reasoning behind each chosen technique is kept in detail, while the next-slice queue and the tried/rejected A/B history were dropped (they live here and in memory). The lab is considered done; the perceptual gap to the reference look (Skyrim 2011, now an explicit target for a later lab) is not shadows but AO, fog/aerial perspective, exposure/tonemapping and material detail, so `PF03` takes over.
- Painter mip chains (2026-08-19, `RND-38`): a resource declares how many levels it HAS (`mips = 4`, or `mips = auto` for a full chain to 1×1 computed from the level-0 extent and recomputed on resize) because that determines allocation; a binding declares WHICH level it touches (`mip = k`, absent = the whole chain). The split is dictated by Vulkan rather than taste: `imageLoad`/`imageStore` have no LOD parameter, so a storage-image view spans exactly one level — therefore `texel_write`, `texel_read`, `general` and attachments MUST name a level (loud error otherwise), while `sampled` without a level yields a whole-chain view the shader reads through `textureLod`. Implementation: per copy there is one chain view plus one view per level; layout is tracked PER LEVEL (`resource_inst::usage_levels`), because "level k in sampled while k+1 is being written" cannot be expressed by a single state per resource; the step's usage-conflict check is keyed by (resource, level) for the same reason; barriers are derived from bindings (`mip = k` → that level, no mip → all levels) and consecutive levels in the same state are coalesced into one barrier, so a mip-less image costs exactly what it cost before. A SILENT TRAP was fixed on the way: `create_samplers` never set a LOD range, and with `maxLod = 0` sampling any level above zero silently returns level 0 — the chain would look created and be unusable; `mipmap = nearest|linear` is now parsed and the upper LOD bound is lifted. Verification: PF03's bloom pyramid moved from four separate resources to one with `mips = 4` and the frame is BIT-IDENTICAL (`AE = 0`, `RMSE = 0`), at three fewer resource declarations and two fewer `declare_values`. Not supported yet: rendering into an arbitrary level (a render target cannot name a level — it fails loudly instead of silently writing level 0) and blit-based chain generation.
- PF03 bloom and light shafts (2026-08-19, `RND-21` in part, `RND-37`): one shared pyramid as planned — four downsamples (`1/2 … 1/16`), three additive upsamples, and the shafts take their mask from a quarter level of the same pyramid, differing only in the filter (radial instead of isotropic). Both are added in LINEAR HDR before exposure, because they are light that went where it should not have (scattering in optics and atmosphere), not an effect over a finished image. Filters are chosen deliberately: the 13-tap Jimenez downsample, because a plain box passes frequencies above the new Nyquist at every level and the pyramid then pulses under motion; a 3×3 tent upsample, otherwise each level shows the lower resolution's squares; and a soft-knee threshold, because hard cutoff gives a boundary where the glow switches on abruptly. THE THRESHOLD MUST BE IN POST-EXPOSURE UNITS: the first version thresholded linear values and bloom captured the WHOLE scene, since in an HDR frame the sky is `19` and lit surfaces `3.6`, so almost everything clears any absolute bar — the debug view showed a blurred copy of the frame with buffer mean `0.536`. "Bright" only means anything relative to what counts as middle grey, so the threshold now applies to luminance times exposure while the colour stays linear, which forced the exposure pass to move BEFORE the pyramid; afterwards the buffer holds only the source (mean `0.021`), and a threshold above the source kills the effect entirely (`--bloom=0` and threshold `100` give identical values). Glow profile along the wall away from the panel decays and is controlled by the upsample weight: `9.3 → 3.6` at spread `0.4` versus `70.0 → 20.4` at `0.95`. Screen-space shafts do exactly what the method allows: the sun must be on screen (behind the camera the buffer is exactly zero, `mean = 0`), off-screen occluders are invisible to it, and — learned in practice — THE SCENE NEEDS GAPS IN THE MASK, since radially blurring a solid sky produces a wash rather than shafts, so five pillars were added near the far wall and only between them did the rays become visible and measurable; default intensity dropped to `0.08` because it is a scattered-light fraction, not a brightness (at `0.8` it swamped the frame). ENGINE FINDINGS: (1) `RND-38` — render-graph resources are single-mip (`res_mips = 1` hardwired), so the pyramid is built from SEPARATE resources per level sized via `declare_values` + `scale`; it works and reads well, but a real mip chain would be cheaper in memory, descriptors and declarations, and every pyramid effect will want it; (2) `RND-39` fixed — `usage = general` on an image is now an honest read-write storage image (its layout, image-usage flag and access mask were already right, only the descriptor type came out buffer-shaped), which is exactly what the additive upsample needs and removes the need for ping-pong resources per level; (3) `submit` caps at sixteen groups while the chain already has twelve passes, so downsample and upsample are MULTI-STEP passes — inter-step barriers come from the steps' descriptors and no group budget is spent; (4) Vulkan requires the layout to contain every binding DECLARED in the shader even when a specialization constant disables that branch, so the exposure binding had to be added to all four downsample descriptors though only the first uses it. Whole twelve-pass chain averages `3.2 ms` at 1280×720 on Iris Xe, but run-to-run spread here is `±1 ms`, so that is an order of magnitude, not a cost measurement.
- PF03 TAA (2026-08-19, `RND-18`): three mandatory parts — sub-pixel Halton jitter of the PROJECTION (not post-processing: the matrix moves, so each frame samples different points inside the pixel; without jitter there is nothing to accumulate), reprojection of the ACCUMULATED frame through motion vectors (`taa_color` reads its own history, so the average is long rather than a two-frame blend), and a 3×3 neighbourhood clamp against ghosting. Blending happens in a reversibly range-compressed space (`c/(1+luma)`, Karis) because in linear HDR one bright pixel dominates the mean and leaves a glowing trail. THE JITTER SIGN HAD TO BE DETERMINED BY MEASUREMENT and I fell into the trap I had written the warning comment about: the jitter must be subtracted from motion vectors, and with the wrong sign the invariant "on a fully static scene with a static camera, motion must be ZERO under jitter" read `1.624 px` instead of `0.008 px`. The dangerous part was that TAA still *looked* right — the image appeared smoother and shimmer dropped 20% — while the output was actually FARTHER from the true supersampled frame than a single jittered frame (RMSE `795` vs `538`). Only the numeric convergence test exposed it; TAA cannot be validated by eye. After the fix everything matched theory: convergence to supersampling (`96.7` RMSE against the mean of 8 jittered frames, versus `564.6` for a single frame), shimmer suppression `1238 → 49.5` frame-to-frame (25×, as predicted for history weight `0.92`), edge high-frequency energy `121.4 → 84.4` (and `26.1` with the clamp off, i.e. maximally soft — the clamp trades smoothness for responsiveness), ghosting measured from both sides (deviation from the no-TAA frame in the movers' region is `1058` with clamp versus `2249` without, so the clamp halves the smear), and AO noise `44.4 → 36.7` at 8 samples once the AO sample rotation got a temporal offset — that is the "stochastic sampling + temporal accumulation" pair promised in PF02's conclusions. Passthrough is honest: `--taa-weight=0` and `--jitter=0` reproduce the TAA-off frame to within 0.01% of pixels. ENGINE FIX (`RND-36`): the derived cross-frame ordering created one semaphore per WRITING pass, but a binary semaphore is one signal per one wait — as soon as two passes read the same resource's history (TAA accumulation and compose's debug views), the second wait hung on an unsignalled primitive (validation: `no way to be signaled`). Semaphores are now created per writer→reader PAIR and the writer signals one per reader. Note `--jitter` is deliberately independent of `--taa` so that "jitter without accumulation" — the state that proves jitter alone does not anti-alias — can be shown. FOLLOW-UP INVESTIGATION (author's observation: distant static geometry shows a sharp staircase on slight camera motion that clears after a couple of frames): measured to be the technique's inherent limit, not missing data. When the camera moves, reprojected history lands between texels and outside the local neighbourhood range — narrow on a distant thin edge — so it is rejected and the output collapses to the UN-accumulated frame, after which the accumulator re-converges over ~12 frames at weight `0.92`. Loosening rejection does NOT help: min/max box, YCoCg variance clip and velocity-scaled clip-along-the-ray measure `75.5 / 75.3 / 76.2` on the smoothness metric, so box width is not the limiter. More important, THE SMOOTHNESS METRIC IS UNFIT for this question because it cannot separate "anti-aliased" from "blurred" — a proper ground truth was needed (same viewpoint, camera frozen, mean of 8 jittered frames). Against it: no TAA `404`, min/max+bilinear `596`, min/max+Catmull-Rom `536`, no rejection `788`; i.e. the SMOOTHEST option (no rejection, `49.6` smoothness) is the LEAST accurate because its smoothness comes from lag and blur, and under continuous camera motion an honest aliased frame is closer to truth than an accumulated one. That is the fundamental tradeoff of a single-history exponential accumulator. The one measurable win was the history resampling filter: bilinear re-blurs history every frame under motion, and Catmull-Rom (5 bilinear taps, negative lobes) improved accuracy `596 → 536` (10%), so it is now the default together with the min/max box, which measured more accurate than the velocity clip under that filter (`536` vs `554`); `--taa-clamp=velocity` remains as "fewer staircases on motion at the cost of more lag". Remaining known cures: post-accumulation sharpen, velocity-weighted history, and fundamentally MORE input samples per frame (MSAA+TAA hybrid or TAAU) — a single sample per pixel with one history cannot pass this limit.
- PF03 SSAO (2026-08-19, `RND-19`): AO modulates AMBIENT, not the image — the SSAO passes run BEFORE shading and shading multiplies only the sky term, because multiplying the finished frame would also dim direct sunlight, which in reality is occluded by geometry and shadows rather than by neighbour statistics. Direct consequence to accept up front: in a sun-dominated scene SSAO is weak BY CONSTRUCTION and only earns its place where ambient is large (overcast, interiors), hence the new `--ambient=<fraction>` knob. Half resolution via `declare_values` `screensize` + `scale` (so cost is a fixed fraction of the frame at any resolution), then a depth-aware blur, then a depth-aware upsample in shading picking the nearest-depth tap; the half-res buffer carries linear depth in `.g` — the same contract PF02 used for contact shadows, for the same reason (plain bilinear leaks across silhouettes). Sample count is a step specialization constant with a CLI override applied to the PARSED config before graph commit (PF02's technique). THE ESTIMATOR WAS REWRITTEN BECAUSE OF A MEASUREMENT: the first version compared sample depths (the textbook hemisphere formulation) and measurement immediately exposed self-occlusion on an OPEN FLAT FLOOR — AO fell `0.99 → 0.89` as the radius grew `0.3 → 2.5`. The second version decides occlusion from the RECONSTRUCTED occluder position and its elevation above the tangent plane, so a coplanar surface yields `dot(v,n) ≈ 0` and is rejected by the bias: open floor now reads `0.9999–1.0000` at any radius and even at zero bias, while the wall corner keeps its signal (`0.898`). A second defect showed up on the AO map: a fan of radial streaks on the grazing-angle floor at the bottom of the frame. It is not depth precision (tested with a distance floor — no effect) but variance: one pixel there covers a huge world area, so the same sample count spreads thinner. The standard cure is clamping the radius in SCREEN units — measured `std 30.3 → 0.075` and mean `0.983 → 0.99999` in that band while the wall-corner signal stayed byte-identical (`0.8214` before and after), i.e. the clamp removed only noise. Also measured: raw AO noise `41.4 → 31.2 → 26.7 → 25.2` at `4/8/16/32` samples but the blurred result flattens to ~`23` regardless, so the blur is part of the technique and beyond ~8 samples it eats the difference; AO's contribution to the frame is the wall corner going `99.4 → 89.9` at `ambient = 0.55` with FIXED exposure — measuring AO under auto exposure is pointless because the darkening raises exposure and cancels itself, the same trap as fog contrast. Cost is NOT measurable with this harness: identical runs vary `3.46–4.65 ms` average frame time and 4 samples "beat" 32, so per-pass GPU timestamps are needed (the engine has `gpu_timing`, the lab has not wired it yet). Residual AO noise under motion is TAA's job, which is why TAA is next.
- PF03 fog and aerial perspective (2026-08-19, `RND-35`): fog is applied in LINEAR HDR *before* exposure, because it is scene radiance rather than image post-processing — the meter must see the frame the viewer will see, which fixes the pass order as `G-buffer → shade → fog → exposure → compose → present`. Single scattering with exponential height falloff, optical depth taken ANALYTICALLY (`tau = d0·exp(-(cam.y-h0)/H)·H/dir.y·(1-exp(-dist·dir.y/H))`), which is why fog costs one `exp` per pixel instead of a ray march; the near-horizontal ray degenerates and gets its own branch (`tau = d·dist`). Directionality comes from a Henyey–Greenstein phase function — without it fog is a uniform grey veil and the recognisable part of aerial perspective is lost. Sky is excluded from fog since the atmosphere is already baked into it. Transmittance is stored in the alpha of the fogged target so later effects (god rays) need not recompute it. Measured: `--fog=0` yields transmittance exactly `1.000`; `T` falls monotonically with distance (`0.924/0.814`, `0.731/0.439`, `0.376/0.077` near/far at density `0.02/0.08/0.25`); distant contrast — aerial perspective proper — drops `28.05 → 17.68 → 8.71` at density `0/0.08/0.25`; and the phase function gives a `2.2×` difference between forward and backward scattering with the sun near the horizon. TWO MEASUREMENT TRAPS, both hit first: (1) contrast loss is invisible under AUTO exposure because the meter renormalises brightness and restores apparent contrast — such measurements need a fixed exposure; (2) the first phase-function test sat where the view is nearly perpendicular to the sun, and at `cos θ ≈ 0` Henyey–Greenstein is SYMMETRIC in the sign of `g`, so it measured 1% by construction — `--sun-dir=` was added to put the sun near the horizon. Also confirmed by measurement that the darkening of everything around a bright emissive panel is global auto exposure and not a local effect: the panel raises the metered average by `1.5` stops and exposure drops by exactly `1.5` stops, and with the same camera a floor patch reads `39.5/255` under auto versus `116.9/255` at the exposure the scene would have without the panel.
- PF03 histogram metering and per-pass GPU timings (2026-08-19, `RND-41`, `RND-48`): log-average metering was replaced by a 256-bin histogram — a shared-memory local histogram with a `16×16` group sized TO THE BIN COUNT, so each thread owns one bin and performs exactly one global atomic add. The three steps live in one pass with DELIBERATELY DIFFERENT usages (clear `storage_write` → build `general` → resolve `storage_read`), because the engine derives barriers from usage transitions: with the same usage there would be no barrier between steps at all and the resolve would read a half-written histogram. Measured levers: the upper percentile is ROBUSTNESS (a ×1000 lamp shifts metering `0.031 → 0.000` stops; cutting the top `10/30/50%` on a sky-heavy frame moves it `−0.125/−0.345/−0.627` stops), the lower percentile is BIAS toward midtones (`+0.38/+0.79` stops at `25/45%`) — easy to conflate, since both "discard part of the distribution"; center weighting pulls metering toward the centre; asymmetric adaptation lags `+0.88` stops when adapting fast to bright and `−0.50` in the mirror setup, and exactly `0` when symmetric. Metering bounds are the input for the dark-room barrier (a dark corner reads `94` free versus `22` clamped). Metering on a HALF-density grid gives the identical result (`+2.431`, 87 bins versus 93) at a quarter of the work. THREE BUGS, all found only by visualising: (1) the clear covered a quarter of the bins (64 threads for 256), so bins `64..255` accumulated across frames and quietly broke the percentiles; (2) DOUBLE DIVISION in the dispatch helper, which always divided by 8 while the histogram needs 16 — passing already-divided values produced a `10×6` grid instead of `80×45`, so metering saw only the FIRST SIXTEEN ROWS of the frame, and the symptom was deeply misleading (the distribution looked degenerate at four adjacent bins, because the top strip of the frame is flat sky); (3) the debug readout's own precision (`0.125` stops per step) was coarser than the effects being measured (hundredths of a stop). LESSON: an aggregate hides what it was built from — while metering emitted one number, all three bugs looked like "the scene is like that", and only a distribution plot plus a "what metering sees" view exposed them. Then `RND-48` wired the engine's existing `gpu_timestamp_profiler` into the lab (average/min/max per pass over a run, read right after `prepare_frame` and belonging to the frame submitted `frames_in_flight` ago). It immediately CONTRADICTED wall-clock guesses in both directions: metering, suspected of costing ~1 ms, costs `0.144`; the most expensive pass turned out to be compose (`0.956`), which nobody had questioned, and it is expensive in its baseline (a dozen fetches plus a full-res write) rather than in the lens knobs (`0.906` with sharpen versus `0.877` without). Chain total `3.967` ms average / `3.596` min at 1280×720 on Iris Xe against a `5–7` ms frame, i.e. ~2 ms outside the GPU. It also closed two hanging questions: SSAO costs `0.351` ms at 8 samples versus `0.950` at 32 while post-blur quality is identical, and metering costs `0.316/0.136/0.121` ms at grid `1/2/4` with an IDENTICAL result. And it put a number on the audit's claim that `--ao=0` saves nothing because the pass still runs — quality tiers must change the graph shape, not a UBO value.
- PF03 exposure and tone mapping, plus `history = N` pinned to frames (2026-08-19, `RND-21` in part): exposure is a MULTIPLIER in linear HDR ("what counts as middle grey here") and tone mapping is a CURVE compressing the already-exposed range into `[0,1]` ("what to do with highlights that still do not fit") — the order cannot be swapped. Metering is its own pass: one workgroup takes 4096 taps and averages the LOGARITHM of Rec.709 luminance, i.e. a geometric mean, because an arithmetic mean lets one bright sun drag the whole scene into darkness. Adaptation blends against the previous value through the history of the same 1×1 `role = exposure` image, exponentially in REAL dt so the adaptation speed does not depend on frame rate — the third consumer of the history contract. Verified numerically: metering is linear in scene luminance (sun `1 → 6 → 30` yields `2.51` and `2.38` stops against the theoretical `2.58`/`2.32`), exposure cancels it exactly (final image mean holds `74–82` across a 30× lighting range versus `180 → 247` at fixed exposure), `log2(E)` matches `log2(key) − adapted` in every sample, freezing adaptation (`--adapt-rate=0`) changes 33.5k pixels versus instant, and without a curve `1.2%` of the frame burns to flat white while Reinhard/Hable clip nothing. The DISPLAY TRANSFER FUNCTION was measured rather than assumed: debug view 8 emits exactly linear `0.5` and the screenshot reads `187/255`, so the blit into a `B8G8R8A8Srgb` swapchain performs the sRGB encode itself and encoding in the shader would double-encode (a `--encode-srgb` toggle stays for linear presentation formats). The scene needed real dynamic range to be testable at all: a sky brighter than lit surfaces (as in reality), ambient as a FRACTION of the sun, a broad glossy highlight and an emissive panel — the panel matters because auto exposure normalises the AVERAGE, so it tucks the sky under 1.0 and leaves the curve nothing to do, whereas a small source two orders above the average barely moves the average and only a curve can retrieve it. Hable needs its customary `2.0` exposure bias or it systematically under-exposes (measured: mean fell threefold). CONTRACT PINNED: `history = N` now means FRAMES only and requires `swap = per_frame` (loud error otherwise, previously a warning) — for a host-advanced counter "the previous copy" is the previous UPDATE, which may not have moved between two frames at all; update-history is an orthogonal axis and belongs to the caller, written explicitly as a second field of the record. ENGINE BUG FOUND AND FIXED (`RND-34`): `constant::offset` is a BYTE offset while constant memory is a `uint32_t` array, and `get_constant_data`/`write_constant_data` added it to a `uint32_t*`, landing four times too far. The first constant (offset 0) always worked, so the bug survived until a config used two constants by value — the dispatch then read garbage (validation reported `groupCountY = 1072693248`, the high half of a double). Covered by a regression test.
- PF03 per-object motion and a host-written-history gotcha (2026-08-19): per-object motion needs no notion of *which objects are on screen* — the fragment already knows which instance drew it, so it is enough for the object to carry its previous transform next to the current one; the vertex shader produces two clip positions of the same surface point and the difference lands in the G-buffer. Object identity matters later, for history *rejection* (disocclusion), where comparing depth and normal usually suffices. The previous transform is not duplicated by hand: transforms live in an ordinary per_frame resource and a second binding of the same resource declares `history = 1`, so the host writes only the current frame — this is the second consumer of the history contract and the first on a *buffer* rather than an image. ENGINE FINDING (`RND-33`): the first attempt put the previous transform into the draw group's instance record and produced exactly the failure the metric exists to catch — history matched the current frame BIT-EXACTLY. Host-visible draw-group instance/indirect buffers are hardwired to the `per_update` counter with `doublebuffer`, and `per_update` does not advance in the frame loop, so every frame in flight reads the same memory and "the previous frame" does not exist for that data; anything that changes per frame must not live there. Workaround in the lab: the instance lane carries only the object index (a constant, for which shared memory is safe). Also refined the cross-frame derivation: the producer of history may be the HOST, not only a pass — the host writes copy `c(N)` while the GPU reads `c(N-1)`, which are different copies by the period+history arithmetic — so a host-visible resource needs no semaphore and demanding a writing pass was wrong (it now logs at flow level instead of erroring). Object translation and spin are separate knobs on purpose: spin changes the normal, shading depends on it, so a surface point changes colour between frames and reprojection cannot fix that by construction — measuring the vectors needs an invariant signal (`--object-spin=0`), while spin stays as future load for TAA's neighbourhood clamp. Measured with a locked camera and pure translation: naive error grows with object speed (`107 → 581 → 885` at `0.5/1.0/2.0`), compensated stays far lower (`74 → 248 → 391`, gain `1.5× → 2.3×`), and the residual sits as a thin rim exactly on the silhouettes (disocclusion) while the uncompensated map lights the whole displacement contour. Planned separately: PNG screenshots via libpng as a user-facing tool (`RND-32`) — a screenshot folder with overrides and fallback, a blit into a host staging buffer in its own step, and a key that hands the buffer to libpng; the lab's raw PPM `--dump` stays as the codec-free, bit-reproducible measurement path.
- PF03 thin G-buffer slice (2026-08-19, `RND-17` in part): the lab now runs `thin G-buffer → shade → compose/reproject → present`. One graphics pass writes depth, an octahedral world normal and a UV-space motion vector into three attachments; shading and composition are compute passes, so the chain is visible without extra host code. Motion comes from the camera only — the vertex shader carries the same world position through the *previous* frame's view-projection, and `motion = previous_uv - current_uv` with the current uv taken from `gl_FragCoord` (not the interpolated clip position, so the metric measures motion rather than interpolation error). The compose pass reads `scene_color` with `history = 1` and shifts the fetch by that vector, which makes it the first real consumer of the history contract and the first *reader ≠ writer* case of the derived cross-frame order (`pass 'pf03_compose' waits previous frame of pass 'pf03_shade'`). Motion vectors are verified numerically, not by eye: with a locked camera the reprojection error is exactly zero (the frame is bit-black); the motion field scales linearly with camera speed (`3.02 → 6.09 → 11.14` px mean magnitude at `0.5/1.0/2.0` rad/s, i.e. doubling the speed doubles the vector); and at a matched view the motion-compensated error stays below the naive one with the gap widening as displacement grows (`1.22×` at 2.5 px/frame → `2.17×` at 20 px/frame). The residual is resampling of the deliberately hard `step()` checkerboard, which is the stress pattern TAA's neighbourhood clamp is meant to handle, so it stays. MEASUREMENT LESSON repeated from PF02 in a new form: an external screenshot cannot compare debug modes at all, because it lands at an arbitrary moment and the camera phase difference swamps the effect being measured — the first "with motion vs without" comparison was accidentally a comparison of two different viewpoints. The lab therefore owns `--orbit=<rad/s>` (camera position is a pure function of the FRAME INDEX, not wall clock) and `--dump=<file.ppm>` (frame-exact readback of `composed_color` through a staging buffer); two runs with identical arguments are bit-identical, so modes are compared numerically. Debug views `0..6` (shaded, depth, normal, motion, reprojected, error-with-motion, error-without-motion) switch by key. Scene is deliberately static: with camera-only motion a moving object would carry a knowingly wrong vector; per-object motion (instance must also carry its previous transform) is the next step and the error metric will show it immediately.
- Frame-history contract, first `PF03` slice (2026-08-18, `RND-29`): a resource no longer declares how many copies it has. That number is the sum of two orthogonal facts with separate homes: the **rotation period is a property of the counter** (`per_frame` → `frames_in_flight`, `swapchain` → presentation image count; other counters are advanced by the host and their period is unknowable to the engine, so those resources must still state `type` explicitly), and the **history depth is a property of the reading technique**, declared at the read site as `history = N` on a descriptor binding. The resource takes the max over its readers, so two temporal techniques with different depths never have to agree with each other, and copies = `period + max(history)` — verified minimal by brute force in `painter_temporal_history_test` (one copy fewer always races). Evidence that `type` carried no information of its own: across every config all 32 resources formed exactly three `(type, swap)` pairs, i.e. it restated the counter; it survives only as an override. Everything else is *derived* from the same declaration, so a temporal technique adds no semaphores or barriers to the graph config: copies written this frame are fixated into the history layout by a step at the **very end of the frame** (not right after the writing pass — a later pass such as the blit to swapchain would move that copy to `transfer_src` and the next frame would read a layout the descriptor never promised); the reading pass waits on an engine-created semaphore of the writing pass from the **previous frame slot** (the fence cannot substitute: it waits for frame `N - frames_in_flight` while `N-1` is still in flight), with the cross-frame tail of the wait list trimmed on a graph's first submit since nobody has signalled it yet; and copies are cleared to zero and moved into the history layout at creation, while resize repeats that operation ONLY for recreated screensize history. Fixed-size temporal state is neither recreated nor cleared and survives resize/fullscreen. A binding gives the shader **either** the current copy (a single descriptor) **or** a window of `history` copies — the current copy is deliberately excluded, since the pass that writes new history and reads the previous one (exactly TAA's topology) keeps it writable while history stays read-only, and one binding cannot promise both layouts. Consequence: shaders no longer declare `scene_data[3]` arrays sized by accident to `frames_in_flight`, and `history = 1` arrives as a plain `sampler2D`. GOTCHA fixed on the way: a history binding must not contribute to step barriers at all (it addresses other copies), otherwise one step writing new history while reading the old one looks like a usage conflict; `wait_for` on a pass-local semaphore silently waited on the *previous* frame (now same-frame, with a loud error if the signalling pass comes later, and `wait_previous` as the explicit escape hatch); and `pSignalSemaphores` of every execution group pointed into one shared buffer, so only the last group could signal. `PF03_post_processing` is the consumer: a compute-only stand (scene → accumulate → blit) with `--frames=N`, `--history-weight=`, `--verbose`, running 400+ frames validation-clean with a visible trail that disappears at weight 0.
- Shader record layout must live in one file (2026-08-18, learned by regression): adding a field to PF02's cascade/spot records broke shadows on every cascade above the first and corrupted spot shadows entirely, because the record declarations were copied into five shaders and the two *vertex* shaders kept the old stride, so every buffer element except index 0 was read at a shifted offset — the atlas debug inset showed three of four tiles black, since a garbage matrix rasterises nothing. Fix: PF02 now has a single `resources/shaders/pf02_records.glsl` (`PF02_SCENE_BLOCK_BODY`, `DirectionalCascade`, `SpotLight`) included by every lab shader, verified to change zero pixels. This required `painter::shader_crafter::set_include_root`: local `#include` now resolves against the shader filesystem root even without a demiurg registry (previously only the generated shared header and demiurg resources were includable), which is what labs on the fs path need.
- Painter atlas allocation contract (2026-08-18, `RND-27`): `libs/painter/include/devils_engine/painter/atlas_layout.h` owns placement of square regions in one atlas image as a pure mechanism. `allocate_atlas_regions` packs requested sizes deterministically with first-fit over a grid whose step is the gcd of the sizes and the atlas dimensions (shelf packing was tried first and rejected: it wasted the space above smaller regions), `atlas_region_uv` yields the `local_uv * scale + offset` transform and `atlas_occupancy` reports usage. Overflow returns `false` and the *caller* reports it loudly, because only the caller knows what it asked for. `painter_atlas_layout_test` covers reproducing the previous hard `2×2` grid, unequal sizes without overlap, refusal instead of overlap, determinism and uv transforms. Consequence for consumers: cascade/light records carry `uv_scale_offset` and the scene UBO carries the actual region count plus atlas size, so shaders stop knowing the layout — PF02 lost `tile = vec2(index & 1, index >> 1)`, the `* 0.5` divisions and the hardcoded `4` in its cascade-selection loop, and a region's texel size is now derived from that region's real size. GOTCHA: the `draw_regions` wire is packed — spans start immediately after `region_count` commands, not after a max-sized array — so a runtime region count must be written in blocks, otherwise the graph reads spans at the wrong offset and trips the pair-capacity check.
- PF02 budget parameterisation (2026-08-18): the author rejected a quality target as an *input* (hard for a human to reason about) in favour of `--shadow-distance=<m>` (how far shadows must exist; `--cascade-far` kept as a synonym so earlier measurements stay reproducible) plus an atlas budget `--cascade-tiles=<a,b,c,...>` where the region sizes are explicit and the cascade count is the list length (max six), plus `--shadow-atlas=<size>`. Quality is *displayed*, not configured: the overlay prints `Derived quality: <px/texel at slice end>, <worst in slice 0>, atlas WxH, occupancy`. The directional atlas got its own declare_value (`directional_atlas_resolution`) separate from the spot atlas, because the cascade budget must grow independently; it is read from the config rather than a C++ constant, so it can be declared `type = screensize` with a scale to keep quality resolution-independent. Measured example: `--shadow-atlas=4096 --cascade-tiles=2048,1024,1024,512,512,512 --shadow-distance=100` gives splits `2.3/4.9/8.8/16.8/37.8/100.0` and texels `3.3/13.4/23.9/91.8/207.5/551.0 mm` at 42% atlas occupancy — a denser near cascade (`3.3` vs `4.6 mm`) and a `2.3 m` transition-free bubble, at the price of a half-metre texel in the tail.
- PF02 cascade layout insight (2026-08-18): in a practical/logarithmic split `cascade_far` is a *distribution* knob, not a shadow-distance knob, and the author found empirically that `--cascade-far=100` beats `--cascade-far=10` on a local scene. The numbers explain it (4 cascades, `lambda 0.88`, `1024²` tiles): `far=10` puts boundaries at `0.6/1.5/3.7/10` — three cascade transitions inside eight metres of view, plus a hard end of shadows at 10 m — while `far=100` puts them at `3.5/8.8/24.7/100`, leaving one transition and the whole local scene inside cascade 0. At 5-8 m the large `far` even gives a *finer* texel (`24` vs `27.6 mm`) because the small `far` drops those distances into its last cascade. The rule that follows: pick the layout so the radius the player actually scrutinises falls inside one cascade, rather than minimising `far`. The price is a coarser near texel (`10` vs `4.6 mm`), which cascade count buys back: six cascades at `far=100` give cascade 0 = `0.1..2.3 m` at `6.6 mm` with a `7/13/24/46/104/276 mm` tail — which needs the atlas allocation contract, since the atlas is currently a hard `2×2` of four tiles. Follow-up worth doing: parameterise the config as "local scene radius" + "cascade count" and derive `lambda`/`far`, because the current pair interacts non-obviously.
- PF02 far-cascade shadow behaviour, measured (2026-08-18): three separate things were conflated under "shadows stop working at a distance". (1) A standing caster's shadow does not vanish in a far cascade — at `56°` sun elevation a 1 m cube casts only `0.67 m`, so from a camera on the sun's side it hides behind the caster; from the opposite side the same shadows are fully visible. Measured with the new `--cascade-far` / `--cascade-lambda` knobs at a fixed camera, shadow area moves only `21.8% → 25.8%` across layouts from 8 m to 80 m, i.e. there is no wholesale loss. (2) What genuinely degrades with cascade index is the *contact region*: cascade texels are `4.5 / 11.2 / 31.0 / 108.6 mm` and the texel-scaled normal offset reaches ~`49 mm` on the last cascade, so the first 10-15 cm of shadow next to a base is unresolvable there — a resolution budget, not a defect. (3) Screen-space contact shadows cannot fix that: their ray is `0.24 m` long, which at 15-20 m is sub-pixel — contact contributes `max 198/255` in the near `corner` fixture and effectively nothing far away, and pushing the fade out to `--contact-fade=40,60` changes not a single pixel. A real defect was found and fixed along the way: the contact pass's "no silhouette here" gate compared the *first* difference of linear depth against a fraction of the distance, so grazing and distant flat receivers were rejected wholesale; a plane is linear in reverse-Z screen space, so the gate now tests *curvature* against the local slope, which is scale-free. Near-field contact contribution is unchanged (`max 198`) and the acne metric stays at its floor. New knobs: `--cascade-far`, `--cascade-lambda`, `--contact-fade=<start>,<end>`.
- PF02 screen-aware filter width and edge-roughness metric (2026-08-18): the AA kernel is defined in shadow-map texels, but a texel's *screen* size shrinks with distance, so on coarse cascades the whole footprint collapses below a pixel and edge smoothing stops working — the edge reads as a staircase again in every AA mode. The tap spacing is now scaled so the transition covers at least `pcf_min_screen_pixels` screen pixels, accounting for the surface's grazing angle to the camera, capped by `pcf_max_radius_scale`; both are step specialization constants (`draw_scene`) with `--pcf-radius=<N>` / `--pcf-min-pixels=<V>` CLI overrides that patch the parsed config before graph commit (`--pcf-radius=0` = a single hardware tap, the minimum possible smoothing). Verified to activate only in the distant/grazing band (per-tile difference max 76-95 there, exactly zero elsewhere) and to leave the acne metric untouched. Edge quality is now measured rather than eyeballed: subpixel 50%-crossing edge, linear-trend removal, median residual RMS over sliding windows. That metric shows roughness is proportional to the cascade's world texel (`1.09 px` at atlas 2048 → `0.62 px` at 4096, hard mode) and drops to `0.42 px` under weighted PCF, i.e. the "jagged at distance" complaint is the sub-pixel footprint, not a separate defect. `--camera-at=<x>,<y>,<z>,<yaw>,<pitch>` plus a `P` key that prints exactly that string make a viewpoint-specific artifact communicable at all.
- PF02 cascade-boundary line fixed by per-cascade raster bias (2026-08-17): a thin dark full-width stripe appeared at a fixed camera distance, only for directional (cascade) shadows and only in filtered AA modes. Root cause: the normal offset scales with each cascade's world texel, but the raster depth bias is expressed in depth units and was identical for every cascade, while cascade texels differ by 24× (`4.64 → 111 mm`). Coarse cascades therefore lacked margin and the blend band produced false occlusion that the hard path never sees. The `draw_regions` stream already carries dynamic depth bias per region, so the host now scales it by `texel[i] / texel[0]`. Measured on the new `cascades` fixture: band amplitude `max 1.10 → 0.02` with the acne metric unchanged at its floor. Doubling the normal bias does not touch the band (`1.12`), and raising raster bias globally fixes it but costs near-cascade attachment — hence the per-cascade scale. The receiver-plane gradient also moved from `dFdx/dFdy` to the analytic Jacobian of the light matrix; that is a robustness fix, not a visual one (`AE = 1.7` px of 1.25M): derivatives are undefined in the divergent spot loop and are garbage on the row where the cascade index changes, because pixels of one quad land in different atlas tiles.
- PF02 A/B correctness requirements (2026-08-17, learned the hard way): the lab reads resources from the SOURCE tree (`PF02_RESOURCE_ROOT`), not from the `build-*` copy, so config/shader edits for an experiment must be made in the source tree — patching the build copy silently changes nothing. The scene is animated and the camera reads the mouse, so a screenshot A/B needs both `--time=<seconds>` and `--lock-camera`; without them an external screenshot tool nudges the camera and the diff shows large, entirely spurious differences. With both, two runs of the same state are bit-identical (`AE = 0`).
- PF02 repeatable-comparison tooling and two rejected hypotheses (2026-08-17): the lab gained `--camera=<overview|corner|grazing|ramp|thin|topdown>` fixture bookmarks, `--time=<seconds>` animation freeze, `--no-overlay`, and `--normal-bias=<base>,<slope>` / `--raster-bias=<constant>,<slope>`. The freeze matters more than it looks: with a moving spot light and a moving caster, two runs are not comparable pixel-by-pixel, and the first `equal`-vs-`greater_or_equal` A/B produced convincing but entirely spurious concentric rings. With time frozen, that A/B differs in exactly zero scene pixels, so the depth-prepass `compare = equal` is exonerated — no fragments are being rejected. Acne is now tuned by measurement rather than by eye: high-pass standard deviation of a floor patch in the `grazing` view reads `3.75` at zero bias, `0.39` at normal bias `0.15/0.85` and `0.27` (the metric's floor, where the residual is geometry edges) at `0.25/1.20`, which is the new default; dropping raster bias to zero regresses the same metric to `1.23`, so raster bias stays. **Front-face shadow casting (`cull = front`) was tried and rejected**, measured with a locked camera: it does not improve the acne metric at all (both configurations already sit at the `0.270` floor once bias is tuned) and it loses the dark contact sliver at a caster's base, because the light ray entering a floor-resting solid exits at or below floor level. It only makes sense together with a midpoint `(front+back)/2` map, i.e. two depth passes, which is a separate slice. Note the oblique receiver also casts its own legitimate thin line: the ramp sits 4 cm above the floor, so its leading edge draws a narrow sharp shadow fixed in world space — not to be confused with the cascade-boundary stripe above.
- Established integration playground: `subprojects/tile_frontier` remains the broad simulation/threading proof, but it is not the current task selector.
- Goal for `tile_frontier`: draw a large tile map, support world/resource streaming, and run many AI actors to stress multithreading.
- GPU asset lifetime (2026-07-19): hot texture unload waits the owning graphics frame fences, rewrites the bindless slot to the default/null texture, destroys VMA image/view and returns the slot to `empty`; mesh unload removes the slot from every draw-group pair before destroying buffers and returning it to reuse. `painter_asset_lifetime_test` covers state transitions and cross-group unregister.
- Snapshot query repair (2026-07-19): `aesthetics::snapshot_loaded_event` is common, and both `world::query_t` and `lazy_query_t` subscribe and rebuild their cached entity containers after `load_world`. Queries created before a load immediately see the restored world.
- First player/spawn slice (2026-07-19): `actor_world_slice` owns one serialized `player_controller` plus an ephemeral deduplicating `player_intent_queue`. `spawn_food` (default `mouse_right`) converts screen to world and emits typed `act::intent_kind::spawn_prefab`; gameplay consumes it at the next tick boundary. Food maintenance uses semantic `spawn_point` entities/group `food`; ds exposes `spawn(prefab, group)`, while `spawn_at` remains the coordinate escape hatch.
- UI budgets (2026-07-19): app/user settings carry Lua instruction/wall-time, incremental GC, Nuklear convert time/output and consecutive-failure limits. `visage::system` skips a failed/over-budget frame and disables after the configured streak; settings reload resets it. `lua.update` and `nuklear.convert` feed a catalogue statistics domain.
- Canonical module fingerprints (2026-08-12): `demiurg::module_system` exports the actually loaded ordered module descriptors plus a versioned aggregate SHA-256. Directory fingerprints hash sorted root-relative paths/sizes/content and reject symlinks; archive fingerprints hash the exact `.zip/.mod` artifact and remain compatible with module-list JSON. Absolute roots/timestamps are excluded, generated cache modules stay in a separate module system, folder/archive logical naming was repaired, and `demiurg_resource_loader_test` covers relocation, order, missing modules and content changes.
- Planned module profiles (2026-08-12): the game-facing module layer will discover installed folder/`.zip`/`.mod` packages under a project module root (normally `mods/`), expose metadata/version/dependencies to UI, and persist several ordered TAVL profiles plus one active-profile pointer beside user settings using atomic file transactions. The active profile is applied before the game resource registry is built; switching profiles defaults to a full runtime restart boundary. Every save will carry the actually loaded ordered module ids/versions/fingerprints and produce a structured exact/reordered/missing/changed/extra compatibility report. Missing or changed modules may be an explicit warning instead of a hard failure when project policy permits it; unknown module-owned save sections must be preserved opaquely or the next save must be labelled destructive.
- Atomic file transactions (2026-08-12): `utils/atomic_file.h` owns exclusive same-directory `target.tmp` transactions with streamed writes, file flush, atomic replace, parent-directory flush, RAII abort and explicit stale-temp recovery. Errors carry stage/code/paths and distinguish pre-commit failure from a committed rename whose directory flush failed. `utils_contract_test` covers success, abort, stale recovery, validation and replace failure. `AUD-01` pinning and the miniaudio-vs-OpenAL A/B decision are complete; miniaudio is the selected production backend and the OpenAL path is archived.
- Opt-in FSM and phase metadata diagnostics (2026-08-14): `mood/diagnostics.h` owns address-independent graph snapshots, guard-by-guard step traces and actual settle traces with explicit stop reasons; the existing `step/settle` runtime translation unit is untouched, and diagnostic settle executes actions exactly once rather than previewing. `catalogue/phase.h` adds passive constexpr owner/read/write/write-policy/budget descriptors and derives arbitration/commit/conflict from the existing MT domain strategy. Executors never look metadata up or publish diagnostic events; tooling explicitly builds a caller-owned registry. `tile_frontier` exposes local/eat/flag effect descriptors and validates them only in `tile_frontier_resume_smoke`. A universal `CAT-01` aggregation service is deferred until two consumers prove the same snapshot/reset lifecycle; local timings remain in `statistics_store`, while semantic rejection/overflow/divergence stay with their owners.
- Replay remains inactive until three prerequisites: ordered runtime module list/priority loading in demiurg, camera as its own intent provider with a replay-only presentation track (not necessarily network input), and exact per-tick `game_delta_ticks` recording. The eventual replay artifact is a versioned intent/input log plus checkpoint and resource/config/build fingerprint, not catalogue serialization.
- Generic interaction resolution kernel (2026-07-22): new header-only `libs/resolve` owns pointer-free work provenance, bounded MT journals with semantic seal/id assignment, target grouping (parallel between targets, serial within one target), host-paced `frontier_state begin/advance`, neutral damage route/outcome data and the hard retaliation contract. Retaliation deduplicates by `(triggering_instance, rule)`, may occur once per hit/rule, and neither retaliation nor any descendant of its lineage can retaliate. Atomic append order has no gameplay meaning; ambiguous provenance and every deterministic budget overflow fail loudly. There is deliberately no fundamental `run_to_completion`: an FPS host loops `advance` within a sim tick, while a turn FSM paces the same boundaries with presentation checkpoints. Card ordering, elemental tables, status storage/stacking and death policy remain project-owned. Detailed contract: `libs/resolve/README.md`; `resolve_pipeline_test` covers 1-vs-4 identity, target order, frontier faults, retaliation and minimum-HP guard without resurrection.
- Cardgame grouped combat slice (2026-07-23): `subprojects/cardgame` is the first live `libs/resolve` consumer and owns the pointer-free hierarchy `effect_program → beat → authored_effect/effect_ref → emitted effect_instance_ref → typed outcomes`. Every beat freezes all target snapshots before cue; authored cues share one gameplay barrier, scripts execute in stable order, and one aggregated result per authored effect carries its outcome range. Targeters are target/random/all; effects select independently by default, while an explicit nonzero binding key reuses one snapshot and mismatched reuse fails loudly. Death is checked after every damage/status outcome and gates only the next beat. Primary hits still use an owned frontier; elemental children and thorns retaliation retain resolve provenance. The serializable project cursor runs ActionBegin/card execution → player/enemy party passes → source ActorStateTick → explicit countdown boundary → a fresh mirrored enemy execution/party passes/tick → ActionEnd. Its append-only `action_report` seals one segment per group: each group reads only the prefix sealed before it, appends zero or more complete `resolution_work` executions, and exposes those outputs only to the next explicit group. Thus player follow-ups see card work, enemy follow-ups see card+player work, ActorStateTick sees card+both passes, and the report resets before enemy execution; the mirrored sequence builds a separate report. A stolen card contributes one `executed=false` execution segment, skips party passes, still ticks its actor and advances countdown. Forced end-turn enemy executions receive their own monotonic action-cycle token and skip player ActionEnd. Party order is frozen from `(combat_seed, action_token, side_domain, stable entity id)`. ActorStateTick still only records its consumed prefix; real DoT/negative/positive programs remain next. `cardgame_action_pipeline_test` covers staged report visibility/reset plus normal/theft/mirrored/forced/resume order.
- Cardgame typed effect envelope and routed damage (2026-07-23): attack/healing/shield/agility-damage/status stores feed one semantic-order `outcome_ref {kind,index}` without a thick variant; sign never changes kind (`attack(-N)` remains damage, `healing(-N)` remains healing, `shield(-N)` remains shield removal), and death is checked after every typed outcome. Authored `emit_shield` is a first-class DS leaf: it adds a signed amount to the shield stat, clamps at zero, produces its own report range/category/presentation result and survives pre-invocation resume. One unrouted damage root gathers modifiers/resistance exactly once into `damage_preparation`, then emits at most two bounded provenance children in fixed order: shield leaf followed by residual health leaf. Only leaves mutate stats/become outcomes; shield-only hits have no synthetic HP outcome and terminal shield/HP drives elemental continuation once. Every committed damage leaf—shield, health, zero, negative, primary, reaction or periodic—offers subscribed retaliation rules the same immediate DS boundary; only `resolve::may_emit_retaliation` is a hard C++ gate, so retaliation and every descendant remain non-recursive. Routed leaf IDs share the frontier allocator, presentation exposes `shield_damage`, and each emitted response keeps its separate cue. `cardgame_typed_effect_test` covers shield-only and negative-attack retaliation, two rule invocations for shield→HP overflow, vulnerability without double resistance, signed healing, self targeting, attribute resistance, report/death boundaries and animated/headless identity; `cardgame_combat_script_test` covers authored shield emission.
- Cardgame DS reaction contract (2026-07-23): retaliation is an immediate continuation of one committed instance inside its outer execution; neither retaliation nor its descendants can retaliate. Follow-up is instead a fixed FSM group over a frozen `action_report` prefix. It cannot recursively observe output from its own party pass, but its completed executions are sealed into the ledger and may enable the next opposing-party group. A retaliation always owns a nested authored attack cue/result/finished (no inline merge) and a full attack route, but receives retaliation lineage, no action/countdown/tick and no separate follow-up group. Cardgame DS is deliberately two-phase: a rule/program prepare scope runs before cue and materializes targets/authored-effect requests; an authored-effect emit scope runs after the gameplay marker and records typed instances only. `emit_*` never resolves synchronously, lifecycle channel is invocation-owned, and direct reflected `add_*` stat mutation is forbidden because it bypasses outcomes/death/presentation. Preserve the existing mutable `register_stats`; add a separate read-only registration entry point for combat scopes.
- Cardgame DS leaf/resource slice (2026-07-23): devils_script is pinned to v1.2.1. `act::register_stats_readonly` sits beside unchanged mutable `register_stats` (plus separable reader/writer registration). `combat_effect_scope` executes a compiled ds script against a transient writer; its `each_target` iterator traverses the frozen authored target snapshot in project order and installs one `combat_target_scope`. Each `emit_attack/healing/shield/attribute_damage/status` call appends exactly one bounded pointer-free instance plus a semantic plan ref directly into `resolution_work`; target-major callback order is gameplay order, signed literals such as `emit_healing = -2` are native syntax, and attack root IDs remain unresolved until the resolver selects the plan item. `combat_effect_script_compiler` is the project adapter for generic `act::script_resource` roots `combat_effect` and `retaliation_rule`; authored recipes snapshot only stable demiurg hashes, while `combat_effect_script_resources` resolves externally owned compiled containers. `retaliation_rule_scope` exposes rule stacks plus the trigger's requested/delta/before/after/clamped, element/tags, destination/channel/death/id/root/source/target and bounded `emit_retaliation_attack`; shipped `scripts/thorns_retaliation` now owns the thorns response decision instead of a native HP-loss predicate. `cardgame_combat_script_test` covers both roots and pre-invocation resume.
- Cardgame execution-report DS view (2026-07-23): generic `act::script_resource` also accepts the read-only `execution_report` root. A transient `execution_report_view_context` rebinds a frozen pointer-free report to the `resolution_work` that owns its typed stores; no pointer enters snapshots, and every plan/outcome range plus typed ref is checked before dereference. DS sees execution/actor/selected-target/executed/category metadata, typed counts and stable `each_attack/damage/healing/shield/attribute_damage/status` iterators with route/result accessors. The scope deliberately has no emitter. The owning `action_report` ledger retains all prior executions through party passes and ActorStateTick, and the follow-up prepare scope below iterates its sealed input prefix. `cardgame_execution_report_script_test` covers resource compilation, metadata, typed fields, snapshot-style rebinding and corrupt-range failure.
- Cardgame follow-up prepare/target slice (2026-07-23): generic `act::script_resource` accepts `scope = follow_up_rule`. Its transient context exposes rule actor, action actor, optional original target, frozen priority-ordered live opponents, aggregate input category checks/count and `each_execution`, which installs the read-only `execution_report_scope` for every work in the sealed prefix. Emitters cannot guess a target: scripts must enter `select_original_opponent`, `select_priority_opponent`, `select_original_or_priority_opponent` or `select_action_actor`, receiving one `follow_up_target_scope`. Original-or-priority keeps the still-eligible card target, falls back to the first project-priority opponent and emits nothing when neither exists; action-actor is the stable defensive target even when the rule owner differs. Authored effects now support a per-effect pointer-free explicit target, so one execution is not restricted to its report's convenience `selected_target`. Bounded `emit_follow_up_attack` and `emit_follow_up_shield` prepare recipes/beats only; the ordinary FSM revalidates target liveness, freezes snapshots, resolves leaves/outcomes/presentation, then appends completed work to the party segment. Self-only cards carry `selected_target=invalid`. The temporary native fixture remains one optional attack rule per participant; real component/resource rule lists are next. `cardgame_combat_script_test` covers original/fallback/no-target selection, defensive targeting across different actors, authored shield resolution, 4→5→6 visibility, fresh enemy reports and follow-up-cue resume.
- Cardgame retaliation presentation (2026-07-22): each retaliation response now opens its own nested `returned_damage` cue/gameplay/result/finished checkpoint immediately after the triggering attack subtree. The response outcome is excluded from the outer authored-effect presentation result, so it is not visually merged twice. Response task ids stay transient; resume at the retaliation cue drops presentation and continues the serialized `response_commit` exactly once. `cardgame_headless_smoke` covers the separate cycle and resume boundary.
- The cross-actor contract question (main/gameplay, render, sound, assets) is now ANSWERED: a single lock-free **broker** holds every inter-thread channel (see "Message broker" below). The old `utils::actor_ref` / `actors.h` path was archived under root `exclude/` on 2026-07-13; `message_dispatcher<T>` and advancer `actor`/`get_actor()` remnants are no longer present in the live tree.
- `tile_frontier` core is split by concern under `subprojects/tile_frontier/{include,src}/core/`: project-specific `config`, `messages`, `render_system`, `assets_system`, `actor_simulation`, resources and `tile_frontier_game`; shared broker/topology/runtime pieces live under `libs/simul`. `simulation.cpp` is now only the thin `game_host` adapter/state+worker wiring. `tile_frontier_game` owns chunk request/receive/apply, map/camera + UBO, tile/actor publication, actor update, presentation-sound culling and project metrics/UI values. It consumes generic host outputs (`scene_binding`, `phase_gate`, framebuffer/presence) without moving any game policy into engine.
- Runtime scene composition is config-driven: `states/*` owns UI `script`/`default_font`/allowlist and points at a standard `scenes/*` manifest. `simul::scene_manifest_resource` expands `{id,target,group,alias,startup}` into scope grants, resource transitions and target-aware loading progress; without render, external targets stop at the pre-external level. `tile_frontier` interprets only manifest groups/aliases and reads chunk/grid/actor/brain settings from its CPU-only `worlds/*` descriptor. The texture/sound id arrays and default-font hook are gone from `simulation.cpp`.
- Scene group semantics matter: `worlds/*.tile_texture_group` is the terrain palette and currently contains only `grass`, `grass1_0`, and `grass3`. `grad1`, `grad2`, and `quad` belong to `aux_textures`; putting them into the tile group makes mock chunk generation use UI/test textures as terrain.
- Generic gameplay config resources live with their owner libraries: `mood::fsm_resource`, `acumen::goap_resource` (including merge/flatten), `prefab::prefab_resource`, and `act::script_resource`. Embedded devils_script compilation crosses one `act::script_compiler` seam; `tile_frontier::script_environment` supplies the concrete `entity_scope` dispatch. The project assets system only registers these types and native building blocks.
- Actor gameplay config is fail-fast and has no production fallback: `load_required_brain_config` requires the scene-selected script/FSM/GOAP resources plus both `actor` and `food` prefabs; `actor_world_slice::init/load` require that assembled config. `world_scene_config` has no C++ value/id defaults. Resume/config/MT tests use `test_brain_fixture`, which loads the shipped module resources instead of reproducing graphs or prefab text in C++. Config-defined GOAP metrics/actions become `act::script_function` facades; the duplicate native predicate/flee/chase/think table was removed. As of 2026-07-18 ALL six actions are config-only: `eat = prey` (target scope via the `prey` object function over perception) and `seek_food`/`wander` = `set_course = chance` (ds's own deterministic RNG).
- FSM disk syntax is native TAVL rather than quoted mood lines: `state + event [guard0, guard1] / (action0, action1) = next_state`. Parentheses around actions are required so TAVL emits a tuple. `mood::fsm_resource` parses directly into owner-level `mood::transition_config`; `mood::system` has a structured constructor and does not reparse synthetic strings. The legacy string constructor remains for C++ fixtures. `fsm_config_test` covers guards/actions and `tile_frontier_config_effect_smoke` loads the shipped six-transition FSM with 1-vs-4 identity.
- `tile_frontier` actor update phase timing now uses `libs/catalogue` instead of `utils::perf`: `actor_simulation.cpp` wraps sense gather/tail build, cognition, apply, combined `integration+drives`, structural tail and `actor_batch::build` with `catalogue::domain<...>::fn_traits`. `tile_frontier_mt_benchmark` resets the store after warmup and prints avg/min/max plus wall-tick shares. Release 4096×120 measurements identified the serial kD build as the first bottleneck; `utils::kd_tree::build_parallel` now partitions a deterministic top frontier and builds disjoint subtrees in the pool. The next-tick tree is gathered after structural changes and built at the current tick tail concurrently with `actor_batch::build`; init/load uses an explicit fallback. The optimization snapshot for 1/2/4/8 was 1.304/1.248/0.937/0.865 ms/tick (speedup 1.00/1.05/1.39/1.51); a longer 4096×600 control measured 1.438/1.360/1.085/0.920 (1.00/1.06/1.33/1.56). Both retained identical hashes and bytes across worker counts.
- `libs/aesthetics/system_runner.h` owns the minimal multi-system phase contract: `run(pool,time,systems...)` submits independent range-query systems and explicit worklists; a plain serial `update(time)` or `single(fn)` becomes one indivisible pool task. It then performs one shared `compute/wait`. It is deliberately not a dependency graph: callers guarantee disjoint writes/order independence, indivisible `update` tasks must not perform nested dispatch/wait, and structural commits remain later explicit steps. Legacy `template_system_mt::update()` and `worklist_system::run()` delegate to it. The first live group is tile_frontier `integration + drives` (shared velocity read; disjoint position/stats writes), replacing two pool barriers with one.
- Catalogue MT effects (live tile pipeline, 2026-07-17): gameplay effects remain ordinary free `void(Args...)` functions; devils_script/act register `catalogue::mt::domain<IDENTITY_TAG, STRATEGY>::fn_traits<&fn,...>::fn_deferred_ptr`. Identity owns the static executor binding, while reusable policy types can be shared safely; neutral presets are `preset::{parallel_collect,serial_elect,structural_elect}`. Existing `catalogue::domain<auto>` remains the orthogonal trace value-domain. `mt::executor<Strategy>` phases are `begin_record → MT record → seal → commit` or parallel `dispatch_group` + `finish_commit`. `record_scope(source_id,dense_index)` owns TLS provenance/local ordinal; executor derives global sequence as `source_index * sequence_capacity + local_ordinal`. Storage is a dense budget-sized journal with fixed 128-byte inline payload; oversized/over-aligned signatures fail compile-time, and only nested dynamic values such as `std::string` contents may still allocate. Atomic append position has no gameplay meaning; `seal()` restores `(key, source, local_sequence, function)`. Source uniqueness is a producer/worklist invariant. Implemented policies are collect/elect with `parallel_groups`, `serial`, `serial_structural` and optional `target_not_source`; multi-participant reserve/write-set is distant backlog. tile_frontier runs local effects as parallel collect groups and eat as ST structural elect before FSM/sound. `tile_frontier_resume_smoke` checks resume and 1-vs-4 identity; `tile_frontier_mt_benchmark` requires identical hash+bytes for 1/2/4/8 workers. Catalogue is NOT replay/RPC/netcode/on-disk serialization.
- Deferred ds-effect semantics (agreed 2026-07-17): each true effect branch records independently and routes by that function's strategy; a collect `add_strength` still commits if a neighboring `eat_prey` loses elect. Multi-effect atomic/all-or-nothing groups are future explicit syntax, never inferred from neighboring ds blocks. `devils_script::on_effect` is a gameplay reaction to the recorded call, not an MT hook and not proof of successful commit; it may enqueue later work. Success-dependent reactions inspect actual post-commit components/state in a later gameplay step. `on_effect` itself may use a catalogue strategy wrapper.
- Config-loaded void actions are live: GOAP action objects accept optional `effect = <devils_script>` co-parsed into an owned container. `actor_simulation` registers an `act::script_function<void>` under the semantic action name, while `script_environment` exposes the same catalogue `fn_deferred_ptr` building blocks. All six shipped actions use this path (2026-07-18): `flee`/`chase`/`think`, `eat = prey`, and `seek_food`/`wander` = `set_course = chance`. `act::script_function::invoke/describe` seeds `vm->prng_state = utils::mix(rng_seed, rng_entity, rng_tick)`, so ds `chance`/`random` blocks are deterministic per (seed, entity, tick) with per-callsite parse salt; `act::rng_source` stays a provenance carrier for the native packer path only. The `prey` scope function materializes the target as a typed ds argument so elect arbitration sees it at record time; `actor_eating` switched from an absolute `until_tick` to a `ticks_left` countdown so the eat body needs no tick. `tile_frontier_config_effect_smoke` loads the shipped `goap/actor` resource, requires 6 scripted actions, 300-tick 1-vs-4 worker byte identity, and a nonzero eating peak (the config eat pipeline actually grabs prey).
- Registration facade (2026-07-18): `act::building_blocks` (libs/act/building_blocks.h) is the single declarative registration point for native gameplay building blocks. `effect<Traits>()` registers a catalogue `fn_deferred_ptr` into `ds::system`; `pure<&fn>(name)` registers a plain ds function; `effect_native<Traits>()`/`native<&fn>(name)` are the exceptional act-only path bypassing ds; `reg_interaction(name, desc)` attaches arbitration to the semantic name regardless of backend. The ds side registers once via `register_ds(sys)` (owner of `ds::system` calls it in its ctor); the act side replays via `install(registry&)` on every registry rebuild, and a name collision with a config script fails loudly inside `registry::reg` (no silent fallback). tile_frontier keeps one static list in `actor_simulation.cpp` (`actor_building_blocks()`); `script_environment` calls `register_ds`, `setup_brain_registry` calls `install` before `build_goap_from_config`. Covered by `act_building_blocks_test`. Note: test sources now live under `subprojects/tests/` (the old top-level `tests/` paths in this file are historical).
- Game time & generic flags (2026-07-18). Speed model is VARIABLE dt (author's decision, model B): the sim tick stays one per host frame and `game_delta_ticks` (µs of game time, already scaled/paused by `utils::timelines`) is the gameplay input. `actor_world_slice::update(uint64_t game_delta_ticks, ...)` accumulates its own `game_now()` (serialized in `sim_globals` so countdowns survive resume) and derives float dt for integration/drives. Live speed control: Lua `app.set_game_speed(1.2)`/`app.game_speed()` — the double exists only at the UI boundary; `game_host::set_game_speed(double)` converts it ONCE to a milli-precision rational (1.2 → 6/5 via llround+gcd) and the exact `set_game_speed(num, den)` core stays integer, so `timelines`' remainder-carry keeps the game clock an exact ratio of the engine clock (no per-frame float rounding drift; the recorded replay input is the rational). Multiplier sits over the nominal `settings.time` mapping (reset to nominal on runtime settings reload; zero/NaN forbidden — stopping is pause; the two-arg overload deliberately has no default denominator so a single-int C++ call is not ambiguous). Tick-based gameplay was converted to game time: `actor_eating.remaining` is a game-duration countdown and cognition cadence is `commit_game_ticks_` (150ms game = the old 3 ticks at 20fps); `think_budget_` deliberately stays a per-tick CPU budget. Consequence: at small dt (1/60s smokes/benchmark) actors re-think less often per tick, so the MT benchmark dropped to ~0.77 ms/tick (hash 0x6314d39ac4e9f875, 1/2/4/8 identical).
- `aesthetics::flag_set` — engine-generic per-entity flags with expiry (flag = string hash bound to the entity): an aggregate with a public sorted `entries` vector of `{utils::id flag, utils::game_duration remaining}` (aggregate so reflect/SERIALIZABLE_COMPONENT serialize it; `std::vector` fields work). COUNTDOWN model, not absolute deadlines — deferred ds-block bodies run at commit with no time context and reader predicates need none either; pause (dt=0) and time scale act through the game delta automatically. API: set (refresh overwrites; <=0 remaining is dead until sweep), has (expired == absent even before sweep), remove, advance(dt) (saturating subtract + erase exhausted). tile_frontier: ds blocks `set_flag = { name, seconds }` (seconds <= 0 ⇒ permanent) / `clear_flag = name` in a dedicated collect+serial_structural lane (`create<flag_set>` on demand is structural), `has_flag(name)` pure predicate; `expire_flags(game_delta_ticks)` phase after resolve_eating; live consumer: resolve_eating sets `sated` for 10 game seconds on the finished eater (post-commit fact — NOT in the eat script, since compound-script branches record independently) and drives_system freezes hunger growth while it lives. Covered by `aesthetics_flag_set_test` + config_effect_smoke (mid-run speed ×3 → pause → ×0.5 with 1-vs-4 identity, flag block parse checks).
- Events/triggers/relations contract (fixed 2026-07-18, ROADMAP session item 3). There is NO separate "event" entity (author's decision). Four forms: an effect is a catalogue deferred call; intra-tick system-to-system communication is `aesthetics::message_buffer`/`message_registry`; gameplay input (player/network) is an `act::intent` — the only replayed input; a presentation side-output follows the `sound_emit` pattern (ephemeral, never read by the sim, re-emitted on replay). Cross-tick events wait for the deferred-call queue (deferred). Relations rule: a relation is an entityid component field holding the FULL versioned id; readers must `exists()`-check; no automatic cleanup (versioned ids make dangling safe) — `actor_eating.target`/`actor_grabbed.by` are the pattern and serial already resolves references on load; generic relation containers (entity-entity, entity-faction-entity lazy matrix) are deliberately deferred as game-type-specific. Planned UI intent queue (build when a player entity exists): a UI action button emits an `act::intent` into the player intent queue with mandatory anti-spam dedup by action type (re-emitting the same type while one is pending is ignored).
- Read-only UI seam to act (2026-07-18): `actor_world_slice::ui_predicate/ui_number/ui_string/ui_describe(name, entityid)` invoke pure act categories by name in a dry-run ctx on the main thread (dedicated `ui_scratch_`, no MT lanes); the effect category is intentionally unreachable (Lua is not a mutating backend). Lua side: `app.act_predicate/act_number/act_string(name, id)` (nil = missing/wrong category/dead entity; act_string returns the loc-key hash as a 64-bit Lua integer) and `app.act_describe(name, id [, callback])` — with a callback it streams execution nodes one by one (ds `container::describe` node names; the intent is for Lua to build a small execution-graph tooltip), without it returns nodes joined with newlines. Covered headlessly in config_effect_smoke.
- Per-entity GOAP/FSM refs (2026-07-19): components `goap_ref`/`fsm_ref` hold utils::id hashes of BRAIN NAMES (resource id minus prefix), assigned at spawn via `prefab::reference<C>` rows (`goap = prey`, loud validation) and immutable (runtime switch deferred). Scene config declares brain SETS via `fsm_prefix`/`goap_prefix` (each resource = a named acumen/mood system in the slice registries); decide/apply resolve per entity, miss = loud error. Metric/action names are a slice-global act namespace: same-name registrations from multiple flattened goap configs dedup by `origin` (new goap_config field; same origin = same script, different = loud error). acumen `plan_key.system_salt` (set by registry::add) keeps shared per-thread solution_caches sound across multiple systems — without it plans (action INDICES) would leak between brains. Live world spawns `actor_prefab_cycle = [prey, prey, prey, actor]`; prey = actor_base with `disable_actions = [chase, eat]`, shared fsm/actor proves the two refs are independent.
- Positional sound (2026-07-19): `sound::task.max_distance > 0` => spatialized voice (linear attenuation in [min_distance, max_distance] world units), `0` (default) => spatialization DISABLED (UI/music play flat). Voices are pooled, so the toggle is set on every voice grant. `command_sound_play` carries volume/pitch/pos/min/max_distance; `command_sound_listener` (mailbox `sound_listener`, top-down default orientation) feeds the ma_engine listener each frame — tile_frontier publishes camera center as listener (also during pause) and emits sim sounds with min=0.25*audible, max=audible(=1.5*half_width); the old radius cull is now just a send budget aligned with the audibility edge.
- Input mapping from settings (2026-07-19): `input::bindings_config` (libs/input/bindings.h) is a tavl section `input = { actions = { camera_up = [key_w] } }` in the project settings schema (btree_map -> sorted deterministic file). The engine applies it OVER `bind_default_actions` on window creation and on reload_settings (listed action is rebound wholesale, empty list = unbind, unknown key name = warn+skip); `save_settings` dumps the effective live map back (guarded by `events::has_bindings`). Key names: keyboard canonical table + mouse_left/right/middle/mouse_4..8 + `scancode_N` lossless fallback. Lua: `app.save_settings()` / `app.reload_settings()`.
- WASD camera (first live input case, 2026-07-18): `bind_default_actions` binds the generic `camera_up/left/down/right` vocabulary to WASD (full config-driven bindings remain libs/input debt); `tile_frontier_game::move_camera` moves the camera at `half_width * 1.5` world units/sec using real frame time (presentation-plane: works during gameplay pause, unaffected by game speed) and clamps the camera point to the tile-world box (`world_extent_`); screen-up is -y in world space (Vulkan clip-Y points down — verified live). World enlarged to 8x8 chunks (128x128 tiles) in `worlds/tile_frontier.tavl`. Actors are clamped to the same world box in `integration_system` (after obstacle pushout). Camera is interpolated on the render thread: main publishes `command_draw_camera` snapshots (center/half_width/aspect/fb size + frame_time), render blends prev->cur with `nominal_clock` real-time alpha and composes the `camera_buffer` global UBO itself every render frame (main no longer writes it via write_buffer) — camera and actors share the one-snapshot presentation lag, so they stay visually in sync.
- `libs/simul/game_state.h` owns the standard main/game-thread state inherited by projects: loading/lifecycle, window/input, framebuffer policy, clocks/calendar/pause, broker ownership, visage/font handles and UI sound/device state. `game_host` owns standard UI startup from the active runtime-state, engine Lua bindings, entry-module load, config-selected default font, font GPU-view updates, device logging and sound-state merge. `tile_frontier::simulation_init` now contains only the assets-worker seam plus one `tile_frontier_game`; UI hooks forward project `perf_stats` and scene counters to that facade.
- `tile_frontier` lifecycle rule: main `simulation::init()` loads `resources/engine/config/app.tavl` (via engine demiurg registry), creates the broker, creates subsystem objects, wires `set_broker(br)` on each BEFORE starting their `std::jthread`s (no window required). Window creation is a late platform event controlled by `window.create_on_start`; when a window exists, main publishes `command_window_recreation` into the broker. Shutdown stops subsystem advancers, joins threads, destroys the window before GLFW termination, tolerates partial init.
- First render slice in `tile_frontier`: `render_simulation` owns Vulkan instance/device, `graphics_base`, `assets_base`, and `graphics_ctx`. It can bootstrap instance/device/basic render resources before a window if `main_device.tavl` exists; otherwise it waits for `command_window_recreation`, creates a surface, chooses/caches a physical device, creates swapchain/render graph, registers a test triangle mesh, fills instance/indirect buffers, and draws four test triangles using the same render config/shaders as root `main.cpp`.
- `graphics_base::recreate_basic_resources()` expects its config folder argument to end with `/` because parser paths are built as `path + "resources/"`, `path + "render_graphs/"`, etc. `tile_frontier` normalizes `render.config_folder` with a trailing slash before passing it to painter.
- Vulkan bootstrap note: with `VK_NO_PROTOTYPES` and Vulkan-Hpp dynamic dispatch, `painter::load_dispatcher1()` must initialize `VULKAN_HPP_DEFAULT_DISPATCHER` from a real `vkGetInstanceProcAddr`. `libs/input` now exposes this through GLFW as `input::get_instance_proc_addr()`. A null dispatcher here crashes as an address-0 segfault during `vk::createInstance()`.
- Vulkan device bootstrap note: `painter::device_maker::create()` initializes `VULKAN_HPP_DEFAULT_DISPATCHER` with the newly created `vk::Device` before any debug-utils object naming. `painter::set_name()` is intentionally a no-op if debug names are disabled or `vkSetDebugUtilsObjectNameEXT` is unavailable; object naming must never crash renderer startup.
- Render shutdown order matters: stop/join render thread first, explicitly drain render work in the render system, destroy `graphics_base`/swapchain, destroy surface, destroy device, then destroy GLFW window/input. `graphics_base` is a heavy resource container but does not own `VkDevice`/`VkQueue`; avoid hiding strong device/queue stalls in its destructor.
- `graphics_base` now separates parsed render-config storage from active graph instances. `set_startup_graph` seeds `resident_graphs_`; `add_resident_graph`/`set_resident_graphs` define graphs that may live at the same time. `commit_parsed_resources` builds active masks over resident graphs, creates only their resources/descriptors, and unions Vulkan usage from active graph steps plus active descriptor layouts. More than 256 graphs/resources/descriptors hard-errors via `std::bitset<256>` masks until a larger policy is designed.
- Render graph switching is staged: `create_render_graph_instance(index)` builds a new graph instance off to the side, `change_render_graph(index)` builds the target, then syncs with **`wait_all_fences()`** (per-frame fences of this `graphics_base`, NOT device/queue-wide waitIdle — fences are created signaled so the initial call is instant), swaps the active `execution_graph`, and clears the old graph-local objects. No resource recreation on swap: the used-set is the UNION of resident graphs, so pipelines rebuild (from assets-prepared SPIR-V + cache) but resources/descriptors stay. A few-frame pause is acceptable. Runtime swap is exercised via `command_set_active_graph` (config `render.demo_graph_toggle_ms` toggles graphics1↔menu1 for demo). `render_graph_instance::clear()` frees command buffers and graph-local semaphores/steps; `graphics_base` shutdown clears graph/descriptors/resources before command pool/swapchain/cache.
- `tile_frontier` render modes are runtime config driven: `render.enabled = false` skips render thread creation entirely, while `render.headless = true` allows a no-present render state without window/surface attachment. Window creation and `command_window_recreation` are only sent when render is enabled and not headless.
- Headless Vulkan bootstrap must not touch GLFW/input. `painter::load_dispatcher1(false)` loads `vkGetInstanceProcAddr` from the system Vulkan loader directly and `painter::get_required_extensions(false)` omits GLFW surface extensions. Headless device selection uses `system_info::choose_physical_device_headless()` and does not require presentation support or `VK_KHR_swapchain`.
- Root CMake defaults single-config generators to `CMAKE_BUILD_TYPE=Debug` when the user does not specify a build type. It also enables `CMAKE_POSITION_INDEPENDENT_CODE` globally so static third-party dependencies can link into shared libraries in Debug.
- New resource/render-prep contract tests: `tests/demiurg_resource_loader_test.cpp` covers CPU prepare vs external GPU commit and dependency gating; `tests/painter_shader_prepare_test.cpp` covers `glsl_source_file` SPIR-V preparation cache. Useful checks: `ctest --test-dir build-debug -R "(painter_shader_prepare_test|demiurg_resource_loader_test)" --output-on-failure` and `cmake --build build-debug --target tile_frontier`.

### Act scratch, generic stats, timelines/pause, and AI registries (2026-07-13)

- `act::call_context` has exactly ds's standard 8 scalar argument slots inline (`static_assert == 8`),
  plus reusable named lists/result. `act::execution_scratch` owns one ds VM + call frame and is embedded
  per worker rather than allocated locally or distributed globally/TLS. `acumen::execution_scratch`
  composes it with A* container + solution cache; tile_frontier holds one stable deque lane per pool slot.
  Script bind/collect works for bool/integer/number/entity basics; string/object/vector policy is pending.
- Generic numeric aggregate stats use real ds scopes, not prefixes. `register_stats<T,Parent,Getter,Domain>`
  registers the named scope getter and reflected local `field`/`add_field` functions. Scripts use
  `stats.hunger`, `stats = { add_hunger(…) }`, `combat_stats.abc`; ds overloads identical field names by
  returned `stat_scope<T>`. `initialize_stats<T>()` selects C++ default member initializers; its callback
  overload assigns every reflected numeric field. `stats_script_test` covers both modes and two aggregates.
- Time has four orthogonal coordinates: engine (never paused), presentation (real-rate world animation,
  pausable), game (pausable + rationally scaled), and turn (discrete). `game_time_scale` maps DURATIONS
  only; absolute timestamp conversion is intentionally absent because pause is non-bijective. Config
  convention: unqualified duration means nominal real/engine time; gameplay loaders convert through the
  project scale, while explicit game/calendar/turn values keep their domain. `simul::pause_state` owns
  gameplay/presentation masks; tile_frontier exposes Lua `app.set_paused/paused`, configures the ratio as
  `time.game_seconds / time.real_seconds`, and feeds actor systems the scaled game-clock delta. Calendar is
  a separate typed domain: immutable `calendar_clock` selects `time.calendar.source = game_time|turn` once
  at project startup; turn steps support seconds/days/months/years and are derived from the configured epoch.
- `acumen::registry` and `mood::registry` independently own stable immutable systems. There is no combined
  `brain_ref`: FSM may exist without GOAP and GOAP without FSM. `acumen::goap_resource` supports a
  single `base`, same-key replace-in-place, append, `disable_metrics/actions/goals`, cycle detection and
  flatten before `acumen::system` construction; `goap/actor` now inherits `goap/actor_base`. Compatibility
  contracts and runtime GOAP profile switching remain long-running. Tests: `act_call_context_test`,
  `stats_script_test`, `timeline_test`, `simul_lifecycle_test`, `goap_config_test`, `acumen_test`.

### Prefab & spawn (tile_frontier ↔ `libs/prefab`, 2026-07-12)

- `libs/prefab` (`devils_engine::prefab`): engine mechanism `prefab_registry<SpawnArgs>` = recipe
  of components. Forms: `data<C>` (tavl-deserialize, field-level inheritance via `base`), `list<C,Item>`,
  `callback<C>` (name→`act` fn hash), `reference<C>` (name→component via project resolver — fsm/goap by name),
  `custom` (project builder sees raw value text — inline-ds, custom list-inserters). Global + per-prefab
  `on_construct(name, fn)` for DERIVED components; `spawn(name, world, args)` = spawn_at (args → construct).
- tile_frontier: `food` and `actor` both spawn through the registry. Prefab TEXT comes from disk via
  owner-level `prefab::prefab_resource` (`prefab/*.tavl`; collected by `load_required_brain_config` → `brain_config.prefabs`);
  C++ registers component specs + construct in `setup_brain_registry` (shared init/load point); missing prefab
  resources or the required `actor_tuning`/`food_item` data fail loudly. `actor_tuning` (data component, NOT serialized) carries config knobs (speed/hunger/strength
  ranges); construct derives seed-based brain/visual/stats. Determinism: `SERIALIZABLE_COMPONENT` pins type_ids at
  static-init so the extra unregistered `actor_tuning` neither shifts registered ids nor enters `dump_world`.
- **GOTCHA:** a `//​---` literal in a `prefab/*.tavl` comment triggers demiurg's list splitter (`resource_manifest`
  splits `.tavl` on `//​---`) — the file breaks into sections and the prefab name becomes `name:0`. Never write
  three dashes after `//` in tavl comments.
- Primitive `spawn_at` is registered in `devils_script`: native `spawn_at(prefab, x, y)` over `spawn_scope`
  (carries a mutable `spawn_sink`, implemented by `actor_world_slice::spawn_prefab`). Bareword prefab → `string_view`.
  `tests/spawn_script_test.cpp` covers it via a mock sink. Seeding `spawn_scope` into live event/trigger scripts
  comes with the spawner work below.
- **TECH DEBT (deferred, author-confirmed 2026-07-12 — build later):**
  1. **per-entity system refs** — common independent acumen/mood registries are ready, but tile_frontier
     still selects one GOAP and one FSM for the whole slice. Do NOT introduce a combined brain_ref: use
     independent optional goap_ref/fsm_ref only where needed. FSM is normally fixed by entity type; GOAP may
     also belong to a squad/abstract thinker with no FSM. Compatibility validation + runtime GOAP switching
     (including per-profile caches/action indices) are a separate long-running task.
  2. **spawner entities + query (explicitly deferred again 2026-07-13)** — real spawn "where" = spawner ECS entities (static designer points OR dynamic
     rules), many of them, queried spatially (reuse `kd_tree` like perception) + predicate-filtered (tags, cooldown,
     capacity). `spawn_at(point)` stays the primitive; a selection/placement layer resolves intent → point.
     Determinism: spawn = catalogue effect on sim rng+state; "off-camera" is presentation (MP: sim must not read a
     local camera — use a replicated region/all-players notion).
  3. **ds `spawn`/`filter`/`pick`** — ergonomic script composition (`spawn(horse, pick(filter(spawners, mount, off_camera)))`);
     triggers = per-entity reference-list of spawners + on-enter ds script; dynamic spawner = point source is a ds/native fn.

### Message broker (inter-actor channels)

- All inter-thread messaging goes through one `core::broker` (`broker.h`), owned by main, created in `simulation::init` BEFORE subsystems and handed to each via `set_broker(br)` BEFORE its thread starts (ordering: create → set_broker×3 → threads). Each channel is strictly 1-producer/1-consumer. Primitives live in `libs/utils/include/devils_engine/thread/`:
  - `mailbox<T>` — latest-wins triple-buffer: producer fills `write_slot()` IN PLACE (T's capacity, e.g. a `std::vector`, is reused across the 3 slots → no per-frame alloc), `publish()`; consumer `consume() -> const T*|nullptr`. drop-oldest is normal. One atomic. Use for snapshot/latest channels.
  - `spsc_queue<T>` — fixed-capacity FIFO ring, move-aware, overflow = drop-newest (`try_push` returns false). Use for reliable commands + lossy bursts.
  - `payload_channel<Msg>` — `spsc_queue<Msg>` + `byte_ring` for messages with a byte payload. `write(size, fill)` where `fill(region,pos)->Msg` (channel owns alloc+push atomicity: queue-full check BEFORE `byte_ring::alloc`, else arena leaks); `drain(handler)` where `handler(const Msg&, span<const std::byte>)` then FIFO-release. Msg MUST carry `int64_t pos; uint32_t size;`.
  - `byte_ring` — SPSC bip-buffer, MONOTONIC positions (padding at wrap self-reclaims), single atomic (`tail`); payload-byte visibility piggybacks on the MESSAGE queue's release/acquire (payload written before `push`). Reclaim is a monotonic cursor because consumption order == allocation order — arena is PER-CHANNEL, never per-edge (multiplexed edges break the cursor). Payload valid only until `release`; copy out if kept longer.
- Channel policy by semantics (each channel in `broker`): latest-wins → `mailbox` (window_recreation, set_active_graph, draw_tiles, draw_actors, shaders_prepared, sound_state); reliable payload → `write_buffer_channel` (camera/UI); reliable/lossy FIFO → `spsc_queue` (gpu_transition/gpu_done/prepare_shaders/load_resource/load_chunk/chunk_loaded/sound_play/stop/update/devices/recreate_sound). Budgets are FIXED in the broker ctor (256 reliable, 64 sound-lossy, 8 one-shot, write_buffer 64 msgs + 1 MiB arena).
- Payload budget note: the two biggest per-frame payloads (draw_actors ~92 KB, camera+UI via write_buffer) now allocate ZERO per frame — mailbox slot vectors and the byte_ring are preallocated and reused.
- Tests for the primitives: `tests/thread_general_test.cpp` (byte_ring wrap/reclaim/overflow, payload_channel FIFO+overflow, mailbox latest-wins + slot-capacity reuse), alongside the existing `spsc_queue` tests.
- Deferred (see memory `message-broker-design`): segmented runtime growth for reliable channels (current overflow is drop-newest at fixed capacity). The dead actor/dispatcher baseline cleanup is complete; `actor_ref`/`actors.h` remain locally inspectable under ignored `exclude/`. Deviation: assets `gpu_transition` producer now unconditionally `try_push`es (benign — loader marks jobs in-flight so ≤1 push/resource; with render off resources just stay warm).

### Tile map / world slice (current WIP)

- First world/tile slice is now wired through the existing actors. `tile_map.{h,cpp}` owns the main-side CPU model: `tile`, `tile_grid`, `chunk_coord`, `tile_chunk`, `camera2d`, `visible_tiles`, plus `generate_mock_chunk` and `apply_chunk`. It is intentionally painter-free.
- Mock world streaming uses broker channels, not direct calls: main pushes `command_load_chunk{x,y,size,texture_count}` into `broker.load_chunk`; assets generates a deterministic CPU `tile_chunk` on the assets thread and pushes `command_chunk_loaded` into `broker.chunk_loaded`; main drains it and applies into the 4x4 chunk grid (16x16 tiles each). (Replies go to fixed broker channels — no `reply_to` field anymore.)
- Main-side render path for the map: `simulation::update()` computes `visible_tiles(cam, grid, margin=1)`, `tile_batch` packs `tile_instance{world_center, texture}` as layout `v2ui1` directly into `broker.draw_tiles.write_slot().bytes` (in-place, reused), writes `global_ubo_t` to `camera_buffer` via `broker.write_buffer`, then `broker.draw_tiles.publish()`. draw_tiles/draw_actors are latest-wins **mailboxes** (render `consume()`s the freshest snapshot; stale ones drop) — see Message broker.
- Render-side tile path: render config has `dg_tiles` (host_visible, layout `v2ui1`) and `draw_tiles` before `draw_ui`; shaders are `tests/shaders/tile.vert.glsl` and `tile.frag.glsl`, material is `tests/test_render_config/materials/tile.tavl`. `render_create_tile_draw()` creates one GPU `tile_quad` mesh and registers one pair with max 5000 instances; `render_update_tile_draw()` writes BOTH per_update instance/indirect buffers each update.
- Tile textures currently reuse the asset texture descriptor `textures`; `tile_instance.texture` is the texture array index. This is still a bootstrap path, not the final terrain/material system.
- Minimal actor slice is visible on screen. `actor_simulation.{h,cpp}` owns the first lightweight ECS slice: `actor_position`, `actor_velocity`, `actor_brain`, `actor_visual`; deterministic brains produce sorted `actor_move_intent`s, apply mutates positions, and `actor_batch` packs `actor_instance{pos, texture, color, size}` as layout `v2ui1c4v1` (`color` is packed `c4` RGBA8, `size` is 32-bit float via `v1`).
- Main publishes `command_draw_actors` into `broker.draw_actors` (mailbox) every update after tiles, filling the slot's `bytes`/`ids` in place (reused). Render config has `dg_actors` (host_visible, layout `v2ui1c4v1`) and `draw_actors` after `draw_tiles` and before `draw_ui`; shaders are `tests/shaders/actor.vert.glsl` and `actor.frag.glsl`, material is `tests/test_render_config/materials/actor.tavl`. `render_create_actor_draw()` creates one GPU `actor_triangle` mesh and registers one pair with max 5000 instances; `render_update_actor_draw()` writes BOTH per_update instance/indirect buffers.
- Actor render gotchas already hit: if `actor.vert.glsl` declares an input such as `in_tex` but shader optimization removes it, validation reports `Vertex attribute at location N not consumed by vertex shader`; keep declared vertex inputs observably used. Actor quads were also invisible when their world Z was `0.08`: with the current `glm::ortho(..., -1, 1)` / Vulkan clip convention they clipped behind the tile layer. Use the same world Z as tiles (`0.0`) unless the camera depth convention is deliberately fixed.

### Assets / resource pipeline (tile_frontier ↔ demiurg ↔ painter)

- **Stable handles (2026-07-06, `11a4e93`/`8ce23c1`):** the durable resource currency is now `demiurg::resource_handle` = `{const resource_system*, utils::id hash}` (hash of the logical id, NOT a pointer); `handle.get()` = `system->get(hash)` (O(1) `resources_by_hash`), `get<T>()` checks `is_type(type_id<T>())` = exact `type_id` OR `loading_type_id` (split 2026-07-06: `type_id` = exact C++ identity, always set by `register_type`; `loading_type_id` = loader-dispatch key, may be a BASE — so `get<visage::font_resource>()` and `get<painter::gpu_texture_resource>()` both work on a font), `operator bool`. It SURVIVES `clear()`+re-`parse_resources()` — the same id re-resolves to the freshly-instantiated object (`rebuild_hash_index()`/`register_hash_key()` run at the end of parse/append; `register_hash_key` errors on a true hash collision, warns+keeps-first on duplicate id). Make one via `resource_system::handle(id)`/`handle(hash)`/static `resource_hash(id)`. Consumers migrated: `flow::image_ref`/`sprite_sample` hold a handle; tile_frontier's inter-thread commands (`command_sound_play`/`command_load_resource`/`command_gpu_transition`/`command_gpu_done`) carry a `resource_ref` wrapper (`messages.h`) = handle + `direct` raw-pointer fallback (factories `from_handle`/`from_direct`/`from_system`; `from_system` only keeps `direct` if the id doesn't round-trip). `direct` exists for SYNTHETIC resources built outside any registry (none in-tree since fonts moved into the assets registry 2026-07-06). A bare `resource_interface*` is still fine for same-thread, same-lifetime reads (assets registry is built once, read-only), but cross-thread/persistent references should ride a handle/`resource_ref`.
- Resource currency was `demiurg::resource_interface*` (NOT a string id); see stable-handle bullet above. State machine in atomic `_state` is a GENERIC LEVEL ladder `0..top_state()` (was fixed cold/warm/hot). Defaults: cold(0)/warm(1)/hot(2), `top_state()`=hot. A resource with more steps overrides `top_state()` + `load_step(from)`/`unload_step(from)` (default dispatch to named `load_cold`/`load_warm`/`unload_*` for the 3 base rungs) + `is_external_step(from)` (which transition runs on the render/GPU thread). `state()` returns the raw int32 level (NO warm→hot remap anymore); `usable()` = `_state >= final_state()`; `final_state()` = `warm` if flag `warm_and_hot_same` else `top_state()`. Example multi-step: `tile_frontier::font_resource` (4-state, ttf→MSDF→GPU) — see font bullet below.
- `demiurg::resource_interface::dependencies` (a `std::vector<resource_interface*>`, filled by the resource type via `add_dependency`) — NOT the ring-lists (those are for mod override chains). The loader drives the whole transitive closure and gates a resource's UP-movement until all its direct deps are `usable()` (transitive correctness holds by induction since a dep isn't usable until ITS deps are; shared deps dedupe). INVARIANT: a resource with deps MUST be driven through the loader, not `res->load()` directly (direct load bypasses gating). Assumes a DAG.
- `demiurg::resource_loader` (lives ONLY in the assets actor) is a reconciler: `request(res, target)` sets desired LEVEL (clamped to `res->final_state()`; also recursively `request`s deps) ; `update(out)` steps each ready resource one transition toward its target. A transition is local (disk/CPU, run inline) unless `res->is_external_step(from)` → emitted as `external_job{res, load}` for the render actor (GPU table is render-owned — avoids cross-thread sync). Currently SINGLE-THREADED per tick (one step per ready resource); planned future: inject a `thread_pool` for level-load bulk fan-out + a per-tick budget for gentle background loading (API already accommodates both). Heavy render resources should be staged: assets may do CPU-heavy prepare/compile in local steps, while external steps publish/commit render-owned GPU handles. Creating `VkPipeline` off-thread is technically possible with `VkDevice`/cache sync, but the default design keeps `VkPipeline`, layouts, render-pass dependencies, and destruction on the render side.
- Contract: main holds `resource_interface*` from the assets registry (built once in `assets_simulation::init` via `resource_system::parse_resources`, then read-only) and pushes `command_load_resource{res, res->final_state()}` into `broker.load_resource`. assets runs local (CPU) steps, pushes `command_gpu_transition{res,load}` into `broker.gpu_transition` for external steps; render runs `res->load(safe_handle_t(&gpu_load_context))` → the external step (e.g. `load_warm`/`load_step`) does the GPU upload, writes `gpu_index`, frees the CPU copy, advances `_state`, acks `command_gpu_done`. main observes `usable()` + reads `gpu_index`. (Send `final_state()`, NOT `state::hot` — multi-step resources like font have a higher top level.)
- Common resource loaders now live in their OWNER libs, not tile_frontier: **painter** has `gpu_texture_resource` (base: RGBA `memory`+w/h+`gpu_index` + `load_warm`/`unload_*` GPU upload; `load_cold` no-op), `texture_resource : gpu_texture_resource` (png/stb `load_cold`; painter gained the stb-src include + `stb_image_impl.cpp`), `mesh_resource`, `gpu_load_context.h`. **visage** has `font_resource : painter::gpu_texture_resource` (ttf→MSDF CPU steps fill base `memory`, GPU step reuses base `load_warm`; NO longer `: texture_resource` — png/stb not needed; visage gained a `demiurg` dep). Generalization: textures register as `register_type<gpu_texture_resource, texture_resource>(...)` so `loading_type_id` is the BASE — render's texture check, `texture_set`, and `get<>` all work through `gpu_texture_resource` (only need `gpu_index`), unaware of the concrete png decoder. `gpu_index` = the asset's slot in `assets_base`. tile_frontier keeps only app-specific `app_config_resource` + `texture_set`.
- demiurg GOTCHA: `resource_system::find_proper_type` matches a registered type NAME against a path SEGMENT of the resource id — resources MUST live in a type-named folder. e.g. type `register_type<mesh_resource>("mesh","mesh")` needs the file at `<module>/core/mesh/test.mesh` (id `mesh/test`); a bare `core/test.mesh` is skipped. Module layout: `module_system(root)` looks for `root/core/` (or `core.zip`); call `resource_system::parse_resources(module_system*)` (it does open/parse/close + finalize), not `module_system::parse_resources`.
- `copy_directory` in CMake POST_BUILD copies but does NOT delete files removed from source — stale assets linger in the build output (e.g. a moved mesh). Clean the output dir if a phantom file warning appears.
- **Engine (non-replaceable) registry** — SEPARATE `resource_system`+`module_system` from the game/mod registry (Q2: don't mix; mods get free path namespace). Root `resources/engine/`, loaded as a module directory via `module_system::load_modules({list_entry{"engine",...}})` (no new demiurg API needed). `tile_frontier::simulation::init` builds it and preloads: `app_config_resource` (`config/app`, CPU-only, `load_cold` parses `app.tavl` via tavl → replaces old raw-`file_io` `load_app_config`, removed) and `painter::render_config_source` (`render_config/*`, CPU-only text of each render-graph tavl file). CMake copies `tests/test_render_config` into `resources/engine/render_config/` (source unmoved — shared with fast_test/tests).
- **Render-graph loads through demiurg** (not a folder scan): `graphics_base::recreate_basic_resources` has two overloads — `(folder)` (fs, fast_test) and `(const demiurg::resource_system*, prefix)` (engine registry). `structures.cpp` `parse_data` is now content-source-agnostic (a `config_source{read_file,read_folder}` seam with fs + demiurg builders); `parse_config_content<T>(text)` unchanged. tile_frontier passes the engine registry + `"render_config/"` prefix; render config resources are preloaded to `warm` on main at init (render thread only reads them). `commit_parsed_resources` holds the shared post-parse tail.
- **Prepared shader/pipeline path (first slice)**: shader compilation moved to assets-side preparation. `painter::glsl_source_file` keeps source `memory` plus prepared `spirv`/`spirv_shader_kind`; `prepare_spirv(reg, kind)` compiles with `shader_crafter` and demiurg include resolution. Assets drains `command_prepare_shaders{registry,prefix}` from the broker, compiles engine GLSL resources, then publishes `command_shaders_prepared` into the broker mailbox; render delays `change_render_graph()` until that ack and then only creates `VkShaderModule`/`VkPipeline` from prepared SPIR-V. `load_shader_module()` still has a synchronous render-thread compile fallback with a warning, but normal `tile_frontier` startup should use the assets path. This is not yet a full `prepared_pipeline_resource`: material/geometry/render-pass pipeline assembly remains render-owned.
- **`visage::font_resource`** (`: painter::gpu_texture_resource`, moved from tile_frontier, replaced `font_atlas_resource`) — the reference multi-step resource: `top_state()`=3, `load_step` 0→1 reads ttf via `module->load_binary(path)`, 1→2 runs `font_atlas_packer` (MSDF atlas RGBA into base `memory` + glyph `font_t`), 2→3 reuses base `load_warm` (GPU); `is_external_step` true only for 2→3. Since 2026-07-06 fonts are REGULAR assets-registry resources: `register_type<painter::gpu_texture_resource, visage::font_resource>("fonts","ttf")` (files in `resources/modules/core/fonts/`, moddable like textures; `loading_type_id` = base → render treats the atlas as a texture, exact `type_id` → `handle.get<font_resource>()` works). The runtime-state allowlist drives their CPU/GPU steps; `game_host` creates visage from the selected default font's CPU metrics and `standard_game_state::ui_fonts` tracks when render writes each `gpu_index` so host can stamp `font_t::texture_id`. Lua selects a font by HANDLE: `nk.push_font{ font = request("fonts/crimson.italic"), ... }` — the old string-name registry (`add_font`/`resolve_font`/`fonts_`) was removed from `visage::system`. Legacy `visage::load_font(painter::host_image_container*,…)` + its freetype/msdf helpers were removed; the obsolete `arbitrary_image_container` pair is archived in `exclude/`.
- **Engine cache registry merged (2026-07-04):** the pipeline cache is no longer a separate `resource_system`; its MODULE stays separate (rooted at `<cache>/painter/`) but its resources are appended into the shared engine registry via new `demiurg::resource_system::append_resources(module_system*)` (ingest + add only new to the sorted index, dedup via `get()`; does NOT re-run override/ring dedup — for disjoint sub-registries). Needed because `parse_resources` clears and `cache_folder` is only known after the config parse.

### Texturing / render-graph descriptors (painter)

- `painter::sampler` is a render-graph type (config `samplers/*.tavl`: name, filter, address; VkSampler created in `graphics_base::create_samplers` before descriptor layouts). `descriptor.layout` entries are `(slot, usage, sampler_index, shader_stages)`; when an entry has a sampler the binding becomes `eCombinedImageSampler` with an IMMUTABLE sampler baked into the layout (so no per-frame sampler buffering needed). Config descriptor layout entries are `{resource, usage, sampler, stage}` structs (stage parses `fragment|vertex|...|all`).
- Asset textures are NOT render-graph resources. A descriptor declares a `texture_count`/`texture_sampler`/`texture_stage` asset-texture binding (combinedImageSampler ARRAY, binding index = `layout.size()`); `update_descriptors` skips it and the render actor fills views from `assets_base.texture_slots` (slot index = `gpu_index`). A draw step binds descriptors via its `sets = [name]` config list (set 0-based in list order); `dg<n>.descriptor` is NOT auto-bound, instance data is a VERTEX BUFFER (binding 1).
- Bindless table is sized 4096 (`descriptors/textures.tavl` `texture_count`), clamped in `create_descriptor_set_layouts` to device limits (min of maxPerStageDescriptorSampledImages/Samplers + set-level). `graphics_base::recreate_descriptor_pool()` (called in commit before `create_descriptor_sets`) sizes the pool from the ACTUAL descriptors (fixed 256/type couldn't hold 4096×frames → `vk::OutOfPoolMemoryError`). **Every array element must be a valid view or validation fires `VUID-vkCmdDrawIndirect-None-08114` on the first draws (before textures load).** Fix: `assets_base::default_texture` — a permanent 1×1 magenta placeholder created at render init (OUTSIDE `texture_slots`); `render_bind_textures` fills ALL N elements with it once at graph-ready, and each texture load does a POINT update (`render_bind_texture_slot`, `dstArrayElement=slot,count=1`) instead of rewriting the whole array.
- `assets_base` slot state is a PLAIN `asset_state` (atomics removed 2026-07-04): `assets_base` lives strictly on the render thread; cross-thread index publication rides `demiurg::resource::_state`, so the per-slot atomics were dead weight.

### Painter gotchas hit while wiring textured draws (Linux/Intel windowed path was undertested)

- `descriptor_set_layout_maker::combined` stores `pImmutableSamplers = samplers.data()` — the sampler vector MUST outlive `create()` (keep it in a container, don't pass a loop-local temporary, or the driver derefs freed memory and crashes).
- `image_acquire` used a 1ms `acquireNextImageKHR` timeout → near-certain `Timeout` crash under vsync (~16ms/frame); now a generous finite timeout.
- Texture upload barrier in `assets_base::populate_texture_storage`: transitioning to ShaderReadOnly needs a dst stage that supports `eShaderRead` (`eAllCommands`), not `eTransfer` (valid only when graphics+transfer share a queue family).
- Draw-group instance/indirect buffers for a host_visible draw group are DOUBLE-buffered (per_update). Static data written asynchronously while the render loop is running can land in the wrong buffer after `update_event()` shifts parity — write to BOTH buffers (`get_current_*_resource_frame(pair, off)` for off in {0,1}).
- `register_pair(dg, mesh, max_count)` strides each pair's indirect offset by `max_count * indirect_buffer_size`; the indirect buffer is small, so a large `max_count` across multiple pairs overflows it (VUID-vkCmdDrawIndirect-00487). Keep `max_count` modest, or enlarge the indirect buffer / stride by one command per pair.
- Descriptor buffering gotcha: use `resource::compute_buffering(base)`, not `static_cast<uint32_t>(resource.type)`, when creating descriptor set layouts or writing descriptor arrays. The enum value is not the runtime buffering count. `VkDescriptorBufferInfo::range` also must be clamped to the actual remaining bytes in the packed `resource_container` from `subbuffer.offset`, otherwise validation reports VUID-VkDescriptorBufferInfo-range-00342.
- Render-graph buffer packing now distinguishes logical frame size from aligned frame stride. `graphics_base::create_resources()` aligns buffer suballocation offsets by final VkBuffer usage: uniform buffers use `minUniformBufferOffsetAlignment`, storage buffers use `minStorageBufferOffsetAlignment`, and texel buffers use `minTexelBufferOffsetAlignment`; descriptor `range` stays the logical `subbuffer.size`.
- Render-graph layout strings support `mat4` and `mat44` aliases. `parse_layout` expands either alias into four `v4` atoms; they are layout macros, not real `VkFormat`s/vertex attributes. Use them for buffer layouts such as `mat4mat4v4v4v4v4`.
- Render-pass barrier gotcha: `vkCmdPipelineBarrier` is illegal inside a render pass subpass unless the render pass has a matching self-dependency. `execution_pass_instance::process()` must apply `pass.barriers[0]` BEFORE `beginRenderPass()`. Avoid adding ordinary `step.barriers` to draw steps inside render passes unless painter grows a pre-pass barrier phase or self-dependencies; `draw_ui` already intentionally skips `make_barriers1()` for this reason.
- KNOWN-OPEN (non-fatal warnings): present semaphore is indexed per-frame-in-flight, not per swapchain image (VUID-vkQueueSubmit-00067 — fix: per-image present semaphores); swapchain images created with only TRANSFER_DST but a view needs attachment usage (VUID-04441). (`VUID-vkCmdDrawIndirect-None-08114` was FIXED via the default placeholder texture — see Texturing section.)

### Sound / miniaudio contract

- `libs/sound` is miniaudio-only as of 2026-08-12. The legacy OpenAL implementation (formerly also named `sound::system`), `al_helper`, decoder-to-OpenAL-buffer overloads, OpenAL link/DLL wiring and completed A/B executable were removed from the live tree and archived under `exclude/openal_sound_legacy/` plus `exclude/audio_spatial_lab_openal/`. After selecting the backend, the live miniaudio class was promoted from `sound::system2` to the canonical `sound::system`; there is no live `system2` compatibility alias.
- `sound::system` owns a miniaudio context/device/engine plus precreated mono spatial and stereo non-spatial voice pools. Mono voices are intended for positional `sfx` / `talk_pos`; stereo voices are intended for UI/music/non-spatial playback. Positional sound with miniaudio should generally use mono sources.
- `system` uses a custom miniaudio data source as a PCM ring stream. That data source should stay dumb: byte/frame ring-buffer state, read/write cursors, `frames_read_total`, `frames_written_total`, underrun count, and format info. It must not know about `task_id`, `after`, or higher-level sound scheduling.
- **Sound resource/task pinning (2026-08-12):** each `sound_resource` atomically publishes an immutable shared `resource_blob`. Normal Lua/tile producers take the pin before enqueueing `command_sound_play`; `resource2` and the copied `system::task` retain it through queued/active/draining lifetime. `unload_warm()` only drops the resource's reference, so an in-flight command/task keeps the old bytes and id alive while new views become empty. The fallback handle resolution path is compatibility-only. `sound_system_test` covers unload while queued/active data remains readable.
- **Spatial audio A/B verdict and `AUD-17` (2026-08-12):** the archived `audio_spatial_lab` compared production miniaudio with direct OpenAL Soft on matched PCM, listener, attenuation, constant-radius orbits and front/up distance pulses. Front `-Z` and up `+Y` attenuation both work; the apparent OpenAL change on the vertical orbit was a very small direction-dependent coloration, not distance drift. Overall miniaudio sounds almost the same as the remembered historical OpenAL path (which likely did not enable HRTF), so miniaudio is final for current production. The accepted optional coloration computes smooth listener-relative behind/above/below weights, drives one post-spatialization high-shelf custom miniaudio node per mono voice, and never changes distance gain. It is default-off; the final subtle profile is behind `-2.25` dB, above `+0.65` dB and below `-0.85` dB at `2.5 kHz`/`80 ms`, with a global strength `[0,2]` defaulting to `1`; final response stays bounded to `[-3,+1]` dB. Headphone listening found the behind cue perceptible but elevation practically unreadable: one shared shelf is not an HRTF/pinna-notch model, so do not keep increasing it to solve height. `tile_frontier` publishes the profile from reloadable sound settings but keeps it disabled because its listener is top-down. `subprojects/playgrounds/AU01_spatial_audio` documents the archived A/B without reviving OpenAL; `subprojects/playgrounds/AU02_directional_coloration` owns the live deterministic production-path executable with `--coloration off/on`, `--strength`, constant-radius headphone passes, alternating above/below holds and continuous harmonic-hum/noise signals. Full HRTF via Steam Audio is a distant `submarine_coop` pre-release evaluation, not current engine debt.
- **Aligned GLM debt (2026-08-12):** consider `GLM_FORCE_DEFAULT_ALIGNED_GENTYPES` only as `BLD-03`, with an ABI/layout audit first. It changes default vector alignment/padding and can affect serialized structs, broker payloads, vertex/instance data and foreign API boundaries; enabling it is not a blanket substitute for profiling or SoA SIMD. The compact sound `vec3` remains unchanged.
- Higher-level sound state lives in `system::sound_task`: task id, resource view, decoder/converter, segment begin frame, segment length, decoded frames, and started/initialized flags. `stat_sound` and `snapshot` report `progress` as the ABSOLUTE normalized position in the SOURCE `[0,1]`, i.e. `(start_frame + local_frames) / source_frames_count` where `start_frame = source_frames_count - stream_frames_count`; before playback it is `task.start`. (It used to be segment-relative `local/stream_frames_count`, which made a sound started at `start=0.5` report 0% and advance at 2× — fixed 2026-07-01; absolute position is what a player seek bar needs.)
- `task.after` means gapless continuation on the same voice. The sound thread registers the next segment only after the previous segment has been fully decoded into the shared stream, so the callback continues reading PCM without task-aware branching. Late `after` messages can still miss the practical gapless window if the stream underruns.
- `system` supports `task.start` as normalized `[0,1]` start position, `update_sound(task_update)` for long positional sounds that need refreshed position/direction/velocity, `snapshot(vector<task_status>&)` for a full current task slice, and basic setup filters (invalid id/resource/type, duplicate task id, positional sounds beyond max distance).
- Decode work per sound update is budgeted by `decode_frames_per_update` and also bounded by ring free space, remaining segment frames, and scratch cache capacity. `stream_buffer_seconds` controls precreated ring-buffer duration; if the sound actor runs slower than the audio buffer coverage, underruns are expected.
- `system::playback_devices(out)` enumerates playback device names. `system(device_name, ...)` tries to create that device; if the requested name is not present in the current list, it logs `utils::warn` with the requested name and the default playback device name, then creates the default device.
- **Sound is a demiurg resource (2026-07-04, staged/compressed):** `sound::sound_resource : demiurg::resource_interface` is CPU-only (`warm_and_hot_same`); `load_cold` reads bytes via module, infers `data_type`, fills metadata and decodes short sounds to PCM. Managed by the ASSETS thread like mesh/texture; sound files live at `resources/modules/core/sounds/<sub>/`, registered for `mp3,flac,wav,ogg,opus`.
- Sound messages (`messages.h`): `command_sound_play{taskid,after,res,resource_pin,start}`, `command_sound_stop{taskid}`, `command_sound_update{taskid,pos/dir/vel}`, device/recreate/gain commands (main→sound), and state snapshots (sound→main). Play/stop are separate types. All flow through broker channels.
- `sound_system` does not preload assets. Normal producers resolve a typed handle while warm, take its immutable pin and enqueue both identity and pin. The sound actor skips an empty/not-ready generation; it only resolves the handle as a compatibility fallback when an older/internal producer omitted the pin.
- `sound::resource2` owns its immutable bytes/id when `owner` is set. Manually assembled non-owning views remain supported only for local fixtures with an external lifetime guarantee.

### UI sound API & cross-thread state delivery (tile_frontier)

- Lua UI scripts reach sound through a host-registered `app` namespace in the visage sandbox (older `tf_*` globals are being phased out): `app.play_sound(res [, {start=0..1, after=handle}])` — as of `d2ce7fe` `res` is a demiurg `resource_handle` from `request(...)`, NOT a string name (also one-table form `app.play_sound{ resource=…, start=, … }`) → opaque `sound_handle`; `app.stop_sound(handle)`; `app.sound_state(handle)` → `progress(0..1)` or `nil` (a number means "in progress", nil means "gone"). Each is just a normal message to the sound thread; presentation sounds are NOT in the replay log.
- visage stays UI-agnostic: it exposes `system::script_state()` / `script_env()` so the HOST registers its own usertypes/functions (sound, later assets) into the UI Lua state/env. `sound_handle` is a usertype with no arithmetic metamethods — a Lua script can't do math on the id or pass a stray number where a handle is expected. The C++ message contract stays a plain `size_t` (the wrapper lives only at the Lua boundary).
- **Cross-thread state is PUSH, not pull.** Each background system publishes its full simplified state periodically; main reads the latest. Now realized as a `broker` **`mailbox`** (`sound_state`): sound fills `broker.sound_state.write_slot()` in place (slot vector reused) and `publish()`s; main `consume()`s the freshest. This replaced an earlier pull scheme (raw `atomic_bool*` + output-vector pointer, rejected). Same pattern planned for assets (in-flight resources → loading progress bar).
- main merges the published `command_sound_state` into a single table `std::vector<ui_sound_state_entry{taskid, progress, deadline}>` (NOT a second container). The publish is authoritative for live sounds (`deadline=0`); `app.play_sound` also inserts an OPTIMISTIC entry (`deadline = frame + grace`) so a just-requested sound reads as `0`, not `nil`, during the 1-2 frame request→register→publish latency — otherwise a Lua player sees the startup as "finished" and resets its handle. consume rebuilds: publish entries (confirmed) + still-young optimistic entries the publish doesn't yet know; a confirmed entry that drops out of the publish = finished. A scratch `sound_state_next` + `swap` avoids per-frame allocation. `app.sound_state` is then a plain lookup (expired-optimistic ⇒ nil).
- `tile_frontier` still has a temporary sound smoke scenario: main asks sound for playback devices, logs all names, then after roughly 45 ticks sends recreate-device followed by a play command. The sound actor processes recreate before play so the new sound is queued into the new `system`.

### Window management, UI control & app-state FSM (tile_frontier, 2026-07-05)

Three debts closed together; all changes additive over the broker/actor model. Built rc=0, 37 tests green, live run confirms `boot→loading→game` + fonts + focus events, no VUID/lua errors.

- **GLFW window management.** `libs/input` gained `framebuffer_size`/`window_focus`/`window_iconify`/`window_maximize` callbacks (dedicated setter names — those signatures collide with existing `window_size`/`cursor_enter`, so overloading `set_window_callback` would be ambiguous), plus helpers `framebuffer_size`/`window_focused`/`window_iconified`/`maximize_window`/`restore_window`/`set_window_monitor`/`window_pos`/`set_window_size`/`set_should_close`. Main accumulates window events in a file-local `g_window_events` (same no-atomics pattern as `g_ui_input` — callbacks fire in `poll_events` on main). A C++-tunable `window_policy` (`draw_when_unfocused`/`draw_when_minimized`/`mute_when_unfocused`/`focused_/unfocused_master_gain`) drives reactions; effective `active = focused && !iconified` (minimize = focus loss).
- **Resize path is SWAPCHAIN-ONLY.** New `command_window_resize{w,h}` (mailbox, main→render); render's `render_resize_swapchain` = `wait_all_fences()` + `graphics_base::resize_viewport(w,h)` (which already does recreate_swapchain + screensize resources + graph viewport — the SAME path `render_try_create_graph` uses at startup, so it's proven). `recreate_swapchain` asserts `extent!=0`, so main NEVER publishes resize on a 0×0 (minimized) framebuffer. **Projection bug fixed:** `ui_proj`/`misc`/`cam.aspect` now derive from the LIVE framebuffer size, not static `config.window.*`.
- **Focus/minimize reactions.** `command_sound_set_master_gain{gain}` (spsc, main→sound) → sound actor calls the pre-existing `system::set_master_volume` (survives device recreate via a stored `master_gain`). `command_render_set_active{draw}` (mailbox) gates render's draw block (`draw_active`). Fullscreen via `apply_fullscreen` (glfwSetWindowMonitor; saved windowed rect for restore) — mode change emits a framebuffer_size event → normal resize path.
- **UI control.** `stop_predicate()` now reads a real `std::atomic_bool quit_requested` + `input::should_close` (the 200-frame `test_counter` is GONE). Host `app.*` namespace (alongside `app.play_sound`): `quit_game`/`maximize`/`restore`/`set_fullscreen`/`is_fullscreen`/`set_master_volume`/`set_resolution`/`set_sound_device` (the last three are the "poke setting → diff → message" pattern: resolution→`set_window_size`→resize path, device→`command_recreate_sound_system`). `app.action_pressed/clicked(name)` query `input::events` (the named-action layer, now wired: `events::init/set_key(escape→quit, f1→toggle_menu)` in window setup, `events::update_key` in the key callback, `events::update` each frame). `app.state()`/`app.loading_progress()` for the FSM.
- **Multi-font (C++ side).** (UPDATED 2026-07-06: the name registry is GONE — `visage::system` keeps only `default_font`; `add_font`/`resolve_font`/`fonts_` removed.) Lua selects the base font by demiurg HANDLE: `push_font{ font = request("fonts/..."), size=, ... }` — the binding resolves it via `handle.get<font_resource>()`, silently falls back to default while `font()` metrics aren't ready (transient), warns ONCE if the value isn't a font handle at all. `sized_font` is keyed by `(base font, height)`.
- **Nuklear styling.** `nk.style_default()` = `nk_style_from_table(ctx, nullptr)`; `nk.style_from_table{ text=, window=, button=, ... }` seeds nuklear's default color table (values duplicated in `seed_default_theme` since `NK_COLOR_MAP` is only visible in the NK_IMPLEMENTATION TU) then overrides named entries and applies — a stateless "load a theme" contract (`nk_style` has no flat `colors` array).
- **App-state FSM.** Lightweight `enum class app_state{boot,loading,game}` in the orchestrator (NOT `mood` — that's per-entity/act). `boot→loading` when the UI font atlas is `usable()` (text renders); `loading→game` when the whole `startup_resources` set (grass textures + both fonts) is `usable()` AND mock chunks applied. Loading progress is computed MAIN-SIDE from `startup_resources` `usable()` counts (main holds the pointers; no assets-side publish — the planned `command_assets_state` push was dropped to avoid dead code). Single render graph throughout (graphics1); state GATES publishing (`draw_tiles`/`draw_actors` + actor sim only in `game`) but the camera buffer / `ui_proj` is written ALWAYS (UI must render on splash/loading). `entry.lua` picks screen by `app.state()`: splash (italic-font logo) / loading (`nk.progress` bar) / game (existing panels + Quit/Fullscreen buttons); Esc→`app.quit_game`.
- **visage `update` now takes `(time, timestamp, rng_state)`** (was `(time)`) and forwards them to the lua entry `function(time, timestamp, rng)`. `timestamp` = monotonic time mark (main accumulates `ui_timestamp += time`, starts at 0) for pinning UI-animation starts; `rng_state` = a per-frame PRNG seed — main advances a dedicated 256-bit `utils::xoshiro256starstar` state (`ui_rng`, seeded `string_hash("visage_ui")`) each frame and passes `value()`. Goal: decouple UI randomness from real math.
- **`bindings::rng_state`** (env.h: `struct rng_state{uint64_t s;}`) is an opaque PRNG-state usertype in the UI lua sandbox (registered in `basic_functions`, under `base`). `base.prng64(rng)→rng` (next state, splitmix hash-step; also keeps the legacy `prng64(int)→int` via `sol::overload`), `base.value(rng)→[0,1)` and `base.value(rng,n,m)→int[n,m]` (also as methods `rng:next()`/`rng:value()`), `base.rng(seed)→rng`, and `+` metamethod = `utils::mix` hash-mix (`local s3 = s1 + s2`) — NOT arithmetic. visage passes `bindings::rng_state{seed}` to the entry. 32-bit `prng32*` free functions kept as-is (legacy). Verified live: game UI calls the whole rng API every frame with zero lua errors.

### libs quick tech-debt pass (block A, 2026-07-05)

Batch of small closures across libs (see `ROADMAP.md` «Тех-долг»). Build rc=0, tests green (except bundled cpuinfo `init-test`), tile_frontier reaches `game` with no lua errors. API-affecting bits:
- **`libs/input/events`**: mouse buttons are now FIRST-CLASS bindable — synthetic scancode range (`mouse_button_scancode(button) = (1<<30)+button`), `events::update_mouse_button(button,state)` / `events::set_mouse_button(id,button,slot)`. The whole `set_key`/`check_event` machinery works over them unchanged; tile_frontier's mouse callback feeds `update_mouse_button`. Wheel stays in `auxiliary` (analog).
- **`libs/painter` clear**: `transfer_clear_color`/`transfer_clear_depth` implemented (were `assert(false)`). Value comes from the step's CONSTANT: `v4`=float32×4 memcpy'd into `ClearColorValue` (also ui4/i4), `c4`=rgba8 unpacked /255 → float; depth = first float, optional 2nd component = stencil. tile_frontier adds `command_update_constant{name,bytes}` (broker→render → `find_constant`+`write_constant_data`+`update_constant_memory`) so external code can update a constant (e.g. clear color).
- **`libs/mood`**: new `mood::settle(sys, cur_state, event, ctx, max_idle_iters=8)` runtime helper (event→apply→settle-idle completion loop with cap; stops on no-transition/internal/self-loop). Parser now bounds-checks guards/actions (>8 → error, was silent overflow) and reports token POSITION in errors. Tests added in `utils_general_test.cpp` (blocked/internal/settle/limits).
- **`libs/act` registry**: `reg()` distinguishes duplicate-NAME re-registration from a true hash-collision of distinct names (tracks `names_` for load-time diagnostics).
- **`libs/simul` advancer**: `run(std::stop_token, wait_mcs)` overload — jthread destruction cooperatively stops the loop; old `run(wait_mcs)` delegates with an empty token. tile_frontier's subsystem jthreads pass their stop_token.
- **`libs/sound`**: `.pcm` removed from `type_from_ext` (raw headerless `.pcm` FILES aren't loadable — no format metadata). `command_sound_play` carries `type` (sound::type; `UINT32_MAX`→sfx); sound actor no longer hardcodes sfx.
- **`libs/sound` PCM path (2026-07-05, follow-up):** `resource2` gained audio metadata (`sample_format`/`channels`/`sample_rate`/`frames_count`), filled ALWAYS in `sound_resource::load_cold` (a temp decoder reads the header). SHORT sounds (`frames_count < 5s·sample_rate`) are decoded WHOLE into PCM there: `data` becomes raw frames, `type` becomes `data_type::pcm`. `system` plays `pcm` via a dedicated branch (`pcm_decoder` passthrough built from the resource2 metadata) instead of `make_decoder`. So PCM = the in-memory decoded-data type (not a loadable file). Verified: eating/fleeing/walking decode to f32 PCM with exact byte sizes, ambient (52s) stays compressed, no decoder errors. As of 2026-08-12 all decoders expose only the backend-neutral memory `get_frames` path.
- **`libs/bindings`**: `rng_state + int` overload = advance state N steps (`meta::addition` = mix for rng+rng, advance for rng+int). Nuklear end function renamed `nk.endf` → **`nk.fin`** (all end-variants: group/chart/popup/combo/contextual/menubar/menu); `entry.lua` updated.
- **`libs/visage`**: dead `draw_resource`/`draw_stage` files deleted (were commented out of the build).
- **`libs/catalogue`**: first active introspection slice added in `catalogue/introspection.h`. Use `catalogue::domain<domain_value>::fn_traits<&fn,"name","arg"...>::fn_ptr` to get a `constexpr` wrapper pointer with a concrete mirrored signature (`Ret(*)(Args...)`, member methods as `Ret(*)(T&, Args...)`, const methods as `Ret(*)(const T&, Args...)`, structural functor NTTPs with simple non-overloaded `operator()`). `fn_traits::loc_fn_t` is the source-location functor form: `using f = traits::loc_fn_t; f{}(args...)` captures call-site `std::source_location` into `call_info.file/line`; for methods object is still the first arg. `domain_value` is still an `auto` NTTP (tests cover both `constexpr size_t` and enum values). Runtime policy lives on `catalogue::domain<domain_value>::intro_i` via `set_introspection(ptr)` and is non-owning/non-atomic for now. If the pointer is `nullptr`, the wrapper fast-path calls the original function directly and does not build `call_info`/argument strings. Built-ins: `trace_introspection`, `timing_introspection`, `dry_run_introspection`, `statistics_introspection<N>`. `function_id` is constexpr and uses `utils::murmur_hash64A(Name)`; `argument_names` is constexpr array on `fn_traits`. `noexcept` is accepted but not preserved on the wrapper pointer; thrown exceptions currently skip `exit()`. Complex args log as `<type_name>` placeholder, bounded to the 64-byte local value buffer (try full type, then strip `devils_engine::`, then truncate with `...`). `call_info.arguments` spans wrapper-local storage, valid only during the synchronous `enter`/`exit`/`skipped` call; `argument_view.value` is a non-owning `string_view` into either the original string-ish arg or a wrapper-local fixed buffer (`to_chars` for numbers), so copy values if retaining them. Wrapper internals use `std::invoke` + forwarding refs so member functions with noncopyable refs (`atomic_pool&`, `world const&`) work. TODO: per-domain function registry keyed by `function_id`.
- **`libs/demiurg` loader**: `request()` now warns on a true dependency CYCLE (DFS `visiting_` path set; distinct from an already-queued independent request). NOT done: stricter zip-type-before-parse contract (existing path already warns+skips; deferred pending a concrete rule).
- **tile_frontier config**: `simulation.sound_enabled` (topology toggle, restart-required like `render.enabled`); disabling render/sound frees a reserved core → `+1` worker thread (dynamic `worker_threads_reserved`).

### Logging (catalogue domains, 2026-07-06)

Domain-scoped logging lives in `libs/catalogue/logging.h` (`DE_LOG`/`DE_TRACE` macros + `catalogue::logs()` registry). Two orthogonal axes plus spdlog severity:
- **Base always-on layer** = plain `utils::info`, reserved for a HANDFUL of lines only: `Using cpu …`, `Using sound device …` (libs/sound), `Using '<type>' device …, driver version` (painter `system_info.cpp`), `Using monitor …`. Everything else must NOT be `utils::info` — it belongs to a domain. `warn`/`error` (spdlog severity) stay always-on (importance, not depth).
- **Domains** = `catalogue::log_domain::` **`constexpr` constants** (NOT an enum): `main/assets/sound/render/ui/gameplay/resource` + `engine_count`. New logging layer = new constant + `register_domain(id,name)`. Projects continue numbering past `engine_count`.
- **Depth** (`catalogue::log_depth`): `off/info/flow/trace`. Default **off** (invisible). `flow` = important transitions + periodic slices; `trace` = full pipeline detail. `DE_LOG(domain, DEPTH, fmt, …)` gates on a relaxed atomic load — near-zero cost when off (release too). `DE_TRACE(domain, fmt, …)` = trace depth + captures call-site `file:line` (path compressed via `utils::make_sane_file_name`, like `utils::error{}`).
- **Runtime toggle** (works in release): `app.set_log_level("sound","trace")` (lua/app) or `logging` section in `app.tavl` (per-domain `off/info/flow/trace` + `console`/`file`). `catalogue::init_logging(file, console)` sets up spdlog console + rotating file (5×8 MiB); called in `simulation::init` right after config load (base layer also hits the file).
- **libs now dep `devils_engine::catalogue`** for `DE_LOG`: painter (strides→render/trace, graphics_base setup→render/flow, texture/mesh→resource/flow), visage (font→ui/flow), sound (sound_resource→resource/flow, data-source callbacks→sound/trace). No dep cycle (catalogue→utils/options only).
- **Automatic function tracing (auto-linked to log level)**: `catalogue::introspection` is a non-virtual mode switch (`off/logging/statistics/tracing/dump`) installed via `domain<D>::set_introspection(const introspection*)`. The config carries `{mode, log_domain, statistics_store* stats}`. The EFFECTIVE mode = `max(base mode, trace-if-log-domain-on-trace)` — so `app.set_log_level("gameplay","trace")` auto-escalates that domain's wrapped functions to `tracing` (enter/exit + compressed `file:line` + elapsed), on top of whatever base mode (e.g. perf `statistics`). No NTTP realignment needed — the config's `log_domain` field is what gates. arg_views built lazily only for `dump`. tile_frontier's actor phases run at base `statistics` (perf UI) and escalate to `tracing` when gameplay hits trace.
- **Debugging workflow**: enable a domain at runtime/config, run, read `logs/*.log` to inspect the flow you need — instead of adding throwaway prints.

## Subsystems

- `libs/acumen`: GOAP planner plus `goap_resource` config parser/merge/flatten. It uses `act` predicates to compute bitset state, A* over symbolic actions, and caller-owned scratch/cache; it returns plans but does not mutate world state.
- `libs/act`: shared gameplay-function registry (`devils_engine::act`) — typed-by-return functions (effect/predicate/number/string/object), immutable `exec_context`, `intent` seam, `registry`, and generic `script_resource`/`script_compiler` boundary. `native_function` and devils_script-backed `script_function` are live; Lua backend remains pending. `acumen` and `mood` consume act contracts.
- `libs/bindings`: Lua/sol2 binding layer for sandbox env, `base` utilities, deterministic `rng_state`, reflect-based table conversion, and large Nuklear bindings. No local README yet; root README summarizes it from code/CMake.
- `libs/catalogue`: active code is function-call introspection/tracing/timing/statistics, the older in-memory `call_log`, and typed deterministic MT strategy wrappers/executors in `deferred.h`. The first live ECS integration is tile_frontier's cognition→collect/elect→structural/FSM pipeline, including config-loaded void actions over the same deferred building blocks. Dense 128-byte inline payload storage is live; remaining nearby work is member/custom bounded codecs and executor ownership after a second consumer. The obsolete `act::effect_sink` seam was deleted on 2026-07-19. Older recording/replay/RPC/channel files are archived under `exclude/` and are not to be revived as catalogue's direction. Replay/network persistence, if ever needed, belongs to a separate layer.
- `libs/demiurg`: module/resource registry and staged resource loader. The durable cross-system handle is now `demiurg::resource_handle` (hash of logical id, survives registry rebuild — see "Assets / resource pipeline" and "Demiurg ↔ Lua resource API"); raw `resource_interface*` is fine only for same-lifetime same-thread reads. Loaders must honor dependency gating and external render/GPU steps. tavl list-pattern (`path:name`/`path:index`) and dependency-cycle diagnostics are done. `demiurg/resource_path.h` owns the ONE engine-wide translation from a written path to an id (`absolute_resource_path`/`resource_parent_path`: drop extension, fold `.`/`..`, keep the `:name` tail; only a dot-prefixed path is relative) — moved out of `simul` on 2026-09-03. The `//---` separator is line-anchored and swallows its own title line, and a file that has one loses its base id (only `path:name`/`path:index` resolve) — say so in consumer error messages, because it reads like a typo in a correct path.
- `libs/flow`: first active 2D/2.5D/UV animation slice. It is now a CMake target `devils_engine::flow` with `flow::library`, `flow::state`, `flow::playback`, `flow::animation_resource`, directional image selection, action events, and UV accumulation/truncation. 3D/skeletal/blending remain future work.
- `libs/input`: GLFW window/input wrapper, Vulkan surface/proc helpers, key-name registry, and abstract input-event state machine.
  - Key-name contract now has three distinct layers in `input/key_names`: canonical names for config/storage (`key_w`, `minus`, `f10`, `right_super`, `kp_1`), US/QWERTY display names (`W`, `-`, `F10`, `Right Super`, `Num 1`), and local names via `glfwGetKeyName` plus platform fallback. Prefer canonical names in configs; parse them with `key_from_canonical`, which returns `(glfw_key, scancode)` in the same order as the GLFW key callback.
  - Input configs should look like `action = [ key_w, minus ]`: parse each canonical token to `(glfw_key, scancode)`, call `events::set_key(event, scancode, glfw_key, slot)`, and update runtime key state from the GLFW callback scancode. Runtime key state is still keyed by scancode.
  - `events` event ids are `utils::id` hashes from `utils::string_hash`, not `std::string_view` keys. Hot-path code should precompute `events::event_id` once with `events::make_event_id("use")` and call id overloads (`check_event(id, ...)`, etc.); string overloads remain as convenience wrappers that hash then forward.
  - Hash collision checking intentionally lives in `events::set_key(std::string_view, ...)`: `event_map` stores the original event name and errors if the same hash is registered with a different name during binding load/setup. `key_mapping` stores event ids per scancode and removes stale scancode entries when a key has no events left.
- `libs/painter`: active Vulkan/render-graph layer (`graphics_base`, `render_config_storage`, `render_graph_instance`, `assets_base`) plus demiurg resources for render config, shaders, pipeline cache, meshes and textures. Older painter files remain beside the active path.
- `libs/originator`: procedural generator framework (typed buffers, config-declared steps, lua as the step body). Volume tools (2026-09-03, `add_volume_tools`): `marching_cubes` is the first tool whose output length is unknown before it runs — capacity is the buffer's declared size, the used length is an ordinary one-element buffer, overflow refuses loudly, and its 256-case table is DERIVED from the cube-face rule rather than transcribed. Four build targets stay separate on purpose — core (`originator`, no lua/ds/demiurg), `originator_script` (devils_script over a dense buffer), `originator_primitives` (FastNoise2/jc_voronoi), `originator_lua` (own `sol::state`, own budget, no visage), plus `originator_config` (2026-09-03): the demiurg seam. A generator is addressed by ONE id — its ENTRY document (`name`/`values`/`buffers`/`steps = [ ... ]`, all references are demiurg ids) — and `load_generator(resources, entry_id)` returns description + ranges + every script text. tavl cannot mix top-level rows with top-level blocks in one document, so steps live inside the entry; both confusions are refused before parsing.
- `libs/sound`: miniaudio-only production sound prototype (`system`); former OpenAL reference/compatibility code is archived under ignored `exclude/`.
- `libs/utils`: broad utility library. It also owns the former `libs/thread` utilities under `libs/utils/include/devils_engine/thread` and `libs/utils/src/thread`; keep `devils_engine::thread` as an alias to `devils_utils` for compatibility instead of reintroducing a separate `libs/thread` target.
- `libs/aesthetics`: ECS storage/view/query/events/snapshot implementation; still exploratory and performance-sensitive.
  - Current storage is `world::sparce_dence_set<T>` in `libs/aesthetics/include/devils_engine/aesthetics/world.h`: `sparce_set` maps entity index → dense component index + version; component payload lives in `component_array<T>` rather than a raw `std::vector<T>`. `component_array` is the compromise that lets const `view()` / `lazy_view()` keep membership const while returning mutable `T*` payloads without `const_cast`.
  - The old public-ish `sparce_dence_set::entities` dense entity list was removed. `world::raw_itr` walks `sparce_set`, skips invalid entries, and reconstructs `entityid_t` from sparse index + stored version. This keeps create cheap and avoids storing owner data in the hot component payload path.
  - Removal currently has a known tradeoff: when removing a non-tail dense component, the moved tail component's owner is found by scanning `sparce_set` (`entity_at_dense_index`). This preserves low memory overhead and create speed, but random/forward deletion can be much slower than reverse/tail deletion. If this becomes a real bottleneck, the likely fix is an internal sidecar dense-owner array (`entityid_t` or just entity index, +4 bytes/component with current `uint32_t` ids) rather than putting owner inside each component slot.
  - `view<T...>` means all requested components and iterates the smallest component storage via `raw_itr`; construction is cheap and `construct+iterate` is mostly traversal. `lazy_view<T...>` gathers a hash-set union of entity ids (any requested component), so construction is relatively expensive. `query<T...>` is a live all-components query updated by create/remove events; `lazy_query<T...>` is the live any-components variant. Keep this semantic difference explicit in benchmarks and API discussion.
  - Query maintenance cost grows with the number of active query receivers for a changed component type. Each create/remove emits events to relevant query receivers, which do `has/get_tuple` checks and vector insert/erase. If many queries and high mutation rates appear, prefer phase rebuilds / dirty query updates over per-mutation live maintenance.
  - Tests and benchmarks: `tests/aesthetics_world_test.cpp` covers sparse storage without `entities`, version reconstruction, mutable payload through const views, query updates, and lazy-query removal. `tests/aesthetics_world_benchmark.cpp` is a manual benchmark target (`aesthetics_world_benchmark`) grouped into all-component traversal, lazy/any traversal, and component mutation. Benchmark arguments are `[entities] [iterations] [mutation_count]`; mutation includes raw vs warm create/remove and forward vs reverse removal to expose deletion-order risk. It is intentionally not registered with CTest.
  - Near-term design direction: keep `libs/aesthetics` focused on storage/view/query contracts. Efficient per-entity handoff between systems should be designed later as batched pipeline/scratch channels, not as per-entity mailbox traffic in the ECS hot path.
- `libs/mood`: FSM built from owner-level structured `mood::transition_config` records (`devils_engine::mood`); a legacy transition-description string constructor remains for C++ fixtures. Split into two layers (deliberate): `mood::system` (`system.{h,cpp}`) is a DUMB, stateless, fast STORE — it holds nothing at runtime, reacts to nothing, and does NOT understand the naming conventions; it only returns the candidate `transition` list for a `(state, event)` pair in O(1), preserving source order within a group (critical for top-down guard evaluation). `mood/runtime.{h,cpp}` is the CONVENTION + stepping layer (free functions): it owns the standard names (`any_state` = wildcard fallback, `idle` = standard "no event", `on_entry`/`on_exit` = entry/exit pseudo-events), `step()` (top-down guard eval → `step_outcome{transitioned|blocked|no_transition}`, effects NOT executed — that is the caller's apply phase), `find_with_fallback()`, and `validate()` (load-time graph warnings). See "FSM mood internals" below.
- `libs/simul`: shared app/runtime layer. It owns `interface`/`advancer`, `app_runtime<Traits>`, standard broker/messages/systems, jthread lifecycle and window/settings/loading/render/sound/assets helpers. `app_runtime` owns an extensible erased `worker_systems<Broker>` rather than fixed render/assets/sound slots, wires one broker before init, exposes optional workers through the typed `runtime_system<T>()` registry, and joins by explicit shutdown order. `make_standard_workers` builds optional render/sound/assets (disabled assets now create neither object nor thread); the topology callback resizes the task pool from the actual standard+project worker count. `tile_frontier::runtime_traits` is reduced to bootstrap/broker/main types plus one worker factory; project-local broker channels and gameplay policy remain project code.
  - **Turn-based pipeline scaffold (2026-07-22, engine side of ROADMAP CG-1)**: `simul/turn_pipeline.h` is a reusable header-only, host-templated resumable step machine for turn-based / discrete-event gameplay, modeled on `lifecycle_controller` (single writer of the cursor). `presentation_barrier` is a TRANSIENT set of typed `{presentation_task_id, gameplay|finished}` checkpoints with an engine-tick watchdog; it is deliberately NOT serialized, and headless steps register nothing and resolve inline. A visible beat is `start animation → gameplay checkpoint → deterministic main-thread commit/result → finished checkpoint → next step`. The host advances the serializable cursor BEFORE each wait, so resume drops in-flight presentation and continues at a deterministic boundary without double commit. `turn_pipeline<Cursor>` owns no gameplay coordinates: turn, player-action, countdown-pulse and forced-generation counters belong to the project cursor/state because none can be derived from another. A watchdog timeout latches `faulted` and reports exactly once. Covered by `turn_pipeline_test` (5 cases/40 assertions).
  - **First `cardgame` consumer (2026-07-22)**: `subprojects/cardgame` now contains a small project-owned combat FSM and project-specific player intent. It proves player card cue/commit/finish, enemy countdown/attack, forced end-turn pulses, snapshot before the gameplay marker, and animated-vs-headless identity across three completed turns. `quick_strike` is deliberately a player action without a countdown pulse; forced end-turn pulses are deliberately pulses without player actions. `cardgame_headless_smoke` ends at identical `{turn=4, actions=5, pulses=6, player_hp=24, enemy_hp=89}`. The hard-coded cards are scaffolding for the next project-owned ds beat recorder, not the content model.
  - **Cardgame atomic resolution slice (2026-07-22)**: the project combat FSM now delegates a materialized, serializable `resolution_work + resolution_cursor` to a small inner resolver; it is NOT another `turn_pipeline` or `mood` system. `attack_instance` and `damage_instance` are distinct: initial card attacks are all gathered first, while `resolution_work.plan` preserves card-text ordering between attack/effect entries; attacks then resolve one by one because an earlier hit may change shields/resists/elements/death for the next. Each attack runs cue/gameplay → resistance modifier journal → shield/HP apply → result/finished, then its elemental reactions, returned-damage instances and death boundary before the next attack. Reaction/returned damage use explicit channels, pass through the common damage resolver, and are non-recursive by default. `thorns` appends one response per eligible attack rather than recursively invoking resolution. Status effects live in a project-owned `combatant_state.effects` list, NEVER `aesthetics::flag_set`; effect application yields explicit `added/updated/immune/invalid_target/rejected` outcomes. Current limits are intentional scaffolding: resistance collection is a native stand-in for a future project ds script, only a minimal elemental collision is present, stacking policy is simple add+refresh, and destructive death effects/cleanup are pending. The smoke covers complex animated/headless identity, resist on attack+reaction, non-recursive per-hit return, effect immunity/update and resume after primary commit with queued reaction/response/effect work.
- `libs/utils/include/devils_engine/utf`: UTF string conversion support.
- `libs/visage`: Lua/Nuklear UI runtime plus MSDF fonts and POD render-output buffers consumed by painter's `draw_ui` path.
  - Host-binding seam: `system::script_state()` (sol::state&) and `script_env()` (sol::environment&) let the host register its own usertypes/functions into the UI Lua sandbox without visage knowing about gameplay (used for the `app.*` sound API in tile_frontier). visage itself stays UI-only.
  - Dynamic font size for `nk_style_push_font` lives in `visage::system` (per-size `nk_user_font` cache `sized_fonts_` + private `sized_font(float)` helper), NOT in `font_t`. `font_t` keeps only the base `nkfont`, and `font_t::set_texture_id` now updates just that base font. `sized_font` refreshes `texture.id` from `nkfont` on every push, so there is no stale-id race when `push_font` runs before the atlas reaches HOT (the old per-font `sized_fonts` fixup loop is gone). Multiple fonts are selected by demiurg handle in `push_font{ font = request("fonts/...") }` (the `add_font` string registry was removed 2026-07-06); the demiurg↔Lua resource API (`request`/`require`/`find`/`filter`, below) drives UI resource access.
  - **UI images (Stage 1, 2026-07-05):** `visage::image` (`image.h`) is a POD lua-facing handle — `{texture_id (bindless slot), w, h, region[4]}`, no nuklear/vulkan dep so the HOST builds it. `visage::system` registers (same seam as `push_font`/`style_*`, NOT in bindings which can't depend on visage): the `image` usertype, an `nk.placement` bitmask table (`fill`/`stretch`/`scale_ratio`(=fit)/`center`/`left`/`right`/`top`/`bottom` + `mirror_u`/`mirror_v`), and `nk.image(img [, placement] [, color])` — takes the widget slot via `nk_widget(&bounds,ctx)`, computes the target rect (`image_placement_rect`: fit-by-min-scale + align, default center), draws `nk_draw_image`. Overrides the dead `bindings` `nk.image` stub. tile_frontier host: `app.image(handle)` → `visage::image` from a loaded `gpu_texture_resource` (`gpu_index`+`width`/`height`), `nil` until `usable()`. (As of `d2ce7fe` `app.image` takes a `resource_handle` from `request(...)`, not a string name — the `image_by_name` map was removed; see "Demiurg ↔ Lua resource API".) Constraint: usable slots 0–7 (`tex[8]` clamp) until the huge-descriptor pass.
  - **UI stencil effects (Stage 2, 2026-07-06) — cooldown + 4-blend via mask texture.** `gui_draw_mode` += `cooldown=4`/`mix=5`; `gui_draw_command_t`/`ui_push_t`/`ui_command_wire` replaced the named SDF fields with a generic `uint32 payload[6]` (48/28 B; config `ui_push` = 7×`ui1`) interpreted BY TYPE in `ui.frag` (msdf→SDF fields; cooldown→`[0]=mask_index [1]=fill`; mix→`[0]=mask_index [1..4]=comp0..3 [5]=is_image bits`). Params ride the SAME per-draw channel as SDF text (`effect_arena` + nk `userdata` offset; new `ui_image_effect` struct; `convert()` dispatches by `tex_id::type_of`). `nk.image_gradient{img,mask,fill}` (icon revealed where `mask.r<=fill`, unrevealed ×0.35) and `nk.image_mix{comps={≤4 image-or-color},mask}` (weights `mask.rgb` + `1-r-g-b`, per-comp image-vs-color by `is_image` bit, `mask.a`=opacity). Mask + comps are just indices into the SAME bindless `tex[]` (no new descriptor). `ui.frag` `tex[8]`→`tex[16]` (Vulkan guaranteed min; dynamic indexing, UI id uniform per draw). **Nearest masks → BINDLESS V2 (2026-07-06):** replaced the whole combined-image-sampler asset model. The `textures` descriptor is now a SEPARATE `SAMPLED_IMAGE` array (binding L, written by render) + an immutable `SAMPLER` POOL (binding L+1, config `sampler_pool = [linear, nearest]` → `samplers/nearest.tavl`). Shaders combine on the fly: `sampler2D(tex[index], samp[sampler_id])`. `sampler_id` is a 4-bit field in `tex_id` (`[20..23]`); UI content uses the id's sampler (default 0=linear), the MIX mask uses `samp[NEAREST]` (crisp seams) and the COOLDOWN gradient uses `samp[LINEAR]` (smooth) as shader constants. This works where per-slot immutable samplers can't (samplers are FEW + static → bakeable pool; slots are MANY + dynamically assigned at load → paired by index at sample time). ALL asset-sampling shaders converted (`fragment1`/`tile`/`ui`; `actor.frag` doesn't sample → untouched); the old `mask_textures` 2nd descriptor was deleted. **GLSL gotcha:** a `sampler2D` built from separate image+sampler may ONLY appear AT POINT OF USE (in `texture()`/`textureSize()`) — it cannot be returned from a function, stored in a local var, or passed as a function arg. So `S(idx,sid)` is a `#define` (not a function) and `screen_px_range(uint index, uint sid)` constructs the combined sampler inside its own `textureSize`. **Stale-asset gotcha hit:** deleting `mask_textures.tavl` from source left a stale copy in the build output (POST_BUILD copies, never deletes) → had to `find build-debug -name ... -delete`. **Tile-set gotcha:** the tile terrain gather (`texture_set::gather`) is a SUBSTRING match — putting `grad*/quad` under `textures/` pulled them into the terrain; narrowed the tile gather to `"textures/grass"` (masks don't contain "grass"). TODO reminder: generate a perfect single-channel gradient in CODE (grad*.png are placeholders with edge artifacts).
  - **UI texture-id encoding (2026-07-06) — TYPE/MIRROR/INDEX packed in one word.** `render_output.h` `namespace tex_id`: `[0..13] index (14b, 16384) | [14] mirror U | [15] mirror V | [16..19] type (4b, = `gui_draw_mode`) | [20..23] sampler_id (4b, index into the descriptor sampler pool) | [24..30] free (7b) | [31] unused (nk `handle.id` is signed `int` → keep bit 31 clear)`. `pack(type,index,mirror_u,mirror_v,sampler_id)` / `index_of`/`type_of`/`sampler_of`/`mirror_*_of`. This REPLACED the mode-inference heuristics: `convert()` is now a passthrough of the packed id (dropped `is_font_texture` + the `gui_draw_command_t.mode` field), `font_t::set_texture_id` packs `type=msdf`, `nk.image` packs `type=image`+mirror, nuklear shapes carry `texture.id==0` → decode `type=0=solid` naturally. `ui.frag` decodes index/type/mirror from the pushed `tex_id` (masks kept in sync with `render_output.h`), branches by type, and flips uv per mirror (mirror correct for whole-texture images; sub-region mirror needs the region uv-rect — TODO). Painter `ui_push_t`/`ui_command_wire` dropped `mode` (44→40 B; config `constants/list.tavl` `ui_push` 24→20 B). Free bits + `composite` type value reserved for Stage 2 (heraldry/effects). Verified: text (msdf via packed font slot), grass image + horizontally-mirrored copy all render, no VUID.

## Data/Config Direction

- The project is actively migrating from JSON to the custom `tavl` configuration format.
- `subprojects/tile_frontier/resources/engine/config/app.tavl` (MOVED from `resources/config/`) is the app/runtime config (window, subsystem FPS, worker threads, render cache paths/GPU preference, metrics). Loaded through the engine demiurg registry as `app_config_resource`, NOT raw file_io (see Assets section).
- `subprojects/tile_frontier/CMakeLists.txt` builds a self-contained project folder at `build/subprojects/tile_frontier/tile_frontier/`; the executable and bundled runtime libraries live in `tile_frontier/bin/`, while resources/config/render data sit beside that `bin/` folder. It copies `tests/test_render_config` into `resources/engine/render_config/` (was top-level `render_config/`; source dir unmoved, still shared with fast_test/tests), and copies `tests/shaders` to both `shaders/` and `tests/shaders/` because painter currently has both path assumptions (`utils::project_folder() + "shaders/"` and `utils::project_folder() + "tests/shaders/"`).
- `libs/painter/src/painter/system_info.cpp` now uses `main_device.tavl` for cached physical-device data instead of `main_device.json`.
- Render graph description already has tavl test data under `tests/test_render_config`.
- tavl config convention: when a value is a `pair`/`tuple`, write it with parentheses — `key = (a, b)` (or `(a, b, c)`) — rather than chained operators like `key = a = b`. tavl accepts both, but the parenthesized form is unambiguous and matches the serializer output. Applied to render-graph subpasses (`{ albedo_res = (undefined, clear) }` = `map<string, tuple<string,string>>`).
- DECIDED, not yet implemented — canonical multi-aggregate file convention: a resource file is a sequence of DOCUMENTS separated by `//---` (tavl's `document_separator`, in `ext.h`); each document is a braceless root aggregate (like `app.tavl`). Singleton = 1 document, list = N documents; NO top-level braces (`{}` only for nested aggregates, `[]` for value arrays). The resource TYPE decides exposure: singleton → id `path`; list → each document as `path:name` (name from a `name` field; positional fallback), enabling per-entry mod override. NB: this is a NEW convention — current render-config DATA files still use consecutive `{...}` blocks (tavl `deserialize_next` over values, a DIFFERENT mechanism); migrating them to `//---` is a pending small slice. UPDATE 2026-07-06 (`5013101`/`7c03758`): demiurg now DOES expand `//---` list files into `path:name`/`path:index` subresources, and painter's `structures.cpp` `parse_data` can CONSUME a list-pattern render-config category (subresources resolved by internal `name`) — so the capability landed on both sides; only the shipped data files haven't been moved over. `:name` is for game data (monsters/spells), NOT render config (the graph consumes whole files and cross-refs by internal `name`).
- The shared gameplay-function registry now exists as `libs/act` (`devils_engine::act`) — see "Gameplay function layer (`libs/act`)" below. It replaced the placeholder callbacks in `acumen` and `mood` (DONE). **acumen** now mirrors mood's pattern: `acumen::system(const act::registry*, metrics, goals, actions)` resolves each `state_metric` to a `const act::predicate_function*` and each `action` to a `const act::effect_function*` BY NAME (`registry->predicate/effect(string_hash(name))`), caching the typed pointer at build (lookup leaves the A* hot path). A `state_metric` is one GOAP state bit (bit index = the metric's position in the metrics vector — dense, implicit) computed by its predicate; `compute(const act::exec_context&)`/`system::compute_state(const act::exec_context&)`. `action.effect` is NOT executed during planning (the plan only picks actions; the effect runs on apply via intent); an empty action name means no effect (pure symbolic transition). `acumen::system`'s constructor is NOT `noexcept` (it throws `utils::error` on a missing/wrong-category function — a noexcept ctor calling the throwing `utils::error` would `std::terminate`; same fix applied to `mood::system`'s ctor). `tests/acumen_test.cpp` builds a tiny GOAP (draw_weapon→attack) over a real `act::registry` and asserts compute_state + find_solution + the load-time throw. Fixed a pre-existing off-by-one in `find_solution` while testing. `find_solution` is now ALLOCATION-FREE: `size_t find_solution(sys, container, start, goal, std::span<const action*> out)` writes the plan (actions in execution order) into the caller's buffer and RETURNS THE FULL plan length — if `> out.size()` the plan was truncated to a prefix (grow the buffer; caps ~32-64 make this practically unreachable). The plan EXCLUDES the start node (its action is null) and INCLUDES the goal node's action (the one that achieved the goal); `start` already satisfying the goal returns 0 (empty plan). No internal `std::vector` — the caller passes a stack `std::array` (mirrors `astar::solution(node**, max_nodes)`). (The old version returned a `std::vector` with a leading `nullptr` and dropped the final action.) `mood::table` was DELETED — it was literally "the shared function table" the author wanted, which is now `act::registry`; `mood::system(const act::registry*, transition_config)` resolves guard names → `registry->predicate(string_hash(name))` and action names → `registry->effect(...)`, caches typed pointers in each `transition`, and `transition::is_valid/process` take `const act::exec_context&`; a legacy lines overload remains for fixtures. The old `int32_t`/`void*` error-return channels are gone (act backends throw via `utils::error`; predicate returns bool, effect returns void). Both libs link `devils_engine::act` PUBLIC; `libs/act` is added in root CMake BEFORE `mood`/`acumen` so the alias exists. `tests/utils_general_test.cpp` mood test builds an `act::registry` of `native_function<void>`/`native_function<bool>`; `fsm_config_test` covers the native TAVL adapter and structured constructor. `mood` was further refactored into a dumb store + a runtime/convention helper layer — see "FSM mood internals" below. `tile_frontier` supplies the live `exec_context` and executes selected effects in its apply phase.

## Gameplay function layer (`libs/act`, `devils_engine::act`)

Shared registry for small-grain gameplay functions over one entity (or a few linked), used by GOAP
(`acumen`), FSM (`mood`), gameplay glue scripts, AND the Lua UI (e.g. a string function → loc key).
The layer is wired into `devils_plane`; `native_function` and `script_function` are live, while the
Lua function backend remains pending. `script_compiler` is the type-erased config-parse seam used by
`act::script_resource` and `acumen::goap_resource`; projects provide concrete root-scope dispatch.

- **Functions are SPLIT by return type, NOT unified into one `value invoke()`.** Categories mirror
  `devils_script::user_function_type`: `effect`(void) / `predicate`(bool) / `number`(real_t) /
  `string`(`utils::id` = loc-key hash) / `object`(`entity_id`). The return type IS the contract and
  also encodes purity (effect = mutating, the rest = pure), so no separate `purity`/signature
  metadata. Purity matters: GOAP A* calls pure functions freely during search; effects are NOT run
  during planning (the GOAP action only emits an `intent`, see below).
- **No combinatorial category×backend explosion.** Category = template `function<RetT>` (aliases
  `effect_function`/`predicate_function`/…); backend = template impl: `native_function<RetT>`
  (raw `RetT(*)(const exec_context&)`, no `std::function` on the A* hot path), `script_function<RetT>`,
  `lua_function<RetT>`. Common base `function_base` (carries `category` tag) for generic storage +
  `describe`.
- **`describe(ctx, callback)`** — run a function WITHOUT applying effects and STREAM useful text into
  a callback (`using describe_callback = std::function<void(std::string_view)>` — temporary/any type
  for now). For UI tooltips ("why can't I", "+5 from X, −2 from Y", effect preview, predicate/number
  breakdown). `devils_script` will stream its compiled-container introspection nodes here.
- **`exec_context` is IMMUTABLE.** Concrete struct, same for all backends, flows by reference into
  `invoke` (`const exec_context&`), NEVER a global (global = silent races under the actor model).
  Fields: fixed `entity_id scope[8]` + count ([0]=this, [1]=target, …), `const world*`, immutable RNG
  inputs (`rng_seed/entity/tick`) and a caller-owned `execution_scratch*`. **PRNG state is NOT held in the
  context** — inputs come from external systems and each call passes a `purpose` explicitly:
  `random(purpose) = utils::mix(seed, entity, tick, purpose)`. This is more deterministic than an
  auto-increment draw counter (order- and count-independent). Mutable VM/call data lives in scratch;
  mutating effects use catalogue deferred wrappers rather than a mode bit in the context.
- **No `fat_handle`.** Gameplay functions work over a finite entity set (essentially one `entity`);
  "type" is distinguished by ECS COMPONENTS, not a tag on the handle. Over disk/network it is a bare
  `entity_id` (uint32) or a context-dependent index. `value::handle` holds a raw uint64. (Note:
  `utils::type_id<T>()` is a constexpr name hash — stable within a build / across builds of the same
  compiler, differs only across compilers — but handles don't need it.)
- **`value`** = slim POD tagged union (none/boolean/integer/number/vector/handle/string), matching
  devils_script categories. NO LONGER the return type (returns are typed by the function class); it is
  only for generic boundaries (devils_script arg marshalling and generic trace/debug). `string` is a
  hash, not inline. GOTCHA: `vec3` has member-initializers (non-trivial ctor) → as a union member it
  deletes `value`'s default ctor; an explicit `value() : kind(none), inum(0) {}` fixes it and zeroes
  the union (no uninitialized bytes → friendly to deterministic checksums). `number`/`vec3` ride
  `using real_t = double` — float→fixed is a one-line change when determinism is taken up.
- **`effect_sink` was removed (2026-07-19).** It had no implementation or emit calls. Pure/read-only
  consumers resolve only pure act categories; mutating ds/native calls use catalogue deferred wrappers;
  replay will use a separate owning input/log layer.
- **`intent`** = the thinker→ECS seam. GOAP/FSM/script do NOT mutate the world; they emit a compact
  `intent` of base verbs (`move_to`/`turn_to`/`call_function`/`fsm_event`, extensible) that ECS systems
  consume in a DETERMINISTIC apply phase (sort by `actor.id`, not message-arrival order). One typed
  command seam instead of millions of scattered mutations; persistent serialization is a separate concern.
  Carries `source_action` provenance ("why", not just "what").
- **`registry`** — one `gtl::flat_hash_map<fn_id, unique_ptr<function_base>>`; `fn_id = string_hash(name)`
  (function names need no dense index). Typed checked accessors `predicate(id)`/`number(id)`/`effect(id)`/
  `string_fn(id)`/`object(id)` (category mismatch → nullptr). `reg()` only in the single-threaded load
  phase (asserts on hash collision via `utils::error`); `get()`/`call()` thread-safe after. Consumers
  cache the typed `const function<RetT>*` at plan/table build → lookup leaves the A* loop.
- **Id numbering (resolved): per-system, not one rule.** Dense/monotonic index ONLY where a system
  genuinely needs it: GOAP state flags = dense (bit position in `bitset<256>`, via `utils::string_pool`),
  a large logic-heavy flag registry (e.g. internal-politics flags) = dense. Function names, effect names,
  per-actor flags = plain hash (`utils::string_hash`); per-actor flags needing extra data like an expiry
  store `(date, hash)` sorted. The two numberings are separate namespaces — do NOT conflate.
- **Throw via `utils::error{}("msg {}", args...)`** (captures source_location, logs + throws), not
  `assertf(false, ...)`. `mix` lives in `devils_engine::utils` (`utils::mix`), not a `prng` namespace.

## FSM mood internals (`libs/mood`, `devils_engine::mood`)

Grammar of one line: `state [+ event] [[guards...]] [/ effects...] [= next_state]`. ORDER MATTERS:
within a `(state, event)` group lines are evaluated TOP-DOWN — a later line's guards are only reached
after earlier lines fail. Order of the initial states does NOT matter. Author's hard design rule:
`mood::system` is a DUMB, stateless, fast STORE that knows NONE of the naming conventions; conventions
+ stepping + validation live in free helper functions in `mood/runtime.h`.

- **Runtime works on hashes, not strings.** Each `transition` precomputes `current_hash/event_hash/
  next_hash` (`utils::string_hash`); the `string_view`s remain only for diagnostics/`describe`. An FSM
  cursor is a `utils::id`, not a string.
- **`system` lookup is O(1) via a hash index.** Construction stable-sorts `m_transitions` by
  `(current_hash, event_hash)` (stable ⇒ source order preserved within a group) and builds
  `gtl::flat_hash_map<uint64_t, range>` keyed `mix(state_hash, event_hash)` → `{offset, count}` into the
  contiguous vector. `find_transitions(state_hash, event_hash)` returns a `span`; string overloads hash+
  forward. (Replaced the old sorted-vector `lower_bound` + string-compare comparator + backward-expand
  loop.) Duplicate check (same state+event+guard-subset) runs once over each sorted group.
- **Conventions live in `mood/runtime.h`, NOT in `system`** (`mood::conv::` hashes): `any_state` =
  wildcard fallback, `idle` = standard "no event", `on_entry`/`on_exit` = entry/exit pseudo-events.
  `find_with_fallback(sys, state, event)` probes `(state,event)` then `(any_state,event)`. NOTE: in the
  test fixture `any_state` is currently only reached via this helper fallback — `system` itself treats it
  as a literal, so `find_transitions("begin","attack")` is empty but `step(...,"begin","attack")` succeeds.
- **`step()` returns a structured `step_outcome`, not a bool.** `step_result` distinguishes
  `transitioned` (a guard-passing edge found; `next_state`/`taken` set), `blocked` (edges exist but ALL
  guards failed — NORMAL gameplay, e.g. ragdoll not settled), `no_transition` (no edge even via
  `any_state` — likely a typo/programmer bug). `step()` only DECIDES; it does NOT run effects. Diagnostics
  are returned, not formatted on the hot path (no per-frame string-building for millions of actors).
  `step_outcome` fields are ordered by decreasing size (uint64, ptr, 2×u16, then the 1-byte enum LAST) to
  avoid padding — `static_assert(sizeof==24)`. This is a project-wide rule for new structs.
- **`apply_transition(sys, cur_state, taken, ctx)` MUTATES (the decide/apply split's apply half).** For an
  external transition (has `= next`): runs on_exit of the old state → the transition's own effects →
  on_entry of the new state (each of on_exit/on_entry is itself a `(state, on_exit|on_entry)` group from
  which the first guard-passing line's effects run). For an internal transition (no `=`): runs ONLY the
  transition's effects (the state is not left, so no exit/entry). Returns `taken.next_hash` (`invalid_id`
  ⇒ caller keeps `cur_state`). Mutating transition actions are deferred catalogue calls committed by
  the owning gameplay pipeline.
- **The per-entity apply loop (design): event then settle idle this same frame.** Consume the entity's
  `fsm_event` intent (else `conv::idle`), `step()`, and if `transitioned` call `apply_transition` and write
  `cur_state`. Then KEEP stepping `conv::idle` until it stops transitioning (UML completion transitions:
  an idle/guarded edge fires the moment its guard holds, which may already be true the same frame after
  entering the new state), with an iteration CAP to break idle A→B→A cycles. Run the whole thing in the
  ordered apply phase (sort by entity_id); guards there observe intra-tick mutations from lower-id
  entities — deterministic because the order is fixed (vs GOAP planning which reads a start-of-tick
  snapshot). The FSM cursor is a component holding `utils::id cur_state` (a hash, NOT a string_view) plus
  a which-FSM `def_id` (multiple `mood::system` tables exist). A `run()`/`settle()` helper wrapping this
  loop is a likely next addition.
- **Typo detection without a hand-maintained registry.** `validate(sys)` (load-time, convention-aware)
  derives the valid state/event sets from the table ITSELF and `utils::warn`s on dead-end states
  (`next_state` with no outgoing edges → terminal or typo) and unreachable states (`current_state` never
  a `next_state`, excluding `any_state`) with a fuzzy "did you mean" (mini Levenshtein over candidate
  names). In the fixture it correctly flagged `prepare_weapon1`→`prepare_weapon` and `melee_attack2`→
  `melee_attack` as likely typos, and `begin`/`initial_state` as initial states.
- **State→animation binding and "blocking" state properties (DESIGN, not in mood).** Author rule:
  states are NOT 1:1 with animations; entering a state runs an `act::effect` that swaps the entity's
  current animation. Per-state capability data (can-move, lock-input, interruptible, …) lives in the
  ENTITY's ECS components, NOT in `mood`. Big-engine pattern (Unreal GAS / Souls-like): the FSM PUBLISHES
  capability tags/bits to a per-entity place and each system gates ITSELF by reading them (FSM never
  calls the movement system). Mapping: "can't move" = a capability bit an on_entry effect writes
  (ref-counted to survive overlap), read by the movement system; "no multi-hit" = same bit or simply no
  outgoing edge for the event; "interruptible mid-anim" = an actual transition edge exists for the
  interrupt event; "ragdoll not settled" = a GUARD predicate on the get-up transition. Timed effects
  (fireball at frame 18) are ANIMATION NOTIFIES emitting intents; "animation finished" is a notify that
  emits an event back into the FSM (that is what `melee_attack + idle = melee_attack_end` really means).

## Gameplay layer → ECS bridge & apply phase (MVP BUILT in tile_frontier)

**MVP wired in `subprojects/tile_frontier` (`actor_simulation.{h,cpp}`, build rc=0, 2026-06-29).** Forks resolved:
**A = reinterpret seam** (`act::world` stays an opaque tag; cast in one `world_of(ctx)` helper),
**B = neither mood nor a GOAP planner yet** (triangles have a single state — a plan/FSM carries no signal):
the "brain" is one native `act::number_function "wander.direction"` (returns a direction index 0..7, reads
`actor_brain` through the bridge, randomness via `utils::mix` on the ctx's immutable inputs), **C = in
tile_frontier**. Concretely: `actor_move_intent` was DELETED → the buffer is now `std::vector<act::intent>`;
`actor_world_slice` owns an `act::registry` + a cached `const act::number_function*`; `think` builds a
dry-run ctx (`sink==nullptr`), invokes the brain fn, emits an `intent_kind::move_to` (payload.target =
dir*speed as a VELOCITY vector for now, `source_action = string_hash("wander")`), sorts by
`get_entityid_index(actor.id)`; `apply` walks the sorted buffer, switches on `kind`, and `move_to` mutates
velocity/position + bounce. No CMake change (`devils_plane` already links `devils_engine::act`). TODO next:
`call_function` intents through the typed catalogue commit pipeline; acumen/mood once actors gain real multi-state behavior;
then `script_function`/catalogue. The original design below stays valid for those next layers.

**ACUMEN BEHAVIOR LAYER ADDED (2026-06-29, build rc=0).** Actors now flee bigger actors / chase smaller
ones via GOAP. New pieces in `actor_simulation.{h,cpp}`: (1) a **target-search layer** `sense()` — naive
O(N²) pass filling a new `actor_perception` component (nearest-bigger = threat_pos/has_threat,
nearest-smaller = prey_pos/has_prey; tie-break by entity index; **grid/quadtree is the obvious next step,
N=4096**); (2) **act functions**: predicates `actor.threat_present`/`actor.prey_present` (read perception,
O(1), pure) and effects `flee`/`chase`/`wander` (mutate velocity via a `mutable_world_of(ctx)` that
`const_cast`s the world — a historical pre-catalogue MVP shortcut); (3) a 1-step **acumen GOAP** (`std::optional
<acumen::system> goap_`): metrics = the two predicates (bits 0/1), a virtual `resolved` bit (2) set by every
action, requirements encode priority (flee if threat; chase if prey & no threat; wander otherwise), goal =
`resolved`. `think` runs `compute_state` + `find_solution` (fresh `astar::container` per actor) and emits a
`call_function` intent carrying the chosen action's effect fn_id; `apply` phase 1 invokes effects in id
order (read positions pre-integration), phase 2 integrates `pos += vel*dt` (movement UNBOUNDED now — the old
min/max clamp + `actor_move_intent`'s velocity-in-`move_to` path are gone). The old `wander.direction`
number-fn was replaced.

**PERF PASS (2026-06-29, build rc=0, acumen_test still green).** Two fixes for the obvious hot spots:
(1) **O(N²) → a simple uniform grid.** `sense()`'s `find_nearest` became a `spatial_grid` class (`gtl::
flat_hash_map<int64 cell_key, vector<entry>>`, cell = `detection_cell = 3.0` world units, rebuilt once/tick);
`query()` scans the 3×3 neighbor cells — both an accelerator (O(actors-per-cell)) and a limited-vision model
(actors perceive within ~one cell; lone actors wander). Hash-map keys ⇒ arbitrary/negative coords (movement
is unbounded). Exact nearest via ring expansion is a later refinement. (2) **Fresh `astar::container`
per-actor → ONE reused** (`actor_world_slice::plan_container_`). This required an acumen LIB change:
`find_solution` now calls `a.free_solution()` after extracting the plan (success path) — the solution chain
(start..goal) that `free_unused` leaves allocated in `node_pool` is returned to the pool, so blocks stay warm
and the container is clean for the next solve. The copied plan stays valid (`action` pts into
`system->actions`, not the freed nodes). The failure path doesn't reach it (early return; `free_all` already
ran). `acumen_test` still green (it makes a fresh container per call — the free is harmless before dtor).

**PROFILE + SCALE PLAN (2026-06-30).** `perf(label, fn, args...)` wrapper (std::invoke, logs µs) wraps the
update phases. Debug 4096 actors: think 113ms / sense 42 / apply 4 / build 1.3 → 6fps. Release: think 8.0 /
sense 6.2 / apply 0.2 / build 0.07 ≈ 14.4ms, capped at `main_fps=20` — **release is healthy; 6fps was a
debug artifact.** Lesson: debug distorted RELATIVE weights (think optimized 14×, sense 7× → in release
they're near-equal, not 70/26) — profile in the target config. Principle: AI cost should scale with the
number of DECISION CHANGES, not actor count. Agreed plan (toward "millions"): **(1) memoize decisions in
acumen → (2) scheduler decimation (think on event/timeout + per-frame budget; count-budget for determinism)
→ (3) kD-tree + AABB tree in utils for sense → (4) MT think across workers.**

**ACUMEN MEMOIZATION DONE (2026-06-30, acumen_test 2/2).** A decision is a pure function of (system, goal,
relevant state bits) and A* is deterministic → EXACT memoization. New `acumen::solution_cache` (cache.h,
header-only): key `plan_key{goal_id, relevant bits projected into array<u64, state_words>}`, value
`cached_plan{array<u16 action-index, max_plan>, length, full_length}`, BYTE budget → entry cap (insert is a
no-op when full → solve live, no churn), `merge()` to share a warm table across threads, hit/miss stats; NOT
thread-safe by design (one-per-thread + merge between frames). `max_plan = 8` (`#ifndef`-define
`DEVILS_ENGINE_ACUMEN_MAX_PLAN` in common.h). `system` computes `relevant_mask_` = ∪ of all action/goal masks
in its ctor (bits outside it can't affect the plan → not in the key → actors in the same relevant state share
one solve). `system::decide(start, goal, goal_id, cache, scratch, out)`: hit → copy plan; miss →
`find_solution` into scratch + insert; `assert(goal.mask ⊆ relevant_mask)` for soundness. Wired into
tile_frontier `think` (`plan_cache_` member, `goal_id = string_hash("resolved")`). Once-warm cost = hash +
probe instead of A*. RESULT (release 4096): **think 8ms → ~350µs**; sense (~6ms) became the new dominant.

**kD-TREE FOR sense (2026-06-30, build rc=0, utils_general_test 5/5).** New reusable
`utils::kd_tree<T, Scalar=float, Dim=2>` (header-only, `libs/utils/.../kd_tree.h`): payload-agnostic static
k-d tree (implicit, in-place median `nth_element` split), `clear()` keeps capacity (per-frame arena reuse),
`insert/build`, `nearest(q, max_radius, pred)` (distance-pruned NN with predicate filter), `radius(q, r,
pred, visit)`. NOTE: nearest REQUIRES a radius — a predicate-NN with no match (e.g. "nothing bigger") else
degrades to a full scan (best=∞ ⇒ no pruning); the radius both prunes and models limited vision. tile_frontier:
the hash grid is gone, `actor_world_slice::sense_tree_` (member), `perception_target{size, entityid}` payload,
`perception_radius = 4.0`; `sense` rebuilds once/tick and runs one `nearest2` query/actor
(threat=bigger, prey=smaller).
Test in utils_general_test compares nearest/radius vs brute force (by distance, not id — tie-robust), the
radius bound, exclude-self, empty tree. Tie-break is "first found by distance" — add a total order for strict
determinism later.

**kD QUERY OPT + SCHEDULER (2026-06-30, build rc=0; historical baseline, superseded by 2026-07-17 measurements).** Split timing then showed sense ≈ build 0.55ms + **query
~5ms** (query dominated at that stage; later decimation/MT made build the producer floor). Incremental kD
modification "breaks balance" since balance is a one-time median property — incremental belongs to grid/DBVH,
not kD). Added `kd_tree::nearest2(q, r, predA, predB)` — both nearest in ONE traversal with shared pruning
(descend the far branch only if closer than BOTH bests) — and dropped `perception_radius` 8→4. sense query
~5ms → ~3.3ms; test `nearest2 == 2×nearest` added. (For uniform-dense dynamic + fixed radius a cell list
beats kD, deferred; kD kept for non-uniform/cull/raycast — grid raycast = DDA/Amanatides-Woo, needs no AABB.)
Then the **cognition scheduler** — the structural lever so AI cost ∝ BUDGET not actor count: new
`actor_cognition{uint64 last_think}` component; `update` split into sense snapshot/build (kD over ALL)
+ `cognition(tick)` + `apply`. `cognition`: (1) scan `view<actor_cognition>`, collect "ripe" actors
(last_think < tick), priority = staleness (overdue = tick − last_think, longest-waiting first, id tie-break →
deterministic), `nth_element`-cap to `think_budget_` (=512, ~1/8 of 4096, a member knob); (2) only the
selected get a kD perception query + GOAP decide → intent, then `last_think = tick`. Unselected coast on their
last velocity. `apply` phase 2 still integrates ALL every tick (cheap, smooth motion). count-budget (not
time) keeps it deterministic; `metrics.intents` now = thinkers/tick. Hooks for later: LOD (weight priority by
camera distance — needs the camera passed into `update`), events/dirty (`last_think` far in the future =
"asleep until an event"), decision-point timestamps. The O(N) selection scan and integrate-all are fine at
4096; at millions → timing-wheel/buckets + active-set culling.

**COMMIT WINDOW + MT + 64k STRESS (2026-06-30, release).** (1) **Commit window**: `actor_cognition` is gated
by `commit_ticks_ = 3` (a stand-in for action/animation duration) — an actor re-decides every 3 ticks;
`think_budget_` raised 512→2048 (above demand N/commit so the commit window, not the budget, is the binding
constraint → lag ~60ms not 150). (2) **MT cognition**: selection/cap stays single-threaded (cheap); the heavy
`due_` sweep is fanned via `pool.distribute(count, job)` (the `atomic_pool` from `container->pool`); the
scratch slot = `pool.thread_index(this_thread)` (0 = caller, 1.. = workers; arrays sized pool.size()+1) so
per-thread `plan_containers_`/`plan_caches_`/`intent_buffers_` are exclusive; `decide_actor` runs per actor;
`pool.wait()`; then merge buffers → `intents_` → sort by id. DETERMINISM HOLDS (selection total order, A* +
per-thread caches deterministic, final sort by id). Safe: each actor handled by exactly one thread (disjoint
perception/cognition writes); `world.get`/`sense_tree.nearest2`/`goap.decide` are read/const; all allocators
created in init (no lazy create under MT). `atomic_pool` is NOT std::function-based — `task_t<F,Args>` lives
in a fixed arena (`stack_pool`), captures ≤128B with no heap. cognition 2.5ms → ~0.5ms (~5×; ceiling = the
serial Amdahl fraction: scan + nth_element + merge/sort). (3) **64k actor stress**: update ~13-16ms (still
capped at main_fps 20). **`sense.tree` ~8.5-14ms DOMINATES** — the kD build is O(N·log N) over ALL actors,
single-threaded = the undecimated "producer floor". `cognition` ~0.9-1.9ms (budget+MT keep it low); `apply`
~0.5-1.1ms and `build` ~0.7-1.1ms are O(N) over 64k SINGLE-THREADED and jumped more than cognition →
candidates for deterministic MT / incremental structures. Confirms: at scale the producer (sense.tree) and
single-threaded O(N) phases bite, the decimated consumer (cognition) does not.

**kD PARALLEL BUILD + TAIL SNAPSHOT (2026-07-17).** This resolves the single-threaded producer floor for
the current slice without making the tree incremental. `kd_tree::build_parallel(pool)` partitions a small
deterministic top frontier, then builds disjoint subtrees with the existing recursive algorithm. Actor sense
gathers the next snapshot after structural changes and builds it at the tick tail concurrently with
`actor_batch::build`; positions at end N equal positions at start N+1, so cognition semantics are unchanged.
Init/load uses a synchronous fallback snapshot. Release 4096×120 improved from 1.479/1.532/1.316/1.165 to
1.304/1.248/0.937/0.865 ms/tick for 1/2/4/8 pool workers; a longer 4096×600 control measured
1.438/1.360/1.085/0.920. Hash/bytes remain identical within every worker-count sweep.

**Perf cleanup (2026-07-13).** The interim `utils::perf` helper has no live consumers and was archived
under root `exclude/`. tile_frontier actor phases now use catalogue domain/introspection wrappers directly;
future service-channel/UI graphs should extend that path instead of reviving the one-shot timer.

### Spatial structures & geometry primitives (libs/utils)

Reusable, dependency-free (no glm) spatial acceleration in `libs/utils/include/devils_engine/utils/`:
`geometry.h` (primitives + predicates), `kd_tree.h`, `grid.h` (`dense_grid` + `hash_grid`), `aabb_tree.h`
(dynamic BVH). All header-only. kd_tree is rebuild-per-frame (arena reuse); the grids and the BVH support
BOTH incremental `insert`/`remove`/`update` by stable handle AND bulk rebuild.

- **Vector type is a template parameter `Vec`, NOT `(Scalar, Dim)` + std::array.** `Vec` is the math
  vector type (default `std::array<float, Dim>`; the author will later feed `glm::vec4` regardless of
  2D/3D). Access is via `operator[]`; `Dim` = number of SIGNIFICANT axes (may be < Vec's component count —
  a `vec4` with `Dim=2` uses only `[0],[1]`); `Scalar` is deduced from `Vec` (`geom::scalar_of<Vec>`).
  `UpAxis` (template, default 1 = Y) is the "up" axis for the 3D `up_cylinder` query only.
- **One geometry contract drives all three structures.** Every shape provides `geom::overlaps(shape, aabb)`
  (node pruning + box-leaf test) and `geom::contains(shape, point)` (point-leaf test). Because `up_cylinder`
  needs `UpAxis`, structures call the dispatch wrappers `geom::test_overlaps<UpAxis>` / `test_contains<UpAxis>`
  (they forward `UpAxis` for `up_cylinder`, ignore it for all other shapes). `geom::query_bounds<UpAxis>(shape)`
  returns the shape's iteration AABB (±inf for unbounded shapes) for grid structures.
- **Unified query API: one overloaded `query(shape, visit)`** on every structure. `visit(const T& payload)`.
  Shapes: `aabb / sphere / up_cylinder / ray / cylinder / obb`. Primitives: `aabb{min,max}`,
  `sphere{center,radius}`, `up_cylinder{center,radius}` (infinite along UpAxis — 3D "radius column"; in 2D
  a radius query is just a `sphere`), `ray{origin, dir(normalized), tmax=inf}`, `cylinder{origin, dir, length,
  radius}` (a finite "ray with radius", FLAT caps: `0<=dot(p-o,dir)<=length && perp_dist<=radius`),
  `obb{center, half, axis[Dim]}` ("AABB + orientation"; axis = orthonormal world-space local axes).
- **Point stores vs box store.** `kd_tree`, `dense_grid`, `hash_grid` store POINTS (leaf = `contains(shape,
  point)`). `aabb_tree` stores EXTENDED objects, each with its own AABB (leaf = `overlaps(shape, box)`).
  Consequence: a `ray` query is degenerate on point stores (measure zero — `contains(ray, point)` is always
  false; use a `cylinder` = ray-with-radius instead) but MEANINGFUL on `aabb_tree` (ray vs bodies).
- **Per-object bounds on a point store → `geom::inflate(shape, margin)`** (Minkowski sum with a ball radius
  `margin`): "a body of radius r overlaps `shape`" ⟺ "its center ∈ `inflate(shape, r)`". Exact for spheres
  (sphere/aabb grow exactly), CONSERVATIVE for cylinder/obb (rounded caps/corners → superset; add a precise
  per-payload filter in `visit`). Inflate by the MAX object radius. `inflate(ray, m)` returns a `cylinder`
  radius `m` (a thick ray). For HETEROGENEOUS per-object bounds use `aabb_tree` (stores each object's AABB).
- `kd_tree` keeps its existing specialized `nearest`/`nearest2`/`radius` (predicate NN with distance pruning,
  used by tile_frontier sense) AND gained the generic `query`. The `Vec` migration is backward compatible —
  `kd_tree<T>` still defaults to `std::array<float,2>`, so tile_frontier's `sense_tree_` is unchanged. kd_tree
  is static (parallel rebuild once per frame) — no incremental remove.
- **Grids share `detail::grid_pool`**: ONE flat entry pool (no per-cell `std::vector`) with a free-list
  (stable indices, holes reused) + DOUBLY-LINKED per-cell chains → `insert`/`remove`/`update` are O(1).
  Returned `grid_handle{index, gen}` survives neighbour add/remove (`gen` catches a stale handle on a reused
  slot); `remove`/`update` on a stale handle are safe no-ops. `dense_grid(origin, dims, cell_size)` = bounded
  grid, arithmetic cell index `cy*width+cx` (no hashing); out-of-bounds points clamp to edge cells and edge
  cell bounds are open to ±inf so pruning never drops a clamped object. `hash_grid(cell_size)` = unbounded
  `gtl::flat_hash_map` by integer cell coord (arbitrary/negative coords); `query` clamps `query_bounds` to the
  occupied range and picks cheaper of {iterate cell box} vs {iterate occupied cells}. GRID GOTCHA: a new
  `hash_grid` cell head must init to `-1` via `try_emplace(c, -1)` — `heads_[c]` (operator[]) value-inits to
  `0`, a VALID entry index → corrupts the chain.
- `aabb_tree` = DYNAMIC BVH (Box2D `b2DynamicTree` style): node pool + free-list, `insert(box,payload)->
  bvh_handle` (best-sibling by SAH surface-area cost + refit ancestors + AVL-like rotation balancing),
  `remove(handle)` (collapse parent, refit), `update(handle, box)` = detach+attach (handle PRESERVED). Leaf
  stores the EXACT object AABB (no fat margin) → query exact. `rebuild()` re-optimizes but INVALIDATES handles.
  Convention: leaf ⇔ `child1==null`; free node ⇔ `height==-2` (threaded in free list). BVH GOTCHA hit:
  `remove` must decrement `leaf_count_` (detach alone keeps `size()` stale though queries stay correct).
- `cylinder`×AABB `overlaps` is CONSERVATIVE (segment bbox expanded by radius) — over-visits, never
  under-visits; the exact leaf test (`contains(cylinder,point)` / body AABB) removes false hits. Tighten with
  a segment/box distance test later if it matters. `obb`×AABB is exact SAT (2D: 4 axes; 3D: 15 axes with edge
  cross products, degenerate crosses skipped). Tests + brute-force cross-validation of all shapes across all
  four structures (2D+3D), plus incremental add/remove/update sequences and `inflate`, in
  `tests/utils_general_test.cpp` (kd_tree/dense_grid/hash_grid agree on points; aabb_tree agrees on boxes).

Next (agreed): **gameplay/animations** — also the real commitment source to generalize the scheduler (see the
AI scheduler notes). Perf picture is closed for now; further synthetic tuning is diminishing returns. Deferred
perf targets if needed: MT for apply/build + sense.tree (or an incremental cell-list), cell list/AABB in utils.

**GAMEPLAY/ANIMATION/SOUND LAYER BUILT (2026-06-30, tile_frontier; debug+release rc=0).** Author decisions:
brain = **GOAP-arbiter → FSM-executor** (acumen picks an action by priority → that action's hash IS the FSM
event → mood holds state/animation/commit/sound); save model = **snapshot of the replicated set** (phase E,
not started). All apply-phase mutations stay in entity-id order (determinism). Phases:
- **A — drives + priority ladder.** `actor_drives{hunger,boredom}` (0..1, REPLICATED). Predicates
  `actor.is_hungry`/`actor.is_bored`. Ladder via acumen requirements (complete disjoint partition, 1-step plan):
  `threat→flee`, `hungry&prey&in_range→eat`, `…&!in_range→chase`, `hungry&!prey→seek_food`, `bored→wander`,
  else `think`. Drive dynamics passively in apply for ALL actors/tick: hunger always rises; boredom rises while
  still (thinking), falls while moving → think⇄wander oscillation.
- **B — mood FSM + animation.** `actor_state{uint64 state-hash}` (REPLICATED; uint64 to keep string_id out of
  the header). `mood::system` from `"any_state + <action> = <state>"` lines. In apply, after the movement effect:
  `mood::step(event = intent.payload.call.fn)` (the action hash == mood's event_hash) → on a REAL state change,
  `apply_transition`. Animation = `animation_scale(state,tick,phase)` sinusoid in `actor_batch::build` (slow
  think … fast eating/flee) — **DERIVED, not stored** (a clean derived-vs-replicated example for save). `build`
  now takes `tick`; the pass is heterogeneous (food/obstacles have no `actor_state` → base size).
- **C — eating handshake (= the REAL commitment source).** `actor_eating{target,until_tick}` + `actor_grabbed{by}`.
  Perception stores the prey's FULL entityid (`prey_id`; kD payload `perception_target{size,id}`). Metric
  `prey_in_range` splits chase/eat. `effect_eat` (apply, id-order → deterministic: lower id grabs first; guards:
  prey exists / not grabbed / not itself eating / in range). FSM guard `[is_eating]` (name MUST be dot-free — the
  mood parser rejects '.' in identifiers; acumen metric names `actor.*` are fine, they never hit that parser)
  gates entry to `eating` to a successful grab. cognition SKIPS eating + grabbed actors → commit by action
  duration, retiring the `commit_ticks` stand-in for eaters. `resolve_eating` (after apply): hunger=0, drop
  `actor_eating`, `remove_entity` the prey (kill-list AFTER the view loop). apply also skips a grabbed actor's own
  queued intent; build_sense_tree excludes grabbed. `eat_duration=18`. Verified working (population drops, eaten
  count grows).
- **C2 — food items + obstacles.** `food_item` = small (0.2) static green entity; in the sense tree it reads as
  "prey" and is consumed by the SAME eat handshake (reliable — it never flees), **respawned** by `maintain_food`
  to `count/8` (cap 64/tick) → fixes depopulation. Zero new GOAP states. `obstacle{radius}` = gray disc, EXCLUDED
  from the sense tree, positional collision resolved in the integration pass (push-out; flat `obstacles_` cache,
  count capped 64).
- **D — sim sounds (HAS AN OPEN BUG).** `sound_for_state` binding (eating→chomp, flee→alert) emits
  `sound_emit{name,pos}` on FSM state ENTRY (EPHEMERAL, NOT replicated). Presentation bridge in `tile_frontier_game.cpp`
  culls by listener (camera) proximity + caps 8/tick → `command_sound{name}`. The sound actor preloads a named
  set from `resources/sounds/<type>/` and plays by `name` one-shot (explicit volume/pitch=1 — `sound::task` is a
  POD). **OPEN BUG: only ONE sound is audible in a run** — suspected sound-system issue, deferred, to debug with
  logging. test.mp3 was removed by the author (it was licensed). The sound dividing line (sim/replay-log vs
  presentation; the sound actor as a dumb mixer; 3 layers resource/trigger/policy) is documented — sim sounds are
  deterministic & replay-for-free, presentation sounds (UI click, ambient player) go straight to the actor and are
  never in the replay log; resources are owned by the asset system (handles), the UI owns playback policy only.

Tuning: hunger_rate 0.08, hungry_threshold 0.5, bored_rate 0.14 / relief 0.30 / threshold 0.5, eat_radius 0.9,
eat_duration 18, perception_radius 4.0, commit_ticks 3, think_budget 2048.

Also tidied `libs/acumen` (before the phases): `system::decide(const decide_params&, std::span<out>)` — inputs
bundled into a struct, `cache` optional (nullptr ⇒ no memoization), `out` a separate trailing arg; `find_solution`
demoted to a file-local static in system.cpp (decide is the sole public entry); hashes moved to a new
`libs/utils/.../hash.h` (fmix64/fmix32/hash_combine/wyhash64/splitmix/murmur_hash3_32). `murmur_hash64A` stays in
type_traits.h (tied to `type_id`).

REMAINING: D-UI (button click `ui_effect` + a Nuklear ambient player — presentation→sound directly, the player is
UI state); phase E (save = replicated-set snapshot + a replicated/derived classification; needs a
`world::for_each_pool` serialize hook in aesthetics — not present yet).

Next step after acumen/mood were ported to `act`: run GOAP/FSM over live `aesthetics::world` entities
and apply their decisions deterministically. NOTE: `subprojects/tile_frontier`'s `actor_world_slice`
(`actor_simulation.{h,cpp}`) ALREADY does a mini version of this — `think(tick)` → `intents_` (sorted
`actor_move_intent`) → `apply(dt)` mutates components. So this is an EVOLUTION of that code (replace the
hardcoded brain with acumen/mood, generalize `actor_move_intent` → `act::intent`), not greenfield, and is
prototyped in tile_frontier first (library-first: formalize into a lib later).

Planned layers:
- **`exec_context` ↔ `aesthetics::world` bridge (lives in tile_frontier, NOT in act — act stays
  ECS-agnostic).** `exec_context.w` is typed `const act::world*` which is an OPAQUE TAG (`act::world` is
  never defined — see the forward-decl comment in `exec_context.h`). The bridge is a reinterpret seam:
  build a ctx per entity with `c.w = reinterpret_cast<const act::world*>(&aesthetics_world)`, `scope[0] =
  act::entity_id{ uint32_t(entityid) }` (the FULL packed aesthetics id — version bits included — round-trips
  through the uint32), and a `world_of(ctx)` helper = `*reinterpret_cast<const aesthetics::world*>(ctx.w)`.
  Native predicates/effects are authored in tile_frontier (where both act and aesthetics are visible) and
  reinterpret `ctx.w` back to the concrete world. Hot path = pointer cast, no virtuals; act keeps zero
  dependency on aesthetics.
- **"think" system** (per entity with a brain/agent component): build ctx with `sink == nullptr`
  (dry-run — planning must not mutate), `system.compute_state(ctx)`, `find_solution(...)` into a stack
  `std::array`, take the first action → emit an `act::intent` (`call_function{fn}` / `move_to{target}`).
  mood analog: `fsm_event` → `mood::step` → if transitioned, emit an intent.
- **intent buffer** = `std::vector<act::intent>` sorted by `actor.id` (the generalized `intents_`). This is
  the "fast event buffer" — a sorted-by-entity array with an advancing cursor.
- **apply system** walks the buffer in id order and mutates each entity: `move_to`→velocity/position;
  `fsm_event`→`mood::apply_transition`; `call_function`→record/commit through the typed catalogue
  pipeline. Deterministic because provenance and commit order are fixed.

Validated facts grounding the design:
- **The cursor-advance model works because `aesthetics::world` iterates in ASCENDING id order**: `raw_itr`
  walks `sparce_set` (sparse index == entity index == id), skipping invalid. So an id-sorted intent buffer
  and the ECS view advance in lockstep — one pass, no per-entity lookup needed.
- **The live pipeline has one gameplay intent consumer plus the player queue.** Typed catalogue strategy
  executors own deferred gameplay effects; replay is not a catalogue consumer responsibility.
- think reads the world (pure predicates) ⇒ parallelizable; apply mutates in fixed id order ⇒ the
  deterministic barrier. Matches the "think in any order, apply in fixed order" rule.

OPEN QUESTIONS / forks (author to decide before building):
- **A. Bridge mechanism:** reinterpret seam (recommended — act decoupled, pointer cast) vs an abstract
  `act::world` interface with virtual accessors (slower, couples act to a world API).
- **B. Which backend to demo first on live actors:** mood (recommended — shortest loop event→transition→
  effect, fastest visible result; GOAP plugs in second by emitting `fsm_event`/`call_function`) vs acumen
  (GOAP planner first) vs the full GOAP→intent→FSM chain at once.
- **C. Where to build:** in tile_frontier's `actor_world_slice` first (recommended — library-first
  prototype), formalize into a lib later.

## Intent persistence/replay notes (superseded ownership; historical design)

The notes below predate the 2026-07-17 boundary decision. Their determinism advice (stable apply order,
per-entity RNG, fixed timestep, checksums) remains useful, but catalogue is no longer the owner of
replay/RPC/binary persistence. If persistence becomes real work, move those responsibilities into a
separate library and reassess the old channel design rather than restoring it into live catalogue.

**Intent as the thinker→ECS seam.** GOAP output is funneled through one compact `intent` struct of
base actions ECS systems consume (turn toward, move, call script, send FSM event, ...). This is the
deterministic-command / Quake-`usercmd` pattern: the single struct type is what makes observability
*tractable* — one serialization point instead of millions of scattered scripts. Hold onto this seam.

**Historical proposal only (superseded):** catalogue once had the following bones:
`INPUT_CHANNEL_ID / MUTATOR_CHANNEL_ID / META / SERVICE` (`channel_data.h`), an abstract `consumer`
(≤8 per channel), `rpc_function`/`mutator` wrappers.
- `intent` → INPUT channel (this is "the input" in catalogue's "log inputs, replay the rest" model).
  Log intents ALWAYS — compact, feeds replay + text export.
- mutations-after-apply → MUTATOR channel, but FILTERED/opt-in (specific entity + tick range) for
  focused debugging. Do NOT log mutations for millions of actors every frame — replay reconstructs
  them in the deterministic case.
- disk writer = just a `consumer` subscribed to the channel.
- **Authoritative on-disk format = binary (varint/zpp_bits), not text.** Text every frame × millions
  = I/O death. Text is an on-demand DECODE of a tick/entity slice; `tavl::serialize<T>` of the intent
  struct is the natural human-readable export (we're adopting tavl anyway).
- **Provenance for "why" not just "what":** the intent (or a META record) must carry which
  `action`/`goal` produced it (maybe plan id) so a trace reads as intent → action → goal. Without it
  you see commands, not decisions.

**Determinism (author is committed to pushing this hard).** Already done: seeds everywhere,
devils_script built around them, bindings hardwire PRNG as pure "next-from-previous".
- **The #1 real risk here is mutation ORDER under multithreading, not float bits** — tile_frontier is
  explicitly a millions-of-actors multithreading stress test. Determinism = deterministic order of
  state mutation first, bit-exact arithmetic second. The intent seam is the fix: think in parallel /
  any order, but APPLY intents in one barrier phase in a fixed order (sort by entity_id, never
  "whoever's message arrived first"). Under the actor model this means an explicit apply phase, not
  apply-on-message-arrival.
- **PRNG must be per-entity sub-streams**, not one global stream: derive `hash(global_seed, entity_id,
  tick, purpose)` (counter-based / splitmix style). Pure next-from-previous on a single shared stream
  still cascades — changing how many draws entity A makes shifts B and C, and order matters. Per-entity
  derivation makes RNG robust to iteration order and to adding/removing a draw. Check whether bindings'
  PRNG is currently one-per-context (the trap) and revisit before scripts pile up.
- **Float flags checklist:** kill `-ffast-math`/`-funsafe-math-optimizations` (the #1 killer, breaks
  determinism even same-binary across opt levels); `-ffp-contract=off` (FMA fusion of `a*b+c`); stay
  on SSE2, avoid x87/`-mfpmath=387`/32-bit; `sqrt` is IEEE-correct but `sin/cos/pow/exp` are NOT
  portable across libm versions/platforms (only matters cross-machine).
- **Pick the bar deliberately.** Same-binary/same-machine determinism (cheap: flags + pinned toolchain)
  fully covers the stated goal — replay, demos, "why did it decide that" debugging. Cross-machine
  determinism (own transcendentals / fixed-point / strict FMA discipline) is expensive and only needed
  for lockstep netcode. Do NOT gold-plate cross-platform float unless netcode is a real requirement.
- **Verify with state checksums** (Factorio / lockstep-RTS technique): hash full world state per tick
  (or every N) → a checksum stream; run the same intent log twice and diff; the first diverging tick
  localizes the bug. Ladder: same binary twice (catches threading order) → debug vs release (catches
  fast-math/reassociation) → cross-machine (catches libm/FMA). This rides catalogue directly: periodic
  full snapshot + checksum into META/SERVICE = the rollback design from the serialization notes, and a
  free desync detector on replay. A CI test that replays a recorded demo and asserts a golden final
  checksum is the regression guard.
- **Sim must run a fixed timestep** (time accumulator), decoupled from render FPS — a frame-varying
  `dt` silently breaks replay. Confirm the gameplay tick in `app.tavl` is fixed-step, not "elapsed".

## Demiurg ↔ Lua resource API (IMPLEMENTED 2026-07-06, first slice)

**Shipped (`d2ce7fe`).** Host-side in `subprojects/tile_frontier/src/core/simulation.cpp` (registered on the
visage sandbox env — demiurg itself has no Lua dep). Four GLOBAL keywords (no namespace), plus the
engine fully OWNS `require`:
- `request(id)` → `demiurg::resource_handle` (or `nil`). Single-resource accessor.
- `require(id)` → runs a `lua_script_resource` and returns its module. No `package.searchers`; resolves
  the handle, requires type lua (`luaL_error` "is not a lua script" otherwise), lazily `load()`s if not
  `usable()`, then `safe_script(text, env, chunk="@"+id)`. Has a `require_cache` keyed by absolute id
  (cycle guard: `true` before running, return value after; reset on error) and a `require_stack` so
  relative paths resolve against the current module.
- `find(prefix)` → Lua array of handles by PATH PREFIX (`resource_system::find`), across BOTH the engine
  and assets registries. `filter(substring)` → Lua array by SUBSTRING (`resource_system::filter<>`).
- `resource_handle` is a Lua usertype (`sol::no_constructor`): methods `valid/id/hash/state/usable/`
  `final_state/top_state`, each reads through `handle.get()` and returns `nil` if unresolved.
- Path resolution: `absolute_resource_path(current_module, path)` — trim, `\`→`/`, split `:selector`,
  strip leading `/`, resolve `.`/`..` vs `resource_parent_path`, drop extension, collapse segments.
  `lookup_resource_handle` searches engine registry first, then assets. (FIXED 2026-07-06: the
  segment-collapse loop as shipped in `d2ce7fe` never advanced `pos` past the final segment and spun
  forever — the FIRST `request()` from `entry.lua` hung main; `pos = end + 1` unconditionally now.)
- **Fonts through the same API (2026-07-06):** `nk.push_font{ font = request("fonts/crimson.italic"), … }`
  — the `font` field takes a `resource_handle`; visage resolves `handle.get<font_resource>()` and falls
  back to default while metrics aren't ready. String font names / `add_font` are gone. Backed by the
  `type_id`/`loading_type_id` split (exact identity vs loader-dispatch base) in demiurg — see
  "Stable handles" bullet.
- New resource type `lua_script_resource` (tile_frontier, `warm_and_hot_same`, text): `load_cold` reads
  script text (`ensure_text_loaded`, supports `path:name`/list-section entries), `unload_warm` drops it.
  Registered `engine_resources->register_type<lua_script_resource>("ui","lua")`.
- Lua-facing shift: `visage::system::load_entry_point(path)` REMOVED → `set_entry_point(const sol::object&)`
  (validates `sol::function`). `init()` bootstraps UI via `require("ui/entry")` and passes the returned
  function to `set_entry_point`. `entry.lua` builds `resources = { grass = request("textures/grass"), … }`
  (handles, not names). `app.image(...)`/`app.play_sound(...)` now take a `resource_handle` (or table with
  `resource=`/`res=`); the old `image_by_name`/`sound_by_name` string maps are GONE (`app.image` downcasts
  to `painter::gpu_texture_resource`, `nil` until `usable()`; `command_sound_play.res` = `resource_ref::from_handle`).
- **Did NOT land** (still design/open below): the "type IS the path" import-rule table (longest-segment
  type match, `trait/icon`→blit rules) — type is still `find_proper_type`'s segment match; `find` here is
  PREFIX-based, not by-kind; `:name` for lua modules is still effectively undefined.

Original design rationale (kept for the open items). Demiurg is fundamental enough that these are GLOBAL
keywords (no namespace), alongside Lua's `require`; the engine fully OWNS `require` (no `package.searchers`
hook) because every script comes from demiurg/mods (there is no plain-lua-from-disk case).

- **Four globals, two orthogonal axes (what to return × how many):**
  - `require(path | resource)` → runs and returns the lua MODULE. Script-only; a passed resource
    handle returns its module when the resource type is lua.
  - `request(path)` → the resource HANDLE itself (inspect size/source/metadata). The single-resource
    accessor (`get<T>`-equivalent). Note: `request`/`require` differ by 2 chars — typo-prone.
  - `filter(prefix)` → collection by PATH PREFIX (by location).
  - `find(type)` → collection by TYPE (by kind) across all paths/mods. This work ALSO fixes the
    dangling-ref bug (`view<>::operator[]`/`find<T>` currently returns a dangling reference).
- **Type IS the path (kills per-resource config):** the directory structure declares the type, and
  one `type → import rule` table in demiurg drives processing (`trait/icon` → blit 32×32,
  `spell/icon` → 256×256, `*/scripts/*` → load as lua). This table replaces scattered
  `{type,path,size}` configs and is what makes `require` "smart" (sees type=lua → returns a module).
  - name / cache key = full normalized path + filename without extension.
  - type = LONGEST registered contiguous-segment match anywhere in the path (`trait/icon/abc` beats
    `trait/icon`); position-independent — `effect/icon` matches both `/act2/spells/effect/icon/abc`
    and `/monsters/abilities/effect/icon/monster123/def`; everything around the type is just name.
  - leaf (last path segment) is ALWAYS excluded from type matching — it is the filename.
- **`:` selector — multiple data entries per file:** `path:index` or `path:local_name` indexes INTO a
  file. Cache the file ONCE by base path; selectors are cheap indexes into the loaded collection (not
  reloads). Sub-resources inherit the file's type and should be enumerable in `filter`/`find`. `:name`
  for lua-type resources is undefined (return `module.name`? or unsupported — `:` is for data/atlases).
- **Load ordering (for correctness):** register type rules → register+resolve all resources (mod
  override = a later mod shadows the same logical path) → THEN run scripts; so by the time `require`
  runs the path already points to the final overridden version and cache-by-path is correct.
- **Open (next session):** segment-boundary matching (whole segments, not substrings —
  `trait/icon` ∌ `traits/iconology`); tie-break when equal-length type matches at different positions;
  behavior when NO type matches (raw/untyped resource vs load error).

## Build/Layout Notes

- Root CMake builds an interface aggregate target `devils_engine::devils_plane`.
- Public includes are under `include/devils_engine/...` inside each `libs/*` directory.
- The root target and `subprojects/tile_frontier` use C++23 via `devils_engine::options`.
- `CODE_STYLE.md` is the canonical project style guide. Text uses UTF-8/LF, own code uses
  2-space indentation and snake_case, macros use SCREAMING_SNAKE_CASE, and third-party naming/layout
  is preserved. `.gitattributes`, `.editorconfig` and `.clang-format` encode the mechanical subset;
  do not run whole-tree formatting together with behavior changes.
- Dependencies are mostly fetched via CMake `FetchContent`, including `tavl`, `devils_script`, `miniaudio`, Vulkan-related libraries, Nuklear, msdfgen, glm, Catch2, etc.
- Root `README.md` was replaced on 2026-07-05 with a human-oriented map of `libs/` only. It is intentionally organized as separate sections per library: role, current shape, relationships, and status. Keep future `tests/` documentation separate unless explicitly asked.
- Documentation split requested by the author: root `README.md` should stay Russian for now and serve as a human-oriented overview for the author; `AGENTS.md` is the place for agent memory, technical gotchas, exact contracts, shutdown/order notes, and implementation details. Future README may become English later, but do not preemptively switch it.
- `libs/flow` is now an active CMake target. Current contract: animation = chain/graph of states; state has `duration_mcs`, `next` index, `images` as `demiurg::resource_handle + mirror_state` (migrated from raw `resource_interface*` in `8ce23c1`), `action` as `utils::id` (`invalid_id` = none), and `uv` delta. Runtime emits action messages; it must not call `act` effects directly from render/flow thread. `libs/bindings` has a CMake target but no local README, so its root README section is reconstructed from headers/sources.
- `libs/catalogue` current contract: use it as a small utility wrapper around selected function calls, not as `act` replacement, serializer or replay engine. Public active pieces are `devils_engine/catalogue/introspection.h`, logging, `call_log` and `deferred.h` (`mt::executor`, `record_scope`, collect/elect, `fn_deferred_ptr`). Old `core.h`/`registry.h`/`channel_data.h`/`rpc_function.h`/`demo.h` are archived under ignored `exclude/` and must not be revived into catalogue merely to add replay.
- Current `libs/` layering from CMake: `options` is the common interface build contract; `utils` is the low-level base; `act` feeds `mood` and `acumen`; `demiurg` feeds resource-backed `sound`/`painter`/`visage`; `input` feeds Vulkan/GLFW integration for `painter`; `bindings` and `visage` are tightly coupled for Lua/Nuklear UI.
- Be explicit about legacy/prototype code in docs and changes: `utils::actor_ref`, old painter renderer/image-container files, abandoned sound source classes and the completed OpenAL path/A-B lab are archived under ignored `exclude/`; live sound is miniaudio-only; `catalogue` is still a prototype, not stable netcode.
- Linux portable runtime bundling is handled by `cmake/devils_portable_runtime.cmake`: it sets local RPATH to `$ORIGIN` and runs `file(GET_RUNTIME_DEPENDENCIES)` after build. The dependency copy script writes resolved `.so` files into the executable's `bin/`, removes stale/symlink `.so*` entries, names copied libraries by SONAME when available, and deliberately excludes the ELF loader plus core glibc-family libraries (`ld-linux`, `libc`, `libm`, `libdl`, `libpthread`, etc.).
- **SOL2 GOTCHA, MEASURED 2026-09-04: never hand `table::set_function` a bare lambda when another
  lambda with the same parameter list is registered from the same function.** sol2 stores the functor
  in a userdata and caches its `__gc` in the lua registry via `luaL_newmetatable(name)`, where `name`
  comes from the DEMANGLED type name. On gcc a lambda demangles as `func()::<lambda(args)>` — no
  ordinal — so two distinct closures with the same parameter list get the SAME metatable name, the
  second reuses the first's `__gc`, and its functor is destroyed through the wrong layout. Confirmed
  directly: `sol.main()::<lambda(sol::table)>.user` is identical for two lambdas with different
  captures. The failure appears as `free(): invalid size` inside `lua_close`, i.e. nowhere near the
  registration, and capture ORDER decides whether it crashes or corrupts silently (a `this`-first
  capture read a bogus string past the object and happened not to abort). Fix used in
  `libs/originator/src/originator/script_host.cpp`: bindings are declared as FUNCTIONS — member
  function pointers (`set_function(key, &script_host::method, this)`) and free functions used as
  usertype methods — never lambdas, because a named function's type name is unique by construction.
  Where a binding must carry a bound name, the closure is built in LUA instead (a factory returning
  `function(args) return run(name, args) end`), which costs no C++ type at all. Note sol2 does NOT
  accept two bound leading arguments for a free function pointer, and that is what forced the choice.
  Related trap already recorded: `sol::optional` in a non-trailing parameter.
- `vulkanmemoryallocator-hpp` is header-only from this project's point of view but brings Vulkan headers/VMA targets through CMake; keep those targets explicit in consumers such as `tile_frontier` instead of assuming a system Vulkan SDK layout. `msdf-atlas-gen` is not header-only; `artery-font` support is intentionally not needed.
- Current focused contract tests live in `tests/thread_general_test.cpp`, `tests/utils_contract_test.cpp`, `tests/sound_system_test.cpp`, and `tests/catalogue_introspection_test.cpp`. `thread_general_test` covers `atomic.h`, `atomic_pool.h`, `lock.h`, legacy `queue1`, and the new `thread::spsc_queue`; `utils_contract_test` covers `memory_pool`, `stack_allocator`, and `fixed_pool_mt` size/alignment checks; `sound_system_test` covers sound math/format helpers, task/resource defaults, playback-device enumeration, and a guarded `system` construction smoke test; `catalogue_introspection_test` covers mirrored wrapper pointers for free functions, methods, const methods, structural functors, dry-run, and rolling stats. Newer: `tests/flow_test.cpp` (animation parse/playback/sampling), `tests/painter_shader_prepare_test.cpp` (assets-side GLSL→SPIR-V prepare + demiurg include resolution + list-pattern render config), and `tests/demiurg_resource_loader_test.cpp` (external GPU step + dependency gating + **stable `resource_handle` survives `clear()`+re-parse**).

## Collaboration Notes

- The author is comfortable with Russian comments and design notes in code; do not remove them just for cleanup.
- Keep changes scoped. Many comments are active design thinking, not dead noise.
- When changing architecture, first identify the existing exploratory direction and make the smallest concrete step that clarifies a contract.
- Avoid large rewrites unless explicitly requested.
