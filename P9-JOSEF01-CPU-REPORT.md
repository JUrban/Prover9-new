# Josef 01 compact-OTTER CPU and RAM report

## Status

Branch `josef01-cpu-next` preserves the Josef 01 search trajectory on every
bounded replay performed here and contains several general, non-Josef-specific
CPU improvements.  It has **not** been run to the Josef 01 proof endpoint on
this 23-GiB development host.  The proof-endpoint CPU result therefore remains
a user-run acceptance gate, not a completed claim.

The completed compact baseline is still an important result: it proves the
same theorem after the same 30,827 given clauses while reducing measured
resident memory by about 80%.  Its CPU cost, however, was 1.90 times old P9.
The work on this branch attacks measured causes of that cost without changing
clause selection, hint answers, or inference order.

### 2026-08-20 measurement correction

The 1.90-times full-run CPU comparison below is real as a record of the two
supplied processes, but it is not a fair measure of the compact search engine.
`Josef_01.out.new3` enabled exact detailed phase clocks with `set(clocks)` and
the default `clock_sample_rate=1`; `Josef_01.out.old` did not enable clocks.
The compact run consequently made kernel CPU-time queries around billions of
search-phase intervals that the old-P9 run never made.

This was isolated on `vmi3142790` with one process at a time, a 2-GiB hard
limit, no swap activity by any measured process, and the exact 1,001-given
endpoint `(Generated=1628048, Kept=320239, proofs=0)`.  All four runs have the
same normalized given-clause SHA-256,
`c23110e9918bbd442865e24d36de6dbc11e2406ceadde0dd36fd55e24bb7be75`.

| Exact 1,001-given control | User CPU | System CPU | Total CPU | Peak RSS | old / control |
|---|---:|---:|---:|---:|---:|
| preserved old P9, clocks off | 111.38 s | 5.44 s | 116.82 s | 697,984 KiB | 1.00 |
| installed pre-polling PGO, exact clocks | 45.90 s | 21.69 s | 67.59 s | 599,492 KiB | 1.73x faster |
| installed pre-polling PGO, clocks sampled 1/16 | 41.16 s | 6.36 s | 47.52 s | 604,256 KiB | 2.46x faster |
| installed pre-polling PGO, clocks off | 37.14 s | 3.71 s | 40.85 s | 609,000 KiB | 2.86x faster |

Thus exact clocks added 26.74 CPU seconds, or 65.5% relative to the clocks-off
process and 39.6% of the exact-clock total, at this prefix.  Sampling retained
approximate phase attribution but still cost 6.67 seconds relative to clocks
off.  `/usr/bin/time -v` remains the authority for total user/system CPU when
internal clocks are disabled.

The installed PGO binary used for this table is SHA-256
`dad5d12683bc8106f4bd43cff4cb7be5d66e200ab4ee7e145579639e209b98f2`;
the preserved old binary is
`bcdf6bafbf608fde463fd43ef541891813f5c49a2d5153711c54925e98d76bcc`.
The clocks-off current output is SHA-256
`164bcc74213885c7e67853dd479c65fa6192ffa83793b2daa65247f63a1248b3` and
the new old-P9 control output is
`2238f376324c7fd5010391063e7686646b914f7067fc7f0492eec89a6113f0a5`.
They are bounded evidence, not substitutes for a proof-endpoint run.

For a throughput comparison, use `clear(clocks)`.  For occasional approximate
phase reports, use `set(clocks)` with `assign(clock_sample_rate,16)`.  Exact
clocks are a diagnostic mode and must be enabled on both competitors if their
process CPU is compared.

### Amortized periodic CPU reports (`josef01-cpu-next`)

Turning detailed clocks off exposed a second diagnostic tax.  With
`assign(report,60)`, the clocks-off profile called `possible_report()` and
`user_time()` once for each of 1,628,048 generated clauses.  The latter is a
`getrusage()` call.  A control with periodic reports disabled reduced the
current PGO prefix from 40.85 to 37.16 total CPU seconds while preserving the
same endpoint and given digest.

`possible_report()` now polls CPU time once per 256 report opportunities.
Given-count reporting remains exact, final statistics are unchanged, and a
CPU-time report can be delayed by at most 255 clause-report opportunities.
On the 1,001-given gate this reduces about 1.63 million CPU-time reads to at
most 6,360; at the completed 1.60-billion-generation endpoint it reduces the
same polling path to about 6.26 million reads.  The change adds no persistent
search memory and cannot affect clause admission, inference or selection.
Every statistics section exposes the cumulative check as
`Periodic_report_poll`; this records generated clauses, actual CPU-time reads
and the fixed interval for an external mature-run audit.

Matched portable `-O2` binaries were run as a serial reversed pair with
clocks off, `report=60`, a 2-GiB limit and no swapping:

| Binary | Mean user CPU | Mean system CPU | Mean total CPU | Mean RSS |
|---|---:|---:|---:|---:|
| parent | 46.80 s | 4.57 s | 51.37 s | 610,108 KiB |
| amortized polling | 45.80 s | 2.48 s | 48.28 s | 609,484 KiB |
| change | -2.1% | -45.8% | **-6.0%** | -624 KiB |

The individual totals were parent/candidate 54.44/48.90 seconds followed by
48.29/47.65 seconds in reversed order.  Every run ended at
`(1001,1628048,320239,0)` with the normalized given digest above.  A separate
100-given `report=1` production smoke emitted six statistics sections,
confirming that short periodic reports remain live after amortization.
An independent `report_given=10` smoke reported at givens 10, 20 and 30
exactly, followed by the final given 31 statistics.

### Balanced PGO retraining audit: not accepted as the general binary

Because `search.c` changed, the installed binary's old profile is not valid
profile-guided optimization data for the current source.  A fresh isolated
PGO audit therefore trained the current branch serially on three workloads:
Josef 01 to 1,001 given clauses, Josef 02 to 601, and CHAT to 601.  All runs
were bounded to 2 GiB, had zero swap, and reproduced their exact endpoint and
normalized selected-given digest.  Training produced 103 profile files
totalling 298,988 bytes.  The instrumented generator and resulting PGO-use
binaries have SHA-256 values:

```text
generator  c1f4c193d096b27426dc323cc2cf4a143cfcdbdac5a3093005b78b42c6597dab
PGO-use   73971bff5c445c558ad7cc3e6fd42b4512f15c96e93fb852768b2dafa467768d
```

The retrained PGO-use binary was promising on its target workload.  In a
serial reversed Josef 01 pair against the installed PGO binary, mean total
CPU fell from 48.12 to 41.17 seconds (-14.5%), mean system CPU from 4.64 to
1.90 seconds (-59.0%), and RSS remained approximately 610 MiB.  Both binaries
ended at `(1001,1628048,320239,0)` with the same given digest.  An independent
Josef 02/600 run also reproduced `(601,524799,26671,0)` in 27.07 total CPU
seconds and 206,712 KiB RSS.

The cross-workload CHAT gate did not confirm a general improvement.  At the
exact `(601,497430,16974,0)` endpoint with a common given digest, the installed
binary averaged 50.81 total CPU seconds and the retrained candidate 52.63
seconds (+3.6%).  The two pair orientations contradicted each other: the
candidate lost the first comparison and won the reversed comparison.  This
is too noisy to claim a regression, but it is also insufficient evidence to
replace a general production executable with a Josef-trained PGO build.

Accordingly, the retrained binary is **not installed** and is not an
authority for the full run.  `bin/prover9` deliberately remains the prior
PGO executable with SHA-256 `dad5d126...`.  The accepted result on this branch
is the portable source-level polling change; final testing must build that
source afresh.  Any future PGO release needs repeated mature gates on Josef
01, Josef 02, CHAT and Osborn, not merely a larger Josef-weighted training
set.

The isolated current-source PGO build also passed
`compact_otter_audit_test.sh` and `compact_generalization_smoke_test.sh` under
hard memory/time limits.  The first checks compact/legacy proof equivalence
and all authoritative/audit index strategies; the second covers legacy and
file-backed compact variants on two distinct problems.  These tests establish
bounded semantic compatibility, not long-run CPU performance.

### Josef 01 packed-hint cache gate

The production default allocates a 2-MiB exact result cache in
`packed_fast`.  It is semantically transparent, but every eligible query must
canonicalize and hash its shallow feature profile, validate dependencies and
usually store a replacement.  The completed Josef 01 output made 2.09
billion cache queries.  Its 8.15% hit rate avoided 26.82 billion raw posting
candidates, but the low reuse rate made it unclear whether that work paid for
the cache machinery.

A clocks-off serial reversed pair now compares the default cache with
`assign(hint_cache_kb,0)` at the exact 1,001-given Josef endpoint.  Every run
used the current portable source binary, one pinned CPU, a 2-GiB limit and
zero process swap:

| Order | 2-MiB cache | cache disabled | cache-off change |
|---|---:|---:|---:|
| first orientation | 48.66 s | 46.11 s | -5.2% |
| reversed orientation | 50.76 s | 49.02 s | -3.4% |
| mean | 49.71 s | 47.57 s | **-4.3%** |

All four runs ended at `(1001,1628048,320239,0)` and emitted the same 1,001
selected-given digest,
`d2c195ffae6f90a9dc63a31c8c1fd5c49d29d564669ad9799575e9febb5c316f`.
Mean RSS changed from 591,826 to 594,932 KiB; that 3-MiB movement is opposite
the 2-MiB allocation difference and is ordinary measurement noise rather
than a RAM regression.  The prefix's 7.52% cache hit rate is close to the
completed run's 8.15%, which makes the CPU result relevant to the mature
workload, although only the full authority run can confirm its final effect.

This is deliberately a **Josef 01 override, not a default change**.  CHAT/600
had identical trajectories and means of 49.15 seconds cache-on versus 48.92
cache-off (-0.5%); its pair orientations contradicted each other.  Josef
02/600 also preserved its exact trajectory, but averaged 35.60 seconds
cache-on and 36.63 cache-off, so disabling the cache was 2.9% slower on mean.
Its pair orientations also contradicted each other, which is not proof of a
cache win but does rule out a robust general cache-off win.  Josef 02's cache
hit rate was 48.58% and it avoided 13.89 million candidates in only 215,118
queries.  The existing 2-MiB default therefore remains appropriate for an
unknown/general workload.

`CHAT_HINT_CACHE_KB` has been added to `chat_test_matrix.sh` so external
production cases can reproduce either policy without editing logical input.
It is unset by default; `CHAT_HINT_CACHE_KB=0` emits the Josef override only
for `new_otter_compact_file_production`.

### Mature file-selector buffer gate

The completed compact run used a 65,536-entry selector buffer.  At the proof
endpoint, its file selector had 36.11 million run entries and had performed
551 flushes and 546 binary-run merges.  It wrote 8.38 GB and reread 7.52 GB.
This is real merge amplification, not inference work, and motivates the
1,048,576-entry setting in the authority configuration below.

The scale probe was first repaired to use the directory's authoritative
runtime record width.  It had retained a historical 64-byte assertion after
later metadata increased the record to 72 bytes.  The probe now accepts a
buffer size and reports directory/selector entry widths, flushes and merges.
Existing selector order, compaction and checkpoint tests remain green.

An optimized serial reversed pair inserted four million age-selected records
with one file selector, selected the exact first ID and used zero swap:

| Buffer | Mean user | Mean system | Mean total | Peak RSS | Flush / merge |
|---:|---:|---:|---:|---:|---:|
| 65,536 | 0.96 s | 1.26 s | 2.22 s | 89,636 KiB | 61 / 56 |
| 1,048,576 | 0.71 s | 0.75 s | 1.46 s | 124,086 KiB | 3 / 1 |
| change | -26.0% | -40.6% | **-34.3%** | +34,450 KiB | bounded |

The larger buffer deterministically reduced bytes written from 498,597,888
to 125,829,120 (-74.8%) and bytes read from 403,636,224 to 50,724,864
(-87.4%).  The test is a selector-only scale proxy, so its 34% process result
must not be applied to the complete prover.  It does establish that the
larger buffer trades tens of MiB for substantially less mature selector CPU
and I/O.  Buffers grow on demand per selector; in Josef 01 only `TheRest`
grows to millions of entries, while the other active selector populations at
the proof endpoint were below 20,000.

## Authoritative completed runs

The source files are:

- `/project/bob/Josef_01.out.old`, SHA-256
  `770ea6fe4bee3adfe0b82a2f5fd9938cb981b1ff32f8c04e3bdcd5fbc0b15955`;
- `/project/bob/Josef_01.out.new3`, SHA-256
  `3448aab96ec6fb4bd1571edf0a3e043f4f3194aa285e09be8adfbe08da61e974`.

| Measurement | old P9 | completed compact baseline | compact / old |
|---|---:|---:|---:|
| Given | 30,827 | 30,827 | exact |
| Generated | 1,602,769,536 | 1,602,769,536 | exact |
| Kept | 36,195,388 | 36,195,388 | exact |
| Hints matched | 48,968 | 48,968 | exact |
| User CPU | 19,064.46 s | 28,163.94 s | 1.477 |
| System CPU | 288.00 s | 8,532.45 s | 29.63 |
| Total CPU | 19,352.46 s | 36,696.39 s | 1.896 |
| Wall clock | 19,356 s | 36,725 s | 1.897 |
| Old internal memory / compact PSS | 45,258.48 MiB | 9,083,561 KiB | about 19.6% |

The last row compares the best available old-P9 internal peak-style memory
number with the compact process PSS, so it is not a same-instrument metric.
It nevertheless agrees with the observed practical result: the compact
process fits in roughly 9 GiB rather than roughly 45 GiB.  The next comparison
should record GNU-time RSS and `/proc/$pid/smaps_rollup` PSS for both binaries
on the same host.

The proof text and endpoint tuple are identical.  The compact baseline is not
a divergent search that happens to find the theorem later; it is the same
OTTER trajectory implemented with different storage and indexes.

## What consumed the compact baseline CPU

The completed output reports these top-level clocks:

| Clock | Seconds |
|---|---:|
| preprocess | 20,814.10 |
| infer | 4,736.38 |
| conflict | 4,337.29 |
| hints | 3,945.86 |
| weigh | 1,903.30 |
| subsume | 967.48 |
| index | 103.13 |

Some search clocks are nested, so this table must not be summed.  It locates
hot paths; total process CPU remains the authoritative total.

Three scaling failures stand out.

1. The compact allocator handled 148.9 billion allocation calls and 4.43 TB
   of cumulative object traffic.  It returned 11,942,479 empty 256-KiB slabs,
   causing 3.13 TB of cumulative `munmap` traffic.  This is the strongest
   explanation for the extra 8,244 seconds of system CPU over old P9.
2. The unit index issued 55.3 million forward-generalization and 71.2 million
   conflict queries.  It spent 745.868 sampled seconds in generalization and
   4,310.748 sampled seconds in unification, visiting 68.17 billion conflict
   tree nodes.  The mature tree averaged about 958 visited nodes per conflict
   query even though 95.8% of the queries returned no candidate.
3. Packed hint matching issued 1.60 billion ordinary and 490 million flipped
   queries.  The sampled timers report 3,331.879 and 922.614 seconds.  The
   2-MiB result cache hit only 8.15%; the conjunction profiles rejected
   235.74 billion posting candidates, so this is already a filtered hot path,
   not a missing elementary index.

The RAM itself is dominated by necessary long-lived compact state rather than
ordinary live clauses:

| Component | Approximate bytes at proof |
|---|---:|
| compact term pool | 3.84 GB |
| compact unit index | 3.79 GB |
| compact nonunit index | 0.16 GB |
| live allocator objects | 0.066 GB |
| process PSS | 9,083,561 KiB (8.66 GiB) |

The dense passive directory, selector runs, and ancestor store are
file-backed.  Their logical/physical files are much larger than their process
PSS and must be accounted separately when comparing RAM with disk/page-cache
use.

## Changes carried into `josef01-cpu-next`

### Instrumentation and adaptive escape

Commit `dc93531` splits unit-tree fanout into query-variable, pending-subtree,
rigid-child, and rigid-sibling sources.  It changes no answers.  This makes a
mature run capable of distinguishing unavoidable variable expansion from a
bad rigid lookup.

Commit `8ced379` adds an opt-in `compact_unit_strategy=adaptive`.  Its complete
position postings are delta-compressed in 256-byte chunks: the 1,000-given
Josef feature store fell from 58,965,408 to 9,151,600 bytes, an 84.5% reduction.
The adaptive route cut tree visits but was slower at 1,000 and 2,000 givens
because it admitted more exact candidates.  It therefore remains an
experiment, not the recommended full-run setting.

### Packed-term access

Commit `b81b9cc` resolves the packed token and logical bases once per public
unit-index query.  It removes repeated public resolver/bounds-check calls from
each radix edge while keeping the exact matcher authoritative.  On an exact
1,000-given replay, sampled generalization time fell from 4.695 to 4.123
seconds and unification from 3.520 to 3.089 seconds, about 12% in both local
operations.  The trajectory and memory were unchanged.

### Bounded slab recycling

Commit `67a6c29` keeps at most 256 detached 256-KiB mappings in a cross-size
class pool.  The hard ceiling is 64 MiB.  Memory-pressure checks and
`memory_release_unused()` purge the pool, so this does not restore old P9's
unbounded append-only behavior.

At 2,000 Josef givens, the old reclamation policy had unmapped 1,861 slabs.
The bounded recycler had unmapped only the initial 141, reused 1,469 mappings,
and held 251 cached mappings.  It did **not** improve total CPU at that short
prefix; initialization and I/O still dominate there.  Its purpose is the
day-scale regime represented by the baseline's 11.94 million unmaps.  The
full run must show whether avoided kernel calls convert into the expected
system-CPU reduction.

### Constant-time symbol metadata

Commit `0af0650` maintains a growable direct `symnum -> Symbol *` table while
retaining the historical hashes for name lookup and iteration.  The pointers
refer to authoritative mutable records, so precedence, type, KB weight, and
other updates remain visible.

Two serial pinned 1,000-given controls averaged 82.14 CPU seconds; two
candidates averaged 78.90, a 3.95% reduction.  All 1,000 selected clauses and
the endpoint `Generated=1,628,048 / Kept=320,239` were identical.  Mean peak
RSS differed by about 1 KiB.  The new 4,096-symbol regression exercises table
growth and mutable metadata semantics.

### Ordered forward-generalization pruning

Commit `9b06885` uses the radix tree's existing sibling ordering during
forward generalization.  A rigid target can match only stored-variable edges
and the one equal rigid edge; incompatible rigid siblings are skipped or end
the scan.  The exact matcher still enforces repeated-variable consistency.

At 1,000 givens, work fell from 41,802,863 to 10,594,120 (-74.7%) and the
sampled generalization timer from a paired mean of 4.424 to 2.741 seconds
(-38.1%).  Whole-process CPU fell from 81.63 to 78.83 seconds (-3.44%).  At
2,000 givens, generalization work fell from 112,056,339 to 28,653,322 (-74.4%)
and its sampled timer from 12.552 to 8.468 seconds (-32.5%).  The selected
clauses and endpoint tuples remained exact.

### Direct two-position unit-conflict refinement

Commit `45ecaac` repairs the principal weakness of the experimental
`compact_unit_strategy=adaptive` route.  The original route selected the
rarest rigid query position, decoded the union of its exact-symbol and
ancestor-variable postings, and sent every surviving record to the exact
unifier.  At the mature completed endpoint, unit conflict retrieval had
visited 68.168 billion code-tree nodes for 71.180 million queries, about 958
nodes/query, while 95.8% of queries returned no candidate.  A route which can
avoid that traversal without inflating exact work is therefore a plausible
long-run CPU escape.

The accepted refinement chooses a second rigid query position, but it does
**not** read a second posting list.  It decodes only the rarest first-position
union.  For each record in that union it walks the compact prefix term to the
second position and requires either the same rigid symbol or a stored variable
at that position or one of its ancestors.  This is a necessary condition for
unification, so it can reject records but cannot reject a unifier.  The full
unifier remains authoritative, and final proof IDs retain their historical
descending order.  A zero-count first-position union is now an immediate
negative answer, avoiding empty bucket and query-stamp work.

The statistics line exposes the mechanism directly:

```text
position_refinement_queries=...
position_refinement_checks=...
position_refinement_rejects=...
```

At the exact Josef 01 601-given endpoint, the refined route performed 4,183
two-position queries and rejected 15,505 of 23,960 first-union records before
unification (64.7%).  Conflict exact tests fell from 36,232 in the prior
single-position adaptive control to 20,727 (-42.8%).  Unlike the rejected
two-posting prototype, posting scans stayed at roughly the single-position
level: 23,960 rather than 71,706.

Pinned, clocks-off, 2-GiB bounded gates preserved the exact search endpoints
and used zero process swap:

| Josef 01 gate | Refined adaptive | `code_tree` | Observation |
|---|---:|---:|---|
| 601 given, CPU | 22.47 and 24.81 s | 24.13 s | adaptive mean 23.64 s; effectively parity (-2.0%) |
| 601 given, peak RSS | 541,496 and 542,832 KiB | 534,704 KiB | about 7--8 MiB extra |
| 1,001 given, CPU | 48.68 s | 49.40 s | effectively parity (-1.5%) |
| 1,001 given, peak RSS | 619,444 KiB | 612,400 KiB | about 7 MiB extra |
| 1,001 given, code-tree nodes | 25,697,573 | 35,045,361 | -26.7% |

The timings are too close and too few to claim a bounded speedup.  The work
counters are the reason to retain the experiment: its avoided code-tree work
grows with the mature index, while its direct checks are bounded by the
rarest posting union.  A structurally different CHAT/601 smoke also preserved
`(Generated=497430, Kept=16974)` and used 41.11 CPU seconds, versus 43.25 in a
single adjacent control; only one query there exercised two-position
refinement, while 31,437 zero-count choices exercised the fast-negative path.

The focused regression constructs two first-position near misses and checks
that the direct second feature rejects exactly one without losing the exact
answer.  `compact_unit_index_test`, `compact_long_run_test`, an adaptive `x2`
audit, and `compact_generalization_smoke_test.sh` all pass.  These are bounded
semantic and scalability gates, not a proof-endpoint CPU result.

This does not yet replace `code_tree` as the primary authority configuration.
Adaptive must maintain compressed position features in addition to the code
tree.  The 2,000-given store used 21,988,160 feature bytes for 695,604 active
units, about 31.6 bytes/unit.  A linear extrapolation to the completed
35,592,170-unit population is approximately 1.1 GB of extra resident index
capacity.  That would move the measured 9,083,561-KiB compact PSS toward about
10.2 million KiB (roughly 9.7 GiB) and reduce the estimated saving versus old
P9 from about 80% to roughly 77--78%.  This estimate is deliberately
conservative and must be replaced by measured mature PSS.  The next design
step, if adaptive wins CPU, is selective or file-backed feature admission
rather than accepting an unbounded duplicate resident index.

### Sparse hint planes and reused generalization edge heads

Commits `f13e552` and `3eb7c2b` remove two high-frequency pieces of redundant
work without changing an index, route, or answer.

`hint_postings_add_profile()` previously tested all 64 feature-mask bits for
every profile posting, even though clause feature masks are sparse.  The
Josef 01/1,000 gprof run attributed about 5.5% of sampled self CPU to roughly
8.1 million calls of this function.  It now iterates only the set bits with
`ctz` and `mask &= mask - 1`; the same bit planes and flags are populated in
the same posting order.

Forward generalization already reads the first packed radix-edge token to
choose a stored-variable edge or the equal rigid edge.  The exact edge matcher
then resolved and compared that same rigid token again.  It now receives the
classified head: equal rigid heads consume the known-equal query/token pair
and matching resumes at token one, while variable heads retain the existing
binding and repeated-variable logic.  This adds no state or RAM.

Pinned, clocks-off, 2-GiB gates preserved exact endpoints and every reported
search/index work counter.  In serial reversed Josef 01 pairs, the combined
candidate averaged 23.97 versus 25.79 CPU seconds at 600 givens (-7.1%) and
49.86 versus 51.22 at 1,000 (-2.6%).  Pair orientations disagreed at the
1,000-given gate, so the latter is bounded evidence, not a robust standalone
speedup claim.  An adjacent CHAT/600 pair favored the candidate 44.07 versus
45.05 seconds (-2.2%).

One combined Josef 02 orientation produced a 39.26-second candidate outlier;
the other was 34.01 versus its 33.80-second control.  The changes were then
isolated in separate binaries: hint-only and generalization-only each used
33.14 CPU seconds at the exact Josef 02/600 endpoint, versus recent controls
of 32.94 and 33.80 seconds.  Together with identical work counters, this
supports treating the 39-second sample as host/code-layout noise rather than
added logical work; the report does not use the outlier-skewed combined mean
as evidence of a gain.

`hint_postings_test`, `compact_unit_index_test`, `compact_long_run_test`, the
adaptive `x2` audit, and `compact_generalization_smoke_test.sh` pass on the
combined source.  Peak RSS was unchanged within measurement noise and every
measured process reported zero swap.

### Reproducible production CHAT smoke

Commit `de55328` adds the `new_otter_compact_file_production` case to
`test.src/chat_test_matrix.sh`.  Existing historical case names keep their
old policies.  On `/project/bob/chat_test.in` (SHA-256 `9781ee...`), the new
case stopped intentionally at 300 givens with
`Generated=120,793 / Kept=5,737`, exactly matching the archived endpoint.  It
used 33.51 user seconds and 462,852 KiB peak RSS.  This is a semantic smoke,
not a mature CPU result; `adaptive32` has visible short-prefix construction
cost.

## Rejected or non-default experiments

These results are important because they prevent tuning a large run with
plausible-looking options that were already negative.

- The pre-`45ecaac` single-position `compact_unit_strategy=adaptive` reduced
  tree work but was about 8.5% slower at 2,000 givens and produced more exact
  tests.  Keep `code_tree` for the first decisive run; the refined adaptive
  route requires a separate staged gate.
- A first attempt at two-position filtering marked the first posting union
  and decoded a complete second union to intersect it.  At 1,000 givens it
  reduced conflict exact tests from 293,410 in the compressed adaptive
  baseline to 230,055, but raised position postings from 111,551 to 424,572.
  It took 56.33 CPU seconds versus 49.40 for `code_tree` (+14.0%).  This
  implementation was replaced completely by direct compact-term checking;
  no second posting union is decoded in commit `45ecaac`.
- Bypassing route learning for a complete one-posting union was rejected.
  At 600 givens, serial reversed pairs were effectively neutral: the refined
  control averaged 26.40 CPU seconds and cold admission 26.49 (+0.4%).  At
  the more informative 1,000-given gate, cold admission took 56.83 CPU
  seconds versus 52.05 for the adjacent control (+9.2%).  It reduced
  code-tree nodes from 25,697,573 to 22,759,380 (-11.4%), but raised position
  postings from 111,867 to 171,424 (+53.2%) and exact tests slightly from
  230,055 to 230,494.  Even one hash/posting decode plus a compact path walk
  costs more than dozens of contiguous radix nodes on this path; cold-score
  thresholds 1 and 4 were removed completely.
- Raising `CUI_ADAPTIVE_POSITION_FACTOR` from 12 to 24 was also rejected in
  a 600-given serial reversed pair.  Factor 24 reduced direct checks from
  23,960 to 8,045, but raised tree nodes from 3,074,870 to 3,364,559.  Its
  paired mean was 26.54 CPU seconds versus 23.79 for factor 12 (+11.6%), and
  it lost in both orientations.  The current learned factor 12 is therefore
  retained; larger factors merely exchange cheap contiguous tree work for
  too little avoided position work.
- A 64-MiB packed-hint result cache raised its hit rate only from 7.42% to
  9.03%, used about 63 MiB more RSS, and increased the 1,000-given CPU result
  from the current paired mean of 78.83 to 82.11 seconds.  Do not add
  `assign(hint_cache_kb,65536)`.
- A negative Bloom summary was slower and was removed completely.
- A query-scoped binding trail preserved all answers but raised the sampled
  generalization timer from roughly 2.72--2.76 to 2.920 seconds.  It was
  reverted completely.
- Moving the 256-clause CPU-report cadence from `possible_report()` into its
  caller removed the residual per-generated function call and separated
  exact given-count reports.  In a clocks-off, cache-off reversed Josef
  01/1000 pair, mean system CPU fell from 2.25 to 2.08 seconds, but mean total
  CPU rose from 49.13 to 49.50 seconds (+0.8%) and the pair orientations
  disagreed.  Every given digest remained exact; the dispatch split was
  nevertheless reverted completely because the release compiler already
  makes the residual call negligible.
- A suspected long-run statistics scan was also audited and rejected as a
  target before merging extra bookkeeping.  `update_memory_stats()` scans
  `clause_store_length(Glob.disabled)`, but compact OTTER's dense passives are
  detached archive records and are not members of that retained-handle
  array.  At the completed endpoint the ancestor store reports 36,195,194
  cumulative records and 36,195,190 detached records: only four records ever
  used ordinary handles.  Its 1,053,512 handle bytes are the fixed 1-MiB
  write buffer, the 4-KiB I/O buffer, the 328-byte store, and a 64-pointer
  allocation.  Thus each report scans four entries, not 36 million.  A
  synthetic million-handle scan confirmed that such a layout would be
  measurable, but it does not model Josef's detached directory.  The proposed
  resident side list was reverted completely; it would have added lifecycle
  state without improving the production path.
- Requiring a packed-hint profile to appear three times before admitting its
  exact result was rejected at the first 101-given Josef gate.  A bounded
  two-row frequency sketch reduced cache stores from 10,398 to 390, but it
  also lost useful first-repeat results: hits fell from 2,620 to 885 and
  posting candidates avoided from 345,334 to 153,819.  Total CPU increased
  from 13.40 to 14.13 seconds (+5.4%) with the exact same
  `(Given=101, Generated=13675, Kept=7309)` endpoint, approximately equal RSS,
  and zero swap.  The sketch and admission policy were reverted completely.
  This also explains why the proven Josef policy disables the result cache
  outright instead of retaining lookup/hash cost while selectively refusing
  stores.
- Reusing the first literal's already computed structural mask for the
  packed-fast clause profile was also rejected.  It removes one recursive
  `packed_term_feature_mask` traversal per ordinary query without changing
  any candidate or index state, but release-code layout outweighed that saved
  work.  At the exact 601-given Josef 01 endpoint, a pinned clocks-off reversed
  pair averaged 25.75 CPU seconds candidate versus 24.85 control (+3.6%).  An
  adjacent 1,001-given check took 50.94 seconds candidate versus 49.66 control
  (+2.6%).  All runs preserved `(Generated=560216, Kept=109842)` or
  `(Generated=1628048, Kept=320239)`, every packed-hint counter, approximately
  526/611 MiB peak RSS, and zero process swap.  The source was reverted
  completely; eliminating a source-level recursive call is not sufficient
  evidence when the whole optimized binary is slower at both bounded gates.
- Replacing KBO's temporary linked variable multisets with stack counters was
  rejected even though it substantially reduced allocator traffic.  A first
  direct 100-counter form fell back for the legal high variable numbers that
  occur after standardization: at Josef 01/1,001 it removed only 3.17 million
  of 180.03 million allocation calls and averaged 58.33 CPU seconds versus
  55.93 control (+4.3%).  A sparse `(variable,count)` stack table handled
  high numbers without allocation and removed 21.40 million calls plus 342.4
  MB of cumulative object traffic at the same endpoint.  It looked excellent
  in the shorter adaptive 601-given gate and improved the adaptive 1,001-given
  reversed mean from 54.77 to 52.64 seconds (-3.9%).  That result did not
  generalize to the primary `code_tree` authority configuration: its reversed
  1,001-given mean was 49.31 seconds candidate versus 48.45 control (+1.8%).
  Josef 02/601 also moved from 36.18 to 36.91 seconds (+2.0%) despite 7.70
  million fewer allocation calls, while CHAT/601 was neutral at 45.47 versus
  45.30 seconds with 2.45 million fewer calls.  All endpoints, search/index
  counters and allocator live/reserved state were exact; RSS was unchanged
  within layout noise and every process used zero swap.  A focused test also
  covered multiplicities, high variable IDs, and the more-than-`MAX_VARS`
  fallback.  Both implementations and the temporary test were nevertheless
  removed: fewer allocations are useful telemetry, not a CPU win, and the
  production configuration plus a second Josef workload both regressed.
- A native `-O3 -march=native -flto` build used 77.60 CPU seconds in one
  pinned 1,000-given run versus 81.69 for the immediately following portable
  `-O2` control.  Earlier portable pairs averaged 78.83, so host-frequency
  variation is material.  `NATIVE=1` is an optional final-build experiment,
  not part of the algorithmic claim.

## Likely RAM and CPU effect

### RAM

The accepted CPU changes do not materially undo the compact memory result.
The direct symbol table costs one pointer per rounded symbol slot; term-base
caching and sibling pruning add no long-lived search structure.  The slab
pool has a strict 64-MiB ceiling, and changing the selector buffer from 65,536
to 1,048,576 entries adds at most roughly 24 MiB for each selector that
actually grows to the cap.  Only Josef's `TheRest` selector is large enough;
the four-million-record scale gate observed a 34-MiB PSS increase including
the associated mapping/page-cache effects.

The optional refined adaptive unit strategy is the exception: it maintains
compressed position features as well as the code tree.  Its full-population
projection is about 1.1 GB extra and is therefore excluded from the primary
80%-saving estimate.  Use `code_tree` for that authority run.  If adaptive is
staged for CPU, measure its PSS separately and expect a preliminary saving of
roughly 77--78% until selective/file-backed feature storage is implemented.

Starting from 9,083,561 KiB (8.66 GiB) PSS, charging the complete 64-MiB slab
allowance and the observed 34-MiB selector increment gives roughly 8.76 GiB.
The expected saving against the approximately 45,258-MiB old result therefore
remains about 80.2%, subject to same-host measurement.  File-backed logical
bytes and page cache must be reported separately.

### CPU

CPU estimates are less certain and the gains are not additive:

- disabling exact detailed clocks removes a measured 39.6% of total CPU at
  the exact 1,001-given gate; this corrects a configuration mismatch in the
  completed full-run comparison rather than changing proof search;
- disabling the low-hit 2-MiB packed-hint result cache is a measured 4.3%
  whole-prefix Josef 01 gain, but remains a problem-specific override because
  the Josef 02 mean favors retaining the cache;
- the direct symbol table is a measured 3.95% whole-prefix gain;
- term-base caching saves about 12% only inside unit lookups;
- sibling pruning saves about 38% only inside forward generalization;
- direct two-position refinement cuts bounded Josef code-tree traversal by
  26.7% at 1,001 givens while remaining within about 2% of `code_tree` total
  CPU; its mature benefit is unknown and must be measured, not extrapolated;
- sparse hint-plane population and first-edge reuse remove measured hot-path
  instructions with no new storage; their combined Josef 01/1,000 paired mean
  improved 2.6%, but frequency noise prevents a tighter full-run estimate;
- the 1-Mi-entry selector buffer cuts scale-probe selector CPU by 34% and
  read/write amplification by 87%/75%; its whole-prover contribution is
  unknown until the authority run;
- the slab recycler changes almost no bounded CPU but removes nearly all
  post-warm-up slab unmaps by 2,000 givens;
- native compilation may add 0--5% depending on the host.

The earlier 28,000--34,000-second estimate was made before the clock mismatch
was discovered and is superseded.  It charged exact-clock work to the compact
storage/index design.  A precise replacement cannot be obtained by scaling a
1,001-given result across machines: the timer cost is approximately linear in
phase intervals, while unit-tree, hint and selector work grow differently.
The installed clocks-off engine is 2.86 times faster than old P9 at the
bounded exact state, and current source additionally contains the amortized
report polling, slab recycler and mature index changes that were absent from
`new3`.  It is therefore plausible that a clocks-off, freshly built
current-source full run reaches the 24,191-second 1.25-times-old gate, but
**CPU parity has not yet been demonstrated**.  Do not publish a tighter
full-run estimate until that external acceptance run completes.

## Build and run the decisive Josef 01 comparison

### Portable baseline build

From this repository and branch:

```sh
git switch josef01-cpu-next
make -C ladr lib
make -C provers.src prover9
cp -p provers.src/prover9 bin/prover9
sha256sum bin/prover9
```

Do not reuse objects from a differently instrumented, sanitized, profiled, or
`NATIVE=1` build.  If in doubt, use a fresh worktree or clean the build before
compiling.  In particular, the repository's currently installed `bin/prover9`
is intentionally the older PGO executable and does not contain commit
`1c3c413`; run the build and copy steps above before the authority run.

### Prover9 options

Place this block before the problem's ordinary search commands.  The Josef 01
input later executes `clear(back_demod).`; retain that command because it is
part of the authoritative old trajectory.  The compact back-demodulation
options are harmless telemetry/configuration while back demodulation is off,
and keeping the common production block makes cross-problem runs comparable.

```prolog
assign(search_loop,otter).
assign(passive_store,dense).
assign(passive_directory,file).
assign(passive_selector_store,file).
assign(passive_selector_buffer,1048576).
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
clear(compress_disabled).

set(compact_otter_demodulation).
set(compact_otter_unit_index).
set(compact_otter_back_demod_index).
set(compact_otter_nonunit_index).

assign(compact_unit_strategy,code_tree).
set(compact_nonunit_path_filter).

assign(compact_back_demod_strategy,adaptive32).
set(compact_back_sparse_positions).
assign(compact_back_position_budget_kb,0).
assign(compact_back_position_budget_pct,50).
assign(compact_back_position_build_factor,32).
assign(compact_back_eager_position_depth,4).
assign(compact_back_tree_budget_kb,65536).
assign(compact_back_tree_budget_pct,200).
clear(compact_back_edge_filter).

assign(compact_passive_cache,0).
assign(hint_cache_kb,0).  % Josef 01 only; retain the 2048 default generally.
assign(compact_index_stale_pct,25).
assign(compact_term_reclaim_kb,8192).
assign(compact_rewrite_deep_cache_kb,0).

% Maximum-throughput authority run.  /usr/bin/time -v supplies total CPU.
clear(clocks).
set(hint_match_stats).
assign(stats,all).
assign(report,900).
```

Do not add the 64-MiB hint cache and do not select the adaptive unit strategy
for this first authority run.  The primary comparison should isolate the
already measured compact RAM design from the adaptive strategy's projected
extra feature store.

After that run, or as a bounded 4,000/10,000-given preflight on the large
host, the refined CPU experiment changes exactly one line:

```prolog
assign(compact_unit_strategy,adaptive).
```

Keep every other option, including `clear(clocks)` and
`assign(hint_cache_kb,0)`, identical.  Continue only if the given/generated/
kept/hint trajectory is exact, the interval CPU/given slope is no worse than
`code_tree`, there is no swap, and the ratio of
`position_refinement_rejects` to `position_refinement_checks` remains
substantial.  Record feature bytes and PSS; an adaptive full run is a CPU/RAM
tradeoff experiment, not the clean 80%-RAM authority result.

If approximate internal phase attribution is needed, replace `clear(clocks)`
with:

```prolog
set(clocks).
assign(clock_sample_rate,16).
```

Do not use exact rate 1 for a throughput comparison unless old P9 is also run
with exact clocks.

### Invocation and temporary files

Use a fast filesystem with ample free space for `TMPDIR`; `/local` is a good
choice on the previously described machine.  For example:

```sh
mkdir -p /local/mptp/prover9-tmp
TMPDIR=/local/mptp/prover9-tmp \
  /usr/bin/time -v ./bin/prover9 \
  < /path/to/Josef_01.compact.in \
  > /path/to/Josef_01.compact.latest.out \
  2> /path/to/Josef_01.compact.latest.time
```

The matrix driver can generate the same throughput configuration with
`CHAT_CLOCKS=0` and `CHAT_HINT_CACHE_KB=0`; for example, set them alongside
`CHAT_CASES=new_otter_compact_file_production`.  `CHAT_CLOCKS=1` remains the
compatibility default for historical diagnostic matrices, and an unset
`CHAT_HINT_CACHE_KB` retains the general 2-MiB cache default.

The file-backed passive/selector/ancestor stores are normally created as
unlinked temporary files.  `/proc/$pid/fd/*` can show them with a `(deleted)`
suffix while the process is alive; the kernel removes them automatically when
the process exits or is killed.  Logical file size, allocated blocks, process
PSS, and filesystem free space are different quantities.

For an optional native comparison, build in a separate clean worktree with
`NATIVE=1`.  Mixing portable and native objects is invalid.  Run it only after
the portable authority result so compilation variance cannot obscure whether
the algorithmic changes worked.

## Acceptance checklist

The new run is accepted only if all of the following hold:

1. `THEOREM PROVED` is present and the endpoint remains
   `Given=30827 / Generated=1602769536 / Kept=36195388`.
2. The hint report remains `matched=48968`, with the same proof and selector
   endpoint.  A mismatch means search divergence and invalidates a pure CPU
   comparison.
3. GNU time reports user, system, elapsed, maximum RSS, major faults, file
   input/output, and swaps.  Swapping invalidates the CPU comparison.
4. Capture `/proc/$pid/smaps_rollup` or cgroup `memory.peak` during the run so
   anonymous PSS and file-backed page cache are not confused.
5. Save the binary SHA-256, commit ID, complete echoed input, and periodic
   `stats=all` reports.
6. Inspect `Memory report` for cached/reused/evicted slabs.  If system CPU is
   still large despite high mapping reuse, the remaining cause is not slab
   churn.
7. Inspect `Compact_unit_fanout` and unit query profiles.  If pending-subtree
   traversal dominates in `code_tree`, compare the staged refined-adaptive
   run.  For adaptive, record `position_refinement_queries/checks/rejects`,
   position postings, conflict exact tests, code-tree nodes and feature bytes.
   A low reject rate means the extra feature store is not buying selectivity.
8. Inspect `Dense_passive_selector` flushes, merges and read/write bytes.  The
   1-Mi-entry policy should be far below the baseline's 551/546 merge counts;
   otherwise another selector is unexpectedly reaching the cap.

## Reproducing the bounded CHAT smoke

The exact production case is now one command:

```sh
CHAT_CASES=new_otter_compact_file_production \
CHAT_CPU=0 CHAT_REPORT_SECONDS=30 CHAT_CLOCKS=0 \
./test.src/chat_test_matrix.sh /project/bob/chat_test.in \
  chat-production-300 300 240 2048 300
```

It writes the generated input, raw output, GNU-time report, hashes, status,
summary TSV, and long-run parser output under `chat-production-300/`.

## If the full run is still slow

The next change should follow the mature telemetry, in this order:

1. If system CPU remains the outlier, separate allocator mapping reuse from
   passive/selector/ancestor file I/O and page-cache eviction.  Increase no
   cache until this attribution is known.
2. If unit pending-subtree work dominates, stage the direct-refinement
   adaptive route from commit `45ecaac`.  If it wins CPU but its projected
   feature store compromises the RAM target, make feature admission selective
   or file-backed; do not restore the rejected two-posting intersection.
3. If packed hints remain near 4,250 seconds, optimize profile intersection
   and dependency validation.  A larger result cache has already failed; a
   different key/profile representation is required.
4. If selector read amplification dominates, tune run merging from measured
   run counts and bytes, not from the short-prefix 65,536/1,048,576 buffer
   difference.

These are general long-run mechanisms.  None should depend on Josef symbol
names, a singleton term shape, or a fixed hint count.
