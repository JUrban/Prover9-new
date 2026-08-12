# Adaptive backward-index crossover plan

Status: implemented and locally validated on branch
`adaptive-back-index-crossover`; mature-host acceptance remains open.

This tranche addresses the remaining CPU risk in compact backward
demodulation.  The complete large-chat `mask8` run saved 73.8% of peak RSS
against old Prover9, but took 1.87 times the old run's user CPU.  Its backward
lookup alone consumed 3,607.118 seconds and examined 20,764,898,738 posting
groups.  A 1,000-given adaptive sample removed 87.7% of those groups at that
prefix, but total user CPU was still effectively tied with `mask8`.  Short-run
success is therefore not evidence that the current irreversible root-level
tree decision will remain good after days of index growth.

The immediate defect is structural.  Once a root discrimination tree is
admitted, `adaptive` sends every nonvariable query for that root through it.
A selective rigid query, a broad true-answer query, and a variable-prefix query
with a selective rigid suffix can have radically different costs under the
same root.  Aggregate root history can consequently lock expensive shapes into
the tree.  Admitted position postings are also selected unconditionally even
if their posting population later grows past the cheaper alternative.

## 1. Invariants and non-goals

1. Routing changes only the complete candidate generator.  Candidate IDs and
   their final decreasing-ID order, exact tests, proof, and search trajectory
   must remain identical for every route.
2. Decisions use stable term-shape fingerprints and integer operation counts,
   never elapsed time, symbol-table insertion order, allocator addresses, or a
   problem-name special case.
3. The planner is fixed-size.  Its resident cost must not grow with clauses,
   roots, queries, or runtime, and replacement must be deterministic.
4. No decision is reused across every scale.  A power-of-two population-class
   transition retrains mask and tree, while the exact current position gate is
   reevaluated on every eligible query.
5. The complete `mask8` path remains available.  An unavailable, collided, or
   untrained planner entry falls back safely rather than omitting an answer.
6. This work does not change given-clause scheduling, hint matching,
   demodulator orientation, or the semantics of back demodulation.  Ordinary
   demodulation's independent 3,230-second clock in the complete run is a
   separate later CPU tranche.

## 2. Deterministic bounded router

The implemented planner uses a fixed 4,096-entry, two-way set-associative
table.  Its key combines the stable semantic root hash with a deliberately
coarse structural class: total nodes, rigid preorder prefix before the first
variable, variable occurrence count, repeated-variable bit, maximum depth,
root arity, and the base-two scale class of the current compatible-mask
population.  Variable numbers are absent, so alpha renaming shares evidence.
The population class generalizes related queries without assuming evidence at
1,000 records remains valid at a million.  Each entry retains bounded
saturated counters for:

- query count;
- observed symbol/mask, tree, and position logical costs;
- sample counts for each available route; and
- current preferred route and the evidence-backed switch count.

The table is a cache, never an authority.  Collisions use deterministic least-
evidence replacement and merely lose performance history.  A compile-time
assertion fixes each entry at 104 bytes, so all 4,096 entries occupy 425,984
bytes (416 KiB), independent of clauses, queries, and runtime.

Use operation-weighted integer cost rather than the sampled CPU clocks.  The
cost function includes posting groups decoded, tree nodes, sibling checks,
child-cache probes, position records/bitmap words, and candidates that must be
validated.  Weights are fixed from implementation-level operation classes and
tested on adversarial synthetic distributions; they are not fitted to Osborn
or chat runtimes.  Saturating arithmetic prevents a month-long run from
wrapping.

Before doing an expensive retrieval, the implementation counts compatible
posting-list populations from path-bucket metadata.  Each bucket now maintains
an exact 32-bit posting count.  Position routes already expose posting counts
and dense-bitmap word counts.  Tree cost is learned by one real complete query
because branching depends on pattern shape.  A new structural/scale class
first records a mask baseline, then one tree sample; later queries use the
preferred route and update an integer EWMA.  Switching requires a 20% margin.
Crossing a power-of-two mask-population boundary creates fresh evidence, which
is the bounded mature-growth recheck; there are no periodic intra-class
exploration probes.  This replaced a geometric-probe experiment which
retrained too often on the integrated workload.

## 3. Route lifecycle and maintenance

Root tree construction remains cost- and byte-gated.  Admission makes the tree
available; it no longer makes it mandatory.  Cold roots do not allocate or
hash route profiles: their complete mask queries continue accumulating the
existing tree-admission evidence.  After admission, each new structural/scale
class obtains a mask baseline and at most one initial tree probe.

Admitted rigid-position postings are alternatives, not unconditional winners.
The current policy deliberately bypasses the profile only when its exact
current work estimate is at least four times smaller than the compatible mask
population, the same minimum gain required for position construction.  A
marginal position remains indexed but cannot displace mask/tree routing.
Expensive complete routes still contribute evidence for admitting a missing
selective position.

Compaction copies the bounded route table and counters.  Its samples use
logical decoded work rather than addresses or allocation capacities, so no
physical-generation invalidation is needed.  Checkpoints now write
`compact_back_adaptive.txt` beside the existing checkpoint files.  It restores
tree admission evidence, active position definitions, probation state, the
position budget high-water mark, and every occupied route slot before search
continues.  An older checkpoint without this sidecar cold-calibrates safely.

## 4. Observability

Extend backward-index statistics with:

- fixed route-table capacity and bytes;
- occupied entries, collisions, and replacements;
- mask, tree, and position choices and exploration probes;
- switches, reversions, hysteresis holds, and bounded initial probes;
- cumulative estimated and observed cost by route; and
- per-route exact candidate counts.

The existing query fingerprint remains the semantic differential oracle.  The
long-run reporter will show route mix and interval logical cost per query so a
flat total can no longer hide a progressively failing shape.

## 5. Validation ladder

1. Focused differential tests compare all candidate IDs and order for every
   strategy and route before and after admission.
2. Mixed-shape adversarial tests use the same root for a selective rigid
   pattern, a variable prefix with a selective suffix, and a broad true-answer
   pattern.  They require different route choices and bounded probes rather
   than one root-wide choice.
3. Crossover tests grow the population through powers of ten and verify that
   selective queries promote the tree, broad queries can return to mask, and a
   useful position can replace either without oscillation.
4. Fixed-live deletion/compaction tests verify exact answers, bounded table
   bytes, state preservation, and renewed calibration when physical work
   changes.
5. Checkpoint/restart and ASan/UBSan runs verify deterministic behavior and
   ownership.  Existing unrelated LADR process-exit leaks remain documented,
   not attributed to this table.
6. Bounded `chat_test.in` comparisons run in parallel at 300 and 1,000 givens:
   `mask8`, the parent-branch adaptive mode, and the new router.  Generated,
   kept, given, hints, proofs, query fingerprints, and output fingerprints must
   match at corresponding boundaries.
7. The new router may not regress total 1,000-given user CPU by more than 5%
   from the faster compact control.  More importantly, its interval backward
   work/query must have a materially flatter mature slope than `mask8`; a
   short-prefix CPU tie is acceptable only when the counted crossover evidence
   predicts lower mature work without growing resident state.
8. Promotion still requires a complete large-chat or equivalent mature run on
   the larger host.  Target: no more than 1.25 times old Prover9 total CPU while
   preserving at least 70% measured peak-RSS saving, followed by work on the
   independent ordinary-demodulation bottleneck.  The local bounded tests will
   be reported as provisional rather than extrapolated into a false final RAM
   or CPU claim.

## 6. Reviewable commit sequence

1. Freeze this plan and the complete-run baseline.
2. Add stable canonical pattern keys, cheap route estimates, the bounded
   profile table, statistics, and unit tests.
3. Make tree selection reversible with deterministic exploration and
   hysteresis; add mixed-shape and crossover tests.
4. Cost-gate position selection and preserve calibration across compaction and
   checkpoint/reconstruction boundaries.
5. Run sanitizer and bounded integrated comparisons; record raw commands,
   hashes, trajectories, costs, CPU, RAM, limitations, and the mature-host
   acceptance command.

Every implementation commit will state the theorem-proving invariant it
preserves, the focused tests run, and which mature-scale claim remains open.
Generated binaries and user-owned result directories remain untracked.

## 7. Implemented validation and current decision

Focused tests cover selective and broad patterns under one root, alpha
renaming, a power-of-two population transition, bounded one-probe calibration,
position cost gating, forced compaction, checkpoint save/rebuild/restore, and
the fixed 416 KiB table bound.  Every route returns the same decreasing clause
ID sequence.  The 10,000-record variable-prefix longevity case uses one
position group/query instead of 10,000 mask groups or 39,998 combined raw-tree
operations.  Optimized tests and the checkpoint/restart proof differential
pass; sanitizer validation is recorded with the final implementation commit.

On the accepted structural/scale-class implementation, the 1,000-given chat
candidate reproduced 1,268,285 generated and 33,909 kept clauses.  It examined
6,300,093 mask groups versus 10,871,045 for the matched mask control, a 42.0%
reduction.  The route table had 3,992 occupied entries; cumulative choices were
20,166 mask, 7,212 tree, and 386 position.  Total user CPU was 108.18 seconds
versus 105.46 for the mask control.  An identical-work rerun took 112.27
seconds, showing material host timing spread; the result is inside the planned
5% short-prefix gate only if that noise is acknowledged.  It is not a CPU-win
claim.

The completed archived `chat_test.new.out4` mask run is the mature reference:
9,573.63 user seconds, 20,764,898,738 posting groups, and 3,607.118 seconds in
back lookup.  It saved 73.8% peak RSS against old P9 but used 1.87 times its
user CPU.  No adaptive run yet reaches its 1.53-million-active back index.
Consequently the branch is ready for a bounded/mature host comparison, not for
promotion as the default strategy.
