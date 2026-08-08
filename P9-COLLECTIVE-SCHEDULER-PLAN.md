# Stronger collective scheduler plan

Date: 2026-08-08 (Europe/Berlin)

Branch: `better-collective-scheduler`

Status: implementation in progress on the opt-in development branch.

## Implementation log

### 2026-08-08: Phase 0 attribution

- Frozen artifact hashes and configurations are recorded in
  `P9-COLLECTIVE-SCHEDULER-BASELINES.md`.
- `test.src/osborn_collective_controls.sh` prepares the four bounded control
  policies with explicit time, memory, and given limits.
- Aggregate reporting now distinguishes physical raw enumeration from
  emitted/replayed candidates, reports pending rule work and exact descriptor
  lag quantiles without allocating per-descriptor report memory, counts
  cumulative hint-selected givens by selector, and reports distinct matched
  hints.
- Periodic reports include interval deltas for raw visits, committed generated
  clauses, hint-selected givens, and newly matched distinct hints.
- The legacy collective schedule and proof traces are unchanged; the focused
  collective, DISCOUNT, hint-index, hint-checkpoint, and standard proof suites
  pass.

### 2026-08-08: Phase 1 split work and bounded admission

- `collective_scheduler=balanced_hint` is a separate opt-in policy; the
  default remains `legacy` and old `P9COLL5`--`P9COLLA` checkpoints continue
  to select only that policy.
- Balanced activation creates independent descriptors for positive hyper,
  negative hyper, paramodulation from the given, and paramodulation into the
  given.  No rule is hidden behind another rule's descriptor.
- Deterministic weighted lane service is supplemented by a mandatory oldest
  queue turn.  Rule weights, the oldest-turn interval, descriptor high/low
  watermarks, and activation-lag pressure are explicit options.
- Admission reserves the maximum number of descriptors one activation can
  create and asserts the hard high-water on every append.  Drain mode uses
  hysteresis and records entries, exits, and withheld givens.
- In dense mode the candidate bound measures transient exposed bodies rather
  than already-compacted passive records.  This permits inference draining
  without rebuilding the full passive-body frontier or deadlocking two
  unrelated logical cardinality bounds.
- `P9COLLB` records the policy, lane/credit position, mandatory-fairness
  position, and drain state.  Policy mismatches fail closed.
- `collective_balanced_test.sh` forces all rule lanes, both paramodulation
  directions, high-water drain/recovery, a peak descriptor bound of eight,
  and deterministic checkpoint/resume.  Existing legacy collective,
  DISCOUNT, hint-index, and hint-checkpoint tests remain unchanged and pass.

Phase 1 deliberately still uses the replay generators.  Its reports do not
claim a bounded raw-work turn; native continuation is the next phase.

### 2026-08-08: Phase 2 native paramodulation continuation

- The LADR paramodulator now exposes a stable continuation consisting of the
  from literal, into literal, equality side, atom argument, and a dynamically
  bounded subterm path.  It resumes directly at that coordinate instead of
  regenerating an ordinal prefix.
- Every eligible subterm visit is charged, including failed unifications and
  roots suppressed by `check_top`.  `collective_raw_work_budget` is therefore
  a hard per-turn paramodulation bound rather than a conclusion-only target.
- Balanced from/into descriptors use the native iterator and the ordinary
  `cl_process` callback.  Existing legacy descriptors and a partially replayed
  `P9COLLB` pair retain their compatibility path until that pair completes.
- `P9COLLC` serializes the complete paramodulation coordinate and path in
  addition to scheduler state.  Dynamic path bytes are included in descriptor
  memory accounting.
- `paramod_iterator_test` compares eager structural sequences with every
  forced raw budget from 1 through 64, for both directions, `check_top`, and
  ordered/instance-checked operation.  It clones and resumes the remaining
  suffix after every unit-budget coordinate boundary.
- The balanced proof test forces a raw budget of one, observes a raw-turn peak
  of one and zero replay, and validates the proof with `prooftrans`.  The
  policy checkpoint/resume comparison, legacy focused suites, and `make test1`
  pass.

### 2026-08-08: Phase 3 native hyperresolution continuation

- Hyperresolution now has a pointer-free continuation over stable historical
  parent/literal coordinates, nested satellite choices, normal/flipped mate
  phases, the built-in `x=x` phase, and the outer satellite/nucleus cursor.
  Selected substitutions are reconstructed only to nucleus depth; no raw
  conclusion prefix is regenerated or retained.
- Historical parents and literals are visited in reverse append/insertion
  order, matching the FPA oracle while remaining directly checkpointable.
  Inactive-parent and literal visits consume raw work, so a long rejected
  prefix cannot evade `collective_raw_work_budget`.
- Both positive and negative hyperresolution, and both given-as-nucleus and
  given-as-satellite paths, use the bounded iterator in balanced mode.  The
  legacy FPA/replay implementation remains unchanged as an oracle and for old
  partially replayed checkpoints.
- `P9COLLD` serializes outer/nucleus cursors, nested choices, resume positions,
  and mate phases.  Choice-array memory is included in descriptor accounting.
- `hyper_iterator_test` compares exact eager sequences for positive/negative
  nucleus and satellite cases at budgets 1--64 and restores the exact suffix
  after every unit raw step.  The balanced hyper proof observes a raw-turn
  peak of one with zero replay and passes `prooftrans`.
- Paramod/hyper iterator tests, balanced checkpoint/fairness/backpressure,
  all focused legacy suites, and `make test1` pass together.

### 2026-08-08: Phase 4 bounded windows and read-only preview

- Balanced iterators now transfer every yielded raw conclusion into one
  global heap-owned candidate pool.  `collective_candidate_cache` remains the
  hard count bound across the pool and current limbo body, while
  `collective_candidate_window` and
  `collective_candidate_commit_interval` bound cross-descriptor lookahead and
  force regular authoritative commits.  One oldest-insertion commit per
  `collective_candidate_fair_interval` makes every resident window member
  finite-delay work even under an unbounded stream of better advisory keys.
- Preview normalization runs on an owned scratch copy, queries the selected
  exact hint index with the same flipped-unit and degradation rules, and
  evaluates the actual high/low given-selection properties.  CAC discovery is
  excluded from preview and forward-demodulation/packed-hint accounting is
  suppressed, so the query cannot change active simplifiers, hint epochs,
  match counters, labels, IDs, or authoritative operation statistics.
- Heap keys use high/low selector priority, exact hint ID, adjusted weight,
  given activation age, raw ordinal, and insertion ordinal.  They are advisory
  only: every entry is sent through the unchanged `cl_process`, and predicted
  versus authoritative matcher outcomes are counted there.
- Entries carry hint and simplifier epochs.  A stale heap top is normalized,
  rematched, and reheapified before it can be committed; lazy refresh work and
  false positives/changed IDs are reported explicitly.
- `P9COLLE` adds each descriptor's monotonically increasing candidate ordinal
  and a bounded `collective_candidates.txt` containing live pool bodies and
  deterministic preview metadata.  `checkpoint_candidate_pool` provides a
  one-shot bounded test/operations trigger; old balanced `P9COLLB`--`P9COLLD`
  checkpoints remain accepted with an initially empty pool.
- `hint_preview_test` audits FPA, packed-legacy, and packed preview purity and
  exact matcher/weight agreement.  The balanced integration test forces hint
  matches, count/byte accounting, stale refresh, and checkpoint/resume with a
  nonempty candidate pool.

### 2026-08-08: Phase 5 bounded dual-cursor discovery

- Every balanced rule descriptor now owns independent native fair and
  discovery continuations.  The discovery continuation is cloned from the
  fair coordinate once and then advances monotonically; it never restarts a
  raw prefix.  A looked-ahead conclusion is therefore visited at most once by
  discovery and once by the authoritative fair iterator.
- Exact previewed hint matches may enter the existing bounded global pool
  early.  Each promotion records its local raw ordinal and an unsimplified
  structural fingerprint.  When the fair cursor reaches that ordinal it must
  reproduce the fingerprint exactly, removes the bounded record, and deletes
  the duplicate raw body instead of committing it twice.
- Per-descriptor promotion count and conclusion-distance caps stop lookahead
  until fair work catches up.  Discovery turns also have their own raw-work
  budget, alternate with mandatory fair descriptor turns, favor known hot
  descriptors, and reserve a deterministic low-rate turn for the oldest
  ordinary descriptor.
- Promotions remain advisory.  They pass through unchanged authoritative
  `cl_process` simplification and hint matching; every committed promotion is
  counted as confirmed or as a preview false positive.  Pool fairness still
  guarantees finite-delay commitment even if better preview keys continue to
  arrive.
- `P9COLLF` serializes the independent iterator coordinates, scheduler cycle
  positions, discovery flags, and bounded ordinal/fingerprint sets.
  `P9CPOOL3` marks promoted entries.  `P9COLLE`/`P9CPOOL2` checkpoints remain
  readable with empty discovery state, while a mismatched frontier/pool pair
  fails closed.
- `checkpoint_discovery_promotions` provides a deterministic one-shot test
  trigger.  The integration suite checkpoints with both a promoted pool body
  and an ahead-consumed record live, verifies the pool marker, resumes, and
  obtains the same terminal aggregate scheduler state as an uninterrupted
  run.  It also requires hot and general discovery, forced fair service,
  exact promotion/confirmation/skip counts, bounded distance, completed work
  in every rule lane, and zero residual consumed records.

## Objective

Make the collective DISCOUNT frontier discover and propagate useful hint
matches without returning to the old representation in which millions of
passive clauses are materialized and indexed at once.

The stronger scheduler must address the Osborn failure mode measured in
`../bob/rr_osbe.out2-unl-rad-comp6h.gz`: a large given count accompanied by
very little completed inference work and a nearly exhausted hint stream.  It
must retain the radical RAM architecture, exact hint semantics for every
committed clause, sound proofs, fair eventual inference, and deterministic
checkpoint/resume.

This is not a promise to reproduce the OTTER given-clause trace.  It is a
plan to make the new calculus perform enough of the right inference work soon
enough for the hints to guide it in practice.

## Evidence and diagnosis

The six-hour full-hint run used DISCOUNT, dense passives, the compact hint
index, and the collective frontier.  It also used a four-givens-per-expansion
ratio and explicitly disabled all three experimental discovery aids.

At the last complete report it had:

| Measurement | Value |
| --- | ---: |
| Given clauses | 178,177 |
| Generated clauses reaching `cl_process` | 6,512,790 |
| Kept clauses | 297,330 |
| Hint-selected givens (`Hha` + `Hw` + `LH`) | 1,872 |
| Distinct input hints matched at least once | 301 |
| Collective batches created | 178,177 |
| Collective batches completed | 6 |
| Collective batches pending | 178,171 |
| Hyper turns / completed hyper sets | 99,156 / 315 |
| Paramodulation turns / completed pairs | 1,084 / 1,084 |
| Candidate conclusions emitted / replayed | 6,333,161 / 2,421,696 |

The archived OTTER proof generated 212,152,996 clauses by given 11,242 and
kept 11,131,760.  Its filtered `outaH` file contains 11,048 hint-selected
givens: 10,048 `Hha` and 1,000 `Hw`.  It does not report how many distinct
input hints these clauses represented.

The important comparison is generated work, not given count.  The OTTER run
generated about 18,871 clauses per given; the six-hour collective run
generated about 36.6.  A collective given therefore represented about 516
times less materialized inference work at these report points.

The current scheduler has four interacting problems:

1. One combined descriptor is created for almost every activation, while
   expansion happens more slowly.  The unbounded logical inference debt is
   visible as 178,171 pending descriptors.
2. Within a descriptor, hyperresolution is attempted before paramodulation.
   A large chunked hyper set can therefore postpone that given's
   paramodulation for a practically unbounded time.
3. The generator APIs are not truly resumable.  Partial conclusion sets are
   regenerated and verified from a saved prefix or ordering threshold, which
   spends CPU on replay instead of new discovery.
4. Hint matching occurs only after a conclusion is materialized and reaches
   `cl_process`.  Raw-weight ordering cannot recognize a conclusion whose
   decisive selector key comes from an exact hint match after simplification.

The compact hint index is not implicated.  The paired 1,000-given radical
packed and compact artifacts both have `Generated=9126`, `Kept=1717`, 101
distinct matched hints, and the same selector counts.  The stronger
scheduler must therefore be developed independently of the better-packed
hint-index work.

## Non-goals

- Do not change the meanings of `search_loop=otter`,
  `inference_frontier=clauses`, or the existing conservative collective
  policy.
- Do not make approximate hint matching authoritative.
- Do not generate all conclusions merely to recreate the OTTER trace.
- Do not hide an unbounded raw-clause queue in RAM, an mmap file, or a
  checkpoint.
- Do not use given count alone as a progress or performance metric.
- Do not run multi-hour or multi-thousand-given experiments on the current
  low-RAM machine during development.

## Required invariants

### Proof and inference invariants

1. Every committed candidate uses the existing simplification, exact hint
   matcher, keep/delete rules, weight adjustment, proof-parent construction,
   and passive-selection path.
2. A scheduling preview may affect ordering only.  It cannot prove that a
   candidate is redundant, delete it permanently, or mutate hint state.
3. Every finite eligible inference unit receives fair ordinary service and
   every conclusion in it is eventually committed or rejected by the normal
   clause-processing rules.
4. Rule fairness is explicit: hyperresolution cannot indefinitely hide
   paramodulation, and paramodulation cannot indefinitely hide
   hyperresolution.
5. Historical parents and simplifier epochs are the same ones that the
   current collective descriptor would use.

### Hint invariants

1. Preview matching is read-only.  It assigns no clause ID, increments no
   hint counter, copies no persistent label, and performs no degradation.
2. A promoted candidate is matched again by the authoritative existing path
   when it is committed.
3. Matcher identity, adjusted weight, labels, degradation,
   `limit_hint_matchers`, `hint_match_once`, `breadth_first_hints`, and
   `hint_age` retain their present semantics at that commit point.
4. Preview records carry the hint epoch.  A record with a stale hint or
   simplifier epoch is refreshed or demoted before it can affect priority.
5. A preview false positive may waste a priority slot.  A preview false
   negative may delay a match but may never remove it from the fair path.

### Resource invariants

1. Resident candidate bodies remain bounded by
   `collective_candidate_cache` or a stricter replacement.
2. Pending descriptor memory has a configured hard high-water mark.  Given
   activation stops and descriptor draining begins before that mark can be
   exceeded.
3. Each scheduler turn has a bounded amount of raw enumeration work.
4. No candidate prefix is repeatedly regenerated from ordinal zero.  Once
   native iterators are enabled, ordinary fair enumeration visits each raw
   conclusion once; optional hint lookahead may visit it at most one
   additional time.
5. Checkpoints contain bounded iterator and scheduler state, not all unseen
   raw conclusions.

## Proposed architecture

### 1. An opt-in scheduler mode

Add a separate experimental policy, provisionally:

```prolog
assign(collective_scheduler,balanced_hint).
```

The current queue and all existing checkpoint formats remain the
`collective_scheduler=legacy` behavior.  The new policy must not silently
change old command files or the running 4,000-given experiment.

The implementation should share inference and clause-processing code with
the legacy policy, but its queues, iterator state, and checkpoint section
must be separately versioned until every gate below passes.

### 2. Split descriptors by rule and bounded work unit

Do not combine every enabled inference rule behind one given-level queue
entry.  Activation should create independent work items for:

- positive hyperresolution;
- negative hyperresolution, when enabled;
- paramodulation from the given into historical partners;
- paramodulation from historical partners into the given; and
- any later collective rule only after it defines the same bounded-turn
  contract.

Paramodulation work should be partitioned into bounded partner ranges.  A
large hyper set should expose a resumable enumeration state rather than
holding the given's paramodulation hostage until the set is complete.

Each work item records stable parent IDs, its historical activation limit,
simplifier epoch, rule kind, generator continuation, and deterministic local
ordinal.  It never owns a persistent `Topform *` that should be evictable.

This split is necessary but not sufficient: simply placing more entries in a
FIFO queue would still let activation outrun inference.

### 3. Bounded inference debt and admission backpressure

Replace `collective_given_ratio` as the primary control in the new policy
with high/low-water admission control.

Provisional controls are:

```prolog
assign(collective_descriptor_high_water,4096).
assign(collective_descriptor_low_water,3072).
assign(collective_oldest_lag_limit,4096).
```

The exact defaults must come from bounded measurements; these names express
the intended contract.

When pending work reaches the high-water mark, or the oldest unfinished work
is more than the configured activation lag behind, the search enters drain
mode.  It performs bounded descriptor turns and commits candidates until both
the low-water and lag conditions recover.  It selects another given only
when doing so cannot violate the hard descriptor bound.

Hysteresis avoids switching on every iteration.  If no given is available,
the scheduler remains work-conserving and drains finite inference work.  If
the candidate cache is full, ordinary given selection drains that cache
before more candidates are exposed.

This makes a reported given count meaningful: it can no longer be hundreds
of thousands of activations ahead of almost all inference work.

### 4. Deficit-based rule fairness

Use deterministic weighted deficit round robin across rule lanes rather than
a single queue whose internal rule ordering is fixed.

The initial lanes should be:

1. mandatory oldest-work fairness;
2. hint propagation and discovery;
3. paramodulation;
4. positive hyperresolution; and
5. negative hyperresolution.

A turn consumes measured work units, preferably raw candidates enumerated or
historical partners examined, rather than one opaque generator call.  Lane
deficits carry across turns.  After at most a fixed number of priority turns,
one oldest fair turn is mandatory.

Fairness tests must exercise an infinite stream of newly hinted work and show
that an older ordinary descriptor still advances.  Separate tests must show
that huge hyper work cannot prevent an early paramodulation proof.

### 5. Truly resumable inference iterators

The decisive CPU fix is to give paramodulation and hyperresolution explicit,
checkpointable iterators.  A call should accept a raw-candidate budget and
return one of:

```text
budget exhausted with continuation
inference unit complete
historical unit invalidated according to the existing epoch rules
```

The continuation must consist of stable enumeration coordinates, such as
parent positions, literal/term positions, partner position, and substitution
enumerator state.  It must not contain process-local pointers.

Implementation should proceed one rule at a time:

1. Paramodulation is the first target because the existing descriptor already
   has a historical partner cursor and the Osborn hint chain contains many
   paramodulation and back-rewrite consequences.
2. Hyperresolution follows, with separate continuation state for retrieved
   nucleus/satellite candidates and combination enumeration.
3. The old replay/checksum implementation remains available as a differential
   oracle until continuation traces match complete raw enumeration.

For each finite inference unit, a test records structural hashes of its raw
conclusions under eager enumeration and under every forced iterator budget
from one through a small maximum.  The sequences must agree exactly.  Resume
at every continuation field boundary must produce the same remaining
sequence.

If a generator cannot yet be made resumable, its lane remains on the legacy
path and is not allowed to claim the bounded-turn or CPU acceptance gates.
An unbounded mmap spill of raw candidates is not an acceptable substitute.

### 6. Bounded candidate windows

Each iterator exposes a small window of raw conclusions into one global
bounded candidate pool.  All candidates in a window remain represented until
they are committed through `cl_process`; ordering within the window may
change, but no member is discarded because it looked unpromising.

The pool owns transient clause bodies.  Descriptors own only iterator state
and small references to pool entries.  The global bound is shared across all
lanes, so thousands of descriptors cannot each allocate their own full
window.

Once a descriptor's window is empty, it may request another window through
the lane scheduler.  This gives cross-descriptor choice without rescanning a
whole inference set or retaining every conclusion.

### 7. Side-effect-free selector and hint preview

Factor a preview operation out of the existing clause-processing machinery.
On a scratch candidate it should:

- perform the normalization needed to obtain a meaningful selector estimate;
- query the selected exact hint-index implementation read-only;
- calculate the predicted matching hint ID and adjusted weight;
- determine predicted membership in `Hha`, `Hw`, `LH`, and the ordinary
  selector parts; and
- return the simplifier epoch, hint epoch, structural fingerprint, and stable
  raw ordinal used for deterministic tie breaking.

The preview should reuse existing matching and weight code rather than
implement a second interpretation of hints.  An audit and focused test must
prove that preview leaves all global hint counters, epochs, labels, IDs,
statistics that affect search, and allocator-owned persistent objects
unchanged.

The candidate-pool key is provisional and should mirror the actual selector
intent:

```text
predicted selector priority
matching hint ID for hint_age parts
predicted adjusted weight
rule-lane fairness age
given ID
local raw ordinal
```

The key is advisory.  Immediately before commitment, stale previews are
recomputed.  The normal `cl_process` result is recorded beside the preview so
false-positive and changed-key rates can be measured.

### 8. A dual-cursor hint-discovery lane

Ordinary bounded windows improve local ordering but may still take a long
time to reach a hint deep inside a large inference unit.  The stronger
design therefore permits bounded read-ahead without sacrificing the fair
path.

Each eligible work item has:

- a fair cursor, which eventually commits every conclusion in generator
  order; and
- an optional discovery cursor, which may inspect a bounded distance ahead
  using read-only preview.

The discovery lane initially gives extra service to descriptors whose parent
is already hint-matched or whose exposed candidates have produced a previewed
hint.  It also reserves a low-rate general-discovery turn for the oldest
ordinary descriptors; otherwise a search could propagate known hint chains
but never discover a new chain hidden deep in nonhint work.  Both kinds of
lookahead receive a hard per-cycle work budget and a maximum distance ahead
of the fair cursor.

If discovery finds a likely hint matcher, that raw candidate may be committed
early through the normal path.  Its local ordinal is recorded in a bounded
ahead-of-fair consumed set so the fair cursor skips it later.  Records are
removed as soon as the fair cursor catches up.  The number and distance of
such promotions are capped; reaching either cap disables further lookahead
for that descriptor until fair work catches up.

Nonmatching discovery candidates are not lost.  They are discarded only as
scratch previews, and the independent fair cursor later enumerates and
commits them normally.  Thus ordinary candidates are visited once and a
looked-ahead prefix at most twice, rather than the current repeated-prefix
behavior.

This mechanism is stronger than the existing `collective_hint_probes` flag:
the current probe merely advances one paramodulation pair belonging to an
already hint-matched given.  The dual cursor can discover and promote a
hint-matching conclusion within bounded future work while mandatory fair
enumeration continues.

### 9. Epoch invalidation

Deferred raw conclusions are ranked in a changing active simplifier and hint
state.  The new scheduler must make invalidation explicit.

- An uncommitted candidate-pool entry carries the epochs used for preview.
- A changed simplifier epoch triggers normalization and key refresh before
  commitment.
- A changed hint epoch triggers read-only rematching and key refresh.
- A historical inference iterator continues to use its descriptor's original
  parent snapshot, matching current collective semantics.
- If refreshing changes the structural fingerprint, the old preview and any
  ahead-consumed bookkeeping are checked before the candidate proceeds.

No broad eager reheap of every candidate is required.  Lazy validation at
heap top is acceptable if stale skips and refresh cost are counted and cannot
starve valid entries.

### 10. Checkpoint and compatibility design

Introduce a new collective checkpoint version only after the in-memory
iterator is deterministic.  It must store:

- scheduler policy and lane deficits;
- high/low-water drain state;
- split work-item queue order;
- every stable iterator continuation;
- candidate metadata and compact bodies currently occupying the bounded
  pool;
- fair and discovery cursors;
- bounded ahead-consumed ordinals;
- hint/simplifier preview epochs; and
- deterministic priority and fairness cycle positions.

The reader should continue accepting `P9COLL5` through `P9COLLA` in legacy
mode.  It need not reinterpret an old checkpoint as the new policy.  A policy
mismatch should fail with a precise diagnostic rather than silently changing
the search.

## Instrumentation required before optimization

Add aggregate, low-overhead counters for:

- descriptors created, completed, pending, and peak by rule lane;
- descriptor age and activation lag: mean, p50, p95, and maximum;
- entries into drain mode and givens withheld by backpressure;
- iterator raw steps, yielded candidates, completions, and invalidations;
- generator replayed candidates, retained only while a legacy path exists;
- scheduler turns and charged work by lane;
- candidate-pool occupancy and bytes;
- preview calls, predicted hint matches, authoritative matches, false
  positives, and changed hint IDs;
- discovery distance, candidates inspected, promotions, duplicate skips, and
  fair-cursor catch-ups;
- cumulative hint-selected givens by `Hha`, `Hw`, and `LH`;
- distinct hints first matched per reporting interval; and
- time in enumeration, preview normalization, hint preview, authoritative
  clause processing, and stale refresh.

Periodic reports must support comparison at equal CPU time, equal raw
enumeration work, equal generated/committed clauses, and equal given counts.
Only the first three are meaningful primary comparisons for this problem.

## Delivery phases and stop/go gates

### Phase 0: freeze controls and add attribution

- Record hashes and command lines for the current 1,000-given legacy,
  radical packed, and radical compact artifacts, plus the six-hour gzip.
- Add the aggregate counters above without changing scheduling.
- Prepare four explicit controls:
  1. OTTER + compact/FPA as the historical reference;
  2. DISCOUNT + dense + compact + `inference_frontier=clauses`;
  3. current conservative collective policy; and
  4. current collective policy with its three optional aids enabled.

Gate: instrumentation-on and instrumentation-off runs have identical given,
generated, kept, matcher, and proof traces on focused tests and bounded
Osborn prefixes.

### Phase 1: split rule work and add backpressure

- Add the opt-in policy and independent rule descriptors.
- Add lane accounting, deterministic fairness, and descriptor high/low-water
  admission control.
- Continue using legacy generator calls temporarily, but do not claim the
  bounded-turn gate.

Gate: forced tests prove that hyper and paramodulation both advance; pending
descriptors never exceed the configured hard bound; and focused proofs remain
valid.  On an Osborn prefix, completed work must grow continuously instead of
leaving virtually every created descriptor pending.

### Phase 2: resumable paramodulation

- Implement stable continuation state for both paramodulation directions.
- Differentially compare raw conclusion sequences against eager and replay
  enumeration for many budgets.
- Checkpoint in the middle of every continuation dimension.

Gate: zero raw-sequence differences, zero proof regressions, no paramodulation
prefix replay, and a bounded maximum raw work count per paramodulation turn.

### Phase 3: resumable hyperresolution

- Implement positive and negative hyper iterators.
- Shard index retrieval and combination enumeration into bounded turns.
- Preserve historical parent filtering and raw order exactly within the
  iterator contract.

Gate: the same raw-sequence and checkpoint gates as Phase 2; a synthetic huge
hyper set can no longer monopolize one scheduler turn; and paramodulation
continues to advance beside it.

### Phase 4: bounded windows and read-only preview

- Add the global candidate pool and deterministic advisory keys.
- Factor and audit the read-only normalization/hint preview.
- Commit every window member through the unchanged authoritative path.

Gate: preview causes no persistent state changes, authoritative hint tuples
remain exact, pool memory never exceeds its bound, and stale-epoch refresh is
covered by tests.

### Phase 5: bounded dual-cursor hint discovery

- Add fair/discovery cursors, promotion caps, and ahead-consumed tracking.
- Give hinted descendants extra lookahead while retaining a low-rate general
  discovery turn across ordinary descriptors.
- Add mandatory fair turns and adversarial starvation tests.

Gate: every promoted candidate is either confirmed or counted as a preview
false positive; every skipped-ahead ordinal is encountered exactly once by
the fair cursor; consumed metadata remains bounded; and a finite ordinary
descriptor completes under an unbounded stream of hint work.

### Phase 6: bounded Osborn tuning

Tune lane weights, watermarks, window size, and discovery budget only after
the correctness gates pass.  Do not tune by one proof alone.

Run in this order on the current machine:

1. focused unit/proof/checkpoint tests;
2. 10% deterministic hints at 100, 250, and 500 givens;
3. full hints at 100 givens;
4. full hints at 250 and then 500 givens only if the preceding RAM/time gates
   pass; and
5. stop for inspection before any 1,000-given or multi-hour run.

Every experiment gets explicit `max_seconds`, `max_megs`, and `max_given`
limits.  The harness records `/usr/bin/time -v`, input and binary hashes, and
periodic reports.  No development run should compete with the user's ongoing
4,000-given comparison.

Gate: relative to current conservative collective mode at matched CPU and
raw work, the stronger policy must:

- increase rather than flatten the rate of new distinct hint matches;
- continue discovering `Hha` clauses after the initial prefix;
- make measurable progress in both paramodulation and hyperresolution;
- keep descriptor backlog within its configured bound;
- eliminate repeated-prefix CPU after the relevant iterator phase; and
- retain the compact/dense RAM advantage, with scheduler overhead fully
  accounted and no unbounded hidden store.

The DISCOUNT clauses-frontier control defines the locally attainable hint
stream without collective deferral.  It is a diagnostic target, not a trace
requirement.  Numerical hint-coverage thresholds should be set only after
that control is measured, because the current artifacts do not contain it.

### Phase 7: suitable-host acceptance

Only after user inspection and bounded acceptance:

- run paired one-hour full Osborn jobs from the same input and build;
- compare current conservative collective and balanced-hint policies at
  equal wall/CPU budgets;
- checkpoint and resume the better policy before extending it to six hours;
- compare hint-discovery slope, raw and committed work, completed inference
  units, proof progress, RSS, allocator accounting, and checkpoint hashes;
  and
- then run the 1,000/4,000-given and week-scale AIM cases on a machine with
  suitable RAM and time.

The long-run gate is not merely “more hints.”  It requires a better or at
least credible solved-problem trajectory, bounded pending state, validated
proofs, and the original project target of roughly 80--90% lower peak RAM
than the passive-heavy old P9 runs.

## Focused tests to add

1. `collective_rule_fairness_test`: huge hyper work plus an early
   paramodulation proof; paramodulation advances within a fixed turn bound.
2. `collective_backpressure_test`: an infinite-producing synthetic prefix;
   descriptor count drains below low water and never exceeds hard high water.
3. `paramod_iterator_test`: eager versus budgets 1--64, both directions, and
   checkpoint at every cursor field.
4. `hyper_iterator_test`: eager versus budgets 1--64 with multiple satellites,
   historical deactivation, and checkpoint/resume.
5. `collective_preview_purity_test`: hash all hint state and global counters
   before and after previews; require exact equality.
6. `collective_preview_epoch_test`: alter hint state while a preview is queued
   and require safe key refresh.
7. `collective_hint_lookahead_test`: place a hint match beyond the first
   window, promote it, and later skip exactly that ordinal on the fair path.
8. `collective_hint_starvation_test`: continuously create hinted descendants
   while an old nonhint descriptor still completes.
9. `collective_pool_bound_test`: many descriptors expose windows while
   measured body bytes remain below the configured pool limit.
10. `collective_balanced_checkpoint_test`: save nonzero lane deficits, drain
    mode, stale previews, separated cursors, and consumed ordinals; resumed
    and uninterrupted traces agree.

All successful proofs must pass `prooftrans`; supported equality proofs must
also pass `directproof`.

## Principal risks and mitigations

| Risk | Mitigation |
| --- | --- |
| Preview changes hint semantics | Reuse factored read-only helpers, hash mutable hint state in purity tests, and rematch authoritatively at commit. |
| Native iterators change inference coverage or order | Keep eager/replay enumeration as an oracle; compare structural sequences at many forced budgets and checkpoints. |
| Hint priority starves completeness | Mandatory oldest-work lane, capped discovery distance/promotions, and adversarial infinite-hint tests. |
| Backpressure leaves no selectable candidates | Work-conserving drain logic exposes bounded windows until a candidate exists or finite work is exhausted. |
| Split descriptors duplicate inferences | Stable rule/direction IDs and exact eager-versus-split sequence tests. |
| Epoch changes invalidate queued keys | Store epochs, lazily refresh at heap top, and never use preview for deletion. |
| Ahead promotions create unbounded skip metadata | Cap distance/count, stop lookahead at the cap, and reclaim ordinals when fair work catches up. |
| Better hint coverage recreates passive RAM growth | Keep dense passives and a hard candidate pool; measure dense-record/body growth separately and stop at RAM gates. |
| Checkpoint format becomes fragile | Version only after deterministic iterators pass; fail closed on policy/version mismatch. |
| Scheduler tuning overfits Osborn | Require synthetic tests and multiple AIM/Osborn prefixes before long runs. |

## Recommended implementation order

The first code change after approval should be instrumentation plus the split
rule-lane/backpressure scaffold.  It provides immediate evidence about
starvation and bounds the runaway descriptor queue without entangling hint
semantics.

Native paramodulation continuation should come next, followed by native
hyperresolution continuation.  Read-only hint preview and dual-cursor
lookahead should not be implemented on top of repeated full-prefix replay;
otherwise an apparent hint improvement could worsen the already dominant CPU
problem.

The existing optional promising scheduler and hint probe remain useful
comparison policies.  They should not be promoted to defaults as a shortcut
for the work above.
