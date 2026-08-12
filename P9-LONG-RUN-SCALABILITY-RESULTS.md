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
