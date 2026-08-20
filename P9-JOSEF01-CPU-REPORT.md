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
| current PGO, exact clocks | 45.90 s | 21.69 s | 67.59 s | 599,492 KiB | 1.73x faster |
| current PGO, clocks sampled 1/16 | 41.16 s | 6.36 s | 47.52 s | 604,256 KiB | 2.46x faster |
| current PGO, clocks off | 37.14 s | 3.71 s | 40.85 s | 609,000 KiB | 2.86x faster |

Thus exact clocks added 26.74 CPU seconds, or 65.5% relative to the clocks-off
process and 39.6% of the exact-clock total, at this prefix.  Sampling retained
approximate phase attribution but still cost 6.67 seconds relative to clocks
off.  `/usr/bin/time -v` remains the authority for total user/system CPU when
internal clocks are disabled.

The current PGO binary is SHA-256
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
| process PSS | 9.08 GB |

The dense passive directory, selector runs, and ancestor store are
file-backed.  Their logical/physical files are much larger than their process
PSS and must be accounted separately when comparing RAM with disk/page-cache
use.

## Changes on `josef01-cpu`

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

- `compact_unit_strategy=adaptive` reduced tree work but was about 8.5%
  slower at 2,000 givens and produced more exact tests.  Keep `code_tree` for
  the first decisive run.
- A 64-MiB packed-hint result cache raised its hit rate only from 7.42% to
  9.03%, used about 63 MiB more RSS, and increased the 1,000-given CPU result
  from the current paired mean of 78.83 to 82.11 seconds.  Do not add
  `assign(hint_cache_kb,65536)`.
- A negative Bloom summary was slower and was removed completely.
- A query-scoped binding trail preserved all answers but raised the sampled
  generalization timer from roughly 2.72--2.76 to 2.920 seconds.  It was
  reverted completely.
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
to 1,048,576 entries adds at most roughly 24 MiB of buffered selector entries.

Starting from 9,083,561 KiB PSS, even charging the complete slab and selector
allowances gives roughly 9.17 GiB-equivalent PSS.  The expected saving against
the approximately 45,258-MiB old result therefore remains about 80%, subject
to same-host measurement.  File-backed logical bytes and page cache must be
reported separately.

### CPU

CPU estimates are less certain and the gains are not additive:

- disabling exact detailed clocks removes a measured 39.6% of total CPU at
  the exact 1,001-given gate; this corrects a configuration mismatch in the
  completed full-run comparison rather than changing proof search;
- the direct symbol table is a measured 3.95% whole-prefix gain;
- term-base caching saves about 12% only inside unit lookups;
- sibling pruning saves about 38% only inside forward generalization;
- the slab recycler changes almost no bounded CPU but removes nearly all
  post-warm-up slab unmaps by 2,000 givens;
- native compilation may add 0--5% depending on the host.

The earlier 28,000--34,000-second estimate was made before the clock mismatch
was discovered and is superseded.  It charged exact-clock work to the compact
storage/index design.  A precise replacement cannot be obtained by scaling a
1,001-given result across machines: the timer cost is approximately linear in
phase intervals, while unit-tree, hint and selector work grow differently.
The current clocks-off engine is 2.86 times old P9 at the bounded exact state,
and current source also contains the slab recycler and mature index changes
that were absent from `new3`.  It is therefore plausible that a clocks-off,
current-PGO full run reaches the 24,191-second 1.25-times-old gate, but **CPU
parity has not yet been demonstrated**.  Do not publish a tighter full-run
estimate until that external acceptance run completes.

## Build and run the decisive Josef 01 comparison

### Portable baseline build

From this repository and branch:

```sh
git switch josef01-cpu
make -C ladr lib
make -C provers.src prover9
cp -p provers.src/prover9 bin/prover9
sha256sum bin/prover9
```

Do not reuse objects from a differently instrumented, sanitized, profiled, or
`NATIVE=1` build.  If in doubt, use a fresh worktree or clean the build before
compiling.

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
for this first authority run.  If approximate internal phase attribution is
needed, replace `clear(clocks)` with:

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
`CHAT_CLOCKS=0`; for example, set it alongside
`CHAT_CASES=new_otter_compact_file_production`.  `CHAT_CLOCKS=1` remains the
compatibility default for historical diagnostic matrices.

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
   traversal dominates, the next work is a lower-exact-test position or
   substitution-tree route—not a larger result cache.

## Reproducing the bounded CHAT smoke

The exact production case is now one command:

```sh
CHAT_CASES=new_otter_compact_file_production \
CHAT_CPU=0 CHAT_REPORT_SECONDS=30 \
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
2. If unit pending-subtree work dominates, redesign the adaptive position
   route so it keeps the code tree's selectivity and canonical order without
   inflating exact candidates.  The compressed complete postings already
   solve its RAM representation.
3. If packed hints remain near 4,250 seconds, optimize profile intersection
   and dependency validation.  A larger result cache has already failed; a
   different key/profile representation is required.
4. If selector read amplification dominates, tune run merging from measured
   run counts and bytes, not from the short-prefix 65,536/1,048,576 buffer
   difference.

These are general long-run mechanisms.  None should depend on Josef symbol
names, a singleton term shape, or a fixed hint count.
