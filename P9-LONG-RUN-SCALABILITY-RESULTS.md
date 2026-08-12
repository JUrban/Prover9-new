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

| Population | mask8 groups/query | hot groups/query | hot tree nodes/query | hot sibling checks/query | hot combined | adaptive combined | Adaptive position admissions |
|---:|---:|---:|---:|---:|---:|---:|---:|
| 1,000 | 1,000 | 1 | 1,999 | 1,998 | 3,998 | 1 | 1 |
| 10,000 | 10,000 | 1 | 19,999 | 19,998 | 39,998 | 1 | 1 |

Hot-root trees alone therefore fail this generality gate even though their
posting count looks perfect.  These corrected combined figures include the
sibling-edge comparisons which the first version of the longevity test did
not report.  The new `adaptive` strategy maintains both
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

The first implementation did **not** make the compressed scan itself smaller
than the smallest singleton posting.  A second cost-gated path now handles
broad admitted features: it ANDs their dense membership maps word by word and
scans full tokens only for set-bit survivors.  The planner compares the
logical bitmap-word count (not overallocated capacity) with the smallest
posting count and retains compressed scanning for sparse features.  Zero
words short-circuit later bitmap reads.  Query work and statistics charge
every bitmap word actually read, so this path cannot manufacture a posting
count win by hiding its alternative work.

Focused tests cover both choices.  A sparse family continues to decode its
four-record posting.  In a 512-record dense family, the two singleton postings
contain 128 records each; a dense query reads only 17 bitmap words and retains
the same 32 exact candidates.  This is still not a claim that the 5.7
billion-group `out41` slope is solved: the decisive evidence must come from a
much longer adaptive run where postings and bitmaps have reached their mature
cardinalities.

### Same-host 1,000-given check after intersection support

Fresh sequential runs used the current optimized executable, identical input,
and a 1,000-given cap.  Both reproduced 1,268,285 generated and 33,909 kept
clauses.

| Strategy/build | User CPU (s) | Back groups | Tree nodes | Dense intersections | Sparse scans | Bitmap word reads | Peak RSS (KiB) |
|:---|---:|---:|---:|---:|---:|---:|---:|
| `mask8` | 88.54 | 9,323,859 | 0 | 0 | 0 | 0 | 90,408 |
| adaptive, record probes | 85.45 | 2,835,834 | 15,249,493 | 0 | 306,685 | 0 | 90,268 |
| adaptive, cost-gated dense | 83.66 | 2,768,376 | 15,249,493 | 69 | 239,227 | 51,806 | 90,548 |

The current adaptive mode is 5.5% faster than the same-host `mask8` run and
retains the exact search trajectory.  Against the immediately preceding
record-probe build, dense planning replaces 67,458 compressed scans with
51,806 contiguous bitmap-word reads and lowers this one user-CPU sample by
2.1%.  The 69 dense queries are a small fraction of the workload, as intended;
most intersections remain on their cheaper sparse plan.  Peak RSS changes by
less than 0.3 MiB and is dominated by the shared prover state.

These are single bounded samples, not a statistically strong speed claim.
They validate correctness, accounting, and plan selection.  The long-run
crossover and work-slope gate remain open.

### Stable semantic hashing and rigid-child pruning

An exact-trace differential exposed a general reproducibility defect in the
preceding integrated samples.  Prover9 interns option constants in the same
global symbol table as theorem symbols.  Changing only
`passive_selector_store` from `heap` to `file` therefore shifted later raw
symbol numbers.  The proof trajectory and exact candidate stream stayed the
same, but compact path-signature collisions, position-probation collisions,
and adaptive admissions changed.  Consequently the preceding same-host table
is useful historical evidence but is superseded for current CPU comparison.

Compact backward demodulation now caches a deterministic hash of each
symbol's name and arity.  Lossy path signatures, bucket placement, and
adaptive probation use that semantic hash; exact root arrays and equality
checks continue to use the raw symbol number.  Cumulative semantic query-input
and output/work fingerprints are reported in `Compact_back_demod`.  Setting
`P9_COMPACT_BACK_TRACE=1` additionally writes one `CBD_QUERY` event to standard
error per lookup.  A regression changes the irrelevant selector-store option
and requires identical normalized work, admission, and fingerprint reports.
In a forced-file 300-given differential, all 4,292 query events and the entire
compact backward-demodulation statistics line matched exactly; both runs
generated 120,793 clauses and kept 5,737.

A fresh stable-hash 1,000-given pair first showed that replacing broad posting
scans with tree traversal was only a tie:

| Strategy | User CPU (s) | Back groups | Tree nodes | Back bytes | Peak RSS (KiB) |
|:---|---:|---:|---:|---:|---:|
| `mask8` | 85.87 | 10,871,045 | 0 | 3,073,016 | 90,536 |
| adaptive before child pruning | 86.79 | 3,317,482 | 15,190,004 | 7,124,972 | 90,408 |

The tree collector was recursively entering every sibling even when the next
query token was rigid and sibling edge prefixes were ordered and distinct.
It now scans to the matching edge and descends only that subtree.  Sibling
comparisons are separately reported and are included in the synthetic
combined-work gate rather than hidden.  The resulting adaptive run preserved
the exact 1,268,285-generated, 33,909-kept trajectory and took 80.72 user
seconds, 6.0% below the fresh `mask8` sample.  It examined 1,338,073 posting
groups, 12,598,143 tree nodes, and 17,808,810 sibling edges; 15 position
features were admitted.  Back-index storage was 6,984,708 bytes and peak RSS
was 90,540 KiB, essentially unchanged.

Focused exactness, checkpoint/compaction, ASan/UBSan, and semantic-hash
stability tests pass.  This is still only a 1,000-given CPU result.  The large
sibling count shows the next potential long-run risk: an ordered linked
sibling scan is still linear in node fan-out even though it avoids expensive
recursive subtree visits.  A bounded child-dispatch structure should be added
only if mature-prefix counters show that sibling checks become material; the
8,800-given `out41` replacement and multi-million-passive IO gates remain
open.

### Cost-gated rigid-child dispatch

A bounded 300-second continuation confirmed that the sibling risk was already
material before the old 8,800-given endpoint.  At the 240-user-second report,
stable `mask8` had reached 1,570 givens while pruned adaptive mode reached
1,616 (2.9% more).  Adaptive retained the same logical trajectory and reduced
69.3 million posting groups to 15.1 million, but had already performed 60.9
million tree-node visits and 87.0 million sibling comparisons.  Peak RSS was
96.9 MiB versus 90.5 MiB.  Thus recursive sibling pruning improved current
CPU, but an ordered linked child list remained an independent long-run slope.

The tree now has a bounded positive child-dispatch cache.  It is deliberately
not an authoritative directory: a miss or direct-mapped collision scans the
original ordered siblings, so completeness and candidate order do not depend
on cache retention.  A parent is enabled only after one real rigid lookup has
scanned at least eight children.  Narrow parents therefore retain their cheap
list lookup, variable queries still traverse every semantically possible
branch, and only observed broad rigid parents pay for hashing.  Cache growth is
geometric, is included in the existing tree budget, is limited to the smaller
of 8 MiB and one eighth of that budget, and is rebuilt rather than persisted
through stale compaction.

Two simultaneous 1,000-given A/B pairs used the exact same executable and
swapped CPU affinities.  Both retained 1,268,285 generated and 33,909 kept
clauses, 12,598,143 tree-node visits, and identical candidates:

| Pair | Selective dispatch CPU (s) | List-only CPU (s) | Change |
|---:|---:|---:|---:|
| 1 | 106.80 | 108.72 | -1.8% |
| 2, affinities swapped | 103.17 | 105.45 | -2.2% |

The cache enabled 720 parents, made 152,293 lookups with 136,410 hits (89.6%),
and replaced 846,941 sibling comparisons.  It used 402,784 bytes and changed
peak RSS by at most 128 KiB in these samples.  An initially tested policy
which hashed every rigid parent was rejected: it made 2.14 million probes to
save only 2.30 million sibling checks and had no defensible integrated CPU
benefit.

A new broad-fanout longevity gate places 10,000 distinct rigid symbols below
one parent.  Its cold lookup scans 10,000 siblings once; every subsequent
lookup uses two direct hits and zero sibling scans with a 100,808-byte cache.
At 100,000 records the corresponding values are 100,000 cold scans, two hits
and zero scans per hot query, and 811,440 cache bytes inside a 7.42 MiB tree.
The variable-prefix counterexample remains intentionally unaccelerated by this
cache and continues to require the adaptive position index.  Forced collision,
growth, deletion/compaction, exactness, checkpoint, full audit, and
ASan/UBSan tests pass.  The mature `out41` replacement remains the promotion
gate; these results remove one demonstrated local linear factor, not the late
inference/preprocessing burst.

## Selective structural retrieval for nonunit back subsumption

The accelerated `same_feature_leaf` gate formerly retained 10,000 numerical
feature matches and rejected 9,999 of them only after walking the posting.
This was not the cause of `out41` (the integrated nonunit component used only
about 3.3 seconds there), but it was a real singleton-overfitting defect.

The structural summaries now have 96 bit-sliced membership maps: 64 rigid
facts and 32 repeated-position facts.  A back-subsumption query chooses its
rarest required fact and uses that map only when a conservative estimate—one
logical bitmap pass plus its current live cardinality—is at least four times
smaller than both the active index and the compatible numerical-feature
population.  Broad/no-fact queries retain radix traversal.  Candidates are
checked against all remaining necessary facts and numerical features, then
sorted back into the exact radix-leaf order (ascending feature vectors and
newest-first within an identical leaf) before the caller's authoritative
subsumption test.

At a 10,000-record identical numerical leaf with one structural match, counted
work falls from 10,001 to 159: 157 bitmap words, one surviving record, and
minimal plan traversal.  Total index storage grows from 857,872 to 1,056,832
bytes in that harness, about 19.9 bytes per record including allocation
rounding and the new parent/live-count fields.  The direct bitmap payload is
12 bytes per physical record.  Focused tests preserve mixed-leaf and
newest-first order before and after forced compaction.  ASan/UBSan reports no
invalid access; the existing LADR parser/symbol lifetime still appears as
process-exit leaks when leak detection is enabled.

Forward subsumption has the reverse subset relation: a valid stored subsumer
may have no rigid fact at all, so selecting one required query bit is not
complete.  Its existing path remains measured separately; adding a bounded
complement/forbidden-fact plan requires its own cost gate rather than reusing
the back-subsumption rule.

A 300-given integrated `chat_test.in` smoke run retained the established
trajectory (120,793 generated and 5,737 kept).  The 635-active nonunit index
used the structural path for 252 back queries; 250 had an empty required-fact
map, and the remaining work totaled 337 bitmap-word reads and two records.
The complete compact nonunit index used 231,232 bytes, including 12,288 bytes
of structural maps.  This validates the empty-result path and reporting in the
real prover, while the synthetic identical-leaf family remains the relevant
scale gate.

## File-backed dense passive directory

`assign(passive_directory,file).` moves the fixed 64-byte dense selection
record array from anonymous memory to a separate private temporary file.  The
file is created in `TMPDIR` (or `/tmp`), unlinked immediately, and therefore
disappears automatically when the process closes or is killed.  It is distinct
from both the ancestor archive and the compressed passive-body file.

The mapping retains a 64 MiB hot tail.  At each additional 64 MiB of logical
growth, page-aligned cold extents are synchronously committed, discarded from
the mapping, and advised `DONTNEED` at the file-descriptor level.  Reports
separate logical/file capacity from anonymous `record_bytes` and count every
eviction, byte, and failure.  Age selectors compare monotone physical indexes
directly, so their heap operations do not fault directory pages merely to read
proof IDs.  Weight and hint-age selectors still access their keys through the
mapping in the default heap mode.  The optional external selector mode below
removes that dependency and bounds their resident queues.

The accelerated scale probe inserted 2.2 million active age-selected records:

| Logical directory | Allocated file | Heap | PSS | Anonymous | Evicted |
|---:|---:|---:|---:|---:|---:|
| 140.8 MB | 155.0 MB | 9.69 MB | 45.97 MiB | 8.70 MiB | 64 MiB |

Peak RSS was 47.36 MiB and the exact first-by-age selection was preserved.  A
separate forced-compaction test rebuilt 900 live records into a new private
mapping and preserved cursor restoration, payload totals, semantics, and all
remaining selections.  The normal checkpoint selector test remains green.

The 300-given real `chat_test.in` file-directory smoke also preserves 120,793
generated and 5,737 kept clauses.  At that point 5,737 physical records used a
367,168-byte logical directory, with zero anonymous directory bytes reported.

At the 65.9-million-passive scale seen in the uploaded nine-hour clauses run,
this stage moves roughly 4.22 GB of fixed directory records out of anonymous
RAM.  It does **not** yet bound selector heaps: extrapolating the measured
`out41` heap density still leaves roughly 0.42 GB at 65.9 million passives.
Thus the directory alone is a material RAM step and a useful isolation
boundary, not completion of the external passive control plane.

## File-backed dense selector runs

`assign(passive_selector_store,file).` replaces every growing in-memory
selector heap with a bounded insertion heap and immutable sorted temporary-file
runs.  `assign(passive_selector_buffer,65536).` controls the entry cap of each
selector's insertion heap.  Each 24-byte run entry stores only the selector's
one necessary key, the stable clause ID tie breaker, and its current physical
record index.  One run is retained at each binary level; a full buffer is
sorted and carried through occupied levels by sequential merge.  Selection
compares the insertion-heap root with the head of each live run, reads runs in
256-entry sequential blocks, and validates activity lazily against the dense
directory.  Directory compaction closes every old run and deterministically
rebuilds queues from active records, so disk and stale entries remain bounded
by the same live/stale policy as the directory.

Age, weight, and hint-age comparisons reproduce the existing heap's exact
tie-breaking.  A differential test combines all three orders and selector
ratios, a deactivate/reactivate cycle, full queue draining, and a mid-run dense
compaction.  Its file schedule is identical to the heap schedule for all 3,000
givens.  The focused compaction test forces 64-entry buffers and preserves all
remaining age selections through run merges and rebuild.  Both tests, plus a
200,000-record scale test, pass ASan/UBSan with leak detection disabled (the
repository has pre-existing process-lifetime parser/symbol allocations).
The full compact-OTTER checkpoint differential now uses a file directory and
1,024-entry file selectors; checkpoint boundaries zero and two reproduce the
uninterrupted candidate/given traces, final search counters, and proof.

With a 65,536-entry buffer, the 2.2-million-age-record probe now reports:

| Directory logical | Selector resident | Selector run logical | PSS | Anonymous | Selector bytes written |
|---:|---:|---:|---:|---:|---:|
| 140.8 MB | 1.50 MiB | 49.50 MiB | 38.62 MiB | 2.46 MiB | 289.5 MiB |

There were two current runs, 40,993 batched writes, and the first selected ID
was still one.  Compared with the preceding directory-only probe, anonymous
memory fell from about 8.70 MiB to 2.46 MiB; the selector runs introduce real
write amplification and are therefore opt-in rather than the default.

Insertion buffers now grow geometrically on demand up to the configured hard
cap rather than reserving the whole cap on first use.  At the 1,000-given chat
prefix this reduced file-selector resident capacity from 7.50 MiB to 1.17 MiB
while retaining the same 37,130 buffered logical entries.  It deliberately
remains larger than the 0.20 MiB compact index heap at this small population;
the file mode is intended for queues which grow far beyond the fixed cap.

An integrated 300-given `chat_test.in` differential used a deliberately small
1,024-entry buffer to force six flushes and three merges.  All 300 printed
given clauses and all 120,793 candidate trace outcomes were byte-identical to
the heap control; both retained 5,737 kept clauses.  The final selector state
held 6,138 entries in three runs and 654 buffered entries using 126 KiB of
resident selector buffers.  Single timing samples (18.5 versus 21.6 user
seconds) reversed earlier repetitions and are treated as noise, not as a speed
claim.  The required 1,000-given and multi-million-passive IO/CPU gates remain
open.

A subsequent default-65,536-buffer pair reached the same 1,000-given
trajectory (1,268,285 generated and 33,909 kept).  Concurrent samples were
88.29 user seconds for heap and 89.56 for file; the demand-grown file rerun was
86.83 seconds.  The spread is too small and unstable for a speed claim, but it
shows no material CPU regression at this prefix.  No individual selector had
yet filled 65,536 entries, so this closes the resident-buffer and queue-policy
gate, not the mature-run IO gate.

At the observed 65.9-million-passive scale, file directory plus file selectors
remove the roughly 4.22 GB directory and approximately 0.42 GB selector heaps
from anonymous RAM.  This does not reduce compact inference-index memory or
the OS-accounted file cache; those remain separate totals in the 80--90% RAM
acceptance calculation.
