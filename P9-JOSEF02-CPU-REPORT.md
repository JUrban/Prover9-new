# Josef 02 compact-P9 CPU investigation

Status: implemented and bounded-validated on branch `josef02-cpu` through
`92009ad`.  The current compact prover is already 3.9--5.4 times faster than
the preserved old-P9 binary on exact 300/600-given Josef 02 prefixes.  A full
old-P9 Josef 02 proof output was not supplied, so this report does not claim a
measured proof-to-proof old/new CPU ratio.  The existing compact proof is the
full-run baseline; the next full run is an external acceptance test, not
something attempted on the low-RAM development machine.

## Reproducible inputs and binaries

| artifact | SHA-256 |
|---|---|
| reconstructed uninterrupted Josef 02 input | `8961d86efd2163010117cbba0fd9c6085d3d71751f2d6f2675406f84c57634ba` |
| completed compact output, `/project/bob/Josef_02.out.new3` | `eaf22c2bda54fbe5eb0d487aca91f4ad27dabe1cb656111f97b74fc9930a1d29` |
| preserved old-P9 binary | `bcdf6bafbf608fde463fd43ef541891813f5c49a2d5153711c54925e98d76bcc` |
| accepted parent binary at `10b6abd` | `d13973d3311ba8e31590f139e48f6560ec24af845fc60acb4b2e342dfd001ddc` |
| accepted rewrite parent at `3846b92` | `25fd1849ea8ac898dd45af9ce96397f61465cfdfb7ab8baacda2b1803010bc73` |
| release binary at `92009ad` | `414f6b14e7350da94db7923b3b0d2f3528f2c52025f035a47b21d936c8a223c1` |

The reconstructed input is
`josef02-debug.heNOIJ/Josef_02-uninterrupted.in`.  It retains every formula,
hint, ordering declaration and inference option from the supplied material;
only the experiment-level compact configuration and bounded limits were made
explicit.

## What the completed run says

The full compact baseline proved Josef 02 at:

| measurement | value |
|---|---:|
| Given / generated / kept | 13,006 / 129,776,312 / 9,226,457 |
| user / system / total CPU | 7,972.54 / 868.76 / 8,841.30 s |
| wall clock | 8,873 s |
| final PSS | 5,001,957 KiB |
| rewrite attempts / rewrites | 3,677,581,851 / 513,972,002 |
| rewrite target nodes | 25,306,441,784 |
| back-demod queries / sampled lookup | 7,440,943 / 957.668 s |
| back mask word checks | 27,753,331,661 |
| compact back-index bytes | 2,722,576,238 |
| allocator calls / object traffic | 16.7 billion / 415.77 GB |

The main clocks were preprocessing 3,739.43 s, demodulation 2,238.59 s,
disable 2,246.41 s, back demodulation 1,022.42 s, indexing 542.61 s, inference
406.87 s, hints 309.07 s and subsumption 284.26 s.  This is not primarily a
clause-selection problem: forward rewriting, clause retirement and the mature
back-demodulation index dominate.

The supplied archive contains no complete old-P9 Josef 02 output.  In
particular, the other Josef 02 files are compact crash/replay artifacts and
must not be labelled an old-P9 baseline.

## Direct old-P9 comparison on bounded prefixes

`test.src/chat_test_matrix.sh` ran one Prover9 process at a time with the same
input and exact endpoint.  The matrix strips compact-only commands before
feeding the preserved old binary.

| max given | old-P9 user | compact user | speedup | old RSS | compact RSS |
|---:|---:|---:|---:|---:|---:|
| 300 | 90.88 s | 16.85 s | 5.39x | 271,744 KiB | 179,744 KiB |
| 600 | 137.36 s | 35.08 s mean | 3.92x | 305,408 KiB | 206,482 KiB mean |

Both comparisons preserve the exact generated/kept state; the 600 endpoint is
`(601, 524799, 26671, 0)`.  The 600 old-P9 number was rerun on the current host
after `3846b92`; the compact number is the mean of two reversed candidate
runs.  The current compact binary is therefore 3.92 times faster and uses
about 32% less RSS at that exact endpoint.  These results establish current
prefix competitiveness, not the missing full old-P9 proof time.

## Accepted changes

### Correct deep-cache initialization (`e2bcb67`)

`compact_rewrite_deep_cache_kb` used to be applied after construction of the
compact rewrite bank.  Inputs echoed a nonzero value while the live bank
silently kept a zero-byte budget.  The option is now applied before bank
construction and the audit checks the requested byte count.  Josef 02 still
recommends zero: this commit makes experiments truthful but does not enable
the cache.

### O(1) compact population reads (`7965f66`, `6c472ca`)

Term-pool lifecycle polling needed only active/physical populations but called
complete back-demodulation statistics, which scan all position features.  A
600-given profile had already made 10,367 such calls; the complete run has
262,803 position features.  Narrow accessors now read the maintained counters
in constant time.

The same treatment was applied to compact rewrite availability and
unit/rewrite physical counts.  At 600 givens this removes roughly 524,846
complete rewrite-stat snapshots and 10,365 unit/rewrite lifecycle snapshots.
The full proof generated 129.8 million clauses and disabled 4.1 million, so
this is a long-run scaling fix even though two reversed 1,000-given timing
pairs were neutral (87.44 versus 87.49 mean user seconds for the first scalar
change).

### Skip mask censuses that cannot affect routing (`b143cc3`)

An eager exact-position query formerly traversed the complete mask directory
before using the position index.  Therefore the full run's 3.48 million
position choices still contributed to all 7.27 million mask queries and 27.75
billion word checks.

The exact `(root, required-mask)` bucket is one member of the compatible-mask
superset.  Its posting count is an O(1) lower bound.  If a complete position
posting is already no larger than that bucket, the full census cannot change
the existing decision and is skipped; otherwise the original census and
comparison run.  Route choice and answer order are unchanged by construction.

| Josef 02 gate | control | candidate | directory effect |
|---|---:|---:|---|
| 1,000 given user | 84.16 s | 88.92 s | 6,772 censuses and 1.14M word checks avoided; short-prefix regression |
| 1,500 given user | 205.79 s | 203.64 s | 24,316 censuses, 8.56M word checks and 2.44M bucket visits avoided |

At 1,500 givens both runs end at
`(1501, 3039735, 176392, 0)` with identical back-demod answer/output
fingerprints.  Candidate and control use about 300 MiB RSS.  The sampled
lookup estimate is 6.229 versus 5.960 s, so the result is a first total-CPU
crossover plus a deterministic work reduction, not proof of a uniform win at
every prefix.

### Snapshot shared-term addressing once per clause (`10b6abd`)

Every rigid-subterm rewrite attempt refreshed the shared compact term pool's
token and logical-address bases through two out-of-line accessors.  The pool
can move while clauses and rules are serialized, but compact normalization
does not mutate it while one clause is being rewritten.  The safe
invalidation boundary is therefore the clause, not every subterm.

The completed baseline made 3,677,581,851 rewrite attempts but processed only
130,519,375 subject atoms.  Moving the two refreshes to clause entry therefore
eliminates at least 7.09 billion redundant accessor calls on that trajectory;
there are no new allocations or indexes.

| gate | parent user | candidate user | CPU change | parent/candidate RSS |
|---|---:|---:|---:|---:|
| Josef 02, 600 given, two reversed pairs | 40.03 s mean | 39.63 s mean | -1.0% | 206.3 / 206.9 MiB mean |
| Josef 02, 1,000 given, adjacent | 91.21 s | 86.42 s | -5.25% | 230,576 / 230,848 KiB |
| CHAT, 600 given, adjacent | 55.27 s | 52.06 s | -5.81% | 467,748 / 467,464 KiB |
| Josef 01, 1,000 given, reverse adjacent | 60.77 s | 60.56 s | -0.35% | 620,808 / 617,848 KiB |

The Josef 02 comparisons preserve `(601, 524799, 26671, 0)` and
`(1001, 1310234, 65416, 0)`, respectively, with identical rewrite counters
and back-demod input, output and answer fingerprints.  CHAT likewise preserves
`(601, 497430, 16974, 0)` and all three fingerprints.  Josef 01 deliberately
clears back demodulation and records zero compact rewrite attempts, so its
neutral reverse pair is the expected independent no-regression result.  An
earlier Josef 01 candidate observation of 68.10 s did not repeat after the
adjacent 60.77 s control and is retained as host-load noise, not discarded
from the interpretation.

### Carry recursive rewrite state through one query context (`3846b92`)

The radix retrieval function formerly passed eleven arguments at every
recursive edge.  On x86-64, five invariant arguments were rebuilt on the
stack for each call.  A clause-local query context now owns the same target,
end pointer, bindings, binding trail, ordering flag and result; recursion
passes only the context pointer, node and current subject position.  Binding
creation, undo order, radix traversal and first-success order are unchanged.

This is a representation-only hot-path change: it adds no persistent memory,
cache or tuning option.  In the release object, `retrieve_rec` shrinks from
3,517 to 3,219 bytes (8.5%) and `find_rewrite` from 807 to 677 bytes (16.1%).

| gate | parent user | candidate user | CPU change | parent/candidate RSS |
|---|---:|---:|---:|---:|
| Josef 02, 600 given, two reversed pairs | 39.81 s mean | 35.08 s mean | -11.87% | 206,446 / 206,482 KiB mean |
| Josef 02, 1,000 given, adjacent | 79.55 s | 74.58 s | -6.25% | 230,848 / 230,908 KiB |
| CHAT, 600 given, two reversed pairs | 48.59 s mean | 48.87 s mean | +0.58% | 467,422 / 467,502 KiB mean |

Both Josef endpoints and CHAT preserve generated/kept counts, rewrite
attempts, and the back-demod input, output and answer fingerprints.  CHAT is
properly classified as neutral: its 45--52 s host spread is much larger than
the 0.28 s mean difference.  The accepted result is therefore a strong Josef
02 improvement without a demonstrated cross-workload regression, not a claim
that every rewrite population benefits equally.

### Batch packed-hint intersection counters (`92009ad`)

An isolated `gprof` build of the accepted rewrite code reached the exact
600-given endpoint in 66.96 profiled user seconds.  Its leading self-time was
recursive rewrite retrieval at 13.4%, followed by packed dense hint
intersection at 7.0% and slab allocation at 4.4%.  Dense hint intersection
had already visited 7.84 million summary words and 64.13 million data words.

The dense and sparse loops formerly updated global diagnostic counters for
every visited word, posting candidate, feature test, rejection and result.
Those counters are observational: candidate admission and order do not read
them.  They are now accumulated locally and published once per query.  At the
exact 1,000-given Josef endpoint this batches 20,009,856 summary-word,
161,660,929 data-word, 38,765,984 result and the associated posting-candidate
increments.  The compiled `fast_dense_collect_candidates` body is 9.5%
smaller and no persistent memory is added.

| gate | parent user | candidate user | CPU change | result |
|---|---:|---:|---:|---|
| Josef 02, 600 given, two reversed pairs | 44.30 s mean | 42.43 s mean | -4.21% | exact |
| Josef 02, 1,000 given, adjacent | 91.95 s | 91.82 s | -0.14% | exact/neutral |
| CHAT, 600 given, adjacent | 52.46 s | 52.29 s | -0.32% | exact/neutral |

All final packed-dense counters, packed-hint operation counters, generated and
kept counts, and back-demod input/output/answer fingerprints match.  Periodic
reports can occur at different given counts when host speed differs, so only
the final cumulative reports are equality authorities.

A second isolated profile of the accepted `92009ad` binary, stored in
`josef02-gprof-hint-600/`, reproduced the exact 600-given endpoint and every
final packed-dense counter.  Host load made its absolute profiled user time
84.46 s, so percentages rather than elapsed time are the useful comparison:
`retrieve_rec` accounted for 14.93% of samples, the batched dense intersection
6.13%, and `slab_get` 5.14% across 165,390,617 calls.  The earlier profile had
dense intersection at 7.00%; this confirms the intended local work reduction
without pretending that two separate `gprof` runs are a controlled timing
pair.

## Rejected options and experiments

- Raising `hint_conjunction_kb` to 384 MiB is rejected.  Rewritten hints grew
  the actual table beyond that cap, disabled it after only 23 queries, and a
  300-given run rose from 16.85 s / 179,744 KiB to 29.77 s / 648,152 KiB.
  Keep the default 320 MiB cap; Josef 02 correctly rejects the all-or-nothing
  table by a few kilobytes.
- A low-threshold deep rewrite-child cache reduced sibling-check counters but
  made the 1,000-given run 10.7% slower.  Keep
  `compact_rewrite_deep_cache_kb=0`.
- Direct clause serialization removed a measured traversal but was neutral in
  reversed 600-given pairs (45.27 control versus 45.45 candidate mean).  Most
  Josef 02 clauses are positive units, so the expected OR/NOT wrapper saving
  was absent.  The experiment was reverted.
- `mask32` is not a CPU replacement for `adaptive32`.  At the exact 1,000
  endpoint it needs 90.65 versus 77.96 user seconds and examines 4.883M versus
  2.281M back-index work units.  It saves about 22 MiB at this prefix, but the
  CPU cost is already 16%.
- Enabling the rigid-edge side index is not justified by the existing CHAT
  and Josef profiles.  Leave `compact_back_edge_filter` clear.
- A bounded generation-aware cache of compatible mask buckets reduced the
  sampled 1,000-given back lookup from 2.542 to 2.069 s, but increased total
  user CPU from 90.47 to 99.26 s and RSS from 228,860 to 239,624 KiB.  Its
  8 MiB directory churn merely moved work into refresh/preprocessing.  The
  experiment was fully reverted; do not trade whole-run CPU for an isolated
  lookup counter.
- Splitting out a zero-deep-cache recursive matcher shrank the common
  `retrieve_rec` body from 3,219 to 1,739 bytes.  Two noisy reversed 600-given
  pairs averaged 39.05 s candidate versus 40.70 s control, but the decisive
  adjacent 1,000-given pair regressed from 79.61 to 83.97 s (+5.5%) with no
  memory benefit.  The extra dispatch and duplicated inlining outweighed the
  smaller recursive body at the longer prefix.  The specialization was fully
  reverted and the accepted release binary hash restored exactly.
- Dispatching a terminal radix child directly to leaf processing avoided a
  redundant recursive end test in principle, but duplicated enough leaf code
  into the inlined child wrapper to worsen two reversed 600-given pairs from
  39.29 to 41.36 s mean (+5.3%).  It was fully reverted.
- Starting dense hint intersection from the posting with the smallest sparse
  reference count was also rejected.  Sparse count did not predict dense
  summary selectivity and the reorder disturbed the cache-friendly feature
  order: two reversed 600-given pairs regressed from 36.62 to 41.33 s mean
  (+12.9%).  Candidate sets and counters were exact, and the code was fully
  reverted.
- Replacing KBO's transient variable-multiset lists with a stack table removed
  the main source of the allocator profile: 51.60 million `multiset_add`
  allocations by given 600.  The first implementation averaged 44.13 s versus
  45.02 s control across reversed 600-given pairs, only a noisy 2.0% apparent
  gain whose individual pairs disagreed.  A direct-indexed refinement then
  took 44.98 s after a 41.69 s adjacent control (+7.9%).  Linear lookup or
  clearing a 100-entry table merely replaced cheap recycled-slab work with
  other hot-loop work.  Both forms were fully reverted; the release binary
  hash returned exactly to the accepted value.
- Turning deterministic rigid radix descent into a loop reduced the compiled
  `retrieve_rec` body from 3,219 to 2,716 bytes, but the changed live-state and
  undo behavior cost total CPU.  It took 42.34 versus 41.59 user seconds at
  600 givens (+1.8%) and 97.62 versus 94.41 at 1,000 (+3.4%).  Both endpoints,
  all hint counters and RSS were unchanged.  The experiment was fully
  reverted; recursive rigid descent remains faster on the tested compiler and
  host.

## Cross-workload gates

- CHAT at 600 givens reaches the known exact
  `(601, 497430, 16974, 0)` endpoint and 3,775 back-demod candidates.  The
  conjunction table is admitted for this different hint population, so its
  467,464 KiB peak RSS is expected and is not Josef 02 back-index growth.
  The clause-level address snapshot was 5.81% faster than its adjacent parent;
  the subsequent query-context refactor is neutral in two reversed pairs.
  Packed-hint counter batching is also neutral at 52.29 versus 52.46 s.
  All preserve the rewrite and back-demod fingerprints.
- Josef 01 at 1,000 givens exactly reproduces
  `(1001, 1628048, 320239, 0)` and every compact unit-index counter.  Current
  host observations span 59.85--70.30 s; the controlled reverse-adjacent gate
  was 60.56 s versus 60.77 s for the parent.  That input clears back
  demodulation and records zero compact rewrite attempts.  It also records
  zero packed-dense queries, so it is an independent trajectory gate rather
  than timing evidence for the counter batching.  No bounded validation
  process swapped.
- `compact_back_demod_test`, `compact_long_run_test`,
  `compact_rewrite_test`, `compact_unit_index_test` and
  `compact_otter_audit_test` pass.  The hint-postings, hint-preview and
  compressed-unit-match tests also pass after the packed-counter change.

## Recommended full-run options

Use the following block unchanged for the external Josef 02 rerun:

```prolog
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

assign(compact_rewrite_deep_cache_kb,0).
assign(compact_passive_cache,0).
assign(compact_index_stale_pct,25).
assign(compact_term_reclaim_kb,8192).
```

Retain the original inference, ordering, weight and hint commands after this
block.  For an unlimited run remove/replace only bounded `max_given`,
`max_seconds` and `max_megs` commands.  Do not raise the hint-conjunction
budget or enable the deep rewrite cache for the first authority run.

## Expected full result and acceptance gate

The completed compact baseline is 8,841 total CPU seconds.  The new scalar
accessors remove costs whose old complexity grows with both lifecycle events
and index width; the mask lower-bound optimization has only just crossed over
at 1,500 givens but targets a directory sixteen times wider at the proof.
Inherited slab recycling and direct symbol/term hot-path work also postdate the
baseline output.

A cautious planning range for the next same-machine total is **7,000--8,000
CPU seconds** (about 10--21% below the compact baseline), with roughly
5.0--5.2 GiB process PSS.  This is deliberately a range, not a measured
claim.  A simple per-attempt extrapolation of only the clause-snapshot delta
from the reversed 600-given mean and the adjacent 1,000-given pair spans about
120--530 user seconds at the proof's 3.678 billion attempts.  That range is
useful for planning but too load-sensitive to add mechanically to the other
unmeasured long-run changes.  The later query-context refactor removes another
6.25--11.87% of bounded Josef 02 user CPU, but the different mature rule and
subject mix makes that percentage equally unsafe to apply directly to the
full baseline.  The back-index optimization likewise cannot be extrapolated
linearly from the 1,500-given prefix, and no full old-P9 Josef 02 time exists.

Accept the external run only if it:

1. proves at the exact 13,006-given trajectory, or explains any trajectory
   difference with normalized given-clause and answer fingerprints;
2. reports `Generated=129776312`, `Kept=9226457`, one proof, and the expected
   back-demod answer fingerprint when replay is exact;
3. shows no swap and records GNU-time RSS, final PSS, filesystem I/O and the
   file-backed directory sizes separately;
4. improves total CPU on 8,841.30 s without an unexplained increase over the
   approximately 5 GiB compact PSS baseline; and
5. retains the full `Compact_rewrite`, `Compact_back_demod`, route, query
   profile, hint and allocator statistics for the next scaling audit.

Until that run exists, the precise conclusion is: current compact P9 is
decisively faster than old P9 on exact Josef 02 prefixes, and the identified
long-run accidental work is removed, but full proof-to-proof old/new CPU
competitiveness remains unmeasured.
