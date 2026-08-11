# General-purpose compact indexing plan

## From an Osborn-tuned prototype to a broadly usable Prover9 architecture

**Plan date:** 11 August 2026

**Starting branch:** `phase5-compact-frontier`

**Starting evidence:** `bob/chat_test.new.out1.gz`,
`bob/chat_test.new.out2.gz`, and `bob/chat_test.new.out3.gz`

## 1. Decision and objective

The current compact-OTTER implementation is logically general: it contains no
Osborn-specific symbols or proof steps, and ordinary Prover9 matching,
unification, rewriting, and subsumption still decide the final answers.
However, its performance engineering and acceptance process were too narrowly
based on Osborn and `chat_test` prefixes.  The result is a correct compact
representation whose lookup algorithms do not scale generally.

This project will correct both problems:

1. replace broad compact-index scans with structurally selective algorithms;
2. measure the cost of every index operation before tuning it;
3. develop against a frozen, diverse training suite;
4. reserve a separate holdout suite that is not used to choose algorithms or
   constants;
5. require per-problem correctness, CPU, and memory gates, so a good average
   cannot hide one catastrophic case; and
6. keep deterministic fallback modes until the new implementations pass all
   gates.

The intended product remains an OTTER-compatible search that stores passive
clause bodies outside process RAM while preserving immediate demodulation,
unit operations, backward demodulation, subsumption, hints, selection order,
and proof reconstruction.

This is not a plan to tune `compact_index_stale_pct` until one benchmark looks
better.  Those parameters control maintenance work.  They cannot repair an
index that asks thousands of live clauses or occurrences to undergo an exact
test for every query.

### 1.1 Terms used in this plan

- A **possible candidate** is a clause or term returned by an index for the
  ordinary exact operation to check.  Returning extras is safe but costs CPU;
  omitting a possible answer is incorrect.
- **Candidate amplification** is the ratio between possible candidates
  examined and actual answers.  Billions of examinations for thousands of
  answers indicate a weak index even when every index entry is small.
- A **posting list** is the stored list of clause, term, or occurrence numbers
  sharing one structural property.
- **Materialization** means reconstructing Prover9's ordinary in-memory clause
  objects from a compressed archive record.
- The **training suite** is used while choosing algorithms and parameters.  A
  **holdout suite** is frozen in advance and examined only after those choices
  are fixed; it tests whether the work generalizes.
- A **promotion gate** is a required correctness or resource result that must
  pass before an experimental implementation can become the recommended mode
  or proceed to expensive runs.

## 2. Evidence establishing the problem

The new 11,000-given comparison is sufficiently similar in logical work to
support a performance diagnosis:

| Run | User CPU | Peak RSS | Given | Generated | Result |
| --- | ---: | ---: | ---: | ---: | --- |
| Ordinary OTTER/FPA, `out1` | 5,110.38 s | 3,603,800 KiB | 11,368 | 253,338,893 | proof |
| Early compact OTTER, `out2` | 15,144.10 s | 1,492,656 KiB | 11,369 | 253,302,129 | proof |
| Current compact OTTER, `out3` | 12,201.61 s | 834,780 KiB | 11,369 | 253,302,129 | proof |

The current implementation is 19.4% faster than the earlier compact version,
but still takes 2.39 times the old-P9 CPU.  Its process peak RSS is 76.84%
lower than old P9.  Therefore the storage design is useful, but the claim of
no CPU penalty applies only to the smaller 2,945-given acceptance proof.

The `out3` counters identify four scaling failures.

### 2.1 Unit conflict

The compact unit index performed 19,190,246,832 direct unification tests for
4,206,509 unit-conflict queries, or about 4,562 tests per query.  The current
unification lookup links together units having the requested sign and root
symbol, then tests the live entries in that list.  Index compaction can remove
dead entries, but it cannot make the live same-root list more selective.

### 2.2 Backward demodulation

The compact backward-demodulation index examined 21,500,124,125 posting groups
and 20,330,927,111 individual occurrences for 1,941,156 queries.  It returned
695,309 clause candidates.  The preliminary description of a subterm is a
depth-three, eight-bit hashed path mask.  Many structurally different subterms
therefore share a bucket and are rejected only after their occurrence records
have been decoded and inspected.

### 2.3 Nonunit subsumption and archive reconstruction

The nonunit index performed 21,010,178 exact forward-subsumption tests.  The
ancestor archive materialized 21,054,705 clauses.  The near equality is strong
evidence that nonunit candidate checking causes most clause reconstruction.
The file issued 42,222,240 small reads totaling about 4.04 GB.  File I/O is not
the whole CPU regression—the additional system CPU is small—but reconstructing
and destroying millions of ordinary clause objects is avoidable work.

### 2.4 Hints

Packed hint matching processed 2.38 billion posting entries and skipped
832 million obsolete entries.  Its fixed cache avoided a further 2.63 billion
posting entries, so the cache is valuable, but the remaining work is large.
Several cache and dense-list constants were selected from short `chat_test`
profiles.  They need either a workload-independent basis or validation across
the new suite.

### 2.5 What parameter tuning can and cannot do

Small bounded experiments should still compare:

- `compact_index_stale_pct=5,10,25`;
- `compact_term_reclaim_kb=2048,8192,32768`;
- `compact_passive_cache=0,32,128`; and
- `hint_index=fpa` versus `packed_fast` when the hint collection is small.

These experiments can reveal maintenance, reconstruction, and hint tradeoffs.
They are not the principal implementation path.  No current input parameter
turns the root-only unit lookup into a unification index or enlarges the
eight-bit backward-demodulation signature into a structurally selective index.

The `out3` run also reports `compact_policy=disabled`; the final memory rerun
must use `P9_COMPACT_HEAP=1`.  That setting can affect returned memory but is
not expected to close the CPU gap.

## 3. Rules preventing another singleton optimization

These rules are project requirements rather than optional methodology.

### 3.1 Freeze workloads before changing algorithms

Before implementing a new index, commit a benchmark manifest containing:

- the complete input or a reproducible derivation of it;
- its SHA-256 digest;
- a control binary and commit;
- the exact final option values;
- fixed given, CPU, RAM, and wall-time limits;
- the boundaries at which statistics are collected;
- whether the case belongs to the training or holdout suite; and
- the logical workload category that the case represents.

The echoed input in `chat_test.new.out3.gz` must be extracted once into a
versioned benchmark input, rather than relying on a mutable external filename.
Existing large output files supply historical controls; they need not be rerun
on the current low-memory machine merely to recreate their statistics.

### 3.2 Separate training from holdout evaluation

Approximately two thirds of the real cases will form the training suite.  The
remaining third will be marked holdout before implementation starts.  Holdout
results may be examined only at phase promotion gates, not while choosing
features, thresholds, or cache sizes.

If a design fails a holdout gate, the failure becomes a new documented
requirement.  The next design revision is tested against a newly frozen
holdout partition as well as all former failures.  We do not repeatedly tune a
constant against the same nominal holdout until it passes.

### 3.3 Prefer structural guarantees over magic constants

A parameter is acceptable when it controls an explicit resource budget, for
example maximum cache bytes or maximum dead bytes.  A constant justified only
by “98.7% of queries in one 1,000-given run” is not a general design argument.

Index decisions that affect only performance should be based on deterministic
structural quantities such as posting lengths, live/dead bytes, tree fanout,
and the number of required query features.  They must not depend on wall-clock
timing, machine load, or nondeterministic iteration order.

### 3.4 Report distributions, not just averages

Every candidate-producing operation must report at least these answer-size
and work distributions:

- 0, 1, 2--7, 8--31, 32--127, 128--1,023, 1,024--16,383, and 16,384 or more;
- median, 95th percentile, 99th percentile, and maximum;
- live, dead, and duplicate records examined;
- exact tests and successful results;
- bytes read or decoded; and
- time in lookup, exact checking, materialization, and maintenance.

A rare query that scans a million clauses must be visible even if the mean is
small.

### 3.5 Require per-case gates

Aggregate geometric means will be reported, but every case also has a maximum
slowdown and memory limit.  Promotion is forbidden if one workload becomes
catastrophic while the others improve.

## 4. Frozen validation matrix

The exact manifest will be committed in Phase 1.  It must cover the following
classes.

### 4.1 Focused and adversarial index cases

Deterministic C tests will construct terms and clauses designed to expose the
known weak cases:

- many unit clauses with the same root but different deep symbols;
- ground, linear-variable, repeated-variable, and variable-heavy unit terms;
- equality units requiring ordinary and flipped lookup;
- backward-demodulation subjects with equal roots and shallow shapes but
  different deeper paths;
- demodulator left sides ranging from ground to highly variable;
- clauses with many occurrences of the same symbol;
- nonunit clauses sharing numerical feature vectors but failing exact
  subsumption;
- repeated insert, rewrite, delete, rebuild, and clause-number reuse patterns;
- hint collections with few/many hints, broad/narrow postings, `_AnyConst`,
  frequent rewriting, and high obsolete-entry rates; and
- deliberately poor hash distributions and monotonically increasing large
  clause numbers.

The test oracle is the established Prover9 operation.  Generated compact
answers must contain every legacy answer, and after the ordinary exact test
the ordered answers must agree exactly.

### 4.2 Short real development cases

These run first on every substantial change:

1. the normal `make test1` suite;
2. `chat_test.in` at 100 and 300 givens, with deterministic sampled hints and
   then all hints;
3. the extracted large-input case at 100, 300, and 1,000 givens;
4. at least one AIM problem dominated by hyperresolution and having no
   demodulators, for example `nil3_1K.in`;
5. at least one larger no-demodulator AIM case, for example `aa_to_nil3.in`;
6. at least one mixed unit/nonunit subsumption problem from the standard
   Prover9 tests; and
7. at least one rewrite-heavy, variable-rich equational prefix distinct from
   Osborn.

The point of the no-demodulator cases is not RAM stress.  They ensure that an
index optimized for large equational searches does not slow ordinary
hyperresolution work or allocate large unused structures.

### 4.3 Medium cases

After short gates pass:

- full-hint Osborn/`chat_test` at 1,000 givens;
- the extracted large-input case at 2,000--4,000 givens;
- selected AIM inputs such as `mbol_nil3_a.in` and a nilpotency case;
- a hint-rewrite stress case; and
- synthetic unit and backward-demodulation indexes with at least one million
  live records but no full theorem-proving search.

### 4.4 Long acceptance cases

Only a suitable host should run:

- the current 2,945-given full-hint acceptance proof;
- the 11,000-given input represented by `chat_test.new.out1/out3`;
- an Osborn prefix with millions of retained passive clauses;
- an AAPERM prefix with millions of passives and demodulators; and
- at least one large holdout problem not used to choose the implementation.

Short correctness/counter runs may execute on separate real cores.  CPU
comparisons used for a promotion gate must run sequentially on an otherwise
quiet machine; parallel runs can contend for caches and memory bandwidth.

## 5. Correctness and ownership invariants

Every phase must preserve these invariants.

1. Compact indexes store stable clause numbers and compact term references;
   they do not own a second forest of ordinary clause and term objects.
2. An index filter may return extra candidates but may never omit a possible
   match, unifier, rewrite target, or subsumption partner.
3. Ordinary Prover9 logic remains the final authority until a direct compact
   operation has its own differential proof and promotion gate.
4. Candidate ordering is deterministic.  Where old search behavior depends on
   the first answer, final exact answers appear in the same order as the
   appropriate control.
5. Variables are standardized and interpreted exactly as in the existing
   matcher/unifier, including repeated variables, occurs checks, and equality
   flipping.
6. Insert, delete, backward rewrite, disable, rebuild, checkpoint, and resume
   preserve both logical membership and version state.
7. Maintenance memory has an explicit bound.  Rebuilding cannot silently hold
   complete old and new indexes plus unbounded translation arrays in RAM.
8. Any adaptive policy is deterministic from logical/index counters and is
   saved in checkpoints when its state affects later decisions.
9. Diagnostic and fallback modes remain available until the new mode passes
   the complete matrix.

## 6. Instrumentation required before further optimization

The present aggregate counters establish that work is excessive but do not
attribute enough CPU.  `out3` also has zero-valued hint clocks because the
relevant clock reporting was not enabled.  Phase 1 will add or activate the
following measurements.

### 6.1 Unit index

- queries separated into generalization, instance, and unification;
- nodes or postings visited;
- live, inactive, same-root, and structurally compatible records;
- direct compressed-term exact tests and successes;
- result sorting and allocation time; and
- candidate-count histograms for each operation.

### 6.2 Backward demodulation

- root buckets and structural-feature buckets visited;
- posting blocks and bytes decoded;
- live, dead, duplicate, and exact occurrence checks;
- successful occurrences and distinct clause candidates;
- time in bucket lookup, posting decoding, compact matching, clause
  materialization, and rebuilding; and
- the worst queries, reported by stable demodulator clause number and counts,
  without printing complete clauses by default.

### 6.3 Nonunit subsumption

- feature-index nodes/postings visited;
- possible clauses returned;
- archive materializations;
- exact subsumption attempts and first-success position;
- successful forward and backward answers; and
- separate lookup, archive, and exact-test time.

### 6.4 Hints

- cache lookup and validation time;
- sparse and bit-set intersection time;
- live and obsolete posting entries inspected;
- exact direct versus reconstructed matching time;
- rebuild time and bytes; and
- candidate distributions per equivalence, match, flipped match, and backward
  demodulation operation.

### 6.5 Archive and maintenance

- archive lookup, read, checksum, decode, ordinary-object construction, and
  release time;
- cache hits by reuse distance and object size;
- each index rebuild's CPU, peak temporary bytes, input live/dead records, and
  output bytes;
- shared-term reorganization CPU and temporary disk/RAM; and
- process RSS/PSS and cgroup file-cache measurements at fixed boundaries.

Top-level query clocks should be inexpensive enough for bounded profiling.
Any per-record tracing must be sampled or enabled only by a diagnostic option.
Machine-readable summary lines are required so the benchmark harness does not
scrape human prose.

The same profiling mode must count candidate retrieval in the ordinary FPA,
discrimination-tree, and feature-vector indexes.  Those controls provide the
denominators for gates such as “within twice the legacy candidate count”; CPU
time alone cannot reveal whether a new index retrieves the same-sized answer
more slowly or retrieves vastly more possible answers.

## 7. Target architecture

### 7.1 Reusable exact structural postings

Create a reusable compact posting component for clause and occurrence indexes.
A feature key represents an exact structural fact such as “symbol `f` occurs
at relative term position `1.2`.”  Its posting list contains increasing compact
record or occurrence numbers.  The component provides:

- append and versioned deletion;
- rarest-list selection;
- intersection or membership tests without decoding unrelated lists;
- compressed blocks with skip information;
- canonical decreasing-clause-number output when required;
- deterministic bounded rebuilding;
- current, dead, and temporary byte counters; and
- no dependency on live `Topform` pointers.

This component should be shared where its semantics fit, but the project must
not force unit unification, backward matching, and subsumption into one
average structure.  They have different compatibility rules.

### 7.2 Unit operations: separate retrieval semantics

The unit index currently has a useful compact path structure for some matching
operations but uses a root-symbol list for unification.  The replacement must
treat these as separate retrieval problems:

- **generalization:** stored unit matches the query;
- **instance:** the query matches the stored unit; and
- **unification:** substitutions on both sides make the terms equal.

Two implementations should be prototyped behind a diagnostic strategy option.

#### Position-compatible postings

For every rigid path—one whose ancestors are fixed function symbols—record the
symbol at that path.  For a query rigid feature, a unification candidate is
compatible if it has the same symbol there or contains a variable at an
ancestor that covers the path.  Intersect the rarest safe compatibility sets,
then run the existing direct compressed unifier on survivors.

This is easy to audit and can reuse the compact term pool.  It must not use a
fixed shallow depth merely because one benchmark was shallow.  Features are
added under an explicit bytes-per-live-unit budget, with preference for paths
whose postings actually divide large candidate sets.

#### Substitution or code tree

Prototype a compact substitution-tree or code-tree retrieval path over the
serialized terms.  Such an index performs more of unification while traversing
the structure and is likely to handle variable-rich terms better than a finite
set of path features.

The Phase-2 gate, not preference or Osborn alone, selects the production
implementation.  A hybrid is acceptable if its deterministic choice is based
on tree fanout and query structure and both paths return the same ordered
answers.

The production unit index must not scan all live same-root units unless the
query genuinely has a comparably large answer set.

### 7.3 Backward demodulation: index occurrences by exact paths

Replace the eight-bit path mask as the primary filter.  It may remain as a
cheap secondary check, but it cannot select the large posting stream.

For every indexed subterm occurrence, record exact safe features keyed by
relative position and symbol.  A demodulator left side contributes features
only at positions above which it has no variable.  A matching subject must
possess all of those exact features.  Start from the rarest feature and either
intersect the remaining lists or test membership in them.  Return occurrence
references, not merely clause IDs, so the exact compact matcher examines only
the relevant positions.

In parallel, prototype a radix-compressed discrimination/code tree over
serialized subterms.  Compare it with exact position postings on:

- bytes per live occurrence;
- nodes/posting bytes decoded per query;
- exact occurrence tests;
- insertion/deletion and rebuilding CPU; and
- worst-case variable-rich queries.

For inherently broad patterns, fall back to a sequential compressed scan in
large blocks rather than millions of random file reads.  Such a fallback must
be counted explicitly and must not become the common path unnoticed.

### 7.4 Nonunit subsumption: reject before materialization

Retain standard safe numerical clause features, but add a multi-feature
posting layer for selective properties such as:

- signed literal and equality counts;
- predicate/root-symbol multiplicities;
- selected rigid path symbols;
- term-depth and symbol-count lower bounds; and
- literal-shape summaries that are necessary for subsumption.

Use measured posting lengths to choose a safe intersection; do not hard-code
one global feature order.  Preserve the ordinary subsumption routine for the
final decision initially.

After candidate amplification is controlled, add a compact clause view that
allows cheap failure tests—and eventually full subsumption—to operate directly
on serialized literals.  Direct compact subsumption is promoted only after
differential testing covers literal permutation, variable consistency,
equality, attributes, and all current subsumption options.

### 7.5 Hint indexing: general policy, not one fixed profile

Keep `fpa` and `packed_fast` as explicit choices during development.  For a
small hint collection, ordinary FPA may be the correct speed/RAM tradeoff; this
must be measured rather than assumed.

Improve packed hints by:

- triggering deletion/rebuilding from observed obsolete entries scanned per
  query as well as total obsolete storage;
- sizing the cache by an explicit byte budget;
- allowing complete variable-length query keys within that budget instead of
  a fixed maximum chosen from one prefix;
- retaining sparse versus bit-set choice based on actual list lengths;
- preserving direct compact unit matching and `_AnyConst` semantics; and
- canonicalizing final candidate order so representation changes cannot alter
  hint behavior.

An eventual `hint_index=auto` may choose FPA or packed storage from hint-set
size, feature-list distribution, rewrite settings, and an explicit RAM budget.
It must be deterministic and report its choice.  It cannot become the default
until both representations have passed the holdout suite.

### 7.6 Archive access: avoid creating ordinary objects for failures

The file archive remains the single owner of compact passive bodies.  The next
goal is not to remove file access at any cost, but to avoid reconstruction when
a compact test can reject a candidate.

Introduce a read-only compact clause view exposing serialized literals,
attributes needed by the operation, and justification metadata.  Unit and
backward-demodulation exact checks already operate largely on compact terms;
nonunit filtering should do the same before requesting a `Topform`.

Retain a byte-bounded materialization cache as an experiment.  Admission should
require observed reuse—for example, cache on a second access—so a one-pass scan
cannot fill it with objects that will never be used again.  Cache policy and
capacity are deterministic, reported, and included in the RAM total.

### 7.7 Adaptive maintenance with explicit bounds

Replace unrelated fixed maintenance percentages with deterministic cost and
memory rules:

- rebuild when dead bytes exceed an explicit fraction of live bytes **or**
  measured dead scans exceed the estimated work of a rebuild;
- reorganize the shared term pool when reclaimable bytes exceed both a minimum
  byte budget and a live-data fraction;
- cap temporary rebuild RAM and sort large translations through the file
  backend as today; and
- log the reason, input/output sizes, CPU, and next trigger after every rebuild.

Time measurements are for reporting, not policy decisions.  Decisions use
logical work counters so the same input and options behave identically on a
different machine.

## 8. Delivery phases and stop/go gates

### Phase 0: correct the claim and freeze baselines

- Amend the technical report so “no CPU penalty” is explicitly limited to the
  2,945-given proof.
- Add the `out3` 11,000-given result and its 2.39-times CPU ratio.
- Extract and hash the echoed large input.
- Commit the training/holdout manifest and commands.
- Record current option values, binary hashes, host details, and all existing
  output digests.

**Gate:** another developer can reproduce every short control and identify
exactly which long results are historical artifacts rather than new runs.

### Phase 1: attribution and benchmark harness

- Add the clocks, histograms, and worst-query counters from Section 6.
- Add a parser producing one TSV/JSON row per run and operation.
- Extend `test.src/chat_test_matrix.sh` or add
  `test.src/compact_generalization_matrix.sh` with explicit CPU/RAM limits.
- Add focused adversarial generators and differential tests.
- Run short cases first; no multi-hour search is authorized by this phase.

**Gate:** at least 95% of compact-index CPU in the 300/1,000-given profiles is
assigned to lookup, exact checking, materialization, or maintenance, and all
candidate/work distributions are populated.

### Phase 2: replace root-only unit unification

- Add an experimental position-posting strategy.
- Add a substitution/code-tree prototype if position postings fail the
  variable-rich or worst-case gates.
- Stream or reuse result storage instead of allocating and sorting large
  candidate arrays when the caller can consume candidates in canonical order.
- Differentially test all unit operations, equality flips, variables,
  mutation, and checkpoint rebuild.

**Gate:** on every training case, exact unit-unification tests are no more than
twice the legacy FPA candidate count, and no query scans a fixed fraction of
all same-root units unless the legacy result itself is broad.  The extracted
large 1,000-given prefix must show at least a 100-fold reduction from its
current same-root candidate amplification without exceeding the unit-index RAM
budget.

### Phase 3: replace the eight-bit backward-demodulation filter

- Implement exact position postings and occurrence references.
- Prototype the compressed tree alternative.
- Preserve compact direct matching and clause-level deduplication.
- Add deterministic handling for broad variable patterns and rebuilding.

**Gate:** exact returned clause IDs and order agree with the ordinary index;
posting groups/occurrences examined fall by at least two orders of magnitude on
the current large prefix; and no training case increases examined work by more
than 25% relative to its legacy candidate retrieval without a documented
reason.

### Phase 4: strengthen nonunit filtering and compact clause views

- Add selective multi-feature postings.
- Measure first-success position and avoid materializing candidates rejected by
  compact necessary checks.
- Add direct compact failure checks, followed by full direct subsumption only
  if needed and independently validated.
- Evaluate the reuse-admission materialization cache.

**Gate:** archive materializations for nonunit subsumption fall by at least 90%
on the extracted large prefix or to within 1.5 times the legacy exact-candidate
count, whichever is the more meaningful bound.  Search events and proof output
remain exact against the same-hint-representation control.

### Phase 5: generalize hint storage and lookup

- Run FPA versus packed-fast on small, medium, and large hint collections.
- Replace single-workload cache limits with byte-budgeted complete keys.
- Reduce obsolete-entry scanning and retain direct compact matching.
- Add a diagnostic deterministic auto policy, but keep explicit modes.

**Gate:** each explicit mode remains correct; the selected production policy
has no training-case CPU slowdown above 20% relative to the faster explicit
mode unless it buys a measured, necessary RAM saving; holdout remains unopened.

### Phase 6: general maintenance and memory control

- Add counter-based rebuild decisions and explicit temporary-memory limits.
- Run mutation longevity and checkpoint/resume stress tests.
- Verify the GNU allocator policy, archive location, disk exhaustion behavior,
  and cgroup accounting.

**Gate:** dead and temporary storage remain within documented bounds through
repeated rewrite/delete cycles, and checkpointed/uninterrupted event streams
are identical.

### Phase 7: holdout and long-run acceptance

- Freeze the candidate commit before opening holdout results.
- Run all short holdout cases and then medium cases.
- Only after they pass, run the long 2,945-, 11,000-given, Osborn, and AAPERM
  cases on a suitable host.
- Update reports with all successes and failures, not only the best case.

**Gate:** all criteria in Section 10 pass.  If they do not, retain the old and
experimental strategies explicitly and do not label the new mode generally
accepted.

## 9. Focused tests to add

### `compact_unit_index_test`

- differential random terms with fixed seeds;
- adversarial same-root families;
- generalization, instance, and unification answer/order comparisons;
- nonlinear variables and occurs-check cases;
- equality flip and sign handling;
- live/dead mutation and rebuild; and
- million-record synthetic scaling without full clause bodies.

### `compact_back_demod_test`

- exact path-feature construction;
- deep symbol differences hidden by the former eight-bit mask;
- repeated occurrences and clause deduplication;
- variable-rich patterns and broad fallback;
- compressed-block skipping and corrupted-block failure;
- deletion/rewrite/rebuild order; and
- comparison with ordinary `back_demod_indexed` answers.

### `compact_feature_index_test`

- feature necessity for every supported clause form;
- clauses intentionally colliding on coarse vectors;
- ordered forward first-answer behavior;
- backward answer completeness;
- compact-view rejection versus ordinary subsumption; and
- mutation and large clause-number ranges.

### Hint tests

- FPA/packed/auto differential event logs;
- cache keys with more than eight features;
- sparse/dense crossover across many list distributions;
- rewrite churn with direct removal and rebuilding;
- `_AnyConst`, equality flip, degradation, and `match_once`; and
- checkpoint restoration of policy and mutable state.

### Whole-search tests

- complete 300-given ordered event digest;
- 1,000-given state and per-operation work gates;
- checkpoint at 100 and resume to 301;
- proof validation with `prooftrans parents_only` and `directproof`; and
- low-RAM failure paths that report limits without corrupting the archive.

## 10. Acceptance criteria

### 10.1 Correctness

- zero differential/audit mismatches in focused, random, training, and holdout
  tests;
- identical ordered event logs for representation-only comparisons using the
  same hint representation;
- deterministic candidate order and statistics across repeated runs;
- identical uninterrupted and resumed post-checkpoint streams; and
- every emitted proof accepted by the independent proof tools.

### 10.2 CPU

- geometric-mean user CPU no more than 1.15 times the appropriate old-P9
  control across the real suite;
- no individual promoted case above 1.35 times its control;
- the 11,000-given case no more than 1.25 times old P9 before production
  promotion, with parity as the target;
- no hyperresolution case without demodulators slowed by more than 10% from unused
  compact infrastructure; and
- index maintenance below 10% of total user CPU in steady-state long runs.

### 10.3 Candidate quality

- compact exact-candidate counts within twice the legacy index counts for unit,
  backward-demodulation, and nonunit operations, unless the actual answer is
  itself large;
- no hidden billion-scale scan in any training or holdout case;
- 99th-percentile and maximum query work reported and reviewed; and
- archive materializations proportional to final exact candidates, not to a
  coarse feature bucket.

### 10.4 Memory

Two gates distinguish engineering progress from the original radical product
claim.

1. **Index-replacement gate:** the new selective indexes may use at most 20%
   more RAM than the current compact indexes at a matched boundary, while the
   whole process must retain at least a 70% saving over old P9 on every
   passive-dominated case.
2. **Radical production gate:** across at least three large passive-dominated
   training/holdout problems, total-job peak memory—including cgroup file
   cache—must be at least 80% below old P9, with an 80--90% geometric-mean
   target and no unexplained memory component.

RSS, PSS, archive logical/disk bytes, and cgroup file cache are reported
separately.  An mmap length or Prover9's internal `Megabytes` counter is never
used alone as total RAM.

### 10.5 Generalization

- all workload categories in Section 4 represented in both development and
  acceptance results;
- holdout gates passed without post-hoc threshold tuning;
- every promoted constant justified by a byte/work bound or multi-case data;
- explicit fallback mode for workloads outside the measured range; and
- documentation states the observed domain and any remaining exceptions.

## 11. Configuration and compatibility strategy

During development, keep current behavior available under explicit diagnostic
strategies.  Proposed names are illustrative and should be finalized with the
option documentation:

```text
assign(compact_unit_strategy,root_scan|position|substitution_tree|auto).
assign(compact_back_demod_strategy,mask8|position|code_tree|auto).
assign(compact_nonunit_strategy,prefix|postings|auto).
assign(compact_profile,off|summary|full).
```

The old strategy names make regressions reproducible.  `auto` is not a license
for nondeterminism: it chooses from index size, posting distribution, query
structure, and explicit budgets, prints the choice, and saves relevant state
in checkpoints.

`passive_store=dense` currently requires all four authoritative compact
indexes because ordinary pointer indexes cannot refer to archived clause
bodies.  Component attribution should therefore use `passive_store=full` with
one compact authoritative index at a time on bounded prefixes.  Production
mixed modes require stable-ID versions of every index that must see archived
clauses; silently omitting one is never allowed.

## 12. Implementation map

Expected primary files are:

- `provers.src/compact_unit_index.[ch]`: unit retrieval strategies and direct
  token unification;
- `provers.src/index_lits.c`: authoritative/audit integration and ordered
  candidate consumption;
- `provers.src/compact_back_demod.[ch]`: exact occurrence features or compact
  tree retrieval;
- `provers.src/demodulate.c`: candidate resolution, timing, and audit;
- `provers.src/compact_feature_index.[ch]`: stronger nonunit candidate
  filtering;
- `provers.src/compact_term_pool.[ch]`: shared serialized terms and stable
  references through reorganization;
- `ladr/hint_postings.[ch]` and `ladr/hints.c`: byte-budgeted general hint
  indexing;
- `ladr/clause_store.[ch]`: read-only compact views and archive attribution;
- `provers.src/search.c` and `search-structures.h`: options, validation,
  statistics, checkpoints, and lifecycle;
- `test.src/*compact*test.c`: focused and adversarial tests;
- `test.src/chat_test_matrix.sh`: fast search-level regression; and
- a new benchmark manifest/parser under `test.src` or `scripts`.

If exact structural postings are useful to several prover-level indexes, add a
small `provers.src/compact_postings.[ch]` module rather than duplicating
incompatible encodings.  It must remain independent of complete clause
objects.

## 13. Risks and responses

### Selective indexes consume too much RAM

Use shared token sequences, compressed posting blocks, radix-compressed paths,
and explicit bytes-per-live-record budgets.  Measure whether each additional
feature saves enough exact tests to justify its bytes.  Do not restore a full
pointer-rich term forest merely to recover speed.

### Variable-heavy queries are inherently broad

Compare position postings with substitution/code trees.  Detect genuinely
broad answers separately from false-candidate amplification.  For unavoidable
broad work, process compressed blocks sequentially and report it.

### Adaptive rebuilding changes search behavior

Maintenance may change layout, never logical membership or final candidate
order.  Canonically order exact answers and checkpoint policy state.  Use
logical counters, not time, for decisions.

### A cache hides the problem

Caches have explicit byte limits and reuse statistics.  A candidate index must
pass selectivity gates with the cache disabled.  The cache is an additional
optimization, not a substitute for indexing.

### Long runs consume scarce resources before a defect is visible

Require focused, 100-, 300-, 1,000-, and medium-given gates in that order.
Use existing downloaded statistics as baselines.  Apply explicit CPU/RAM/wall
limits, and reserve week-scale runs for candidate commits that have already
passed holdout prefixes.

### Exact old-P9 output differs because hint representation differs

Use two controls: the same-hint-representation full-body control for exact
event equality, and raw old P9 for proof, CPU, and RAM comparisons.  State
which claim each comparison supports.

## 14. Commit and review discipline

Keep commits small enough to inspect and bisect.  Planned boundaries are:

1. this generalization plan and corrected large-run diagnosis;
2. frozen benchmark manifest and extracted input digests;
3. per-operation timing and distribution instrumentation;
4. adversarial differential generators and harness;
5. reusable exact structural posting primitive;
6. experimental unit position index and tests;
7. unit substitution/code-tree prototype if required;
8. promoted unit strategy with multi-workload evidence;
9. backward-demodulation position index and tests;
10. backward tree prototype/promotion if required;
11. nonunit multi-feature filtering;
12. compact clause views and materialization reduction;
13. general hint policy and stale-entry control;
14. deterministic maintenance policy and checkpoint hardening;
15. training-suite report;
16. frozen holdout evaluation; and
17. long-run acceptance and user documentation.

Every implementation commit body should record:

- the ownership and correctness invariant preserved;
- the algorithmic change in theorem-proving terms;
- new or changed options and statistics;
- focused and search-level commands run;
- before/after candidate, CPU, and byte counters on more than one workload;
- known failures or untested scale; and
- why any new constant is general rather than selected for one problem.

No commit should combine a new retrieval algorithm, unrelated storage rewrite,
and acceptance-document update.  Reviewers must be able to identify which
change produced each performance result.

## 15. Recommended immediate sequence

1. Correct the report with the `out3` result.
2. Freeze and hash the diverse benchmark manifest and extracted large input.
3. Add clocks and distributions; run only bounded prefixes.
4. Isolate components with full passive bodies and one compact authoritative
   index at a time.
5. Implement the position-compatible unit index first, because 19.19 billion
   unification tests are the clearest avoidable cost.
6. Replace the backward-demodulation eight-bit filter next.
7. Reduce nonunit materialization, then reassess hints using FPA as an explicit
   low-hint-count alternative.
8. Open holdout results only after the candidate algorithms and constants are
   frozen.
9. Run the 11,000-given and multi-million-passive cases only after all shorter
   gates pass.

This sequence attacks measured general scaling failures while making it
procedurally difficult to declare success from one favorable theorem-proving
run again.
