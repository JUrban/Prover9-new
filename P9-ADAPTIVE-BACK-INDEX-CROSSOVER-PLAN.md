# Adaptive backward-index crossover plan

Status: active implementation on branch `adaptive-back-index-crossover`.

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
4. No route is permanently promoted.  Sparse deterministic probes must permit
   tree, symbol/mask, and position routes to be selected again as the live
   population and stale fraction change.
5. The complete `mask8` path remains available.  An unavailable, collided, or
   untrained planner entry falls back safely rather than omitting an answer.
6. This work does not change given-clause scheduling, hint matching,
   demodulator orientation, or the semantics of back demodulation.  Ordinary
   demodulation's independent 3,230-second clock in the complete run is a
   separate later CPU tranche.

## 2. Deterministic bounded router

Add a fixed two-way set-associative table keyed by a stable semantic pattern
fingerprint.  Variables are canonicalized by first occurrence so an alpha
renaming has the same route profile.  Each entry retains bounded saturated
counters for:

- query count and the next deterministic re-probe generation;
- observed symbol/mask, tree, and position logical costs;
- sample counts for each available route; and
- current preferred route and the evidence-backed switch count.

The table is a cache, never an authority.  Collisions use deterministic least-
evidence replacement and merely lose performance history.  The initial target
is at most 4,096 entries and substantially less than 1 MiB including all
metadata.

Use operation-weighted integer cost rather than the sampled CPU clocks.  The
cost function includes posting groups decoded, tree nodes, sibling checks,
child-cache probes, position records/bitmap words, and candidates that must be
validated.  Weights are fixed from implementation-level operation classes and
tested on adversarial synthetic distributions; they are not fitted to Osborn
or chat runtimes.  Saturating arithmetic prevents a month-long run from
wrapping.

Before doing an expensive retrieval, cheaply estimate the symbol/mask route by
counting compatible posting-list groups from bucket metadata.  Position routes
already expose posting counts and dense bitmap sizes.  Tree cost must be
learned by actual exact probes because branching depends on pattern shape.
The router chooses a trained route only with a hysteresis margin; otherwise it
uses the complete mask route.  It probes the nonpreferred available route on a
geometric logical schedule, making exploration frequent during warm-up and
vanishingly sparse in mature runs.  A large change in the cheap mask or
position estimate advances the next probe so growth can reverse an earlier
choice.

## 3. Route lifecycle and maintenance

Root tree construction remains cost- and byte-gated.  Admission makes the tree
available; it no longer makes it mandatory.  A newly admitted root obtains one
tree sample per encountered shape before promotion.  Until then the router
uses mask except for its bounded probes.

Admitted rigid-position postings are alternatives, not unconditional winners.
For one posting, intersections, and dense intersections, compute a conservative
pre-query work estimate and compare it with the trained tree and current mask
estimate.  Position fanout may therefore revert to tree or mask without
destroying the posting.  Expensive tree probes still contribute evidence for
admitting a missing selective position.

Compaction copies the bounded route table and counters, then invalidates only
samples whose physical-work basis was changed materially.  Logical query
counts and preferred-route history survive so compaction cannot cause a burst
of unbounded retraining.  Rebuild and checkpoint tests must demonstrate the
same answer sequence.  If the current checkpoint format reconstructs an index
instead of serializing its adaptive state, the implementation will either
serialize the bounded state or explicitly rebuild a deterministic equivalent
before promotion; silently changing post-restart routing is not accepted.

## 4. Observability

Extend backward-index statistics with:

- fixed route-table capacity and bytes;
- occupied entries, collisions, and replacements;
- mask, tree, and position choices and exploration probes;
- promotions, reversions, hysteresis holds, and estimate-triggered rechecks;
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
   pattern.  They require different route choices and bounded re-probes rather
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
