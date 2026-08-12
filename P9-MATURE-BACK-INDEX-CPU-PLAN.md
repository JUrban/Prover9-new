# Mature backward-index CPU recovery plan

Status: active implementation on branch `mature-back-index-cpu`.  The
external mature-run gates remain open.  A completion audit widened the local
work below because parity with compact `mask8` is only an intermediate gate,
not the product CPU goal.

This plan supersedes the short-prefix reasoning behind the first adaptive
router.  The new authoritative baseline is the matched search state in the
user-supplied `chat_test.new.out41` and `chat_test.new.out51`, not the earlier
1,000-given sample.

At almost the same theorem-search point, given 7,363/7,365, both runs print the
same given clauses and have only 0.03% generated-clause and 0.02% kept-clause
differences:

| Metric | `out41` `mask8` | `out51` adaptive | Adaptive/mask |
|:---|---:|---:|---:|
| User CPU | 2,100.03 s | 2,700.01 s | 1.286 |
| User + system CPU | 2,326.86 s | 2,922.20 s | 1.256 |
| Back lookup CPU | 645.798 s | 909.477 s | 1.408 |
| Back maintenance CPU | 10.151 s | 425.058 s | 41.87 |
| Back-index bytes | 53,849,191 | 125,514,518 | 2.331 |
| PSS | 322,519 KiB | 456,972 KiB | 1.417 |

The adaptive run saved 951 million mask posting groups, but added 760 million
tree-node visits, 1.031 billion sibling checks, and 8.4 million child-cache
lookups.  It also performed 1.59 billion position census record visits and
132.6 million position backfill visits.  Its 4,096 route slots suffered 307,398
replacements; 93,017 of 221,851 tree queries were calibration probes.  The
position budget exhausted 514 times: 624 features had been admitted, 517 had
already been demoted, and only 107 remained active.  These counters disprove
the former extrapolation.

The complete-run target is stricter.  The supplied normal-P9
`chat_test.new.out1.gz` proves after 11,368 givens in 5,110.38 user seconds;
compact `mask8` in `chat_test.new.out4` follows essentially the same trajectory
but needs 9,573.63 user seconds and 1,147.68 system seconds.  Its backward
lookup alone costs 3,607.118 seconds.  Recovering adaptive-versus-mask parity
therefore does not prove competitiveness: the product gate remains at most
1.25 times normal-P9 user CPU at the complete proof endpoint, with system CPU
and file-cache memory reported separately.

## 1. Non-negotiable invariants

1. Mask, tree, and position retrieval remain complete candidate generators.
   Exact candidate IDs, decreasing-ID order, given clauses, hints, and proofs
   cannot depend on timing or cache residency.
2. Every adaptive data structure has a fixed byte cap.  More importantly,
   every maintenance activity has a cumulative work budget; bounded RAM alone
   is not a long-run scalability argument.
3. One query may fund at most one unit of global construction credit.  The
   same work cannot be credited independently to every rigid feature in the
   query.
4. Cold or one-off query classes use the complete mask path without allocating
   a route profile and without probing the tree.  Frequency evidence ages, so
   a class cannot become hot merely by accumulating sparse hits over a
   week- or month-long run.
5. Failed position features cannot be reconsidered at every query.  A normal
   selectivity rejection waits until the active population has doubled; a
   budget-demoted feature is not rebuilt in the same index generation.
6. Once position storage first exhausts its hard budget, new admissions stop.
   Existing useful features continue to serve queries and are demoted only if
   their own growth cannot fit.  Compaction may reclaim inactive storage but
   must not restart the admission storm.
7. Checkpoint/resume and index compaction preserve the global credit ledger,
   rejection generations, admission freeze, hot-route evidence, and all
   semantic fingerprints.
8. Steady insertion work cannot be proportional to the number of admitted
   optimization features.  Construction may be amortized yet still lose a
   long run if every later clause is independently rescanned for every active
   feature.
9. Every structural operation used to choose a route is charged to that
   route.  In particular, root-level sibling traversal cannot remain hidden
   from the tree cost merely because it occurs outside the recursive walker.
10. Aggregate traffic sharing a root cannot buy a discrimination tree.  At
    least one recent structural/population class must independently cross the
    route-frequency threshold before its work can fund root construction.
11. A calibration-count bound is insufficient unless each calibration is
    itself bounded.  An initial tree probe may consume no more counted work
    than the twofold promotion margin permits; an incomplete probe must be
    discarded and followed by the complete mask route.

## 2. Admission before calibration

The first router allocated a profile on every miss.  Because the key includes
population scale, its two-way table became a cache for a stream of mostly cold
classes.  Eviction erased evidence, and a returning class paid another tree
probe.

Add a fixed count-min frequency sketch ahead of the profile table.  A class
must be observed at least 32 times at its current structural/population-scale
key before it can receive a profile.  Until then it uses mask.  Two independent
counter rows make collision overestimation possible but bounded; a collision
can cause an unnecessary profile attempt, never a missing candidate.

The fixed sketch cannot use lifetime counters: eventually even singleton
traffic would saturate both rows.  Both 64K rows are therefore halved every
four sketch capacities (262,144 routed lookups).  This is a 256-KiB bounded
working set, an amortized one 16-bit counter visit per two routed lookups, and
makes the threshold measure recent repeated demand rather than run age.

Full-table replacement is admission-controlled.  An incoming class replaces
only a profile whose frequency/recency score is lower, with deterministic
tie-breaking.  Stale profiles from old population scales age out; a one-off
class cannot evict a hot one.  A newly admitted profile records one mask
baseline and at most one tree probe.  Consequently tree calibration is bounded
by the number of admitted hot classes, rather than the number of distinct
classes encountered.

Tree promotion also needs a conservative asymmetric margin.  The old 20%
logical-work margin did not cover cache-unfriendly tree nodes and sibling
links.  Tree must initially demonstrate at least a twofold counted-work gain
over mask.  Once selected, ordinary 20% hysteresis is sufficient for reversion;
this prevents oscillation without treating unequal operations as equal CPU.
The new statistics expose cold fallbacks, admission attempts/rejections,
profile hits, and aged replacements.

Long-run target: after warm-up, tree probes are at most 1/32 of eligible
hot-root queries plus the fixed table population.  Route-table replacement
rate must tend downward rather than remain proportional to total queries.

## 3. Globally amortized position construction

Position construction currently double-counts evidence.  If a pattern has ten
rigid features, the same expensive query adds its entire work to all ten
probation entries; several entries can then independently trigger whole-index
censuses.  Rejection clears the entry, allowing the cycle to repeat.

Introduce one global position-construction ledger:

- each completed lookup contributes its observed logical work exactly once;
  an already selective position lookup can therefore fund a complementary
  intersection feature, but only from work it actually performs;
- a census reserves credit before scanning and debits it permanently;
- at most one qualified feature is censused per admission attempt;
- an accepted feature is charged for both its census and backfill scan; and
- the configurable default build factor is raised from 8 to 64, limiting
  construction record visits to a small fraction of retrieval evidence.

For the first implementation, census and backfill remain synchronous so the
index never exposes an incomplete posting.  The global ledger gives a formal
cumulative bound and is much less risky than a partially queryable builder.
If a later mature trace shows unacceptable individual pauses despite bounded
total CPU, a second tranche can make the same prepaid scan incremental without
changing admission semantics.

The position probation entry retains a `retry_population`.  Selectivity and
projected-byte rejection set it to twice the current active population;
budget-driven demotion sets it to infinity.  The entry accumulates no work
before that boundary.  This permits distribution changes to be reconsidered
only logarithmically often over population growth.

Use a separate 16 MiB default position budget rather than borrowing the full
64 MiB tree budget.  The budget and build factor become explicit P9 options.
The first hard-budget exhaustion freezes admission and is reported separately.
For review, report cumulative credits earned/spent, census work reserved,
rejection cooldown hits, admission freezes, and record visits split between
census and backfill.

## 4. Long-run tests, not short-run proxies

Focused tests must establish:

1. Ten thousand cold route classes do not allocate profiles or cause tree
   probes.  A subsequently repeated class crosses the 32-hit boundary, gets
   one baseline and one probe, and retains exact candidate order.  A separate
   multi-window case accumulates more than 32 lifetime hits per cold class but
   keeps each below the decayed recent-demand threshold.
2. More hot classes than the profile capacity cause deterministic admission
   rejection/aging, not near-one replacement per query.  A retained hot class
   does not retrain when cold traffic returns.
3. One query containing many rigid features contributes construction credit
   once.  It may trigger at most one census, whose reserved debit covers census
   plus possible backfill.
4. A rejected feature cannot retry below double population.  A budget
   exhaustion freezes new admissions but leaves an unrelated active feature
   complete and queryable.
5. At 10,000, 100,000, and an accelerated million logical records, cumulative
   census/backfill visits obey the ledger inequality, counters do not wrap, and
   planner bytes remain fixed.
6. Forced compaction and checkpoint/resume preserve answers, route evidence,
   credit/debit state, cooldowns, and the admission freeze without a retraining
   or construction burst.
7. ASan/UBSan, semantic-symbol stability, high-base offsets, and compact-OTTER
   proof/checkpoint tests pass.

## 5. Integrated acceptance ladder

The same current binary and generated input must be used for each pair.
Search trajectory and query-input fingerprints must match; route-dependent
work fingerprints may differ by design.

1. Run `chat_test.in` at 300 and 1,000 givens to reject obvious fixed overhead.
2. Run through at least the point where cold-profile saturation happened in
   `out51` (about 3,000 givens/80,000 active back records).  Require:
   route-probe fraction below 1/32 after admission warm-up, no proportional
   replacement churn, position maintenance below 5% of user CPU, and no
   position admission/demotion cycle.
3. Run a bounded prefix near given 7,365 on the larger host and compare with
   the archived matched state.  The implementation target is no slower than
   `mask8` at the matched trajectory, with a hard rejection at 1.05 times
   user CPU.  Back lookup and maintenance are reported separately.
4. Only then run to the 1.53-million-active proof endpoint.  Product promotion
   still requires at most 1.25 times old-P9 total user CPU while retaining the
   compact system's radical RAM advantage under total-job cgroup accounting.

No 300- or 1,000-given result will be extrapolated into a mature success claim.
If local hardware cannot reach steps 3--4, the implementation remains
provisional and the report will identify exactly which external gate is open.

## 6. Reviewable commit sequence

1. Commit this diagnosis and amortized-work plan.
2. Add cold-class frequency admission, conservative tree promotion,
   observability, and adversarial churn tests.
3. Add global position credits, single-feature attempts, logarithmic retry,
   admission freeze, explicit budgets, and longevity tests.
4. Preserve all new state across compaction/checkpoint and extend interval
   reporting.
5. Run paired integrated experiments, retain raw counters for failed variants,
   and tune only general thresholds justified by the stated inequalities.
6. Finish sanitizer/release/debug audits and record the exact mature-host
   command and binary/input hashes.

Every commit states the semantic invariant preserved and the remaining
mature-scale uncertainty.  Generated runs and existing untracked user files
remain outside version control.

## 7. Completion-audit extension: steady-state CPU

The first implementation bounds *admission* scans, but inspection found an
independent mature-run term in incremental position maintenance.  For each new
record, `append_admitted_position_features` first checks every active feature
by traversing the serialized clause, then traverses it again per feature while
building postings.  With `F` admitted features and `T` clause tokens this is
`O(F*T)` work per retained demodulator even after admissions freeze.  A fixed
16-MiB byte cap does not make that CPU term acceptable.

Replace it with one record-oriented traversal.  For each subject occurrence
whose root owns an active feature, walk that subterm once, probe the existing
`(root,path,symbol)` hash, collect matching `(bucket,root-offset)` pairs in
bounded scratch storage, sort/deduplicate them, and append every affected
posting.  Budget preflight must consume this same match set rather than repeat
the traversal.  The resulting work is proportional to traversed subject
structure plus actual matches, independent of the total number of features.
Expose records, root traversals, token visits, feature-hash probes, and matches
so a mature run can verify the bound directly.

The adaptive tree audit also found that the top-level sibling loop in
`collect_tree` is absent from `tree_sibling_checks`.  Count it exactly so the
initial probe and later EWMA include the real structural cost.  Exercise both
cache-hit and forced-collision/miss cases; a route may become more conservative
after honest accounting, but candidate completeness and order must not change.

Additional local gates:

1. Admit at least 32 independent exact-position features, then add unmatched
   records.  Incremental token/hash work must stay constant per record rather
   than multiplying by 32, with identical postings for matched records.
2. Run enough adaptive route queries to cross multiple sketch-aging windows
   while the active feature set is nonempty; planner bytes and per-query
   maintenance must remain bounded.
3. Include root sibling comparisons in route-observed work and verify that a
   costly tree probe cannot be promoted by an artificially low counter.
4. Repeat bounded `chat_test.in` parity tests on the final binary, then use the
   external 7,365-given and complete-proof runs to evaluate both compact-mask
   parity and the actual 1.25-times-normal-P9 product gate.

## 8. Second completion audit: construction must follow demonstrated use

The first post-audit 300-given run exposed 399,774 discrimination-tree
insertion sibling comparisons despite zero tree queries and zero admitted
route profiles.  Root admission was pooling fallback work across unrelated
query classes with the same leading symbol.  A byte/work construction budget
does not repair this mismatch: it merely limits how much CPU can be spent on
an optimization for which no repeated consumer exists.

Feed pre-tree adaptive queries through the same bounded, aging count-min
sketch used by profile admission.  Until a structural/population class reaches
32 recent observations, its mask work cannot enter the root construction
ledger.  Once qualified, only subsequent work funds the existing factor-8
root census/backfill test.  This deliberately requires both demonstrated
frequency and demonstrated aggregate savings before a tree is built.

The initial tree calibration is cancellable.  Its allowance is half the
population-scaled mask baseline, matching the required twofold promotion
margin.  Charge nodes, siblings, child dispatch, representative matches,
posting bytes, and posting groups before executing them.  If the allowance is
exhausted, clear only result stamps added by that pattern, discard its partial
IDs, and run the complete mask route under the same query stamp.  The partial
work remains in CPU telemetry, while a conservative failed sample prevents
promotion.

Finally, any posting iterator that calls a builder must retain stable indices,
not pointers into reallocatable storage.  The supplied `chat_test.new.out6`
found exactly this lifecycle violation during root backfill: tree posting
growth moved the shared posting-block array while the path-posting iterator
held an interior pointer.  The acceptance suite therefore includes a
1,024-duplicate-term backfill that forces reallocation during traversal.

Local gates for this tranche:

1. At 300, 600, and 1,000 givens, a cold workload must build no root tree and
   perform no tree insertion work, while matching mask counters/fingerprints.
2. A genuinely repeated synthetic class must still qualify, build, and probe;
   an adversarial variable-prefix probe must abort within its mask-derived
   allowance and reconstruct the exact decreasing-ID mask answer.
3. The archived crash input must reproduce at `b6142ea`, then pass the crash
   point with only the stable-index iterator fix.
4. The external 3,000/7,365/proof-endpoint gates remain mandatory because the
   new short-run result intentionally avoids speculative tree construction and
   cannot prove the behavior after genuinely hot classes emerge.
