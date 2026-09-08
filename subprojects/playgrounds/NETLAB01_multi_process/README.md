# NET-LAB-01 — the same session as two real processes

This is the first slice of NET-LAB-01: the fixture that NET-06/07/08C ran inside one
process, executed by **two operating-system processes over real UDP**, with the engine's
frozen handshake (SESSION-02), the reconnect credential (SESSION-03), the reconnect
policy (SESSION-04) and the hot intent class (HOT-01) all on the same wire at once.
It is the first place where those six closed slices meet.

Run it:

```
NETLAB01_multi_process --verify              # spawns every process and asserts the outcome
NETLAB01_multi_process --verify --quiet      # what ctest runs
NETLAB01_multi_process --authority --rendezvous DIR --scenario continuous
NETLAB01_multi_process --follower  --rendezvous DIR --scenario continuous [--resume]
```

## What the stand asserts

The success criterion is one value: **two independent processes agree on the causal state
root at the same tick**, after both scheduled failures. Everything else is the evidence
that they got there the intended way rather than by accident.

| Scenario | Injected failure | The path it proves |
| --- | --- | --- |
| `continuous` | authority closes the connection at tick 20 | the transport reports a terminal state, so the follower's silence budget is skipped entirely and `session_hold_table` serves the resume |
| `continuous` | authority stops sending at tick 45 without closing | nothing reports anything, so only the silence budget can notice; the resume arrives while the old connection is still nominally alive, and the session **migrates** |
| `continuous` | one batch deliberately proposes already-sealed ticks | the ingress refuses it as late |
| `killed` | follower persists its ticket and leaves with `_exit(7)` | a **brand-new process** rejoins on a ticket read from disk, with no state of its own and no confirmed anchor |

Measured on the current machine, and every number below is asserted rather than printed:

```
continuous  authority tick=70 root=10617983408498789030   follower tick=70 root=10617983408498789030
            admissions=3  from_hold=1  migrated=1  late=1  duplicate=7
            transport_loss=1  silence_loss=1  warnings=1  chunks=8  deferred=1
killed      authority tick=48 root=2915615940752639234    follower tick=48 root=2915615940752639234
            admissions=2  from_hold=1  recoveries=1  replayed=0  chunks=4
```

Both roots are **identical in Debug and Release and across repeated runs**. That is a
property of the state, not of luck: the causal state is integer-only and the tick is its
only coordinate, so timing cannot enter it. The per-process check counts do vary between
runs, because a check fires per message and the message count depends on scheduling; the
harness's own 14 checks are fixed.

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

**The rendezvous is a file.** A fixed port fails on a busy machine, a port scan is merely
flaky, and a pipe would need platform process handles. The authority asks the operating
system for a port and publishes what it got. Recorded while doing it: **the pinned GNS
backend refuses a bind on port zero**, so `listen_any` falls back to a scan and reports
which path it took (`ephemeral_bind=0` above). That is a property of the backend, not of
this stand.

**The authority's clock starts at the first admission.** A follower joining fresh has no
state at all, so a session which had already advanced would owe it a baseline transfer —
a real requirement, and the next slice's. Pretending tick zero is wherever the authority
happens to be would have hidden it. The stand found this immediately: the follower
refused a bundle for tick 21 while expecting tick 1.

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
own, and an ingress branch nothing ever reaches is not a verified refusal.

## Deliberately not here

- **A project world.** The checkpoint is about sixty bytes, so the "bulk" in bulk lane is
  exercised as multi-message assembly and lane priority, not as size. A project-sized
  checkpoint belongs to the slice which attaches a real one.
- **Several followers, and several machines.** This slice is one authority and one
  follower on loopback. The LAN matrix and the Linux↔Windows exchange are the next two.
- **The anchor-avoids-the-transfer optimization.** A client whose confirmed anchor matches
  a retained checkpoint root could replay from its own state and skip the transfer
  entirely. The stand carries the anchor and the authority ignores it, exactly as the
  contract permits; making it an optimization is a measurement, not a correctness fix.
- **Hostile input beyond the declared codecs.** The refusals here are the ones the
  scheduled faults reach. Fuzzing is NET-11.
- **A join credential worth the name.** It is a shared laboratory token, because
  SESSION-01/03 deliberately left join identity an injected policy.

## Files

```
lab.h        causal state, section/schema, canonical bundle, intent layout/registry, MAC policy
link.h       one GNS endpoint, the declared lane mapping, coalesced connection observations
protocol.h   the stand's own message classes and the state root
authority.h  tick clock, ingress, sealing, retention, admission, recovery planning
follower.h   proposals with redundancy, loss detection, reconnect, transactional recovery
main.cpp     role dispatch and the process harness
```
