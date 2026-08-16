# Mature backward-index CPU recovery: implementation and bounded results

Status: implemented and locally validated on branch
`mature-back-index-cpu`.  After the packed back-hint fixes, two reversed-core
1,500-given CHAT pairs average 10.7% faster than normal P9 while compact uses
64.5% less peak RSS.  Two reversed-core 2,000-given pairs average 6.8% faster
while using 63.7% less peak RSS, reversing the pre-fix 8.7% endpoint
regression.  The external 7,365-given run and proof endpoint remain open;
these bounded runs are not a claim that a week-long run has already been
reproduced.

## August 16 packed-hint admission and CPU update

Commit `c62994a` replaces repeated posting-list intersections for eligible
`packed_fast` match queries with a same-sign conjunction table.  A hint whose
complete shallow profile has at most nine keys is stored under every nonempty
subset of that profile.  The query performs one lookup for its complete key
set, and fixed bit planes reject incompatible literal counts and 64-bit
structural summaries before any stable hint ID is read.  Hash collisions can
only admit an extra candidate; activity, exact subsumption, hint ordering, and
the decreasing stable-ID rule remain authoritative.

The first implementation exposed why a general admission policy is necessary.
Josef's 153,681-hint population used 314,579,336 bytes and reduced a
1,000-given control from about 111.2 to 77.7 total CPU seconds.  The 310,153
Osborn hints required about 542 MiB; constructing that table raised a
100-given control from 27.1 seconds and 295 MiB RSS to 63.6 seconds and
1.36 GiB RSS because the table displaced demodulation's working set.  A
prefix extrapolation was rejected after it misclassified Josef: the first
16,384 hints were not representative of the full file.

Commit `bd6c7c9` therefore plans the complete population before committing the
large sidecars.  Initial indexing retains compact sign profiles.  A temporary
16-byte-per-key count table then computes the exact final hash-table capacity,
per-posting allocation capacities, and 64-candidate mask blocks.  The counter
is released before real construction.  If the exact layout exceeds
`hint_conjunction_kb`, the conjunction table is never constructed and the run
uses the established complete dense/sparse matcher.  A partial table is never
queried, and the same hard cap remains in force after hint rewrites and index
rebuilds.

The default is 327,680 KiB (320 MiB).  Set the option to zero to force the
dense/sparse path, or raise it only when the process has a deliberately larger
resident allowance:

```text
assign(hint_index,packed_fast).
assign(hint_conjunction_kb,327680).
```

`Packed_fast_conjunction` reports the budget, accepted/rejected state, exact
estimated bytes, actual peak bytes, planned profiles, scanned profiles, and
budget denials.  The following bounded controls preserve their exact search
trajectories:

| workload | decision | current total CPU | comparison | peak RSS |
|---|---:|---:|---:|---:|
| Osborn, 100 given | reject at 335,545,032 bytes | 25.0 s | 27.1 s before conjunction; 63.6 s unbounded | 295,896 KiB |
| Josef, 1,000 given | accept 314,579,336 bytes | 78.8 s | 111.2 s dense; 77.7 s direct unplanned build | 549,332 KiB |
| CHAT, 600 given | accept 108,087,264 bytes | 21.6 s | 23.0 s immediately before population planning | 143,064 KiB |

These results establish safe per-population selection and a mature-prefix
crossover, not proof-endpoint competitiveness.  The complete 11,369-given
CHAT rerun remains the CPU product gate.

Commit `ddef6c2` also batches append-only ancestor records in a bounded 1 MiB
write buffer: the 2,000-given Josef control issued 121 physical writes for
739,000 logical records.  Commit `d00c6c3` and `3d056be` prune incompatible
unit code-tree siblings, including the sign-root sentinel.  Finally,
`4706283` changes `compact_passive_cache`'s default to zero.  The mature cache
controls found no CPU benefit and higher RSS, so the single-owner archive path
is now both the recommended configuration and the safe default; explicit
nonzero cache experiments remain available.

## August 16 short-prefix CPU parity

The first current-code Osborn trajectory checks still had a real startup
regression: at 510 givens compact needed about 93 user seconds versus 68 for
the legacy-index control, and at 600 it needed about 130 versus 87.  This was
not a search divergence: every given clause and the final
`(given, generated, kept, proofs)` tuple were identical.  A production CHAT
profile and retained Osborn outputs isolated four independent causes.

Commit `be674a2` collapses the compact rewrite radix hot path.  At CHAT given
600, 13.3 million rewrite probes caused about 122 million calls through a
mutually recursive edge wrapper, plus 189 million packed-slice length checks
for an invariant already established during construction.  Rigid rewrite
roots now use the exact root-symbol table directly; edge matching and binding
trail cleanup are inlined; the unusual variable-root case retains the
original ordered DFS.  Profiled demodulation fell from about 10.6 to 7.2
seconds, and the final Osborn demod clock is 9.65 seconds versus 9.82 for the
legacy control.

Commit `89af8ab` replaces a second accidental table scan.  Hint rebuild
admission asked for full posting statistics after every hint lifecycle event,
although it needed only the exact logical reference count.  The full report
still computes its dense/profile histograms; the hot admission path now reads
the maintained counter in O(1).

Commit `997d69b` removes the largest Osborn cost.  The compact unit trie had
already found an exact stable subsumer ID, but the generic interface then
materialized the archived unit clause solely so `cl_process` could read its
ID and immediately release it.  The 600-given prefix performs roughly 89,000
successful unit generalizations.  Ordinary clause deletion and DISCOUNT
refresh now request a stable ID; the Topform-returning and
ancestor-subsumption interfaces remain available when a caller actually has
to inspect the proof.  In the retained pair this change reduced compact user
CPU from 109.12 to 84.96 seconds before rebuild tuning.

Finally, commit `7fe2adb` makes stale compaction growth-aware.  The configured
percentage is unchanged, but a growing live set is no longer recopied every
1,024 retirements.  Below 16K live records the stale threshold approaches the
live population; above that it has a 16K floor until the percentage becomes
larger.  This removed three premature rebuilds each from the rewrite, unit,
and backward indexes at given 600.  The policy converges to the original 25%
rule for mature populations, so it is not a short-run-only configuration.

The final simultaneous Osborn comparison is:

| implementation | user CPU | system CPU | total CPU | final PSS |
|---|---:|---:|---:|---:|
| legacy resident indexes | 75.64 s | 7.71 s | 83.35 s | 281,274 KiB |
| compact file-backed indexes | 77.69 s | 7.18 s | 84.87 s | 230,325 KiB |

Both runs end at `(601, 564041, 33429, 0)`, select the same 600 clauses, and
match the pinned historical `outaH` suffix.  Compact is now within 1.8% total
CPU of the legacy control at the formerly adverse short prefix while using
18.1% less resident memory.  This complements rather than replaces the full
Osborn evidence: `rr_osbe.out2-unl-rad-comp-otter7.gz` reaches the exact old
proof endpoint in 20,757 total CPU seconds versus 80,139 for `outa50`, with
about 93.7% less Prover9-accounted memory.  The latest four changes preserve
the bounded trajectory; a new proof-endpoint run would still be required to
attribute an exact full-run number to their specific commits.

The same code was also compared simultaneously against the actual old-P9
binary on the larger CHAT input through 2,000 given clauses:

| implementation | user CPU | system CPU | total CPU | peak RSS |
|---|---:|---:|---:|---:|
| old P9 | 154.66 s | 43.37 s | 198.03 s | 170,880 KiB |
| compact file-backed indexes | 143.98 s | 42.53 s | 186.51 s | 217,784 KiB |

Both runs end at `(2001, 4432172, 73432, 0)` and have an exact given-clause
trajectory.  Here compact is 5.8% faster in total CPU.  Its higher RSS is an
explicit workload tradeoff, not hidden index growth: CHAT admits the
109,777,280-byte packed-hint conjunction table under the 320 MiB cap, whereas
Osborn rejects its larger proposed table and remains 18.1% smaller than the
resident-index control.  Set `hint_conjunction_kb` lower (or to zero) when the
additional CHAT hint-matching speed is less important than that resident
allowance.

## August 2026 completed-run verdict and current rerun

The now-complete `chat_test.new.out7` and `chat_test.new.out8` runs have the
same compact proof endpoint: 11,369 given, 253,302,129 generated, 2,207,014
kept, and one proof.  `out7`'s capped adaptive index needs 11,282.22 user
seconds and 977.45 MiB final PSS; simultaneous `out8` mask8 needs 10,459.17
seconds and 917.44 MiB.  The old capped adaptive implementation is therefore
rejected: it is 7.9% slower and retains about 60 MiB more PSS even though it
examines fewer fallback posting groups.  Every admitted tree and position
feature has demoted by the proof, leaving mask8 to serve the final interval.

Those files predate the retained depth-four sparse positions, atom-linear
rewrite traversal, stable32 fallback, and bit-plane mask directory.  The
current explicit `adaptive32` mode preserves legacy `adaptive` for
reproducibility while combining those replacements.  In the matched
1,500-given CHAT pair it preserves the exact 1,501 / 2,947,136 / 66,933
trajectory and 23,401 candidates, reduces backward posting groups by 75.4%,
occurrence checks by 75.6%, and sampled lookup CPU by 43.2% relative to
legacy adaptive.  The subsequent bit-plane directory preserves that result
while reducing sampled lookup from 3.983 to 2.494 seconds and allocated
back-index bytes from 17,308,050 to 16,439,490.  Its late directory work is
about one third of the Patricia version, but counted work/query still grows;
only the external mature run can decide the CPU gate.

The subsequently completed `chat_test.new.out9` supplies that missing mature
evidence for the retained sparse-path architecture.  It reaches exactly the
same compact proof endpoint in 6,363.60 user seconds, versus 10,459.17 for
`out8` and 11,282.22 for `out7`.  Back-demod work is 4.392 billion groups and
812.886 sampled seconds, reductions of 84.0% and 81.0% from `out8`.
Positions serve 1,172,987 of 1,941,156 queries, trees serve 143,440, and both
remain complete and retained through the proof.  This is the first complete
confirmation that the bounded-prefix maintenance overhead is repaid at
maturity.

It does not yet close the product gate.  Ordinary P9 uses 5,110.38 user and
18.03 system seconds; `out9` uses 6,363.60 user and 1,094.60 system seconds.
Peak RSS is 1,368,864 KiB, approximately 61.5% below the ordinary-P9
3,519 MiB allocator reference but not the requested 80% reduction.  The
699,541,264-byte back index is now the largest removable in-process memory
component.  The final interval still averages 5,266 groups/query, although
that is only 16.2% of `out8`'s 32,536.

Importantly, `out9` uses the older `adaptive`/leaf-only eager path code.  It
does not contain authoritative absent-path empties, rooted rigid-ancestor
words, sparse multi-posting intersection, the stable32 bit-plane fallback, or
the relaxed census-free eager routing now present in `adaptive32`.  The latest
bounded 1,500-given depth-zero/depth-four pair has identical semantic answer
fingerprints and endpoint counters while cutting back work by 75.5%; total
CPU is still 5.6% worse at that prefix.  `out9` shows why this early overhead
is not grounds to reject the current candidate, but a new full run remains
necessary before claiming its exact CPU/RAM result.

For the next full CHAT comparison, start from the successful `out8` input and
replace its compact-index block with:

```prolog
assign(search_loop,otter).
assign(passive_store,dense).
assign(passive_directory,file).
assign(passive_selector_store,file).
assign(passive_selector_buffer,65536).
assign(hint_index,packed_fast).
assign(hint_conjunction_kb,327680).
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
assign(compact_rewrite_deep_cache_kb,0).
clear(compact_back_edge_filter).

assign(compact_passive_cache,0).
assign(compact_index_stale_pct,25).
assign(compact_term_reclaim_kb,8192).
```

The rigid-edge route is intentionally off for this CHAT authority run.  At
1,500 givens it is chosen for only 9 of 56,373 back queries while maintaining
613,042 postings; it remains available for arbitrary-depth workloads where
telemetry establishes a payoff.  The 200% tree allowance is a CPU-oriented
validation ceiling, not a prediction that it will be consumed.  Report
`Compact_back_demod`, `Compact_back_route`, `Compact_back_edge`, process PSS,
GNU-time peak RSS, and filesystem/cgroup memory separately.

The release binary used for the final local bit-plane checks has SHA-256
`c0ba6689964436a719170d43cd0112077c07771a1b083bcb81c0347e8df5e3c6`;
the corresponding implementation commit is `3dd3afb`.

The later `chat_test.new.out61` is not a valid measurement of the final
recommended block: it omitted both `compact_unit_strategy=code_tree` and
`compact_nonunit_path_filter`.  Its root-scan unit index had performed
14.108 billion exact conflict tests and used 3,252.106 sampled seconds by
11,015 givens.  The completed compact `out4` used only 39.945 million
code-tree candidates and 173.981 sampled seconds by its proof.  This explains
a large part of `out61`, but not all of it: every adaptive tree and position
feature also exhausted its fixed allowance and the run fell back to mature
mask scans.  The retained-linear commits `950b230` and `1f1643d` address that
independent collapse; a new proof-endpoint run is still required.

## Post-`ccd8f43` shallow sparse-path stage

Commit `1f1643d` adds a query-order-independent shallow exact-path index for
back demodulation.  With `compact_back_eager_position_depth=4`, every new
record contributes compressed postings for rigid descendant facts through
depth three.  The narrowest complete posting is queried directly, and the
exact compact matcher remains the final authority.  Space is occurrence
linear for fixed depth; no per-feature record bitmap, historical census, or
backfill is used.

At 600 givens on the large CHAT input, depth three preserved the exact search
trajectory and total user CPU (38.79 seconds versus 38.98 at depth zero), cut
backward posting groups from 3,124,571 to 1,233,135, and increased compact
back-index bytes from 2,319,336 to 4,724,941.  The equality in total CPU is
expected at this prefix because back lookup is not yet dominant.  This is a
structural work reduction and a regression gate, not an extrapolated claim
about the complete run.

Depth four is the measured knee: at 600 givens it reduced posting groups
another 15.0% from depth three for about 0.55 MiB more back-index allocation,
whereas depth five bought only another 0.7% work reduction.  At 1,500 givens,
depth zero, three, and four all ended at the exact same
2,947,136-generated/66,933-kept trajectory.  Depth four reduced user CPU from
187.30 to 165.27 seconds (11.8%), backward posting groups from 43,343,242 to
10,846,020 (75.0%), and sampled back lookup time from 21.828 to 6.900 seconds.
Peak RSS was 107,904 KiB versus 99,280 KiB at depth zero.  This bounded
crossover selects depth four for the external run; it does not prove how the
constant evolves through 11,000 givens.

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
| `b12c84c` | bounded dense intersections for packed-fast hint back-demodulation |
| `f40023b` | conservative deep fingerprints before back-hint materialization |
| `f797729` | sanitizer-found zero-candidate `qsort` contract fix |
| `950b230` | population-relative retention for useful complete tree and sparse indexes |
| `1f1643d` | query-order-independent shallow sparse exact-path postings |
| `edabbe3` | depth-four selection at the measured shallow-path crossover knee |
| `57bed92` | one transient flatterm conversion per rewritten atom |
| `32aca1c` | checkpointed linear-preparation telemetry and depth-200 invariant test |
| `abb0485` | recyclable transient-subject arena and avoided-work counter |
| `83815b5` | interval reporting for mature rewrite-preparation scaling |
| `e0d64c9` | isolation of debug/sanitizer objects from production release links |

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

### One subject conversion per atom

The mature `out61` evidence exposed a second forward-rewrite multiplier that
the earlier radix changes did not remove.  Compact normalization walked a
Term bottom-up, but every rigid subterm then called `flatten_query_rec` over
that subterm's complete subtree.  Preparing all rewrite queries therefore
touched the sum of all subtree sizes.  A chain-shaped atom is quadratic in its
depth before discrimination retrieval itself is counted.  LADR's ordinary
flatterm demodulator instead converts an atom once and skips subtrees with an
`end` link.

Commit `57bed92` retains the compact persistent rule bank but uses the same
one-conversion subject representation.  Radix matching advances over
`next/end` links, variable bindings point directly into the transient subject,
RHS contracta are built as flatterms, and lex-dependent rules use
`flat_greater`.  Copied bindings retain LADR's already-reduced marker, and
predecessor order, sequence numbers, normal forms, and encoded proof
justifications are unchanged.  Commit `abb0485` recycles transient nodes in
1,024-node bank-owned blocks, so the structural fix does not replace scanning
with millions of general allocator calls.  The arena's retained size is
bounded by the largest simultaneously normalized atom/contractum, not by the
number of passive clauses or rewrite attempts.

The new `Compact_rewrite_subjects` line reports atoms, initial atom nodes,
target-subtree nodes, and attempts.  `target_nodes / initial_nodes` is the
exact node-population ratio between the former per-subterm query preparation
and the new one-per-atom preparation.  The long-run report exports this ratio
per interval, which lets the external run verify whether the benefit grows
with mature term shape.  A depth-200 no-match regression checks 202 initial
nodes versus 20,503 former target-subtree node visits while preserving all
202 rigid rewrite probes.

On the exact depth-four 1,500-given CHAT trajectory, the clean release binary
ended at 2,947,136 generated and 66,933 kept in 154.45 user seconds and
109,008 KiB peak RSS.  The pre-change depth-four build used 165.27 seconds
and 107,904 KiB.  This is a 6.5% CPU improvement at 1.0% more peak RSS, and
both the complete given sequence and hint trace compare byte-for-byte after
filtering.  The cumulative former/new query-preparation node ratio was 4.8x.
This validates the mechanism and bounded-prefix crossover; only the external
mature endpoint can show how much of `out61`'s late demodulation slope it
removes.

One discarded 193.09-second run is not performance evidence: `test.src` had
compiled the shared production rewrite object with `-O0`, and the release
link reused it because its timestamp was newer.  Commit `e0d64c9` makes every
focused prover implementation object test-local.  A sanitizer test now leaves
both the optimized object and release binary hashes unchanged, preventing
that contamination from recurring.

### Packed-fast hint back-demodulation

The 2,000-given paired run exposed a second mature multiplier.  Its packed
hint bank had scanned 789,746,162 correlated posting entries for 10,058,011
unique candidates while back-demodulating hints.  `packed_fast` already had
an exact stable-ID dense/sparse intersection engine for ordinary hint
matching, but this path still used the older repeated posting-vector scan.

Commit `b12c84c` sends back-hint retrieval through the same intersection
engine.  A narrow posting remains the sparse seed and is not copied; the
other feature memberships are tested through dense words.  Broad postings
are intersected word-wise after their one-bit-per-word summaries reject empty
blocks.  The decreasing-ID sort, activity check, materialization, and
`rewritable_clause_type` remain authoritative.  If any dense view is absent
or cannot be allocated, the query executes the complete old sparse
intersection.

Dense posting bits plus summaries have a hard 16 MiB cap, including after a
stale-posting rebuild.  If a later hint ID would require a cached view to grow
beyond the cap, that view is discarded before the sparse posting is appended;
an incomplete dense view can therefore never suppress an answer.  The cap is
independent of run duration and the number of queries.  `Better_packed_postings`
reports both `dense_budget_bytes` and `dense_budget_denials`.

In a same-time, fixed-core 600-given comparison against the immediately
preceding binary, total user CPU fell from 40.96 to 38.09 seconds (7.0%).
Back-hint posting entries visited fell from 176,431,500 to 4,100,930 (97.7%).
Both runs ended at 601 given with 497,430 generated, 16,974 kept, 13,568 new
demodulators, 42,741 exact back-hint rewrites, and the same candidate totals.
The final dense bank used 7,143,424 bytes of bits and 111,616 bytes of
summaries; peak RSS changed only from 90,420 to 90,552 KiB.  This is a
mechanical scan reduction, but the longer paired gate below remains the CPU
acceptance authority.

Commit `f40023b` then removes the dominant work left after intersection:
decompressing candidates which share all depth-two posting keys but cannot
possibly contain the demodulator pattern.  Each `packed_fast` hint-capacity
slot carries a 256-bit conservative summary of `(occurrence root, path,
symbol)` facts through depth four.  A query must be contained in that summary
before the hint body is decompressed.  Variables add no required facts, Bloom
collisions admit extra work, and any hint or query involving an associative,
commutative, or AC symbol disables this filter conservatively.  Thus exact
rewritability remains the authority even on theory terms.

Against `b12c84c` in a same-time 600-given pair, this second filter reduced
user CPU from 35.18 to 28.11 seconds (20.1%) and materializations from
2,768,728 to 1,181,396.  It rejected 1,587,332 impossible candidates while
preserving all 42,741 exact positives, rewrites, and reindexes, as well as the
entire search trajectory.  Peak RSS changed from 90,292 to 90,548 KiB.  The
fingerprint array reserves 4 MiB at this input's power-of-two hint capacity;
its size is linear in hint capacity and independent of elapsed run time.

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
- The default factor is 32.  Therefore active census-plus-backfill record
  visits are at most total credited retrieval work divided by 32.  With the
  default 25% stale-index rebuild threshold, physical loop iterations are at
  most credited work divided by 25.6 between rebuilds.
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

Factor 32 was selected only after an exact endpoint crossover.  At 1,500
given, factors 16 and 64 took 210.38 and 210.57 seconds, which is measurement
parity.  At 2,000 given, factor 32 took 340.70 seconds, versus 344.53 for
factor 16 and 357.15 for factor 64.  Factor 32 therefore avoids factor 64's
late admission deficit without paying factor 16's excess maintenance; its
1/32 construction bound remains independent of run length.

At the supplied later `out51` state, the old route-observed work totals about
8.128 billion units.  Applying the new default ledger to the same amount of
work permits at most about 254.0 million active census/backfill visits, or
317.5 million physical loop iterations at 25% staleness.  That is a worst-case
construction bound about 84% below the old 1.823 billion
position-record-examinations plus 164.5 million backfill visits.  It is not a
measured 7,689-given runtime result; it is the implemented accounting bound
that the external run must verify.

## Current options and recommended run

For the next mature comparison, use the complete block below.  In particular,
do not reuse the `out61` block: `code_tree`, the nonunit path filter, sparse
positions, the zero absolute position budget, and eager depth four are all
material parts of this configuration.

```text
assign(search_loop,otter).
assign(passive_store,dense).
assign(passive_directory,file).
assign(passive_selector_store,file).
assign(passive_selector_buffer,65536).
assign(hint_index,packed_fast).
assign(hint_conjunction_kb,327680).
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
assign(compact_unit_strategy,code_tree).
set(compact_nonunit_path_filter).
assign(compact_back_demod_strategy,adaptive).
set(compact_back_sparse_positions).

assign(compact_passive_cache,0).
assign(compact_index_stale_pct,25).
assign(compact_term_reclaim_kb,8192).
assign(compact_back_position_budget_kb,0).
assign(compact_back_position_budget_pct,50).
assign(compact_back_position_build_factor,32).
assign(compact_back_eager_position_depth,4).
assign(compact_back_tree_budget_kb,65536).
assign(compact_back_tree_budget_pct,200).
assign(compact_rewrite_deep_cache_kb,0).
```

`passive_selector_store,heap` remains useful for a controlled comparison; the
file selector is the intended bounded-RAM long-run setting.  Depth zero is the
retained demand-built control and depth two is the lower-memory diagnostic;
depth four is the current production candidate because it covered 41.0% of
routed back lookups at 600 givens and cut total posting-group work by 66.5%.
Do not use the synthetic depth-16 setting on a large run.

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
The forward-rewrite and initial exact 1,500-given gates used release binary
SHA-256
`fc674b7ba3c75092732ab91a86e5d969d3f2cccd17d59a7dddb745927f440463`.
The same-time factor-32 pairs used release binary SHA-256
`70c484539801d9debce67a01e7240a681ef6f5366db5188def7ebc8cb90fbb36`.
The deep back-hint fingerprint gates used release binary SHA-256
`ea24b7a1017bed2988f555055a3df5a8b82721bf1a2158d4dc5240b1d6f8a9cc`.
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

The first exact mature-prefix comparison, retained because it directly
isolates the forward-rewrite change against its predecessor, is:

| 1,500-given gate | User CPU | Peak RSS | Generated / kept | Demod attempts / rewrites |
|:---|---:|---:|---:|---:|
| normal P9 | 200.32 s | 275,584 KiB | 2,947,138 / 66,935 | 83,176,695 / 10,001,163 |
| compact before `4ddf9ed` | 222.65 s | 90,420 KiB | 2,947,136 / 66,933 | 83,176,656 / 10,001,165 |
| compact at `4ddf9ed` | 210.38 s | 90,428 KiB | 2,947,136 / 66,933 | 83,176,656 / 10,001,165 |

In that load window, current compact was 5.0% slower than normal P9 and 5.5%
faster than the preceding compact binary.  It removed 55% of the former
compact CPU penalty while preserving the compact trajectory exactly.  Because
the normal and compact absolute runs were not simultaneous, the authoritative
old-versus-new endpoint is the paired measurement below.

The component clocks explain why the proof endpoint remains open: current
compact forward demodulation takes 61.96 seconds versus 50.60 seconds for
normal P9, and compact backward demodulation takes 22.10 seconds versus 5.92
seconds.  Other compact-path savings nearly offset those gaps at 1,500 given,
but their mature slopes still require the supplied 7,365-given and full-proof
comparisons.

The authoritative same-time pair pinned normal P9 to CPU 2 and compact to CPU
0.  It used separately generated but byte-equivalent bounded inputs and
started the two processes together:

| Paired gate | User CPU | Peak RSS | Generated / kept | Demod attempts / rewrites |
|:---|---:|---:|---:|---:|
| normal P9, 1,500 | 183.44 s | 275,584 KiB | 2,947,138 / 66,935 | 83,176,695 / 10,001,163 |
| compact factor 32, 1,500 | 183.54 s | 90,420 KiB | 2,947,136 / 66,933 | 83,176,656 / 10,001,165 |
| normal P9, 2,000 | 305.93 s | 358,144 KiB | 4,497,692 / 144,514 | 124,676,012 / 16,116,393 |
| compact factor 32, 2,000 | 332.67 s | 119,800 KiB | 4,497,690 / 144,512 | 124,675,973 / 16,116,395 |

At 1,500 given, compact differs by only 0.05% in user CPU and reduces peak
RSS by 67.2%.  At 2,000 given it is 8.7% slower and reduces peak RSS by 66.5%.
More importantly for longevity, normal P9 uses 122.49 seconds for the
1,500-to-2,000 interval while compact uses 149.13 seconds, a 21.7% interval
penalty.  This still passes the present 1.25x bounded acceptance guard, but it
does not close the long-run CPU-slope goal.

After the two packed back-hint changes, the 1,500-given pair was repeated in
both CPU orientations to expose this host's substantial per-core/load
variation:

| CPU assignment | normal P9 | current compact | Compact change | Compact RSS |
|:---|---:|---:|---:|---:|
| old CPU 2 / compact CPU 0 | 153.63 s | 148.69 s | -3.2% | 97,836 KiB |
| old CPU 0 / compact CPU 2 | 186.42 s | 154.97 s | -16.9% | 97,856 KiB |
| arithmetic mean | 170.03 s | 151.83 s | -10.7% | 97,846 KiB |

Both orientations favor current compact; the mean reduces peak RSS by 64.5%
from normal P9's 275,584 KiB.  Every run reached the same semantic endpoint:
normal P9 generated/kept 2,947,138/66,935 and compact 2,947,136/66,933, the
established two-clause implementation difference.  Current compact retained
56,373 back-hint queries, 6,687,804 posting-intersection candidates, 64,435
exact positives, and 64,435 rewrites/reindexes, but materialized only
1,841,637 bodies after 4,846,167 fingerprint rejections.

This fixes the 1,500-given total-CPU gate, but it does not eliminate the
remaining clause-index slope: current compact's ordinary forward/back demod
clocks are 51.49/17.17 seconds in the first orientation versus normal P9's
39.74/4.60 seconds.  Packed hint work supplies the offset at this prefix; the
2,000-given and supplied multi-hour comparisons must still show that the
ordinary back-demodulation gap does not retake the total.

Two post-fingerprint 2,000-given orientations close that bounded endpoint:

| CPU assignment | normal P9 | current compact | Compact change | Compact RSS |
|:---|---:|---:|---:|---:|
| old CPU 2 / compact CPU 0 | 315.92 s | 294.89 s | -6.7% | 129,968 KiB |
| old CPU 0 / compact CPU 2 | 307.94 s | 286.79 s | -6.9% | 130,000 KiB |
| arithmetic mean | 311.93 s | 290.84 s | -6.8% | 129,984 KiB |

Both orientations favor current compact, whose mean peak RSS is 63.7% below
normal P9's 358,144 KiB.  Interval numbers formed by subtracting the separate
1,500-given runs are noisier: compact is 9.9% faster in one orientation and
8.5% slower in the other; the arithmetic-mean 1,500-to-2,000 interval is 1.7%
faster.  Thus the old 21.7% interval regression is not reproduced, but these
short differences should not be projected as a mature-run slope.

In the first orientation, compact forward/back demod clocks remain slower at
94.53/56.71 versus 82.15/24.84 seconds, but its hint, general indexing, and
disable clocks recover more than that difference.  Packed back-hint
materializations are 2,483,191 after 7,622,948 conservative fingerprint
rejections; the dense-intersection-only binary would have materialized all
10,106,139 candidates.  This remains a bounded prefix, so the supplied
multi-hour comparison is still required.

The focused hint tests pass under AddressSanitizer and UBSan with leak checking
disabled for LADR's process-global registries.  UBSan exposed one independent
zero-candidate edge: the packed path called `qsort` with a null base and zero
elements.  Commit `f797729` skips sorting vectors shorter than two; it does not
change candidate order or any measured nonempty workload.

The separate factor-search endpoints were:

| 2,000-given gate | User CPU | Peak RSS | Generated / kept | Demod attempts / rewrites |
|:---|---:|---:|---:|---:|
| normal P9 | 335.92 s | 358,144 KiB | 4,497,692 / 144,514 | 124,676,012 / 16,116,393 |
| compact, factor 64 | 357.15 s | 117,308 KiB | 4,497,690 / 144,512 | 124,675,973 / 16,116,395 |
| compact, factor 16 | 344.53 s | 117,784 KiB | 4,497,690 / 144,512 | 124,675,973 / 16,116,395 |
| compact, factor 32 (new default) | 340.70 s | 119,824 KiB | 4,497,690 / 144,512 | 124,675,973 / 16,116,395 |

These three factor cases establish an internal crossover, not an
old-versus-new percentage: they ran in a different load window from the
normal-P9 row.  Factor 32 saves 16.45 seconds over factor 64 and 3.83 seconds
over factor 16.  Its position maintenance is 11.61 seconds (3.4% of user CPU),
below the 5% acceptance limit.  In the authoritative paired 2,000-given run,
compact forward/back-demod clocks are 96.35/54.71 seconds versus 80.20/23.90
seconds for normal P9.  Backward retrieval is therefore the principal
unclosed slope risk.

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
- checkpoint version 4 and compaction retain bounded adaptive state while
  discarding obsolete leaf-only position calibration;
- high logical term-pool bases preserve exact candidates for every strategy;
- 32 simultaneously active position features still require one record walk,
  and a fully demoted root requires none;
- a variable-prefix calibration can append real candidates, exhaust its
  allowance, roll those candidates/stamps back, and return the exact mask
  order; and
- a 1,024-duplicate root backfill can grow the shared posting array during
  traversal without invalidating its iterator.
- a missing complete eager rooted path returns authoritative empty without a
  fallback scan, and two sparse rooted-path postings intersect without a
  per-record bitmap;
- clauses with the same leaf at different rigid ancestors cannot collide
  merely because their child-number paths are identical; and
- mask-only and rooted-position runs expose the same semantic answer
  fingerprint even when their work fingerprints differ.

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

`out9` establishes the mature crossover for the predecessor retained-path
implementation.  It does not establish the exact current `adaptive32`,
rooted-word result.  Run the configuration above to the proof endpoint and
compare its semantic answer fingerprint and exact trajectory with a current
depth-zero control.  Reject it if the proof-endpoint total CPU does not
improve on `out9`, if resident memory grows without a corresponding posting
count explanation, or if the interval back lookup resumes population-linear
growth.  The ordinary-P9 total CPU and 80% resident-memory reduction remain
the final product gates.

## Osborn OTTER trajectory preservation

The first full `adaptive32` Osborn run was substantially faster than the
ordinary-P9 run, but it did not replay the proof-producing OTTER search.  The
historical `outaH` and the compact run selected identical given clauses
through given 491 and first differed at given 492.  Although the historical
proof clause later appeared in the compact run, it was generated in a
different search state and did not lead to the same proof.

Generation/retention tracing located the earlier cause.  Ordinary FPA indexes
assign `FPA_ID` values when clauses are retained, and FPA retrieval uses those
values to break candidate-order ties.  Authoritative compact unit indexing
had skipped the legacy insertion and therefore delayed assignment until the
clause became given.  The answer sets were identical, but hyper-resolution
candidate order first changed at given 205; that changed kept-clause order and
eventually changed given 492.  The compact authoritative unit and nonunit
paths now reserve the legacy root-literal FPA IDs at the original retention
point while still avoiding the memory cost of the legacy index.

Dense passive archival exposed a second lifecycle issue: serializing a clause
destroyed its term objects and their reserved IDs.  Dense records now retain
the first root-literal FPA ID and restore the consecutive literal IDs when the
clause is materialized.  This adds eight padded bytes to each dense directory
record (72 instead of 64 bytes); it does not restore the legacy FPA postings.

With both fixes, the production compact configuration and a modern legacy-FPA
control select exactly the same 510 given-clause bodies and finish that prefix
with the same `(given, generated, kept, proofs)` tuple:

```text
(511, 390236, 25140, 0)
```

The optional regression requires the supplied Osborn input and can be run as:

```sh
make osborn-trajectory-test
OSBORN_INPUT=/path/to/rr_osbe.in.gz make osborn-trajectory-test
P9_OSBORN_MAX_GIVEN=600 OSBORN_INPUT=/path/to/rr_osbe.in.gz \
  make osborn-trajectory-test
```

It checks all 510 current reference/compact given clauses, the historical
`outaH` suffix from given 195 onward by a pinned digest, and the endpoint
statistics.  A larger `P9_OSBORN_MAX_GIVEN` continues the exact comparison
against the modern legacy-FPA control while retaining the pinned historical
510-clause check.  On the development machine the 510-clause reference took
about 68 user seconds and the compact run about 93; an extended 600-clause
check also passed (about 87 versus 130 user seconds).  These bounded
regressions establish exact early trajectory preservation; a proof-endpoint
run remains the external CPU and proof-replay gate.
