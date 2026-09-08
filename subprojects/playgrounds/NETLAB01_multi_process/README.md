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

`--address` selects the interface to bind and connect, so moving to a second machine is a
command line rather than a code change; `--final-tick` overrides the run length, which a
LAN measurement needs (see the link statistics note below).

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
