# Prover9 long-run scalability implementation plan

Status: active implementation on branch `long-run-scalability`.

This plan follows the frozen cross-problem compact-index candidate.  The
300/1,000-given results establish useful correctness and short-prefix
selectivity evidence, but they do not establish bounded behavior after days of
insert/delete churn or with tens of millions of live passive clauses.  This
branch treats logical generality and asymptotic longevity as separate gates.
RAM and CPU longevity are co-primary: a representation is not scalable merely
because it stays resident-bounded if query, maintenance, or reconstruction work
still grows with the number of clauses ever seen.

## 1. Product invariants

The work must preserve all of the following.

1. The authoritative compact indexes return the same possible answers, in the
   same deterministic order, as before a maintenance operation.  Exact
   matching, subsumption, unification, and rewriting remain final authorities.
2. A maintenance decision depends only on logical counters, never elapsed time
   or allocator addresses.  Checkpoint/resume therefore cannot change the
   search trajectory.
3. Except for the proof archive on disk and explicitly documented live-state
   structures, resident storage is bounded by the active population plus a
   configured stale fraction and fixed scratch budgets.  It must not grow with
   the number of clauses ever inserted.
4. A rebuild must publish a complete replacement atomically.  Failure before
   publication leaves the old authority valid.
5. Temporary RAM during maintenance is bounded and reported.  Reclaiming stale
   state must not require an unreported second full copy of a large index.
6. Archive lookup by a known proof ID/offset is constant or logarithmic time;
   checkpoint work is proportional to the state it writes, not every obsolete
   archive version.
7. Internal offset limits are checked before mutation.  Projected day/week/
   month workloads must not encounter a hidden 32-bit wrap or an unexplained
   fatal exit.
8. Process RSS is not used as a synonym for total job RAM.  Long-run reports
   separate anonymous RSS, mapped/file RSS, and cgroup file cache where the
   host exposes those counters.
9. For a fixed live logical population and query distribution, candidate,
   posting, exact-test, decode, and rebuild work per operation must plateau
   after maintenance.  No compact fallback may trade a memory win for a query
   whose cost grows with historical insertions or an unrelated broad bucket.

## 2. Phase A: close known cumulative-growth defects

### A1. Compact nonunit feature index

The current nonunit index marks deleted records inactive but never reclaims its
records, postings, radix nodes, or labels.  Queries consequently scan dead
posting chains forever.

Implement a deterministic stale threshold shared with
`compact_index_stale_pct`, with a 1,024-record noise floor.  Rebuild live
records in original insertion order so forward-first and backward candidate
orders remain unchanged.  Release predecessor secondary arrays before growing
the replacement where the retained record recipe makes that possible.  Add:

- compaction count, reclaimed bytes, maintenance time, dead postings, and
  physical/live ratio to statistics;
- forced-compaction and threshold APIs for focused tests;
- fixed-live-set aging tests covering at least 100 generations;
- answer/order equality before and after every rebuild; and
- a scale mode that can exercise millions of add/remove operations without
  allocating full clauses.

Gate: after warm-up, both allocated bytes and posting work plateau within the
configured stale bound for a fixed live population.

### A2. Integrate structural selectivity into retrieval

The path summary currently rejects candidates only after their numerical
feature leaf posting has been scanned.  Introduce exact-necessary structural
posting keys or a compact multi-feature intersection so selective queries do
not enumerate the broad leaf first.  Keep a complete fallback for variable-rich
queries.  Select a strategy by measured posting counts and byte budgets, not a
problem-name constant.

Gate: on same-vector adversarial families, false posting work and exact
materializations grow with the selective answer set rather than total bucket
size; no real answer or candidate order changes.

## 3. Phase B: make the archive directory scale with current state

The append-only record file is allowed to grow on disk, but the in-memory
`refs` vector and several by-ID operations currently scan cumulative archive
positions.

1. Add offset-native record access: once the ID table supplies an archive
   offset, validate and read that record directly without searching `refs`.
2. Separate stable archive offsets from the current-position iteration needed
   by passive activation and checkpoints.
3. Reclaim or segment obsolete handle entries while preserving dense passive
   positions.  Prefer stable logical handles translated through chunk tables so
   compaction does not rewrite every passive record.
4. Make checkpoint iteration visit current records directly.  Record scan
   counts and bytes so cumulative behavior is visible.
5. In file mode, add a safe batched durability/`posix_fadvise(DONTNEED)` policy
   and report its calls/bytes.  Validate with cgroup accounting; do not assume
   page cache is absent because it is absent from process RSS.

Gate: random archived-ID lookup stays constant-time as obsolete versions grow,
and resident handle bytes are proportional to current records or bounded
segments rather than cumulative versions.

## 4. Phase C: accelerated longevity and numerical-limit harness

Long theorem searches are final validation, not the first way to discover a
linear leak.  Add a bounded runner that drives each persistent component at
powers of ten and in fixed-live aging mode:

- dense passive records and selector heaps;
- compact rewrite, unit, back-demodulation, and nonunit indexes;
- shared term-pool reclamation;
- packed-hint rewrite/rebuild churn; and
- archive append/activate/rearchive/checkpoint lookup.

Every driver emits machine-readable samples containing active, physical,
retired, allocated/used/peak bytes, query nodes/postings/exact tests, rebuild
count/time, scratch high-water, and relevant disk bytes.  The summary computes
bytes per active object, work per query, dead/live ratio, and the slope between
successive population sizes.  It also records user CPU, system CPU, elapsed
time, query CPU, and maintenance CPU separately; operation counters are the
portable authority when timing noise is too large for a short synthetic run.

Required adversarial distributions include identical roots, identical feature
vectors, variable roots, nonlinear variables, deep terms hidden by shallow
signatures, repeated rewriting, and broad true-answer sets.

Gate:

- fixed-live resident bytes plateau after each maintenance cycle;
- fixed-live postings decoded, nodes examined, candidates, exact tests, and
  materializations per identical query plateau after each maintenance cycle;
- physical/live is at most `1 + stale_pct/100`, apart from the documented
  1,024-record floor;
- maintenance consumes less than 10% of steady-state CPU;
- temporary resident bytes stay below 1.25 times pre-maintenance steady state
  unless an explicitly measured component exception is approved;
- false-candidate work is no more than twice the corresponding legacy/strong
  reference index unless the true answer itself is broad; and
- at successive powers of ten, CPU/query and counted work/query may grow only
  in proportion to the true answer set, not total historical or unrelated
  population; and
- no configured 30-day projection reaches an internal representation limit.

Implementation status: `compact_long_run_report.py` now reads existing plain
or gzip outputs and derives interval CPU, query-work, memory, and file-I/O
slopes from periodic reports.  The bounded chat and generalization runners
write both a machine-readable `long-run-slopes.tsv` and a concise
`long-run-summary.md`; an expensive old baseline therefore need not be rerun.
The reporter is diagnostic rather than an acceptance oracle: candidate and
reference runs must still have matched logical trajectories, and distribution
changes in generated clauses must be separated from index cost.  New reports
carry a statistics-format capacity marker.  Older long `comma_num`-formatted
lines can contain overwritten arguments and are explicitly warned about;
direct `%llu` compact-index lines and the short process-residency line are not
affected by that historical formatter defect.  `--compare-to-first` now audits
the exact final trajectory, total user-CPU ratio, peak-RSS saving, presence of
periodic evidence, and post-warm-up back-lookup CPU normalized by query count
plus exact successful answers.  This permits the unavoidable cost of a larger
true answer set without rewarding false candidates or index scans.  The
promotion gate requires seven complete intervals, ignores the startup interval,
compares the median of intervals 2--4 with the final-three median, and also
bounds the final-three spread.  Thus warm-up is not mistaken for ongoing growth
and a short stable tail cannot hide cumulative drift.  A GNU `time` sidecar
named like `case.time` is preferred for peak RSS; otherwise the allocator's
process peak is used.  The matrix runners put
their first selected case in the reference role automatically, or prepend an
archive supplied by `CHAT_REFERENCE_OUTPUT`/
`P9_MATRIX_REFERENCE_OUTPUT`.  CPU, RAM, and slope thresholds are explicit
reporter options and matrix environment variables, so a compact-versus-compact
CPU crossover does not incorrectly inherit the 80% old-P9 RAM target.
`cgroup_job_memory.sh` supplies the missing total-job measurement: when the
matrix is run with `*_CGROUP_ACCOUNTING=1` inside a delegated cgroup-v2 scope,
each prover gets a fresh child and its `memory.peak`, end-of-job anonymous/file
breakdown, and swap peak are retained.  The report prefers that peak over GNU
time RSS.  Unsupported or non-delegated hosts exit 77 instead of silently
claiming that process RSS includes file cache.

## 5. Phase D: passive control plane for tens of millions of live clauses

The 64-byte dense record and its selector heaps are deliberately much smaller
than full `Topform` graphs, but remain linear in the live SOS.  The uploaded
9-hour clauses-frontier run reached about 65.9 million active passives; merely
keeping one 64-byte record for each requires about 4.2 GB.

Implement an optional external passive directory and priority queues:

1. Store each immutable selection record once in fixed-size disk segments.
2. Buffer new selector entries, sort them into immutable runs, and retain only
   run heads plus a bounded page cache in RAM (an external-memory/LSM priority
   queue).
3. Represent deactivation with segmented bitmaps or generation records; merge
   runs incrementally when stale density or run count crosses deterministic
   thresholds.
4. Preserve exact age/weight/hint-age tie breaking and all selector-ratio state.
5. Keep the current dense in-memory implementation as the small/medium control
   and allow an explicit strategy choice until old/new event logs and CPU gates
   pass.

Gate: a synthetic 100-million-passive population stays within a documented
bounded RAM budget, selection order is byte-identical to the in-memory dense
reference, and restart/checkpoint reconstruction is deterministic.

Implementation status: the file-backed fixed record directory and optional
binary-leveled selector runs are implemented.  Differential ordering,
reactivation, compaction, sanitizer, 200,000-record, 2.2-million-record, and
300-given integrated gates pass.  A short full compact-OTTER checkpoint/restart
differential also passes at two boundaries, and a default-buffer 1,000-given
comparison has no material CPU regression.  The 100-million projection,
week-scale checkpoint validation, mature-run IO gate, and multi-million
Osborn/AIM validation remain open; the file selector therefore stays explicit
and non-default.  Sequential blocks consumed by selection or merging are now
advised out of the kernel cache immediately after `pread`; separate counters
make complete advice coverage and failures visible.  This closes an identified
cumulative cgroup-cache path, but actual whole-job cache residency remains part
of the mature delegated-cgroup gate.

## 6. Phase E: remaining indexes and offset spaces

### E1. Backward demodulation

The compact `mask8` strategy remains complete but has demonstrated
billion-scale posting work.  Develop a byte-bounded adaptive index which admits
stronger position/code structures only for roots whose observed fallback work
justifies their construction.  Account for build and backfill work, retain the
complete fallback, and make admission deterministic and checkpointed.

A single discrimination tree is not sufficient: a variable before a selective
rigid suffix can force it to traverse the whole root.  The adaptive mode must
therefore combine cost-aware hot-root trees with cost-aware rigid-position
postings, choose an already admitted position before a tree, and allow observed
tree fanout to fund a later position admission.  CPU gates count both posting
groups and tree nodes.  Position-budget exhaustion must eventually be isolated
without globally abandoning other useful admitted positions.

Implementation status: cost-aware root and position admission, per-feature
budget isolation, complementary-position intersections, and dense structural
bitmaps are implemented.  Lossy signatures and probation now use stable
name/arity symbol hashes, so irrelevant option constants cannot perturb index
collisions or scheduling.  Rigid tree queries prune nonmatching sibling
subtrees, while explicit sibling-check counters keep the CPU gate honest.  A
fresh 1,000-given `chat_test.in` sample is 6.0% faster than the stable `mask8`
control with the same search trajectory.  This closes the bounded-prefix
correctness and crossover gate only.  A cost-gated, byte-bounded positive child
cache now removes repeated broad rigid sibling scans without affecting the
complete list fallback; 10,000- and 100,000-child longevity probes plateau at
zero hot sibling scans, and two affinity-swapped 1,000-given A/B pairs show a
1.8--2.2% CPU improvement.  Filled budgets, deletion-heavy mature rebuilds,
and the 684,719-active-record `out41` scale still need evidence before
promotion.

### E2. Unit and rewrite indexes

Run fixed-live and adversarial scaling gates against code-tree, position, and
root controls.  Add adaptive selection only if one strategy cannot stay within
the CPU/candidate gates across distributions.  Preserve the existing in-place
stale compactions.

### E3. Segmented offsets

Replace global 32-bit physical offsets which can plausibly exhaust in a month
with segmented `(segment, offset)` references or 64-bit logical references.
Use 32-bit local offsets inside bounded segments where that materially saves
RAM.  Cover record, posting, occurrence, node, and shared-token references.

Gate: tests cross segment boundaries at small configured segment sizes and
exercise the same code paths as production without allocating billions of
objects.

## 7. Validation ladder

Run gates in this order and retain failures as evidence:

1. focused differential and forced-maintenance tests;
2. ASan/UBSan focused tests;
3. `chat_test.in` parallel semantic/debug matrix;
4. 300-given four-case training and unopened/declared holdout controls;
5. 1,000-given matrix with component work and byte gates;
6. accelerated million-operation aging/scale tests;
7. 2,945- and 11,000-given comparisons on a suitable host;
8. multi-million-passive Osborn/AIM prefixes with cgroup accounting; and
9. at least one checkpoint/resume week-scale run and independent proof
   validation for every success.

Long-run promotion requires all of the existing general-compact correctness
and CPU gates, at least 80% lower total-job peak RAM than old P9 on every
passive-dominated case, no material CPU regression at the long prefixes (with
25% a hard rejection threshold unless explicitly approved), and no unexplained
component accounting gap above 5%.

## 8. Commit sequence

Keep implementation commits independently reviewable:

1. this plan and audit baseline;
2. nonunit compaction API, statistics, and focused tests;
3. fixed-live nonunit longevity driver and results;
4. selective nonunit retrieval;
5. direct archive-offset lookup and tests;
6. current-record archive directory/handle reclamation;
7. file-cache eviction/accounting;
8. common longevity runner and machine-readable reports;
9. external passive-directory format;
10. external selector runs/merge and exact-order differential tests;
11. adaptive back-demodulation promotion;
12. segmented offset migration;
13. integrated short/mid-scale results; and
14. long-run acceptance report and user documentation.

Each commit message must state the preserved theorem-proving invariant, tests
run, before/after bytes and work where meaningful, and remaining untested
scale.  Generated binaries and user-owned `00hist1` remain untracked.
