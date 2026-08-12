# Prover9 long-run scalability results

This file records the evidence used to accept or reject long-run compact-index
changes.  Short-prefix wins are not promoted unless their CPU and RAM slopes
remain acceptable as the live population grows.

## `chat_test.new.out41`: mask8 backward-demodulation collapse

Source: user-supplied `bob/chat_test.new.out41`, inspected 2026-08-12.  The run
used the frozen compact OTTER candidate with `compact_back_demod_strategy` left
at `mask8`.  It stopped at the 3,600-user-second limit after 8,800 given
clauses.

| User CPU (s) | Given | Active back index | Queries | Cumulative groups | Groups/query | Back lookup CPU (s) | Given/user-s |
|---:|---:|---:|---:|---:|---:|---:|---:|
| 300 | 3,432 | 109,943 | 174,601 | 206,320,581 | 1,181.7 | 25.675 | 11.440 |
| 600 | 4,603 | 222,514 | 311,929 | 665,790,970 | 2,134.4 | 100.246 | 7.672 |
| 900 | 5,299 | 294,107 | 395,683 | 1,100,722,363 | 2,781.8 | 185.325 | 5.888 |
| 1,200 | 5,898 | 346,033 | 471,535 | 1,593,724,466 | 3,379.9 | 285.142 | 4.915 |
| 1,500 | 6,463 | 401,340 | 546,089 | 2,188,804,649 | 4,008.1 | 399.315 | 4.309 |
| 1,800 | 6,944 | 449,834 | 613,378 | 2,808,281,254 | 4,578.4 | 522.217 | 3.858 |
| 2,100 | 7,363 | 488,014 | 674,291 | 3,361,412,509 | 4,985.1 | 645.798 | 3.506 |
| 2,400 | 7,725 | 510,736 | 743,166 | 4,094,898,699 | 5,510.1 | 800.974 | 3.219 |
| 2,700 | 8,167 | 537,987 | 793,126 | 4,640,895,639 | 5,851.4 | 912.474 | 3.025 |
| 3,000 | 8,330 | 564,499 | 824,564 | 4,977,155,644 | 6,036.1 | 982.226 | 2.777 |
| 3,300 | 8,732 | 659,799 | 914,251 | 5,468,625,500 | 5,981.5 | 1,061.240 | 2.646 |
| 3,600 | 8,800 | 684,719 | 937,830 | 5,706,791,501 | 6,085.1 | 1,094.691 | 2.444 |

The active index grew by 6.23 times from the first to last sample.  Groups per
query grew by 5.15 times, close to linear in the active population.  This is a
true long-prefix CPU scalability failure, not timing noise.  At the last
sample, backward-demodulation lookup alone consumed 30.4% of all user CPU.
The median query was already in the 2,049--4,096 work bucket, the 95th
percentile was in 16,385--32,768, and the worst query examined 817,354 groups.

The wasted work is much larger than the useful answer set: 5.707 billion
groups and 4.941 billion occurrences yielded only 453,802 candidates.  Exact
testing took 0.679 seconds and archive materialization 4.590 seconds, so moving
or caching full clauses cannot cure this slope.  The broad shallow `mask8`
buckets themselves are the bottleneck.

Other measured components do not explain this collapse:

- compact unit code-tree retrieval performed about 27.9 million counted work
  for 2.186 million queries in roughly 3.4 seconds;
- compact nonunit retrieval used about 60.8 million work for 1.237 million
  queries in roughly 3.3 seconds; and
- packed-hint back demodulation used about 16.6 seconds.

The run still demonstrated the intended RAM direction: process RSS was about
503 MB rather than the multi-gigabyte legacy result.  But the 83.6 MB compact
back index bought that RAM reduction by allowing near-linear retrieval work.
It therefore fails the product gate despite its memory result.

The dramatic *given-clause* rate at the end has a second cause which should
not be attributed to the index.  Between the 3,300- and 3,600-second reports,
only 68 additional clauses became given, but they generated 10,662,438 clauses
(about 156,800 generated clauses per given).  The first 300 seconds averaged
about 3,127 generated clauses per given.  During the last interval the
back-demodulation clock grew by about 33.5 seconds, while the demodulation clock
grew by about 172.5 seconds and the enclosing preprocessing clock by about
219.7 seconds.  Thus the near-linear lookup slope is a genuine long-run defect
and a large cumulative cost, but the final throughput cliff also reflects an
inference/preprocessing burst in the fixed search trajectory.  A stronger
index cannot make that generated work disappear.

### Required fix and acceptance test

`hot_root_tree` must replace a fixed shallow fallback only after measured
fallback work can pay for the root's current backfill cost.  A root which is
cheap or rarely queried must stay on `mask8`; a broad repeatedly queried root
must deterministically promote.  Admission must be reconsidered as evidence
accumulates instead of being decided by a problem-specific fixed prefix.

Tree budget exhaustion must be isolated per root.  The current implementation
can make global `tree_complete` false when a later occurrence will not fit,
which disables every already built hot-root tree while retaining its memory.
That is unsafe for long runs.  The affected root should fall back completely,
while unrelated admitted roots remain usable and the global byte budget
remains hard.

The accelerated `compact_long_run_test` reproduces the same shallow-bucket
failure at populations of 1,000, 10,000, and 100,000.  It reports JSON Lines
for mechanical slope checking.  Its initial baseline has `mask8` work/query
equal to the population; the pre-fix hot-root implementation admits after a
fixed amount of fallback work without accounting for construction cost.  The
test also retains a currently failing nonunit same-feature-leaf probe so that
the secondary known selectivity problem remains visible.

Acceptance requires:

1. post-admission work/query proportional to the true answers, not the broad
   root population;
2. reported admission census, deferral, build, and demotion work;
3. construction plus maintenance work amortized by observed fallback savings;
4. a hard tree-memory budget which cannot globally discard useful selectivity;
5. candidate equality and deterministic ordering against `mask8`; and
6. no greater than 25% integrated CPU regression at longer `chat_test` and
   Osborn prefixes, with a plateauing work/query slope as the decisive gate.

### First cost-aware hot-root result

The accelerated probe after implementing cost-aware admission produced:

| Population | Admission query | mask8 steady work/query | hot-root steady work/query | Deferrals | Censuses | Hot bytes |
|---:|---:|---:|---:|---:|---:|---:|
| 1,000 | 8 | 1,000 | 1 | 1 | 2 | 460,429 |
| 10,000 | 8 | 10,000 | 1 | 1 | 2 | 3,496,624 |
| 100,000 | 8 | 100,000 | 1 | 1 | 2 | 33,971,543 |

The default build factor is eight: a root with `N` current occurrences is not
constructed until it has accumulated at least `8*N` fallback work.  The first
census defers construction; the eighth broad query recenses the current root
and admits it.  The reported steady phase begins after admission.  Candidate
IDs and their deterministic order were checked against `mask8` on every
admission and steady query.

This is an accelerated algorithmic result, not yet an integrated acceptance
result.  The 100,000-population process peak was about 163 MB because the
harness deliberately retained the mask index, hot-root index, and every source
`Topform` at once.  It must not be interpreted as expected prover RSS.

A separate focused budget test admits two independent roots, grows one until
the 16 KiB structural budget is exhausted, and verifies that only that root is
demoted.  The other root continues to issue code-tree queries before and after
a forced stale compaction; the demoted root returns the same complete fallback
answer.  This closes the former global-disable failure mode.

## Bounded `chat_test.in` crossover

The frozen candidate (`mask8`) and cost-aware hot-root overlay were run in
parallel on the user-supplied 70,185-hint `bob/chat_test.in`, first to 300 and
then to 1,000 given clauses.  Both comparisons used the same executable and
host.  Search counters and hint matches were identical within each pair.

| Prefix | Strategy | User CPU (s) | Generated | Kept | Hints matched | Back groups | Worst groups | Back bytes | PSS (KiB) |
|---:|:---|---:|---:|---:|---:|---:|---:|---:|---:|
| 300 | mask8 | 19.31 | 120,793 | 5,737 | 123 | 353,095 | 5,183 | 561,237 | 54,405 |
| 300 | hot root | 21.91 | 120,793 | 5,737 | 123 | 243,513 | 2,480 | 813,557 | 54,990 |
| 1,000 | mask8 | 82.39 | 1,268,285 | 33,909 | 362 | 9,323,859 | 19,236 | 3,079,064 | 67,165 |
| 1,000 | hot root | 80.89 | 1,268,285 | 33,909 | 362 | 424,566 | 2,480 | 6,773,496 | 69,048 |

At 300 given clauses, construction had not paid back in wall-clock work: hot
roots reduced posting work by 31.0% but total user CPU was 13.5% higher.  At
1,000 given clauses, hot roots reduced posting work by 95.4%, total user CPU
was 1.8% lower, and measured PSS increased by only 1.8 MiB.  Eight roots were
admitted after 58 cost deferrals and 66 censuses.  No budget exhaustion or
root demotion occurred.

The modest CPU improvement despite the very large posting reduction exposes
the next required measurement.  The hot index visited 24,555,359 code-tree
nodes in 22,044 tree queries at the 1,000-given prefix.  Tree-node traversal is
reported separately but is not yet included in `query_profile.work`.  A
general long-run gate must count posting groups *and* structural nodes; fixed
and variable-rich patterns need separate distributions.  The 1,000-given
result is encouraging, but cannot by itself project the 684,719-active-clause
population in `chat_test.new.out41`.

### Variable-prefix counterexample and adaptive combination

A second adversarial distribution uses clauses whose indexed term is
`f(unique_prefix, unique_deep_suffix)` and queries
`f(x, unique_deep_suffix)`.  There is one true answer.  The shallow fallback
cannot see the suffix, while a prefix discrimination tree must enumerate every
first-argument branch before checking it.  This caught a ground-query
overgeneralization in the first hot-root result.

| Population | mask8 groups/query | hot groups/query | hot tree nodes/query | hot combined | adaptive combined | Adaptive position admissions |
|---:|---:|---:|---:|---:|---:|---:|
| 1,000 | 1,000 | 1 | 1,999 | 2,000 | 1 | 1 |
| 10,000 | 10,000 | 1 | 19,999 | 20,000 | 1 | 1 |

Hot-root trees alone therefore fail this generality gate even though their
posting count looks perfect.  The new `adaptive` strategy maintains both
cost-aware mechanisms: it uses an admitted position posting first, otherwise
an admitted root tree, otherwise the complete `mask8` fallback.  A fanning-out
tree query supplies deterministic work evidence for position probation.  The
position admission floor now uses cumulative work, so repeated sub-4,096-work
queries can eventually pay for construction instead of being ignored forever.

The position probe uses a unique rigid symbol at the deep suffix to isolate the
single-position mechanism.  Correlated suffixes made from a small alphabet can
still require an intersection of multiple admitted positions; that remains an
explicit adversarial case rather than an assumed win.

### `chat_test.in` adaptive result at 1,000 given

The adaptive overlay completed the same 1,000-given trajectory as both prior
runs: 1,268,285 generated, 33,909 kept, and 362 matched hints.

| Strategy | User CPU (s) | Back groups | Tree nodes | Worst groups | Back bytes | PSS (KiB) |
|:---|---:|---:|---:|---:|---:|---:|
| mask8 | 82.39 | 9,323,859 | 0 | 19,236 | 3,079,064 | 67,165 |
| hot root | 80.89 | 424,566 | 24,555,359 | 2,480 | 6,773,496 | 69,048 |
| adaptive | 78.49 | 2,835,834 | 15,249,493 | 2,689 | 6,983,972 | 69,878 |

Adaptive mode admitted eight roots and 28 position features.  Those features
reduced code-tree traversal by 37.9% relative to hot roots alone.  Total user
CPU was 4.7% below `mask8` and 3.0% below hot roots, for about 2.7 MiB more PSS
than `mask8`.  Posting groups and tree nodes are not equal-cost operations, so
they remain separate counters; their slopes and measured CPU are both gates.

This is still a bounded-prefix result.  It does not establish the behavior at
the 684,719-active back index in `chat_test.new.out41`, many competing hot
roots, a filled position/tree budget, or deletion-heavy compaction.  The
frozen candidate remains unchanged until those gates pass.

### Position-budget isolation

The original position mode had the same long-run failure shape as the former
hot-root implementation: when one later record could not fit, it set global
`position_complete` false.  Every useful admitted feature then became
unqueryable while all of its arrays stayed allocated.

Position buckets now carry independent active state.  If incremental growth
would exceed the hard budget, only active features matched by that record are
demoted to the complete path fallback.  Unrelated position features continue
to answer queries.  The next stale compaction copies only active definitions,
so demoted bucket/posting metadata is reclaimed.  Statistics distinguish
active from physical features and report demotions plus the effective budget.

The percentage budget is now a monotone admission ramp within one index
generation.  Its largest previously earned deterministic allowance is kept
across compaction; otherwise shrinking the non-position base during a rebuild
could invalidate a set of features that fit before the rebuild.  The absolute
byte cap remains authoritative.

A focused 16 KiB test admits two independent position features, grows one
until it reaches the hard cap, verifies exactly one demotion and complete
fallback answers, confirms the other feature still uses position retrieval,
and forces compaction.  Physical features then fall from two to the one active
feature without globally disabling the position index.

### Bounded 240-user-second `chat_test.in` comparison

A three-way run requested 2,000 given clauses with a 300-second prover limit.
The external 330-wall-second guard stopped the three concurrent processes
before any reached 2,000; all produced a comparable 240-user-second report.

| Strategy | Given at 240 s | Back groups | Tree nodes | Worst groups | Back bytes | PSS (KiB) |
|:---|---:|---:|---:|---:|---:|---:|
| mask8 | 1,678 | 65,408,330 | 0 | 64,514 | 4,394,315 | 81,096 |
| hot root | 1,671 | 556,500 | 176,787,113 | 7,820 | 11,120,965 | 91,221 |
| adaptive | 1,665 | 37,725,091 | 64,083,061 | 7,820 | 11,494,981 | 89,617 |

At 180 user seconds the given counts were also essentially tied: 1,503,
1,502, and 1,499 respectively.  Thus neither stronger strategy had a stable
integrated CPU win by this prefix.  Hot roots replaced almost all posting work
with a larger number of cheaper tree-node visits.  Adaptive positions removed
about 112.7 million of those tree-node visits, but added about 37.2 million
posting groups.  The relative cost of these operations plus construction and
the rest of preprocessing left `mask8` marginally ahead.

This result supersedes any interpretation of the 1,000-given timings as a
proven crossover.  It does *not* show that `mask8` will remain ahead: the
uploaded 8,800-given trace reaches 6,085 groups per lookup and spends 30.4% of
all user CPU in back lookup, so its slope is still unacceptable.  It shows
that posting-count reduction alone is an invalid proxy for CPU, and that a
stronger index must be selected using measured end-to-end work across longer
prefixes.

An experimental per-feature recheck was also tested and rejected.  It bypassed
position postings once their current size no longer beat the last alternative
measurement by four times.  On the 1,000-given prefix it increased admissions
from 28 to 33 and backfill records from 476,055 to 564,822; total user CPU rose
to 85.77 seconds, worse than both the 82.39-second `mask8` baseline and the
78.49-second earlier adaptive sample.  The implementation was removed rather
than retaining an apparently principled policy which regressed the real case.

### Complementary position intersection

The singleton position policy has another general counterexample: two rigid
deep features can each occur in a large fraction of the archive while their
conjunction is selective.  The adaptive position index now keeps a bounded
record-membership bitmap alongside each admitted compressed posting.  Once
observed work has paid for a second feature, a query scans only the smaller
posting and uses the other bitmap as an exact record-membership prefilter.
The complete term match is still made at an occurrence of the first feature,
so features found under different occurrences in one clause cannot create a
false answer.  The bitmaps are charged to the hard position budget, extended
on insertion, demoted per feature on budget exhaustion, rebuilt by stale
compaction, and freed with their owning feature.

A focused same-shallow-signature family has 128 records.  Each singleton deep
feature selects 32 records and their conjunction selects 8.  After cost-aware
admission, the query performs 32 compressed scans and 32 bitmap probes, then
only 8 occurrence matches.  Candidate IDs and order remain identical through
later insertion, deletion, and forced compaction.  New counters expose
intersection queries, scans, probes, survivors, and allocated bitmap bytes.

This mechanism does **not** make the compressed scan itself smaller than the
smallest singleton posting.  It removes expensive occurrence matching for
nonmembers and is therefore one bounded layer, not a claim that the 5.7
billion-group `out41` slope is solved.  Dense word-wise intersections or a
stronger conjunctive retrieval structure remain candidates if admitted
singleton postings themselves grow too broad.

### Same-host 1,000-given check after intersection support

Fresh sequential runs used the current optimized executable, identical input,
and a 1,000-given cap.  Both reproduced 1,268,285 generated and 33,909 kept
clauses.

| Strategy | User CPU (s) | Back groups | Tree nodes | Intersections | Bitmap probes | Intersection survivors | Peak RSS (KiB) |
|:---|---:|---:|---:|---:|---:|---:|---:|
| `mask8` | 88.54 | 9,323,859 | 0 | 0 | 0 | 0 | 90,408 |
| adaptive | 85.45 | 2,835,834 | 15,249,493 | 722 | 306,685 | 86,479 | 90,268 |

Adaptive mode is 3.5% faster in this same-host pair and retains the exact
search trajectory.  The intersection filter avoids 220,206 full occurrence
checks in the queries which use it, but the aggregate comparison cannot
attribute the whole adaptive difference to this new layer because hot trees
and singleton positions are also active.  In particular, the back-group count
is the same as the earlier adaptive prefix: the current posting-plus-bitmap
algorithm still decodes the smallest posting.  The result is a correctness and
bounded-overhead validation, not a long-run crossover result.
