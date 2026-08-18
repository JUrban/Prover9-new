# Josef crash analysis and lifecycle repair

## Executive result

The Josef failures are not evidence that the compact implementation can only
handle the CHAT or Osborn inputs.  They expose general lifecycle defects at
boundaries that the short tests did not exercise:

1. Josef 01 and Josef 02 enter the compact terminal-proof boundary and receive
   `SIGSEGV` after the final live statistics have already been frozen.  The
   first repair removed an unsafe per-hint teardown and made crash reporting
   survive partially released state, but the mature rerun proves that this was
   not the complete cause.  Terminal proof handling still destroys archive
   and index owners from inside a nested inference callback, before the
   producer releases retrieval state and a materialized passive pin.
2. Josef 03 uses `set(hint_match_once)`.  More than one clause can acquire the
   same `matching_hint` while the clauses are pending in limbo.  Retaining the
   first clause retired the hint; retaining another already-matched pending
   clause retired it again.  The second retirement subtracted equivalence
   memberships from the packed sidecar twice and ended with the explicit
   `better_deactivate_hint: equivalence count underflow` failure.
3. The first exact Josef 02 replay of the safe-point repair exposed a separate
   variant error in the replacement denial-ancestry walker.  Proof ancestry
   contains both clause and formula `Topform`s, but the metadata-only sign
   query passed the Formula arm of the shared body union to
   `negative_clause()`.  It reproduced all 13,006 givens and then faulted in
   that query before it could register or snapshot the terminal proof.

The repair makes active hint membership an explicit Topform invariant, makes
the active-to-retired transition idempotent, retains retired hints as stable
proof/checkpoint owners, preserves retirement over checkpoint/resume, and
uses a checked bulk destroy for a terminal packed index.  These rules apply to
legacy, packed, better-packed, and packed-fast hint modes; they are not keyed
to any Josef formula, symbol, hint count, or search schedule.

Commit `0bddd18` repairs the independent hint-retirement and bulk hint-index
lifecycle described below.  It does **not** repair the remaining nested
terminal-proof ownership defect.  That structural follow-up is implemented
by `9c3d48a` (cancellable inference producers) and `d81e56b` (safe-point
proof finalization), with the permanent regression matrix in `4f2d287`.
The result-ownership audit also found that expanded proofs were copying
non-clausal formula premises through the clause arm of the `Topform` union;
`9878358` makes proof copying variant-aware and keeps autosketch hints
clause-only, with the multi-search regressions in `1d443bd`.
The exact mature replay then found the analogous read-side violation;
`da4abdc` makes resident and archived sign metadata variant-safe and adds
mixed-ancestor terminal and all-backend archive regressions.
The separate exact mature Josef 02 acceptance replay has now passed on
`da4abdc`: it reproduced all 13,006 givens byte-for-byte, produced a closed
proof, and exited normally.  The pre-`da4abdc` replay remains diagnostic
evidence; the successful post-fix replay and its resource audit are recorded
below rather than inferred from the focused tests.

## Evidence from the supplied outputs

### Josef 01: the search did not diverge

`Josef_01.out.old` (original Prover9) and `Josef_01.out` (compact Prover9)
contain exactly the same 30,827 printed given-clause lines, byte for byte.  The
SHA-256 of that extracted sequence in both files is:

```
c13273c0ccc8a7e3e57306e6a7922952c2c41699575fa259968f45544dc4faa8
```

The common final given is clause 36193890.  Original Prover9 then derives
36195463 and the empty clause 36195464, reports `proofs=1`, and proves the
theorem.  Compact Prover9 reaches the same final given and immediately reports
`Prover catching signal 11`; its attempted signal statistics stop after the
heading.  This rules out a scheduler or inference-order explanation for
Josef 01.  It places the failure after the accepted search trajectory, at the
new terminal lifetime split.

The old result is also a useful scale reference: 1,602,769,536 generated,
36,195,388 kept, 30,827 given, and a 5,105-clause proof.  Old Prover9 reports
45,258.48 MB from its allocator and 19,064.46 user seconds.  The compact run's
last complete report has 1,592,161,420 generated and 36,047,225 kept at given
30,749; the missing final statistics are a consequence of the recursive
failure in crash reporting, not evidence of an earlier search cutoff.

### Josef 02: deterministic unresolved terminal signature

Josef 02 reaches given 13,006 after at least 121,904,137 generated and
8,756,754 kept at its last periodic report.  It then has the same output
signature as Josef 01: `Prover catching signal 11`, followed only by the
statistics heading.  There is no original-P9 Josef 02 file in the supplied
set, so its exact proof boundary cannot be compared independently.  The
signature and code path identify terminal teardown as the strong diagnosis.

The subsequent `Josef_02.out.new1` and latest-code `Josef_02.out1` runs both
repeat all 13,006 given-clause bodies exactly (SHA-256
`ac15b3a8aea930e4b6c67d71a60b28aa2ac37e84977611885ae34247eb7136ea`) and
crash at the same boundary.  Therefore `0bddd18` did not fix Josef 02 and the
earlier report was too optimistic.

A focused archive-backed unit-conflict reproducer exposes the remaining
mechanism without a multi-hour search.  With a nonzero passive cache,
terminal teardown aborts because it attempts to evict the currently pinned
conflicting unit.  With cache zero (the Josef setting), the run can reach
proof output but destroys the archived conflicting parent first, producing an
open proof that cites a missing clause.  Both outcomes have the same root
cause: destructive terminal work occurs before the inference callback has
returned ownership.

The first from-start replay of the safe-point implementation reached the
same complete 13,006-given trajectory.  All printed given lines compare
byte-for-byte with `Josef_02.out1`; the SHA-256 of those complete lines in
both files is:

```
f5e0dfa2cbbaf6865656e2db16fd53e7827d4a0f1ff011da166b68abb5d3417a
```

It nevertheless received `SIGSEGV` before incrementing `proofs` or entering
safe-point finalization.  The symbolized native stack is:

```
negative_clause
handle_proof_and_maybe_exit
unit_conflict
cl_process
back_demod
limbo_process
search
```

The faulting instruction dereferences the fake `Literals` pointer supplied by
`clause_negative_by_id()`.  Josef's proof ancestry includes original
non-clausal conjunction/implication assumptions, so this is a deterministic
mixed-Topform boundary, not an index parameter or clause-number effect.  The
metadata-only ancestor walker was introduced by the safe-point repair; the
older materialized-proof `first_negative_clause()` explicitly skipped
formula nodes.  Thus this stack identifies a new general regression in the
replacement walker rather than disproving producer cancellation.

That diagnostic replay took 8:40:44 wall time (28,129.07 user and 2,109.61
system seconds) and peaked at 5,005,296 KiB RSS.  It had `VmSwap: 0` throughout
the monitored final interval.  A supplementary checkpoint-resume process was
stopped after aggregate machine pressure became unsafe; only the exact
from-start replay was retained, and it finished with about 6.3 GiB available
and memory PSI 0.00.  The checkpoint resume was not trajectory-equivalent
because the current checkpoint boundary records a selected given before its
inference transaction, so it is not acceptance evidence.

### Josef 02: post-fix mature acceptance

The sole post-`da4abdc` from-start replay used an optimized native/LTO build
and the unchanged reconstructed Josef 02 input.  Its complete 13,006 printed
given lines are byte-for-byte identical to `Josef_02.out1`, with the same
SHA-256 shown above.  Unlike the reference and diagnostic runs, it then
reported:

```
Given=13006. Generated=129776312. Kept=9226457. proofs=1.
Max_Clause_ID=9226479.
THEOREM PROVED
Exiting with 1 proof.
```

The process exited with status 0 and had no signal, fatal, sanitizer, or
buffer-overflow diagnostic.  Running `prooftrans parents_only` over the
completed output also exited 0, produced a 30,795-line closed ancestor proof,
and ended in clause 9226479, `$F`; no parent was missing.

The acceptance run took 23,202.61 user and 1,886.83 system seconds, or
6:58:21 wall time.  `/usr/bin/time -v` measured 5,124,928 KiB peak RSS and
zero swaps.  A 30-second watchdog independently sampled the prover's RSS,
`VmSwap`, system `MemAvailable`, swap availability, and memory PSI.  Prover
`VmSwap` and PSI remained zero, the watchdog recorded no pressure hit, and no
other Prover9 process was run concurrently.  Therefore this is both the exact
search-path reproduction and the terminal proof/ownership acceptance run,
not a shorter proxy.

### Additional supplied `new2` evidence

The later supplied outputs add two independent checks of scope and identify
one duplicate artifact:

- `Josef_01.out.new2` contains all 30,827 given clauses byte-for-byte
  identical to `Josef_01.out.old`, then the previous binary faults exactly
  where old P9 records `proofs=1`.  This confirms a second instance reaches
  the same terminal boundary without search divergence.
- `Josef_02.out.new2` is byte-for-byte identical to `Josef_02.out1`; it is a
  duplicate artifact, not another independent experiment.
- `Josef_03.out.new2` follows all 15,634 old given clauses exactly and exits
  normally with one proof after 460,797,485 generated and 17,344,322 kept.
  It therefore crosses the original duplicate-retirement boundary without
  the former equivalence-count underflow.  It used 25,993.93 user and
  2,737.82 system seconds (28,809 seconds wall); its final PSS report is
  8,829,502 KiB.

### Josef 03: checked duplicate retirement

Josef 03 finishes with a complete diagnostic report and the unambiguous fatal
message:

```
Fatal error:  better_deactivate_hint: equivalence count underflow
```

At that point it has 106,926 total hints: 67,755 redundant, 20,084 still
active, and 19,087 matched/retired.  It has processed 15,634 givens,
generated 460,505,643 clauses, and kept 17,221,283.  The error is therefore a
long-run state-transition failure: one matching hint can be referenced by
several pending clauses, but the old `keep_hint_matcher()` implementation
unconditionally removed it for every retained matcher when
`hint_match_once` was set.

The packed equivalence count made the corruption visible.  The same logical
bug also existed in the legacy hint index, where a second index deletion did
not have the same checked counter and could corrupt index state less
diagnostically.

## Root causes in the old implementation

### Active membership had two incomplete representations

The packed implementation had a `Packed_hint_active[id]` sidecar, but the
hint Topform itself did not record whether it was active, redundant, retired,
or expired.  Callers performed the following state changes independently:

- ordinary `unindex_hint()`;
- `hint_match_once` retirement in `keep_hint_matcher()`;
- periodic expiry;
- back-demodulation unindex/reindex;
- checkpoint reconstruction;
- normal shutdown; and
- terminal proof teardown.

Several of those paths directly edited the packed bit and counters instead of
using one transition.  None could reliably distinguish the first retirement
from a repeated one.  In particular, zeroing the positive and negative
literal counts after the first retirement made a repeated equivalence
membership calculation nonzero while the global live count was already
zero, producing Josef 03's underflow.

### Terminal teardown used a live maintenance operation as a destructor

The terminal compact optimization needs to keep hint Topforms alive for proof
annotations while freeing search-only posting tables.  It previously walked
`Glob.hints` and called `unindex_hint()` on every clause before calling
`done_with_hints()`.

That has the wrong semantics and complexity for destruction:

- every ordinary removal advances the logical hint epoch and changes active
  or redundant counts;
- every removal tests membership in the redundant clist;
- better-packed removal can invoke the normal stale-posting rebuild policy;
- a rebuild materializes and recompresses surviving hints; and
- the loop runs after terminal statistics are frozen and after compact
  passive, rewrite, unit, nonunit, back-demodulation, and shared-term
  structures have begun disappearing.

The tiny preprocessing-proof test added with the optimization could verify
that proof annotations survived, but it could not exercise this teardown with
100,000--150,000 hints and a mature search state.

### Related lifecycle holes

The audit found three adjacent issues before they had to become separate
large-run failures:

- Expiry removed the only owning `Glob.hints` clist entry even though archived
  clauses retain only the stable hint ID.  Later proof-link restoration could
  no longer resolve that ID.
- Checkpoint metadata distinguished active and redundant hints but not
  retired hints.  Resuming a match-once/expired checkpoint silently reindexed
  the retired hint.
- Checkpoint verification hashed transient FPA IDs on materialized packed hint
  Topforms, although the packed hint bank has no authoritative FPA index.
  The result depended on materialization timing rather than logical state.

### Metadata-only sign lookup crossed the `Topform` union

The safe-point repair must identify the first negative denial ancestor before
materializing a potentially enormous proof.  Its new ID/record walker called
`clause_negative_by_id()`.  The archived branch read a sign bit, but the
resident branch called `negative_clause_possibly_compressed()` without first
checking `is_formula`.  Because formula and clause bodies occupy the same
union, atomic formulas were deterministically misclassified as negative
clauses and compound formulas could be traversed as invalid literal lists.

The same helper was also used when writing and querying generic ancestor
records.  An atomic formula could therefore acquire `AF_NEGATIVE`, preserving
the wrong classification after archival.  The invariant must live at the
Topform/record boundary: a formula is a known Topform but is categorically not
a negative clause.  Filtering a particular Josef ID, formula shape, or proof
position would merely hide the representation error.

## Implemented repair

### Cancellable inference ownership

Inference result callbacks now return a Boolean continuation decision.
Binary resolution, hyperresolution, UR, factoring, paramodulation, indexed
unit conflict, and their bounded traversal variants propagate cancellation
back through every nested producer.  A cancelled producer releases live
Mindex retrieval positions, contexts, trails, equality flips, paramodulation
positions, and bounded-iterator state before returning.

The proof consumer no longer tears down search state or `longjmp`s from one
of those callbacks.  A terminal empty clause is registered, the proof count
is advanced, and a pending-terminal flag asks the producer to cancel.  If a
new clause participates in unit conflict before acquiring a search
container, a terminal-transient owner keeps its official ID and ancestry
alive during unwinding.

### Safe-point proof snapshots

Search finalizes a pending proof only after preprocessing or the complete
given-clause inference transaction has returned.  At that point no producer
is on the C stack.  The finalizer asserts that the compact passive cache has
no outstanding pins, freezes statistics while their indexes remain live,
and reconstructs every proof against the still-authoritative ID/archive
namespace.

Every reconstructed DAG is closure-checked: each justification parent must
occur in the proof.  It is then deep-copied into detached Topforms owning
their bodies, attributes, and justifications but no clause-ID, archive,
container, selector, or compact-index membership.  Closure is checked again
on the copy.  Only after all archive materializations and terminal
transients have been released are compact search indexes and packed hint
indexes discarded.  Proof output, proof actions, and
`collect_prover_results()` use the detached snapshot, so none can query
destroyed storage.

### Result ownership preserves the `Topform` variant

A returned proof is not a list of clauses only.  Its input premises can
include original goals and other non-clausal formulas.  `Topform` stores a
formula and a literal list in different arms of a union, selected by
`is_formula`.  The first expanded-proof integration run exposed an old API
assumption: `copy_clause_ija()` treated every returned node as a literal list,
so copying the goal premise traversed its Formula pointer as Literals.

The shared deep-copy operation is now `copy_topform_ija()`.  It copies the
selected body variant plus ID, justification, and attributes; clause bodies
retain term flags.  Terminal snapshots and whole-proof copies use this
operation.  Expanded-proof construction preserves formula premises directly
and performs clause replay and literal-uplink checks only on clause nodes.

The inverse mistake is prevented at the real type boundary.  Proof and
expanded-proof results remain complete mixed DAGs, but autosketch copies only
clause nodes when deriving the next search's hints.  Thus formulas are neither
misread as clauses nor passed to the clause-only hint index.  Result cleanup
deep-frees both ordinary and expanded snapshots before the next child search.

The closure check is proportional to proof size: it sorts the IDs in the DAG
and performs binary membership lookups.  It does not allocate by the largest
clause ID, which matters for mature runs with tens of millions of retained
IDs.

### Sign metadata preserves the `Topform` variant

`negative_clause_possibly_compressed()` is now the variant-safe sign boundary.
It returns false for a null or formula Topform and only inspects compressed
sign metadata or literals for a clause.  Consequently resident ID queries,
disabled-store queries, and new ancestor records all share the same rule.

Archived readers additionally require `AF_IS_FORMULA` to be clear before
honoring `AF_NEGATIVE`.  Formula-kind metadata therefore dominates even for
an older record whose writer set both bits.  `clause_negative_by_id()` still
reports such an existing formula ID as known, but returns false for its clause
sign.  This is a representation invariant with no Josef symbols, IDs, search
limits, or scheduling conditions.

### One authoritative transition

`struct topform` now has a one-bit `hint_indexed` state.  It fits in the
existing bitfield word: the measured Topform size remains 104 bytes, so this
does not add per-clause RAM.

All active removal goes through idempotent `unindex_hint()`:

- redundant membership is removed once;
- active membership is removed once;
- already retired/expired membership is a no-op;
- packed Topform, ID-table, and active-bit identity must agree;
- active and redundant counter underflow is checked; and
- only a real transition advances the hint epoch.

`keep_hint_matcher()` records every match count, but calls that transition
only while the hint is active.  This preserves degradation statistics for
multiple pending matchers without deleting the index entry twice.

Back-demodulation continues to use unindex/reindex, now with explicit checks
that the old membership ended before a new membership begins.  The same
transition is tested in legacy, basic packed, better-packed, and packed-fast
modes.

### Stable retirement and checkpointing

Expired hints are retired from candidate indexes but remain in the owning
hint clist.  They already had to remain allocated because retained clauses
hold `matching_hint` pointers.  Keeping the small clist ownership record also
makes stable-ID proof restoration and checkpoint serialization valid.  An
active-state guard prevents later expiry sweeps from retiring them again.

Checkpoint metadata now writes `retired_hint`.  Resume reconstructs the
active/redundant/retired partition and restores degradation weights and
`last_matched_given` afterward.  Old checkpoints without the marker retain
their old interpretation, so the format addition is backward compatible.
Packed checkpoint verification records zero for `hints_fpa`, because no such
authoritative index exists; the actual packed semantic structures and hint
trace remain independently verified.

### Bulk terminal destruction

Terminal packed proof processing now uses `discard_packed_hint_indexes()`.
It performs a linear sidecar audit, verifies that every packed active bit
agrees with the Topform state and that the active count agrees with both,
clears the lifecycle bits, detaches the redundant ownership side-list, and
destroys the index once.  It does not run stale-posting maintenance or a
redundant-list lookup for every hint.

The complexity is O(packed ID capacity plus allocator teardown), rather than
ordinary per-hint logical removal with possible repeated posting rebuilds and
redundant-list scans.  The hint Topforms in `Glob.hints` remain available to
proof printing and result collection, exactly as intended by the original
terminal memory optimization.

### Failure reporting after teardown

Signal and memory-limit reporting now detects frozen terminal statistics.  It
replays the frozen snapshot instead of querying freed/partially freed live
indexes.  This does not make a future bug acceptable, but it prevents the
secondary crash that erased the useful Josef 01/02 diagnostics.

Normal `SIGUSR1` reports are unchanged before terminal teardown.  Empty-hint
search cleanup now also destroys the initialized hint package instead of
leaking it.

## Validation performed

### Focused lifecycle tests

The hint component regression now covers all four index families.  In each
case two candidates acquire the same match-once hint before either is kept;
both are then retained, and only the first retirement changes the index and
epoch.  It also checks repeated ordinary cleanup, two expiry sweeps, stable
owner retention, and counter state.  A separate packed-fast case bulk-discards
256 active hints and checks every lifecycle bit.

The hint checkpoint suite now checkpoints a retired match-once hint, requires
the `retired_hint` metadata, resumes it without reactivation, obtains 0 failed
verification fields, and matches the uninterrupted hint trace and final
search counts.

The terminal regression constructs the archive ownership boundary directly:
usable `-p(x) | q(x)` and SOS units `p(a)` and `-q(a)` make generated `q(a)`
find its contradiction while the compact unit index owns a materialized pin
on still-cold `-q(a)`.  With the old ordering, cache one aborts while trying
to evict that pinned clause; cache zero prints an open proof omitting the
conflicting parent.  Both cache configurations now produce the same closed
five-step proof, and `prooftrans parents_only` verifies both parents.

A second fixture makes hyperresolution return an empty conclusion directly
from a nested clash traversal.  It produces a closed four-step proof with the
nucleus and both satellites, proving that the safe-point protocol is not
limited to the unit-conflict entry path.

The same boundary passes after checkpoint/resume with all checkpoint
verification fields intact.  Consumer cancellation/restart tests cover
eager and bounded paramodulation and hyperresolution, plus binary
resolution, UR, factoring, and indexed unit conflict.  Each producer cancels
after its first result and then completes a fresh traversal, detecting stale
retrieval or substitution state.

ASan+UBSan builds pass the cache-zero terminal case, the cached terminal
case, ordinary `x2`, the expanded producer cancellation tests, and compact
checkpoint/resume.  The sanitizer runs disable only leak reporting because
the surrounding historical process retains global package state; address
and undefined-behavior failures remain fatal.

The result-ownership matrix also passes under ASan+UBSan.  One autosketch run
returns an expanded `x2` proof containing its non-clausal goal premise.  A
second run proves a goal from an extra assumption, converts the first mixed
proof to clause-only sketch hints, frees both returned DAGs, and starts a
second child search that exits normally with `sos_empty`.  This covers copy,
expansion, consumer filtering, destruction, and reuse rather than merely
printing one proof.

The sign-query regression is deterministic without a large search: a valid
atomic Formula begins with `ATOM_FORM == 0`, which the old literal walker
classified as a negative clause.  The bookkeeping test now checks resident
formula Topforms and ID-table queries.  The ancestor test archives,
rematerializes, and queries a formula through memory, mmap, and file backends.
All of those assertions fail before `da4abdc` and pass afterward.  A compact
terminal integration proof retains a compound implication premise and an
atomic goal in its mixed proof DAG, completes normally, and passes
`prooftrans parents_only`.  The component tests and the complete compact audit
also pass under ASan+UBSan.

### Supplied full hint banks at a bounded terminal boundary

For each Josef output, the echoed input was reconstructed, the full hint bank
was retained, and a fresh contradictory predicate was injected into SOS so
the run crossed terminal proof cleanup immediately.  Each run had a
300-second/2-GiB internal bound plus a 320-second external bound.  These are
teardown tests, not substitutes for the many-hour searches.

| Input/configuration | Hints | Result | Wall time | Peak RSS |
|---|---:|---|---:|---:|
| Josef 01 | 153,681 | theorem, normal exit | 8.41 s | 156,564 KiB |
| Josef 02 | 148,330 | theorem, normal exit | 8.46 s | 135,968 KiB |
| Josef 03 (`hint_match_once`) | 106,926 | theorem, normal exit | 5.85 s | 106,268 KiB |

The frozen reports confirm that packed postings were genuinely populated
(18,273,864, 24,363,684, and 15,511,404 reference bytes respectively) before
bulk terminal destruction.  No Josef-specific formulas or index parameters
are present in the repair.

### Broader suites

The following pass with the repair:

- optimized build;
- `make test1`;
- hint postings/preview/compressed-unit component suite;
- compact OTTER proof audit;
- compact OTTER checkpoint/resume, including the cold terminal boundary;
- compact unit, nonunit, rewrite, back-demodulation, ID-map, cold-store, and
  selector checkpoint/compaction component tests;
- DISCOUNT, collective-frontier, eager-demodulation, dense-passive, hint
  trace, and hint checkpoint suites;
- long-run compact index scaling and its report tests;
- compact generalization smoke manifest;
- allocator, bookkeeping, ancestor-store, and memory lifecycle tests;
- full optimized build of all prover, model, and utility programs; and
- ordinary LADR and TPTP proof/status smoke tests; and
- expanded-proof autosketch result ownership across two child searches.

### Mature acceptance status

The first exact from-start safe-point replay completed the full 13,006-given
trajectory and supplied the symbolized formula-as-literals stack above.  It
did not prove the theorem.  Commit `da4abdc` repaired that exact fault with a
representation-wide invariant.  The subsequent exact from-start replay now
establishes normal theorem output, proof closure, and exit status 0 after the
same complete trajectory.  Mature Josef 02 acceptance is therefore closed.

The older pre-fix control was stopped near given 11,327 when running it
concurrently with a supplementary resume caused unsafe aggregate RAM
pressure; it cannot provide a terminal stack and is not used as acceptance
evidence.  The accepted replay was instead run alone under the RAM/PSI/swap
watchdog described above.

## What is and is not established

Established:

- Josef 03's reported underflow has a direct reproducible state-machine cause
  and an index-independent fix.
- The packed hint path safely destroys each complete supplied Josef hint
  bank.
- Inference cancellation and safe-point snapshotting repair the nested
  callback/archive ownership violation in direct, cached, checkpointed, and
  sanitizer executions.
- Match-once retirement now composes with limbo batching, expiry, ordinary
  shutdown, and checkpoint/resume.
- The fix adds no Topform RAM and removes potentially expensive terminal
  maintenance work.
- Josef 01's 30,827-given compact trajectory is identical to original P9;
  its failure is not search divergence.
- The first safe-point Josef 02 replay exactly matches all 13,006 supplied
  givens and localizes its remaining failure to a mixed-Topform sign query.
- Resident and archived formula sign queries now obey one tested invariant
  across memory, mmap, and file backends.
- The post-`da4abdc` Josef 02 replay exactly matches all 13,006 givens, exits
  normally with one theorem, and yields a closed `prooftrans parents_only`
  proof.
- The supplied Josef 03 `new2` run passes the original 460-million-generated
  duplicate-retirement boundary and completes the same 15,634-given proof.

Not yet established without rerunning the expensive jobs:

- Josef 01 has not yet completed a post-`da4abdc` mature proof
  reconstruction.  Its exact previous-code `new2` trajectory localizes the
  failure but is diagnostic only.
- The bounded injected-contradiction runs validate full-bank initialization
  and destruction, not the CPU/RAM curve of the complete searches.

This distinction is intentional: the code repair is general and locally
verified, while the supplied week-scale jobs remain the only honest final
end-to-end acceptance tests.

## How to rerun

No new Prover9 options are required.  Rebuild the branch and use the original
Josef inputs/options unchanged.  In particular, keep `hint_index=packed_fast`
and keep `hint_match_once` on Josef 03; disabling the feature would avoid the
old symptom but would not test the repaired semantics.

Capture stderr as well as stdout so any native backtrace is retained:

```sh
/usr/bin/time -v ../bin/prover9 < Josef_01.in \
  > Josef_01.fixed.out 2> Josef_01.fixed.err
```

Acceptance for Josef 01 should include the old-P9 proof boundary (30,827
givens, clauses 36195463/36195464) and a normalized proof comparison.  Josef
02 has met its normal theorem, complete-statistics, exact-trajectory, and
closed-proof gates above.  Josef 03 has also passed its original full-search
boundary; future repetitions should compare hint totals, matched
identities/trace if enabled, and the final proof/search outcome rather than
only elapsed time.
