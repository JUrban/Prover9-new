# Josef crash analysis and lifecycle repair

## Executive result

The Josef failures are not evidence that the compact implementation can only
handle the CHAT or Osborn inputs.  They expose two general lifecycle defects
at boundaries that the short tests did not exercise:

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

The repair makes active hint membership an explicit Topform invariant, makes
the active-to-retired transition idempotent, retains retired hints as stable
proof/checkpoint owners, preserves retirement over checkpoint/resume, and
uses a checked bulk destroy for a terminal packed index.  These rules apply to
legacy, packed, better-packed, and packed-fast hint modes; they are not keyed
to any Josef formula, symbol, hint count, or search schedule.

Commit `0bddd18` repairs the independent hint-retirement and bulk hint-index
lifecycle described below.  It does **not** repair the remaining nested
terminal-proof ownership defect.  The structural follow-up is specified in
`P9-TERMINAL-PROOF-LIFECYCLE-PLAN.md`; its implementation and mature replay
must be recorded separately rather than retroactively attributed to the hint
fix.

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

## Implemented repair

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
- compact OTTER checkpoint/resume;
- compact unit, nonunit, rewrite, back-demodulation, ID-map, and cold-store
  component suites;
- DISCOUNT loop, eager demodulation, collective frontier, hint trace, hint
  checkpoint, and dense passive proof comparisons;
- selector checkpoint/compaction tests;
- allocator, memory lifecycle, bookkeeping, and ancestor-store tests.

Two stale dense-passive test expectations were repaired while running this
matrix: report regexes now include the already-reported directory fields, and
the selector compaction test no longer hard-codes an obsolete 64-byte record
size.  Those test-only changes do not alter prover behavior.

## What is and is not established

Established:

- Josef 03's reported underflow has a direct reproducible state-machine cause
  and an index-independent fix.
- The packed hint path safely destroys each complete supplied Josef hint
  bank, but compact passive/inference teardown is not yet safe inside a
  terminal inference callback.
- Match-once retirement now composes with limbo batching, expiry, ordinary
  shutdown, and checkpoint/resume.
- The fix adds no Topform RAM and removes potentially expensive terminal
  maintenance work.
- Josef 01's 30,827-given compact trajectory is identical to original P9;
  its failure is not search divergence.

Not yet established without rerunning the expensive jobs:

- Josef 01/02 do not yet complete their original mature proof reconstruction.
  Their reruns demonstrate the remaining nested callback/archive ownership
  bug; the general safe-point repair must land before another acceptance run.
- Josef 03 must still pass its original 460-million-generated boundary to
  demonstrate the repaired transition under the same long-run interleaving.
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
givens, clauses 36195463/36195464) and a normalized proof comparison.  For
Josef 02, require a normal theorem/limit exit and complete final statistics.
For Josef 03, require progress beyond the old 15,634-given boundary with no
underflow, and compare hint totals, matched identities/trace if enabled, and
the final proof/search outcome rather than only elapsed time.
