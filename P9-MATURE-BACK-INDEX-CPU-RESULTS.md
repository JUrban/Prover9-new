# Mature backward-index CPU recovery: implementation and bounded results

Status: implemented and locally validated on branch
`mature-back-index-cpu`.  An exact 1,500-given CHAT gate is now within 5.1%
of normal P9 user CPU while using 67.2% less peak RSS.  The external
7,365-given and proof-endpoint gates remain open; these bounded runs are not a
claim that a week-long run has already been reproduced.

## Why the former adaptive mode failed

At the matched 7,363/7,365-given state in the supplied
`chat_test.new.out41`/`chat_test.new.out51`, adaptive used 1.286 times the mask8
user CPU.  Back-index maintenance rose from 10.151 to 425.058 seconds.  The
adaptive run had examined 1.59 billion position records for queries/censuses,
backfilled another 132.6 million records, replaced 307,398 entries in its
4,096-entry route table, and used 93,017 of 221,851 tree choices for probes.
The later 7,689-given `out51` state reached 479.102 maintenance seconds, 688
position admissions, 517 demotions, 515 budget exhaustions, 328,302 route
replacements, and 100,138 tree probes.  This is an admission/churn failure,
not a threshold that can safely be repaired by a small constant adjustment.

## Implemented design

The changes are split into reviewable commits:

| Commit | Change |
|:---|:---|
| `2105a98` | matched-state diagnosis, invariants, and acceptance ladder |
| `d8c4eff` | count-min hot-class admission, age-aware table admission, 2x tree-promotion margin, checkpoint state, telemetry |
| `559af0a` | global position-construction ledger, one-candidate census, population cooldown, admission freeze, independent options |
| `0ad8d9b` | 10,000 cold/5,000 hot route churn test, position retry test, symbol-independent hard-budget semantics |
| `000f7a6` | removal of cold adaptive double scans and premature position-feature traversal |
| `e33f3b9` | frequency-sketch aging and a multi-window cold-churn test for mature runs |
| `f5e55a9` | reopened completion audit against the 5,110-second normal-P9 proof baseline |
| `b6142ea` | record-oriented incremental position maintenance and complete root-sibling accounting |
| `3fa7599` | live position-root counts, relevant-occurrence filtering, and cached mature tree insertion |
| `091c386` | mask-derived execution cap, rollback, and fallback for initial tree probes |
| `3f0d83d` | stable-index root-backfill iterator fixing the supplied `out6` crash |
| `ccd8f43` | repeated-class qualification before any adaptive root-tree construction |
| `4ddf9ed` | bounded compact forward-rewrite hot-path acceleration |

Candidate completeness and decreasing-ID order remain authoritative in the
mask/tree/position paths.  Scheduling, wall time, and cache residency never
affect the selected route or the returned ID set.

### Forward rewrite traversal

The matched mature prefix showed that backward lookup was only part of the
remaining gap.  At about 1,500 given clauses, compact forward demodulation was
also materially slower than normal P9.  A 300-given `gprof` run found more
than 56 million calls each to packed-slice validation/access helpers and more
than 13 million recursive radix retrieval calls.  That work grows with rewrite
attempts, so it cannot be treated as short-run setup overhead.

The compact rewrite bank now:

- snapshots the immutable token-array address and logical base once per
  rewrite query, then decodes already-validated packed slices directly in
  recursive matching and contractum construction;
- stores each radix node's first token code in a four-byte parallel array,
  avoiding repeated token-slice resolution during sibling routing;
- maps rigid children of the rewrite root directly by P9 symbol number.  This
  is a performance cache only: variables retain predecessor order and a miss
  falls back to the original ordered sibling scan; and
- represents bound rewrite variables with two 64-bit words plus the existing
  trail, instead of clearing all 100 binding pointers per rewrite attempt.

Radix insertion and splitting update the cached first codes and root map.
Index compaction rebuilds both from live rules.  Term-pool rebasing does not
change symbol codes, and each query refreshes the token-array snapshot.  The
focused test covers normal-form and justification equality with the legacy
demodulator, wide-root lookup, rewrite-bank compaction, and a pool whose
logical base is above 32 bits.

The long-lived memory cost is four bytes per physical radix node plus one
`uint32_t` per allocated P9 symbol slot.  It is independent of query and
rewrite-attempt counts.  On the 1,500-given CHAT prefix the root map is 1 KiB;
the parallel first-code array is about 256 KiB at the current capacity.

### Route calibration

- A two-row, saturating count-min sketch sees a structural/scale class before
  the 4,096-entry profile table does.
- Both 64K rows are halved every 262,144 routed lookups, so sparse lifetime
  traffic cannot eventually saturate the sketch.  The sweep is one 16-bit
  counter visit per two routed lookups amortized.
- Fewer than 32 observations means an immediate complete mask lookup: no
  profile allocation and no tree probe.
- The same threshold now applies before root construction.  Cold classes may
  share a leading symbol but cannot pool their work to build and maintain a
  tree unless one recent structural/population class is independently hot.
- A full table admits an incoming class only when its deterministic
  frequency/recency score beats the selected resident.  Old population scales
  age out; singleton traffic cannot continuously replace current hot state.
- A class gets one mask baseline and at most one initial tree probe.  Tree must
  show at least a 2x counted-work advantage before promotion.  The ordinary
  20% margin is retained for later reversion.
- The initial probe is stopped when its charged work exceeds half the scaled
  mask baseline.  Partial candidates and only their newly added query stamps
  are rolled back before a complete mask fallback; actual partial work and the
  conservative failed sample are reported separately.
- The profile table is 448 KiB and the two sketch rows are 256 KiB, independent
  of run length.  Version-3 `compact_back_adaptive.txt` sidecars preserve the
  sketch, recency, profiles, and the position state below.

The final cold path does not census mask buckets to estimate the lookup it is
about to perform.  It uses the maintained root population as a cheap scale
hint.  An exact mask-population scan occurs only when an already-admitted
position feature passes a coarse gain screen and must be checked before it can
replace mask retrieval.

### Exact-position construction

- Observed retrieval work enters one global ledger once.  The number of rigid
  features in a query cannot multiply its construction credit.
- A reservation costs
  `2 * active_records * compact_back_position_build_factor`; it buys at most
  one full census and, if accepted, one full backfill.
- The default factor is 64.  Therefore active census-plus-backfill record
  visits are at most total credited retrieval work divided by 64.  With the
  default 25% stale-index rebuild threshold, physical loop iterations are at
  most credited work divided by 51.2 between rebuilds.
- A rejected feature cannot retry until active population doubles.  A
  budget-demoted feature has no retry in the same index generation.
- The first hard position-budget exhaustion freezes new admissions for the
  rest of the run.  Complete unrelated active features remain queryable.
- The default exact-position cap is an independent 16 MiB.  It no longer
  borrows the 64 MiB tree allowance or changes with unrelated global symbol
  numbering.
- Queries below the 4,096-work admission floor still add their work to the
  global ledger, but do not traverse every rigid query feature for probation.
- Incremental posting maintenance is record-oriented.  Each new serialized
  clause is traversed once per subject occurrence whose root has live
  features; hash probes find all matching `(root,path,symbol)` definitions and
  one deduplicated match set drives budget checks, bitmaps, occurrences, and
  postings.  Its work no longer multiplies by the total admitted-feature
  count.  Roots whose last feature is demoted trigger no later traversal.

Factor 16 is intentionally not the new default.  On the exact 1,500-given
gate it took 210.38 seconds versus 210.57 seconds for factor 64, which is
measurement parity.  Factor 16 admitted 43 position features and spent 73.5
million census/backfill credits; factor 64 admitted 2 and spent 53.0 million.
With no endpoint CPU benefit, factor 64 preserves the stronger construction
bound for unseen long-running workloads.

At the supplied later `out51` state, the old route-observed work totals about
8.128 billion units.  Applying the new default ledger to the same amount of
work permits at most about 127.0 million active census/backfill visits, or
158.8 million physical loop iterations at 25% staleness.  That is a worst-case
construction bound about 92% below the old 1.823 billion
position-record-examinations plus 164.5 million backfill visits.  It is not a
measured 7,689-given runtime result; it is the implemented accounting bound
that the external run must verify.

## New options and recommended run

The two new P9 parameters have conservative defaults and may be stated
explicitly for reproducibility:

```text
assign(compact_back_position_budget_kb,16384).
assign(compact_back_position_build_factor,64).
```

Use them with the previously recommended compact OTTER configuration:

```text
assign(search_loop,otter).
assign(passive_store,dense).
assign(passive_directory,file).
assign(passive_selector_store,file).
assign(passive_selector_buffer,65536).
assign(hint_index,packed_fast).
assign(inference_frontier,clauses).
assign(ancestor_store,file).
assign(sos_limit,-1).

set(process_initial_sos).
set(back_demod).
set(back_demod_hints).
clear(unit_deletion).
clear(ancestor_subsume).
clear(eval_rewrite).

set(compact_otter_demodulation).
set(compact_otter_unit_index).
set(compact_otter_back_demod_index).
set(compact_otter_nonunit_index).
assign(compact_back_demod_strategy,adaptive).

assign(compact_passive_cache,0).
assign(compact_index_stale_pct,25).
assign(compact_term_reclaim_kb,8192).
assign(compact_back_position_budget_kb,16384).
assign(compact_back_position_build_factor,64).
```

`passive_selector_store,heap` remains useful for a controlled comparison; the
file selector is the intended bounded-RAM long-run setting.  The two position
parameters should not be relaxed for the first mature comparison.

## Bounded `chat_test.in` evidence

All measurements used input SHA-256
`9781ee07691bc62e01f67534208620ca0be3f026f55248a227e9161f1ed17e6c`.
The 1,000-given and bounded 180-second pairs used release binary SHA-256
`f8a49de3f1d05596209c9b7016742cfb95c499433f209da373b41300963fca6e`.
After the final frequency-aging hardening, the 300-given pair was repeated
with release binary SHA-256
`3a830e1cf4ecfa8a5f43f6b3bde6284c2eef5d1997faa4e2bc33f14b2bc2161d`.
The post-completion-audit 600- and 1,000-given pairs used release binary
SHA-256
`dd308c0d7a95e35d776c184a8e5b8042b108b12d2374435c7537e6be88348bc2`.
The forward-rewrite and exact 1,500-given gates used release binary SHA-256
`fc674b7ba3c75092732ab91a86e5d969d3f2cccd17d59a7dddb745927f440463`.
Mask8 and adaptive cases ran with the same generated input on dedicated CPUs.

| Gate | mask8 | adaptive | Result |
|:---|---:|---:|:---|
| final 300 given, user CPU | 18.62 s | 19.07 s | adaptive +2.4% |
| 300 given, generated / kept | 120,793 / 5,737 | 120,793 / 5,737 | identical |
| 1,000 given, user CPU | 104.30 s | 104.83 s | adaptive +0.51% |
| 1,000 given, generated / kept | 1,268,285 / 33,909 | 1,268,285 / 33,909 | identical |
| 1,000 given, sampled lookup | 3.598 s | 3.488 s | adaptive -3.1% |
| bounded 180-second run, final given | 1,275 | 1,290 | adaptive +1.18% throughput |
| bounded run, measured user CPU | 157.33 s | 156.81 s | parity |

The final-binary cold-prefix gates are:

| Gate | mask8 | adaptive | Result |
|:---|---:|---:|:---|
| 600 given, user CPU | 45.44 s | 45.38 s | parity |
| 600 given, generated / kept | 497,430 / 16,974 | 497,430 / 16,974 | identical |
| 1,000 given, user CPU | 95.23 s | 93.20 s | adaptive -2.1% |
| 1,000 given, generated / kept | 1,268,285 / 33,909 | 1,268,285 / 33,909 | identical |

Four reversed-core 600-given comparisons isolated commit `4ddf9ed` from CPU
assignment.  The pre-change compact binary averaged 48.68 seconds user CPU;
the new binary averaged 44.54 seconds, an 8.5% reduction.  Every run ended at
601 given clauses with an identical generated/kept trajectory, and peak RSS
remained about 90 MiB.

The exact mature-prefix comparison is:

| 1,500-given gate | User CPU | Peak RSS | Generated / kept | Demod attempts / rewrites |
|:---|---:|---:|---:|---:|
| normal P9 | 200.32 s | 275,584 KiB | 2,947,138 / 66,935 | 83,176,695 / 10,001,163 |
| compact before `4ddf9ed` | 222.65 s | 90,420 KiB | 2,947,136 / 66,933 | 83,176,656 / 10,001,165 |
| compact at `4ddf9ed` | 210.38 s | 90,428 KiB | 2,947,136 / 66,933 | 83,176,656 / 10,001,165 |

Thus current compact is 5.0% slower than normal P9 at this endpoint and 5.5%
faster than the preceding compact binary.  It removes 55% of the former
compact CPU penalty while preserving the compact trajectory exactly.  Peak
RSS is 67.2% below normal P9 here.  This prefix has not yet accumulated the
passive population of the supplied multi-hour/day outputs, so this is a CPU
competitiveness result, not an 80--90% endpoint-RAM claim.

The component clocks explain why the proof endpoint remains open: current
compact forward demodulation takes 61.96 seconds versus 50.60 seconds for
normal P9, and compact backward demodulation takes 22.10 seconds versus 5.92
seconds.  Other compact-path savings nearly offset those gaps at 1,500 given,
but their mature slopes still require the supplied 7,365-given and full-proof
comparisons.

The next exact bounded endpoint did not show an early runaway:

| 2,000-given gate | User CPU | Peak RSS | Generated / kept | Demod attempts / rewrites |
|:---|---:|---:|---:|---:|
| normal P9 | 335.92 s | 358,144 KiB | 4,497,692 / 144,514 | 124,676,012 / 16,116,393 |
| compact at `4ddf9ed` | 357.15 s | 117,308 KiB | 4,497,690 / 144,512 | 124,675,973 / 16,116,395 |

Compact is 6.3% slower and uses 67.2% less RSS at 2,000 given.  From the
1,500 endpoint, normal P9 consumed another 135.60 seconds and default-factor
compact another 146.58 seconds, an 8.1% interval penalty.  This is far below
the supplied larger-run regression but is not a substitute for that gate.  At 2,000 given,
compact position maintenance is 11.84 seconds (3.3% of user CPU), below the
5% acceptance limit.  Forward/back-demod clocks are 101.12/60.75 seconds for
compact versus 86.07/26.82 seconds for normal P9, so backward retrieval is
still the principal unclosed slope risk.

At 1,000 givens all 27,764 adaptive lookups had been observed by the pre-tree
frequency sketch and 370 were post-threshold observations, but the separate
factor-8 construction test correctly deferred all six root censuses.  Thus
adaptive built zero tree nodes, performed zero tree insertion comparisons,
allocated zero route profiles, and issued zero probes.  Its back index was
3.91 MB versus mask8's 3.08 MB; both external peak RSS values were about
90.4 MiB.  Query-input/output fingerprints and all search counters matched.
This is evidence that cold prefixes no longer pay speculative maintenance,
not evidence about the still-open mature hot-class phase.

In the pre-completion-audit 1,000-given pair, adaptive admitted 42 of 23,587
route-table misses and performed 34 probes in 27,761 lookups.  At its bounded
1,290-given endpoint it held 64 route profiles and used 56 probes in 38,534
lookups.  Those historical values explain why the new construction gate was
needed; they are not counters from the final binary.

The first pre-fix 300-given pair is retained in `chat-mature-cpu-300`: it took
23.92 versus 18.73 user seconds because adaptive redundantly scanned mask
buckets and updated 19,744 position probations despite admitting no route.
The corrected pair is in `chat-mature-cpu-300b`; position probation updates
fell to 45.  The final post-aging pair is in
`chat-mature-cpu-final-300-mask` and
`chat-mature-cpu-final-300-adaptive`; it retained identical search counters,
admitted and probed zero route classes, and stayed within the 5% CPU gate.
Its sampled lookup clock had only 24 samples per case, so total user CPU is
the meaningful short-run comparison.  The original failed result is important
evidence for why the cold-path work was removed rather than hidden by threshold
tuning.

## Focused and adversarial coverage

`compact_back_demod_test` now checks:

- 10,000 singleton route classes allocate zero profiles and issue zero probes;
- pre-tree singleton classes allocate no root trees, while a genuinely
  repeated class still crosses the qualification boundary and calibrates;
- cold classes totaling 40 observations across two decay windows still
  allocate no profile, while one genuinely hot class remains admitted with a
  single probe;
- heating 5,000 classes against the 4,096-slot table keeps memory fixed,
  admission deterministic, and probes below one per 32 routed lookups;
- one global reservation buys one candidate census, even for multi-feature
  patterns;
- rejection prevents rescanning until population doubles, after which exactly
  one newly funded retry occurs;
- budget exhaustion freezes admission while an unrelated feature remains
  complete through forced compaction;
- checkpoint version 3 and compaction retain bounded adaptive state;
- high logical term-pool bases preserve exact candidates for every strategy;
- 32 simultaneously active position features still require one record walk,
  and a fully demoted root requires none;
- a variable-prefix calibration can append real candidates, exhaust its
  allowance, roll those candidates/stamps back, and return the exact mask
  order; and
- a 1,024-duplicate root backfill can grow the shared posting array during
  traversal without invalidating its iterator.

The last item is a regression for the supplied `chat_test.new.out6` crash.
The exact extracted input reproduces SIGSEGV at given 301 in `b6142ea`, at
`process_root_postings+0x122` (`+0x2b832`).  Tree posting construction had
reallocated the same block array through which root backfill retained a raw
pointer.  With only commit `3f0d83d` applied to that commit, the same input
passed given 523 under a 20-second cap without a fault.

The final-binary accelerated 10,000-record longevity test also passes with
factor 64 and the 16 MiB cap.  Its adversarial variable-prefix query admitted
one selective position on evidence query 59 and then reduced steady retrieval
work from 10,000 units/query to 1, with identical answer order.  The current
debug test binary completed in 7.16 user seconds at 41,040 KiB peak RSS.  A
deliberately capped earlier 100,000-record attempt
reached its 120-second timeout at 392,028 KiB before completing all four
synthetic phases; it is recorded as an incomplete resource-bound run, not as
scale evidence and not as a failure hidden by extrapolation.

The focused test passes under ASan+UBSan with leak reporting disabled;
enabling LeakSanitizer reports the test process's pre-existing process-global
LADR allocations, not an invalid access in this change.  The complete
`compact-frontier-tests`, `compact-generalization-smoke`, and
`long-run-scalability-tests` targets pass, including compact ID-map, rewrite,
unit-index, feature-index, report-parser, OTTER audit, and OTTER checkpoint
coverage.

The long-run report parser now exports interval deltas for the new admission,
churn, credit, census, backfill, cooldown, and freeze counters.  This makes the
external mature run auditable instead of relying on a final aggregate.

## Remaining external gate

These results establish semantic compatibility, bounded fixed overhead, and
amortized maintenance.  They do not establish the final mature CPU result.
Run the exact configuration above through at least the archived 7,365-given
state and reject it if matched-trajectory user CPU exceeds mask8 by 5%, if
position maintenance exceeds 5% of user CPU, if route replacements remain
proportional to queries, or if any admission/demotion cycle continues after
the first position-budget exhaustion.  The proof-endpoint comparison remains
the final product gate.
