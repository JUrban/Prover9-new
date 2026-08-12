# Mature backward-index CPU recovery: implementation and bounded results

Status: implemented and locally validated on branch
`mature-back-index-cpu`.  The external 7,365-given and proof-endpoint gates
remain open; the short runs below are compatibility and fixed-overhead gates,
not a claim that a week-long run has already been reproduced.

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

Candidate completeness and decreasing-ID order remain authoritative in the
mask/tree/position paths.  Scheduling, wall time, and cache residency never
affect the selected route or the returned ID set.

### Route calibration

- A two-row, saturating count-min sketch sees a structural/scale class before
  the 4,096-entry profile table does.
- Both 64K rows are halved every 262,144 routed lookups, so sparse lifetime
  traffic cannot eventually saturate the sketch.  The sweep is one 16-bit
  counter visit per two routed lookups amortized.
- Fewer than 32 observations means an immediate complete mask lookup: no
  profile allocation and no tree probe.
- A full table admits an incoming class only when its deterministic
  frequency/recency score beats the selected resident.  Old population scales
  age out; singleton traffic cannot continuously replace current hot state.
- A class gets one mask baseline and at most one initial tree probe.  Tree must
  show at least a 2x counted-work advantage before promotion.  The ordinary
  20% margin is retained for later reversion.
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
`9781ee07691bc62e01f67534208620ca0be3f026f55248a227e9161f1ed17e6c`
and release binary SHA-256
`f8a49de3f1d05596209c9b7016742cfb95c499433f209da373b41300963fca6e`.
Mask8 and adaptive cases ran with the same generated input on dedicated CPUs.

| Gate | mask8 | adaptive | Result |
|:---|---:|---:|:---|
| 300 given, user CPU | 18.81 s | 18.69 s | adaptive -0.6% |
| 300 given, generated / kept | 120,793 / 5,737 | 120,793 / 5,737 | identical |
| 300 given, sampled lookup | 0.127 s | 0.125 s | parity |
| 1,000 given, user CPU | 104.30 s | 104.83 s | adaptive +0.51% |
| 1,000 given, generated / kept | 1,268,285 / 33,909 | 1,268,285 / 33,909 | identical |
| 1,000 given, sampled lookup | 3.598 s | 3.488 s | adaptive -3.1% |
| bounded 180-second run, final given | 1,275 | 1,290 | adaptive +1.18% throughput |
| bounded run, measured user CPU | 157.33 s | 156.81 s | parity |

At 1,000 givens adaptive admitted only 42 of 23,587 route-table misses,
performed 34 probes in 27,761 lookups, and performed no position census or
backfill.  At the bounded 1,290-given endpoint it held 64 route profiles, used
56 probes in 38,534 lookups, and still performed no position construction.
The adaptive back index was 10.0 MB versus mask8's 4.14 MB at the unequal
bounded endpoints, while both processes reported about 90.4 MiB peak RSS.
The fixed adaptive metadata is visible but is not growing with class traffic.

The first pre-fix 300-given pair is retained in `chat-mature-cpu-300`: it took
23.92 versus 18.73 user seconds because adaptive redundantly scanned mask
buckets and updated 19,744 position probations despite admitting no route.
The corrected pair is in `chat-mature-cpu-300b`; position probation updates
fell to 45.  This failed result is important evidence for why the cold-path
work was removed rather than hidden by threshold tuning.

## Focused and adversarial coverage

`compact_back_demod_test` now checks:

- 10,000 singleton route classes allocate zero profiles and issue zero probes;
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
- checkpoint version 3 and compaction retain bounded adaptive state; and
- high logical term-pool bases preserve exact candidates for every strategy.

The accelerated 10,000-record longevity test also passes with factor 64 and
the 16 MiB cap.  Its adversarial variable-prefix query admitted one selective
position after 58 evidence queries and then reduced steady retrieval work from
10,000 units/query to 1, with identical answer order.  It completed in 5.84
seconds at 40,688 KiB peak RSS.  A deliberately capped 100,000-record attempt
reached its 120-second timeout at 392,028 KiB before completing all four
synthetic phases; it is recorded as an incomplete resource-bound run, not as
scale evidence and not as a failure hidden by extrapolation.

The focused test passes under ASan+UBSan.  Compact ID-map, rewrite, unit-index,
feature-index, report-parser, OTTER audit, and OTTER checkpoint suites also
pass.

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
