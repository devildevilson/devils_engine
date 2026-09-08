# NET-LAB-01 — the same session as several real processes

The fixture that NET-06/07/08C ran inside one process, executed by **separate
operating-system processes over real UDP**: one authority, three followers and one
intruder, with the engine's frozen handshake (SESSION-02), the reconnect credential
(SESSION-03), the reconnect policy (SESSION-04) and the hot intent class (HOT-01) all on
the same wire at once. It is the first place where those six closed slices meet.

Run it:

```
NETLAB01_multi_process --verify              # spawns every process and asserts the outcome
NETLAB01_multi_process --verify --quiet      # what ctest runs
NETLAB01_multi_process --authority --rendezvous DIR [--address A.B.C.D] [--final-tick N]
NETLAB01_multi_process --follower  --rendezvous DIR --index I [--resume] [--address ...]
NETLAB01_multi_process --intruder  --rendezvous DIR --index I
```

`--listen`/`--connect` name the endpoint outright, for peers on machines that share no
directory; `--rendezvous` stays for the local harness, which needs a port the operating
system picked. `--followers`, `--tick-ms` and `--final-tick` are the operator's knobs.

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
continuous  tick=70  root=15053296469727158310   (authority + 3 followers)
            admissions=5  from_hold=1  migrated=1  late=1  duplicate=37
            multi_principal=7  no_capacity=1  chunks=8  deferred=1
            follower 0: transport_loss=1 reconnects=1 recoveries=1
            follower 1: silence_loss=1 warnings=1 reconnects=1 recoveries=1
            follower 2: reconnects=0 recoveries=0
killed      tick=42  root=12358525782810267697   (announced_final 30 -> 42)
            admissions=4  from_hold=1  recoveries=1  no_capacity=1
```

Both roots are **identical in Debug and Release, across repeated runs, and across a
loopback and a real non-loopback interface**. That is a property of the state, not of
luck: the causal state is integer-only and the tick is its only coordinate, so neither
timing nor the transport path can enter it. The per-process check counts do vary between
runs, because a check fires per message and the message count depends on scheduling; the
harness's own 41 checks are fixed.

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

What the target machine must have, and nothing else — **four** entries, down from
eighty-five:

| Dependency | Why it stays dynamic |
| --- | --- |
| `libc.so.6`, `libm.so.6`, `ld-linux` | glibc. The build host's symbol versions are the floor; see below. |
| `libcrypto.so.3` | GameNetworkingSockets requires OpenSSL for AES-GCM/SHA-256 and offers only OpenSSL or libsodium — there is no bundled option for that pair. **Any OpenSSL 3.0 or newer**: the artifact's only symbol version is `OPENSSL_3.0.0` and all 41 symbols it imports are 3.0-era EVP/HMAC/RAND entry points, verified rather than assumed. A 1.1-era distribution (`libcrypto.so.1.1`) will not run it. |

Everything else is linked in: Abseil, protobuf, GameNetworkingSockets, the engine
libraries, and the C++ runtime. The three things that got it there:

- **Abseil is vendored.** Protobuf needs it and protobuf's dependency script prefers
  `find_package(absl CONFIG)`, so on a machine that has Abseil installed the artifact
  quietly depended on **seventy-nine system shared libraries**. The vendored protobuf was
  only half vendored while that was true. The root build now declares Abseil at the version
  protobuf 36.1 names for itself, before protobuf, which is enough — protobuf's script opens
  with `if (NOT TARGET absl::strings)`.
- **`-static-libstdc++ -static-libgcc`.** The C++ runtime is the version-sensitive
  dependency, not its presence: this build uses C++23 features whose libstdc++ symbols are
  newer than many distributions ship. About a megabyte for a whole axis of risk.
- **`-Wl,--as-needed`.** Without it the artifact also declared `libzstd.so.1`, reached only
  through a corner of `devils_utils` it never calls.

Size: 8.7 MB (5.1 MB before, when 79 of its dependencies lived on the host).

### The glibc floor, and what it costs

```
objdump -T NETLAB01_multi_process | grep -oE 'GLIBC_[0-9.]+' | sort -Vu | tail -1
```

For the artifact built here that is **GLIBC_2.38**, so roughly Ubuntu 23.10+, Debian 13,
Fedora 39+ or a rolling distribution. Ubuntu 22.04 (2.35) will refuse it with a clear
loader error rather than misbehaving.

The floor is worth understanding before trying to lower it. It comes from
`__isoc23_strtol`/`sscanf` — glibc's C23 redirects, which appear because everything is
compiled as C++23 — plus `arc4random` at 2.36. Measured per source: GameNetworkingSockets
22 references, protobuf 10, Abseil 2, the engine libraries **0**. So no change to this
project's code can lower it, and dropping the dependencies to C++17 would risk an ABI
split with the C++23 engine (Abseil's `string_view` aliasing is the concrete hazard). The
real cure is to build the artifact against an older glibc — a container — which is a
packaging decision, not a code one.

### Running it across machines

There is no shared directory between machines, so the endpoint is **declared** rather than
discovered. `--listen`/`--connect` replace `--rendezvous`, which stays for the local
harness only.

```
# authority, on the machine that will own the timeline
./NETLAB01_multi_process --authority --listen 10.5.0.120:41200     --followers 3 --tick-ms 20 --final-tick 3000

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
