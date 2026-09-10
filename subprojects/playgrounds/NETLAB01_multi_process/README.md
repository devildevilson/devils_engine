# NET-LAB-01 — the same session as several real processes

The fixture that NET-06/07/08C ran inside one process, executed by **separate
operating-system processes over real UDP**: one authority, three followers and one
intruder, with the engine's frozen handshake (SESSION-02), the reconnect credential
(SESSION-03), the reconnect policy (SESSION-04) and the hot intent class (HOT-01) all on
the same wire at once. It is the first place where those six closed slices meet.

Run it:

```
NETLAB01_multi_process --verify              # runs NET-LAB-01 plus the NET-LAB-02 matrix
NETLAB01_multi_process --verify --quiet      # what ctest runs
NETLAB01_multi_process --authority --rendezvous DIR [--address A.B.C.D] [--final-tick N]
NETLAB01_multi_process --follower  --rendezvous DIR --index I [--resume] [--address ...]
NETLAB01_multi_process --intruder  --rendezvous DIR --index I
```

`--listen`/`--connect` name the endpoint outright, for peers on machines that share no
directory; `--rendezvous` stays for the local harness, which needs a port the operating
system picked. `--followers`, `--tick-ms` and `--final-tick` are the operator's knobs.
`--compatibility` and `--wire-version` are the deliberate NET-LAB-02 fault controls; the
ordinary run leaves both at `compatible`/`current`.

## What the stand asserts

The success criterion is one value: **every independent process agrees on the causal state
root at the same tick**, after every scheduled failure. Everything else is the evidence
that they got there the intended way rather than by accident.

Each failure names the roster position it hits, because with several followers "the
follower" is not a thing — and one follower is deliberately left alone, because a stand
where every peer is disturbed cannot tell whether the undisturbed path survives a
neighbour's recovery.

| Scenario | Injected failure | Hits | The path it proves |
| --- | --- | --- | --- |
| `continuous` | authority closes the connection at tick 20 | follower 0 | the transport reports a terminal state, so the silence budget is skipped entirely and `session_hold_table` serves the resume |
| `continuous` | authority stops sending at tick 45 without closing | follower 1 | nothing reports anything, so only the silence budget can notice; the resume arrives while the old connection is still nominally alive, and the session **migrates** |
| `continuous` | one batch deliberately proposes already-sealed ticks | follower 2 | the ingress refuses it as late |
| `continuous` | nobody touches follower 2's connection | follower 2 | it reaches the same tick and root with **zero reconnects**, so a neighbour's recovery cost it nothing |
| both | a fourth process presents follower 0's credential | intruder | a valid, already-used identity is refused as `no_capacity` — a different answer from `identity_rejected` |
| `killed` | follower persists its ticket and leaves with `_exit(7)` | follower 0 | a **brand-new process** rejoins on a ticket read from disk, with no state of its own and no confirmed anchor, then plays a declared tail |

Golden values on the current machine, and every number below is asserted rather than
printed:

```
continuous  tick=70  root=8518737655127057956   (authority + 3 followers)
            admissions=5  from_hold=1  migrated=1  late=1  duplicate=37
            multi_principal=7  no_capacity=1  chunks=8  deferred=1
            follower 0: transport_loss=1 reconnects=1 recoveries=1
            follower 1: silence_loss=1 warnings=1 reconnects=1 recoveries=1
            follower 2: reconnects=0 recoveries=0
killed      tick=42  root=13531786226404380819   (announced_final 30 -> 42)
            admissions=4  from_hold=1  recoveries=1  no_capacity=1
```

Both roots are **identical in Debug and Release, across repeated runs, and across a
loopback and a real non-loopback interface**. That is a property of the state, not of
luck: the causal state is integer-only and the tick is its only coordinate, so neither
timing nor the transport path can enter it. The per-process check counts do vary between
runs, because a check fires per message and the message count depends on scheduling; the
complete harness's own 125 checks are fixed.

## NET-LAB-02 compatibility matrix

The same executable now runs the incompatible half of the build exchange over real UDP,
not only through the in-memory handshake test. Six followers independently perturb one
field and each is refused with its exact wire reason:

| Perturbed field | Refusal |
| --- | --- |
| handshake format | `handshake_format_mismatch` |
| protocol version | `protocol_version_mismatch` |
| content root | `content_mismatch` |
| state schema | `state_schema_mismatch` |
| intent schema | `intent_schema_mismatch` |
| numeric profile | `numeric_profile_mismatch` |

Every refusal process asserts `tick=0`, no applied bundle, no issued challenge and no
credential admission check. The refusal therefore precedes both authentication and the
simulation rather than merely arriving before an arbitrary later tick.

The session envelope is now version 2 with an explicit compatible floor at version 1.
Both versions decode to the same canonical payload messages, and the authority keeps the
version selected by the hello for the whole exchange. A complete three-follower version-1
run reaches the same tick 70 root `8518737655127057956` as version 2. Version 3 is the
deliberately breaking case: the authority parses only the envelope, sends
`unsupported_wire_version`, and records zero challenge/admission work.

Debug and Release both pass **125/125 harness checks**. The neutral codec/handshake suite
passes **13/13 cases, 381/381 assertions**.

## Decisions this slice had to make

**The causal state is integers only.** Not a simplification — an attribution rule. This
stand exists to blame a mismatch on the protocol, and a float world would let every
failure be blamed on `libm` instead. Numeric portability is the NUM branch's job.

The integers are not a stand-in for a world coordinate either: they are exactly what the
wire carries. A hot intent transports a cell key plus a code inside that cell, and
`(key << 16) | code` is a signed count of quanta, so the authority's decode seam performs
no conversion at all. The only floating-point step in the whole path is the follower
turning an authored target into a split, which is the one step HOT-01 already confined to
`fixed_point.h`.

**The quantum is session identity, so the numeric profile is derived from it.** Two peers
with different quanta decode the same codes into different world values — a systematic,
silent disagreement no digest can localize, because it appears in the state and not in the
codec. This stand originally carried a literal profile number, which would have let such a
peer through the handshake; `lab_numeric_profile()` now hashes the axis split, the code
width and the causal step, so a differing peer is refused before the first tick.

**The rendezvous is a file.** A fixed port fails on a busy machine, a port scan is merely
flaky, and a pipe would need platform process handles. The authority asks the operating
system for a port and publishes what it got. Recorded while doing it: **the pinned GNS
backend refuses a bind on port zero**, so `listen_any` falls back to a scan and reports
which path it took (`ephemeral_bind=0` above). That is a property of the backend, not of
this stand.

**The authority's clock starts once the whole roster is seated.** A follower joining fresh
has no state at all, so a session which had already advanced would owe it a baseline
transfer — a real requirement, and the next slice's. Pretending tick zero is wherever the
authority happens to be would have hidden it. The stand found this immediately: the
follower refused a bundle for tick 21 while expecting tick 1. With one follower this rule
read as "the first admission"; with three it is the last, and that difference is exactly
why a mid-session fresh join needs its own answer.

**Provenance is the whole content of "several followers".** Not N connections — the
canonical order *across principals*. The sealed bundle is ordered by principal first, then
kind, then target, because two peers whose packets interleave differently must still seal
the same bytes, and only a total order keyed on something neither peer controls can promise
that. Correspondingly, "a redundant copy" now means same principal, same kind, same tick;
the same kind from a different principal is a different order, not a duplicate. The
authority checks the principal ordering of every sealed bundle it builds, and
`multi_principal` counts the ticks which actually carried more than one — a schedule where
they never collided would never exercise any of this, so the followers' proposal cadences
are offset on purpose.

**The principal comes from the credential, and the acting peer never names it.** A
follower which could name its own principal could name someone else's, which is the same
forgery as naming its own actor. The follower asserts that the principal it was admitted
under is the one its roster position expects, so being admitted as somebody else is a
failure rather than a surprise.

**A refusal must be delivered, not just sent.** The stand caught the authority closing the
connection immediately after handing the refusal to the transport — which discards it, so
the peer learns only that the connection died and retries, spending exactly the deadline
that SESSION-02's *terminal* refusal exists to protect. The refused connection is now held
open for a declared grace so the refusal can leave. The client side was wrong too: a
connection which dies during the handshake is now terminal for the attempt, because a
client waiting for a refusal that is never coming waits forever.

**The run's end is the authority's to declare, and it can move.** A peer which rejoins at
the very end recovers and then has nothing to do, which proves it caught up but not that
it participates again. So the grant carries the final tick, a late rejoin extends the run
by a declared tail, and the extension is re-announced to everyone still playing — a
follower holding its first grant's value would stop while the session was still running.
The extension is a *fixed* end rather than "wherever we are plus a tail", which is what
keeps the run's length, and therefore its state root, the same in every run instead of a
function of how fast a process happened to start.

**A reconnect ticket's expiry is in the authority's clock.** Two machines' monotonic
clocks share no origin, so a client which used `expires_at` directly would be using an
unrelated number that happens to be milliseconds. The grant therefore carries the
authority's issue instant and the follower anchors its own clock to it. Without that, the
"shared deadline" SESSION-04 relies on is not shared.

**A resume can arrive while the old connection is still alive.** In the quiet-window case
the authority never learns the peer is gone, so consulting the hold table would answer
`unknown` and strand a client whose path died in one direction only. The rule is
therefore: the credential proves the principal; if the session is currently attached the
authority **migrates** it and closes the stale connection, and only an unattached session
is looked up in the retention table. Both branches are exercised, one per failure.

**The bulk transfer is paced, one chunk per owner pass.** A sender which blasts a whole
checkpoint into the lane has not used the budget the lane declares. Pacing it is also
what makes the follower's deferral path a measured property: the replay bundles go out on
the higher-priority control lane and **overtake** the checkpoint, so the follower has to
hold back any bundle past the recovery target until the checkpoint has landed
(`deferred` above). With a single unpaced blast this path never ran, and the property was
a comment rather than a result.

**The stale batch is scripted.** Nothing about loopback produces a late intent on its
own, and an ingress branch nothing ever reaches is not a verified refusal. The same reason
puts a fourth process in the run: the capacity refusal is only reachable by presenting a
credential which is perfectly valid and already in use, because an undeclared token is
`identity_rejected` — a different answer to a different question.

**Link statistics need a run tens of seconds long, and that is measured.** The stand reads
GNS's real-time status at the last tick — deliberately there rather than after the linger,
where the backend reports a connection that has been idle. At the registered test's length
it still answers `quality=-1, in_pps=0`, which is the backend saying *no data*, not *no
loss*: its end-to-end statistics come from a periodic exchange with a longer interval than
the whole run. A `--final-tick 6000` run (~25 s) populates them — `quality=1`,
`in_pps≈244`, `ping_ms=0` — so a LAN measurement has to be long, not merely remote. That
is a requirement for the LAN slice, not a defect here.

## Deliberately not here

- **A project world.** The checkpoint is about sixty bytes, so the "bulk" in bulk lane is
  exercised as multi-message assembly and lane priority, not as size. A project-sized
  checkpoint belongs to the slice which attaches a real one.
- **A second machine.** `--address` is wired and proven on a real non-loopback interface,
  but every process here still runs on one host, so nothing has yet crossed a wire with
  latency or loss. The LAN matrix and the Linux↔Windows exchange are the next two, and the
  statistics note above says how long such a run has to be.
- **Recorded RTT/jitter/loss as a result.** The fields are read and reported; on one host
  there is nothing in them to record.
- **The anchor-avoids-the-transfer optimization.** A client whose confirmed anchor matches
  a retained checkpoint root could replay from its own state and skip the transfer
  entirely. The stand carries the anchor and the authority ignores it, exactly as the
  contract permits; making it an optimization is a measurement, not a correctness fix.
- **Hostile input beyond the declared codecs.** The refusals here are the ones the
  scheduled faults reach. Fuzzing is NET-11.
- **A join credential worth the name.** It is a shared laboratory token, because
  SESSION-01/03 deliberately left join identity an injected policy.

## Carrying the artifact to another machine

The stand is one executable with no data files. Build it here, copy it there:

```
cmake -S . -B build-release -DCMAKE_BUILD_TYPE=Release
cmake --build build-release --target NETLAB01_multi_process -j
# build-release/subprojects/playgrounds/NETLAB01_multi_process/bin/NETLAB01_multi_process
```

What the target machine must have, and nothing else — **six** entries, down from
eighty-five:

| Dependency | Requirement |
| --- | --- |
| `libc.so.6`, `libm.so.6`, `ld-linux` | **glibc ≥ 2.38** (symbol versions; see below) |
| `libstdc++.so.6`, `libgcc_s.so.1` | **GLIBCXX_3.4.36, i.e. GCC 15 or newer** — and one single symbol is responsible; see below |
| `libcrypto.so.3` | **Any OpenSSL 3.0 or newer.** GNS requires OpenSSL for AES-GCM/SHA-256 and offers only OpenSSL or libsodium — no bundled option for that pair. Verified rather than assumed: the artifact's only OpenSSL symbol version is `OPENSSL_3.0.0` and all 41 imported symbols are 3.0-era EVP/HMAC/RAND entry points. A 1.1-era distribution (`libcrypto.so.1.1`) will not run it. |

Everything else is linked in: Abseil, protobuf, GameNetworkingSockets and the engine
libraries. Two things got it there:

- **Abseil is vendored.** Protobuf needs it and protobuf's dependency script prefers
  `find_package(absl CONFIG)`, so on a machine that has Abseil installed the artifact
  quietly depended on **seventy-nine system shared libraries**. The vendored protobuf was
  only half vendored while that was true. The root build now declares Abseil at the version
  protobuf 36.1 names for itself, before protobuf, which is enough — protobuf's script opens
  with `if (NOT TARGET absl::strings)`.
- **`-Wl,--as-needed`.** Without it the artifact also declared `libzstd.so.1`, reached only
  through a corner of `devils_utils` it never calls.

Size: 6.2 MB (5.1 MB before, when 79 of its dependencies lived on the host).

**glibc, libstdc++ and libgcc stay dynamic on purpose, and statically linking them is a
trap worth naming.** We do not control how a distribution builds them and do not want to:
the target's own copies are built for the target's own CPU baseline. A static libstdc++
carries the *build host's* ISA choices instead — this host runs CachyOS, whose packages are
built for x86-64-v3, so its `libstdc++.a` holds ~18,000 unconditional AVX instructions with
no runtime dispatch. Linking it made an artifact that could not run on a CPU without AVX no
matter what `DEVILS_ENGINE_ARCH` said about our own code.

The cost of that choice is the libstdc++ version floor, and it is worth knowing exactly
what sets it: **one symbol**,
`std::basic_format_arg<...>::_M_handle_unrecognized()`, emitted by any use of
`std::format` under GCC 15. Without it the floor would be GLIBCXX_3.4.32 — GCC 13, which is
Ubuntu 24.04 and Debian 13. One symbol costs two GCC generations, so if the target machines
are older, that is where to look before anything else.

### The pinned deployable build

Two things about the target machines drive this, and both were measured rather than assumed.
Decoding a target's `openssl version -a` line `OPENSSL_ia32cap=0x80202001479bfffd` gives a CPU
with **SSE2 and SSE3 and nothing else** — no SSSE3, SSE4.1, SSE4.2, POPCNT, AES-NI, XSAVE or
AVX. And its libstdc++ tops out at `GLIBCXX_3.4.33`, which is GCC 14.

So the deployable build pins **GCC 14** and the baseline ISA:

```
sudo pacman -S gcc14      # or the distribution's equivalent
cmake -S . -B build-deploy -DCMAKE_BUILD_TYPE=Release       -DCMAKE_C_COMPILER=gcc-14 -DCMAKE_CXX_COMPILER=g++-14       -DDEVILS_ENGINE_ARCH=OFF
cmake --build build-deploy --target NETLAB01_multi_process -j
```

**A container is not needed, and the reason matters.** The libstdc++ floor comes from the
*headers* that emit the references, not from the host's runtime — GCC 14's `<format>` never
mentions the GCC 15 symbol, and linking against a newer `libstdc++.so` is harmless because a
symbol keeps the version it was introduced with. The glibc floor *does* come from the host,
but 2.38 is already below the fleet's 2.41. A container would only be needed to go lower than
that.

A fresh tree re-fetches every dependency; pass `-DFETCHCONTENT_SOURCE_DIR_<NAME>=…` pointing
at an existing tree's `_deps/<name>-src` to reuse them.

Measured result against the fleet:

| | artifact | server |
| --- | --- | --- |
| glibc | `GLIBC_2.38` | 2.41 |
| libstdc++ | `GLIBCXX_3.4.32` | `GLIBCXX_3.4.33` |
| C++ ABI | `CXXABI_1.3.15` | 1.3.15 |
| OpenSSL | `OPENSSL_3.0.0` | 3.5.7 |
| ISA | SSE2 baseline | SSE2 + SSE3 |

5.8 MB, six dynamic dependencies.

**Above SSE3 the artifact contains nothing that executes.** Every non-baseline instruction
lives in a zlib-ng function with an explicit ISA suffix — `adler32_avx512`,
`inflate_fast_avx2`, `crc32_fold_vpclmulqdq`, `adler32_ssse3`, `chunkmemset_ssse3` — chosen
from CPUID at runtime, plus one `xgetbv` in zlib-ng's own probe which its source guards
behind the OSXSAVE bit the target lacks. The 2,176 `tzcnt` sit in ordinary protobuf code but
decode as `bsf` without BMI1, so they cannot fault, and `__builtin_ctz(0)` is undefined
anyway.

### "CPU ISA level is lower than required"

The first artifact was refused by the target's `ld.so` with that message even though its
instructions were baseline, and the reason is worth knowing because no amount of `-march`
fixes it. `ld.so` checks the ELF property `GNU_PROPERTY_X86_ISA_1_NEEDED`, and the linker
**propagates that property from the shared libraries it links against** rather than deriving
it from the code. On a distribution whose libc is built for x86-64-v3 — this host runs
CachyOS — every binary linked here inherits a v3 requirement. Measured: a plain
`int main(){return 0;}` compiled here reports `ISA needed: x86-64-baseline, x86-64-v2,
x86-64-v3` while its own `ISA used: x86-64-baseline`. The requirement describes the build
host, not the artifact, and on the target the target's own baseline libc is loaded.

The intended fix, `-Wl,-z,x86-64-baseline`, is unusable here: binutils 2.47 aborts with an
internal error in `_bfd_x86_elf_merge_gnu_properties`. So the build drops the note after
linking with `objcopy --remove-section=.note.gnu.property`. Checked before doing it: this
artifact's note carries only the ISA markers and `feature used: x86, x87, XMM` — no CET
(IBT/SHSTK) properties, so nothing is lost. **If CET is ever enabled for this project, that
step must be replaced by lowering the ISA level rather than stripping the note.**

Verify on the artifact you built:

```
readelf -n BIN | grep 'x86 ISA needed'   # must print nothing
```

Check the result rather than trusting the flag. This counts VEX-encoded (AVX-family)
instructions per symbol:

```
objdump -d --no-show-raw-insn BIN | awk '
  /^[0-9a-f]+ <.*>:$/ { s=$2; gsub(/[<>:]/,"",s); next }
  /^[ \t]+[0-9a-f]+:/ { if ($2 ~ /^v/) c[s]++ }
  END { for (k in c) printf "%8d %s\n", c[k], k }' | sort -rn
```

Measured, both builds side by side:

| | `ARCH=AVX` | `ARCH=OFF` |
| --- | ---: | ---: |
| VEX instructions, total | 3,971 | **957** |
| — zlib-ng, dispatched from CPUID | 957 | 957 |
| — **unconditional** | **3,014** | **0** |

The 957 are the expected residue and are identical in both: zlib-ng compiles
`adler32_avx512`, `crc32_fold_vpclmulqdq`, `inflate_fast_avx2` and their siblings as
separate functions and picks between them from CPUID at runtime, so they exist in a
baseline build too and never execute on a CPU without the feature. **A residual count made
entirely of such symbols is the success signature; a hit in this project's own code is
not.** In the `ARCH=AVX` build those 3,014 unconditional instructions are the stand's own
loops and inlined `std::filesystem`/`std::string` — a `SIGILL` on the first one.

Worth recording while auditing that: the artifact links zlib-ng at all only because
`devils_utils` does, and the stand never compresses anything — 59 deflate/inflate symbols
and every one of those AVX512 paths are dead weight it carries. Removing them means
splitting `devils_utils`, which is an engine change rather than a playground one.

Three defects were found by building this way, and all three are engine-level rather than
playground-level.

**The AVX-free build did not compile at first.** `libs/utils/src/utils/core.cpp` called `_mm_crc32_u64/u32/u8`
unconditionally — SSE4.2 intrinsics which were only enabled incidentally, because `-mavx`
implies SSE4.2 — so `DEVILS_ENGINE_ARCH=OFF` failed to build `devils_utils` at all and had
evidently never been exercised. `utils::crc32c` now has a portable byte-wise path under
`#if !defined(__SSE4_2__)`, verified bit-identical to the intrinsic one across 301 lengths;
CRC32C is a standard-defined value, so the two are one quantity computed two ways rather
than a fast and a slow answer.

**`utils::info`/`warn` named spdlog's format-string type instead of asking spdlog.** They
hard-coded `std::format_string<Args...>`, but spdlog uses that type only when the standard
library advertises `__cpp_lib_format >= 202207L` and falls back to `std::string_view`
otherwise — GCC 16 reports 202304, GCC 14 reports 202110. Fixed by deferring to
`spdlog::format_string_t<Args...>`.

**A classic STL algorithm over a `views::transform` range, and this one was serious.**
`checkpoint_ring::latest_at_or_before` ran `std::upper_bound` over
`bounded_history::entries()`, which is `views::iota | views::transform`. What
`std::iterator_traits` makes of that view's iterator differs between standard libraries —
**libstdc++ 14 answers `output_iterator_tag`** — after which `std::upper_bound` compiles and
silently returns the wrong element. In the stand the consequence looked like something else
entirely: `assess_recovery` saw no checkpoint, answered `no_checkpoint`, and every reconnect
was told recovery was impossible. `bounded_history::find_entry` had the same shape and was
merely lucky. Both now binary-search the index space, which needs no iterator machinery.
NET-04's own unit test caught it — one assertion of 168 under GCC 14, green under GCC 16.

**The state root is identical across all three axes** — Debug/Release, AVX/baseline ISA, and
GCC 16/GCC 14 all give `8518737655127057956` at tick 70 and `13531786226404380819` at tick 42
— which is the integer-only causal state paying off across a change of instruction set and of
compiler, not merely of optimizer.

### The glibc floor, and what it costs

```
objdump -T NETLAB01_multi_process | grep -oE 'GLIBC_[0-9.]+' | sort -Vu | tail -1
```

For the artifact built here that is **GLIBC_2.38**, so roughly Ubuntu 23.10+, Debian 13,
Fedora 39+ or a rolling distribution. Ubuntu 22.04 (2.35) will refuse it with a clear
loader error rather than misbehaving. Note that the libstdc++ floor above (GCC 15) is the
stricter of the two, so check that one first.

The floor is worth understanding before trying to lower it. It comes from
`__isoc23_strtol`/`sscanf` — glibc's C23 redirects, which appear because everything is
compiled as C++23 — plus `arc4random` at 2.36. Measured per source: GameNetworkingSockets
22 references, protobuf 10, Abseil 2, the engine libraries **0**. So no change to this
project's code can lower it, and dropping the dependencies to C++17 would risk an ABI
split with the C++23 engine (Abseil's `string_view` aliasing is the concrete hazard). The
real cure is to build the artifact against an older glibc — a container — which is a
packaging decision, not a code one.

### The run's record

`--report PATH` writes the whole run to a file, and it is written on **every** exit path
including the failing ones — a failed run is when its record is wanted most. Pass it to each
process; comparing four terminals by eye is what the file exists to replace.

```
./NETLAB01_multi_process --authority --listen 0.0.0.0:41200 --followers 3     --tick-ms 20 --final-tick 3000 --report authority.report
./NETLAB01_multi_process --follower --connect HOST:41200 --index 0 --report follower0.report
```

Two parts, because the questions are of two kinds. Scalars answer *what happened*: the state
root and tick, the order accounting, the session events, the build's identity and the
compatibility fingerprints — those last so that two machines disagreeing can be localized by
diffing reports instead of guessed at. Then a sample table answers *what the link was like*,
which one reading at the end cannot: by then the connection has been idle through the linger
and the backend reports a stale interval. The table samples every 25 ticks with a declared
capacity of 512 rows, and reports how many rows it had to drop rather than growing.

The format is deliberately dull — `key = value` lines and one whitespace table — so `grep`
and `awk` are enough:

```
grep '^state\.' *.report              # the criterion: same root at the same tick
grep '^orders\.' follower*.report     # what a player would have noticed
grep '^compat\.' *.report | sort -u   # were these the same build and content?
awk '/^\[samples\]/{t=1;next} t' follower0.report   # the link over time
```

Three details in there are worth knowing because each was originally the wrong quantity:
`run.tick_period_ms_announced` is the authority's pacing rather than the follower's own
argument, which is not used for anything; `orders.landed_fraction` divides by the orders
actually *decided* rather than proposed, because a proposal whose tick never arrived cannot
have been lost; and `orders.pending_at_exit` keeps those proposals separate so the accounting
closes — proposed = landed + lost + unobserved + pending.

### Margin, and the one knob it sizes

The authority records for every arriving intent copy the target tick minus the tick it had
already committed: `margin.plus1` and up beat the seal, `margin.zero` and below were late.
On loopback with the default four-tick lead the three copies of an order land at exactly
`+4, +3, +2` — copy *k* has margin `lead - k` — and `margin.late_fraction` is 0. A 30 ms
round trip shifts that whole distribution down by one and a half to two ticks, which leaves
the third copy at the seal and the second marginal.

`--intent-lead N` sets how far ahead followers propose. **Pass it to the authority only**: it
travels in the grant, because the authority is what seals the tick, and setting it on one
side alone would mean running two different protocols without being told. Size it from
`margin.late_fraction` in a previous run's report — and remember what it costs, because that
is the honest part: the lead *is* input delay, so the right value is the smallest one that
keeps orders landing, not the largest one that looks safe.

### Counting orders, not copies

The authority's `late` counter is dominated by *redundant copies* which arrived after their
tick was sealed — the window working as designed — so on a 30 ms link it reads in the
hundreds while costing nothing. What a player would notice is an order which never entered a
canonical bundle at all, so the follower measures that directly: it keeps each proposal until
its tick is committed and reports `orders_landed`, `orders_lost`, and `orders_unobserved` for
proposals whose tick was crossed by a recovery replay rather than applied live. The
transport's `superseded` count is reported too — a copy the backend discarded because a newer
frame in the same unreliable lane had already arrived. On loopback the accounting closes
exactly: proposed = landed, lost = 0.

### Running it across machines

There is no shared directory between machines, so the endpoint is **declared** rather than
discovered. `--listen`/`--connect` replace `--rendezvous`, which stays for the local
harness only.

```
# authority, on the machine that will own the timeline
# 0.0.0.0 binds every interface, which removes the commonest mistake: naming an
# address the authority's machine does not have, or one the followers cannot route to.
./NETLAB01_multi_process --authority --listen 0.0.0.0:41200 \
    --followers 3 --tick-ms 20 --final-tick 3000 --report authority.report

# each follower, with its own roster position
./NETLAB01_multi_process --follower --connect 10.5.0.120:41200 --index 0
./NETLAB01_multi_process --follower --connect 10.5.0.120:41200 --index 1
./NETLAB01_multi_process --follower --connect 10.5.0.120:41200 --index 2
```

The authority prints the endpoint, the follower count it is waiting for, its pacing and
its silence budgets, plus a reminder that the session is deliberately unauthenticated
(`IP_AllowWithoutAuth`) because standalone GNS has no certificate authority. Every process
ends with one line containing `tick=` and `root=`: **the run succeeded when every root is
equal at the same tick.** A follower that was refused says so with a named reason.

A follower needs only `--connect` and `--index`. **The pacing and the run length travel in
the grant**, so they cannot be got wrong from a command line: a follower which took the
tick period from its own arguments would declare a loss every tick against an authority
pacing slower than it assumed. Pass `--final-tick` and `--tick-ms` to the authority only,
and give a LAN run **tens of seconds** if the link statistics are meant to mean anything.
(In this stand the wall pacing is not causal — the tick is the only coordinate and one tick
is one step — which is why it is announced rather than fingerprinted. A project with
authored durations converts them through the tick rate, and then the rate is causal and
belongs in the compatibility fingerprint instead.)

`--followers N` runs a reduced roster, and every count follows from it: a failure which
names a roster position is skipped when that position was not launched. The stand asserted
its way to that rule — with two followers it demanded a stale batch from a follower nobody
started, and with one it waited forever for a reconnect that could not happen. Verified at
N = 1, 2 and 3, every follower agreeing with the authority's root.

Each roster position is a distinct identity, so two followers must not share an `--index`:
the second is refused with `no_capacity`, which is exactly what the intruder in the local
harness proves.

That also explains a refusal which looks like a bug the first time it is met. Once a
follower has been admitted at a roster position and then goes away, the authority **holds**
that session for its declared window (20 s) so its owner can return with `--resume`. A
*fresh* join at the same position during that window is refused with `no_capacity`, because
the hold belongs to someone. Re-running a follower immediately after a failed attempt
therefore gets `refusal=11`; restart the authority, wait out the window, or use `--resume`.

Do not leave `--followers` at its default when launching peers by hand: the authority's
clock does not start until that many are admitted, and while it waits it prints
`authority waiting: admitted M of N followers` every five seconds so the wait is visible
rather than looking like a hang.

## The harness rule

Every long-lived child is **spawned before any child is collected**. `collect` blocks
until its child exits, so a spawn placed after one runs against a session which has
already ended. This bit the harness three times — the replacement follower, the intruder,
and the collection order of the dying follower — each time producing a plausible-looking
failure somewhere else entirely. The single exception is the replacement, whose spawn is
triggered by a death and is therefore deliberately late; that one is spelled out at the
call site.

## Files

```
lab.h        causal state, section/schema, canonical bundle, intent layout/registry,
             the follower roster, the derived numeric profile, MAC policies
link.h       one GNS endpoint, the declared lane mapping, coalesced connection
             observations, real-time link statistics
protocol.h   the stand's own message classes and the state root
authority.h  tick clock, ingress, sealing, per-session retention and recovery, admission
follower.h   proposals with redundancy, loss detection, reconnect, transactional recovery
main.cpp     role dispatch and the process harness
```
