# Josef 02 compact-P9 CPU investigation

Status: implemented and bounded-validated on branch `josef02-cpu` at
`b143cc3`.  The current compact prover is already 3.6--5.4 times faster than
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
| release binary at `b143cc3` | `a8166bd9caf92dcb38f7c291ca9d33eb3099bb612512273540c24141533c7ba7` |

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
| 600 | 179.20 s | 49.92 s | 3.59x | 305,408 KiB | 206,940 KiB |

Both comparisons preserve the exact generated/kept state; the 600 endpoint is
`(601, 524799, 26671, 0)`.  These results establish current prefix
competitiveness, not the missing full old-P9 proof time.

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

## Cross-workload gates

- CHAT at 600 givens reaches the known exact
  `(601, 497430, 16974, 0)` endpoint and 3,775 back-demod candidates.  The
  conjunction table is admitted for this different hint population, so its
  468,100 KiB peak RSS is expected and is not Josef 02 back-index growth.
- Josef 01 at 1,000 givens exactly reproduces
  `(1001, 1628048, 320239, 0)` and every compact unit-index counter.  Current
  user CPU is 57.27 s versus 57.42 s in the prior bounded output.  That input
  clears back demodulation, independently exercising the shared unit/rewrite
  changes.  Its roughly 64 MiB PSS increase is the inherited bounded slab
  recycler; no swapping occurred.
- `compact_back_demod_test`, `compact_long_run_test`,
  `compact_rewrite_test`, `compact_unit_index_test` and
  `compact_otter_audit_test` pass.

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

A cautious planning range for the next same-machine total is **7,500--8,400
CPU seconds** (about 5--15% below the compact baseline), with roughly
5.0--5.2 GiB process PSS.  This is deliberately a range, not a measured
claim.  The back-index optimization alone cannot be extrapolated linearly
from the 1,500-given prefix, and no full old-P9 Josef 02 time exists.

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
