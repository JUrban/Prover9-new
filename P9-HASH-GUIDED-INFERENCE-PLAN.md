# Hash-Guided Inference Plan

Date: 2026-08-25

Status: **Stage 1 implemented on `lazy-hash-gate`; Stage 2 remains a plan**

Branch: `experimental-generalized-hint-hash`

Planning base: `d9e912a91c7b2cf57010743d5176e644fec675d5`

Implementation note (2026-08-25): the lazy-gate work described in Stage 1
now lives on branch `lazy-hash-gate`.  Its implementation, options, tests,
measurements, and limitations are documented in
`P9-LAZY-HASH-GATE-REPORT.md`.  The target-directed Stage 2 has not been
implemented.

## Decision

Develop hash-guided inference in two deliberately separate stages:

1. **Lazy hash gate:** continue enumerating ordinary inferences, but inspect a
   virtual conclusion before allocating a real clause.  Materialize only a
   hash hit or a conclusion that cannot safely be rejected without ordinary
   processing.
2. **Target-directed inference:** augment the flat hash with structural target
   information and use those targets to find promising parent clauses and
   positions.  This stage is intended to avoid enumerating most conclusions,
   not merely make rejected conclusions cheaper.

The lazy gate is both a useful optimization and a prerequisite for the
target-directed stage: it supplies a cheap authoritative result check, the
substitution-aware term traversal, and the measurements needed to decide
whether target-directed inference can pay for its index.

The two stages must have distinct options, tests, statistics, commits, and
performance reports.  No target-directed code is to be mixed into the lazy
gate implementation before the lazy gate passes its own correctness gate.

## Motivation and measured baseline

At the 26,100-second report in `Josef_04.hash-all.out`, the current
generalized-hash search had:

| Measurement | Value |
|---|---:|
| Given clauses | 43,112 |
| Generated conclusions | 4,094,009,855 |
| Paramodulation conclusions | 3,110,822,766 (76.0%) |
| Hyperresolution conclusions | 983,186,860 (24.0%) |
| Hash queries | 5,000,194,466 |
| Hash hits | 1,356,870 (0.027%) |
| Allocator calls | 282,531,978,002 |
| Logical allocator traffic | 8,950,890,170,064 bytes |
| Resident PSS | about 1.1 GiB |
| System CPU | 14.95 seconds |

The fixed hash remains healthy at 2.41 probes per query.  Passive storage,
compact indexes, file I/O, paging, and resident-memory growth do not explain
the elapsed time.  The search is constructing billions of conclusions and
rejecting virtually all of them.

The allocator statistics measure traffic, not CPU.  They do not prove that
the allocator itself consumes a particular fraction of the run.  They do
show the scale of temporary object construction that a lazy result can avoid:
about 69 allocations and 2.2 KiB of logical allocation per generated
conclusion, with a comparable amount of eventual destruction.

## Terminology

- A **materialized conclusion** is an ordinary `Topform` with allocated
  literals, terms, attributes, and justification.
- A **virtual conclusion** is a read-only description of the same prospective
  result: parent clauses, live unifier contexts, equality side, and rewrite
  position.  Its terms are traversed through the substitutions without being
  copied.
- A **target** is one clause pattern represented by an entry in the
  generalized-hint hash: an exact hint or one of the admitted
  generalizations.
- A **target recipe** is enough information to reconstruct a target's
  structure, for example an active hint ID plus the abstracted subterm
  position.
- A **safe rejection certificate** is a reason that the current search would
  certainly delete a virtual conclusion even if it were materialized.

## Scope and non-goals

The first implementation is intentionally narrow:

- `hint_index=generalized_hash` only;
- static hints, as already required by that mode;
- positive unit-equation paramodulation first;
- the demodulation-free Josef_04 configuration first;
- the ordinary OTTER inference order as the lazy-gate reference;
- hyperresolution and nonunit paramodulation retain their current paths;
- `packed_fast` remains unchanged as a separate control.

The project does **not** initially promise:

- legacy packed/FPA hint-matching completeness;
- support for back-demodulated, expired, or match-once hints;
- early rejection across arbitrary demodulation, evaluation, unit deletion,
  or CAC simplification;
- a target-directed nonunit or hyperresolution calculus;
- the old generated-clause count or search trace in the final
  target-directed mode;
- a speedup inferred merely from allocator counters.

## Semantic contracts

### Contract A: lazy `safe` mode

For every supported inference, lazy `safe` mode must make the same decision
as the current generalized-hash mode on the same normalized conclusion:

```text
(hash hit/miss, selected hint ID, adjusted weight, keep/delete decision)
```

It may skip allocation only after proving that ordinary processing would
delete the conclusion.  Unsupported or uncertain cases fall back to the
existing materialized path.

For deterministic bounded tests, `safe` mode must preserve:

- the selected given-clause formula stream;
- kept formulas and their order;
- matching hint IDs and hint counters;
- proof parents and justifications;
- selector counts;
- logical generated-event actions and reporting behavior.

The implementation must account for a skipped virtual conclusion as a
logical generated event.  The current `Stats.generated`, rule-specific
counters, generated-triggered actions, limit polling, and report polling
cannot silently disappear merely because `cl_process()` was not called.
Generated-event accounting should be centralized so that materialized and
virtual results cannot double-count or omit an event.

### Contract B: lazy `hit_only` experiment

An optional, explicitly incomplete `hit_only` mode may materialize only hash
hits plus mandatory proof-safety cases.  It measures the upper bound of the
lazy approach without claiming the `safe` contract.  Its output must state
prominently that unmatched low-weight and keep-rule conclusions were omitted.

`hit_only` must not be the default and must not share a name with `safe`.

### Contract C: target-directed mode

Target-directed mode is a new experimental search procedure.  It must emit
only sound ordinary Prover9 inferences and validate every emitted conclusion
through the existing authoritative processing path.  It need not reproduce
the old raw generated count because avoiding those events is its purpose.

Before becoming authoritative, it must run in shadow mode and demonstrate
100% recall of all supported hash-hit unit-paramodulation conclusions emitted
by ordinary enumeration.  Targeted candidates may be discovered in a
different internal order, but authoritative mode must define a deterministic
order and document whether that order reproduces or intentionally changes
the generalized-hash given sequence.

Unhinted completeness requires a separate fair fallback lane.  A first
`targeted_only` experiment may omit that lane, but must be labeled incomplete
and cannot be described as a general Prover9 search mode.

## Stage 1: lazy virtual-result hash gate

### 1.1 Establish a sampled baseline before changing behavior

Add low-overhead sampled timing around whole stages, not around every
allocation.  A provisional sampling interval is one successful inference in
65,536; it must be configurable and its overhead measured.

Report, by inference rule and unit/nonunit class:

- parent pairs and eligible rewrite positions visited;
- unification attempts and successful unifications;
- time in unification/enumeration;
- time constructing the materialized result;
- time simplifying and orienting it;
- time weighing and hint matching it;
- time applying keep/delete and subsumption tests;
- time destroying rejected results;
- allocation calls and logical bytes per successful inference.

The sampled estimates must be shown with sample counts.  Zero samples must
never be printed as a measured zero-second cost.

This instrumentation is a measurement subphase of the lazy-gate project, not
a separate optimization.  Timing every `get_mem()`/`free_mem()` call is out
of scope because the timer overhead would be comparable to the operation and
would omit construction, initialization, copying, and cache costs charged to
the caller.

### 1.2 Add a virtual paramodulation result

Immediately after a successful paramodulation unification, while both
substitution contexts are still live, construct a small stack-owned view
containing:

- the from and into clauses and selected literals;
- the selected equality side and replacement side;
- both substitution contexts;
- the into-literal argument and subterm path;
- the inference direction and legacy raw ordinal.

The view owns no terms, literals, attributes, or justification.  It cannot
outlive the unifier contexts and must never be placed in a persistent list.

Add the hook to both existing paramodulation producers:

- ordinary `para_from_into()` used by the OTTER loop;
- `para_from_into_bounded()` used by collective scheduling.

Only the ordinary path becomes authoritative initially.  The bounded path
runs in shadow until checkpoint/resume and iterator behavior are tested.

### 1.3 Stream substituted terms without allocation

Implement a traversal that presents the prospective result term-by-term while
following bindings in the two contexts.  At the rewrite position it traverses
the replacement equality side rather than the original into subterm.

The initial unit-equation walker must handle:

- variables from two independent parent namespaces;
- repeated variables and bindings shared across both conclusion sides;
- root and deep rewrite positions;
- constants and variable replacement sides;
- equality orientation and the existing unoriented-equality flip lookup;
- canonical variable numbering identical to the current generalized hash;
- cyclicity assumptions guaranteed by the existing unifier.

For safety, the first lookup may calculate conservative keys for both equality
orientations and materialize every possible hit.  The authoritative
post-normalization lookup then confirms the selected hint ID.  Extra
materializations are acceptable; false-negative virtual misses are not.

Do not initially implement virtual literal permutation or general nonunit
canonicalization.  Nonunit results use the old path.

### 1.4 Separate shadow, safe, and hit-only policies

Provisional review names are:

```prover9
assign(hash_inference_gate,off).       % existing behavior
assign(hash_inference_gate,shadow).    % measure, always materialize
assign(hash_inference_gate,safe).      % skip only certified deletions
assign(hash_inference_gate,hit_only).  % intentionally incomplete ceiling
assign(hash_inference_sample_rate,65536).
```

These names are not an implementation commitment, but the behavioral
separation is mandatory.  All non-`off` values must be rejected unless
`hint_index=generalized_hash`.

In `shadow` mode:

- calculate the virtual answer;
- materialize every conclusion normally;
- compare virtual and authoritative hash answers;
- report false misses, false hits, changed hint IDs, and orientation changes.

In `safe` mode, a virtual miss can be skipped only if all relevant facts are
known without a `Topform`.  Initially this means:

- the result is a supported positive unit equality and cannot itself be the
  empty clause;
- no enabled simplifier can transform the raw result into a different hash
  key;
- the generalized-hint bank is static;
- the exact post-orientation raw weight is over `max_weight`;
- no configured white/keep rule could override that deletion;
- the clause is not a restricted denial, conflict candidate, or another
  mandatory safety case.

If arbitrary keep/delete rules cannot be evaluated from the view, their
presence forces materialization unless a separately verified fast rule
implementation exists.  Unsupported configurations must fall back rather
than silently prune.

In `hit_only` mode, proof-producing empty/conflict cases remain protected, but
other misses can be omitted deliberately.  The statistics must distinguish
certified safe skips from experimental hit-only skips.

### 1.5 Materialize and validate hits

On a possible virtual hit:

1. call the existing `paramodulate()`;
2. attach the existing parent/position justification;
3. process it through the unmodified authoritative `cl_process()` path;
4. compare the virtual target ID and final matching hint ID;
5. preserve the existing proof and selector ownership rules.

The virtual result is a filter, never the final proof object.

Add an optional assertion mode that materializes a deterministic sample of
virtual misses and verifies that they really miss after normalization and
would be deleted.  This catches false-negative bugs without eliminating the
performance benefit on every miss.

### 1.6 Lazy-gate statistics

Report at every ordinary statistics interval:

```text
Hash_inference_gate:
  mode, supported_results, unsupported_fallbacks,
  virtual_queries, virtual_hits, virtual_misses,
  materialized_hits, materialized_safety_fallbacks,
  certified_skips, hit_only_skips,
  sampled_miss_validations, false_misses, false_hits,
  changed_hint_ids, orientation_fallbacks,
  estimated_allocations_avoided, estimated_bytes_avoided
```

Also report generated conclusions versus materialized conclusions by rule.
The avoided-allocation fields are estimates based on matched sampled controls,
not declarations of CPU time saved.

### 1.7 Lazy-gate tests

Add focused unit tests for virtual versus materialized keys covering:

- unit equality rewrites on both sides and at several depths;
- oriented and unoriented equalities;
- a renamable flipped equality;
- shared and independently named variables in the two parents;
- substitutions that make two previously different variables equal;
- constants, variable replacement sides, and repeated subterms;
- a hash hit, a hash miss, and a 96-bit collision simulation at the table API;
- cancellation from the inference callback;
- the bounded paramodulation iterator at a yield boundary.

Add deterministic differential tests:

- `off` versus `shadow`: identical full trace and zero virtual mismatches;
- `off` versus `safe`: identical given formulas, kept formulas, hint IDs,
  selector report, and proof;
- checkpoint/resume with the bounded iterator still in shadow;
- `chat_test.in` as a mixed/nonunit fallback check;
- an Osborn prefix for equality orientation and negative-literal fallback;
- the existing Josef_04 1k and 3k controls.

Every proof must pass both `prooftrans` and `directproof` where applicable.

### 1.8 Lazy-gate measurement protocol and gate

Run control and candidate on the **same machine and build settings**, without
hint dumping.  Do not compare an ar-2 candidate with a local control.

Use bounded 1k and 3k Josef_04 runs first.  Record:

- startup, search, user, system, and wall time;
- given and generated conclusions by rule;
- virtual and materialized conclusions;
- hash queries, hits, and probes;
- sampled stage times and sample counts;
- allocator calls, logical traffic, new storage, PSS, and peak RSS;
- formula-level given-stream checksum;
- matched hint IDs and final selector counts.

Lazy `safe` mode passes correctness only with zero mismatches and an identical
search trajectory on the supported test configurations.

Performance expectations are deliberately phrased as gates, not forecasts:

- at least 90% of supported unit-paramodulation conclusions should avoid
  materialization on Josef_04;
- allocator calls per logical unit-paramodulation result should fall by at
  least 70%;
- the sampled instrumentation overhead should remain below 2%;
- the 3k search phase should improve by at least 1.5x to justify production
  hardening of the lazy gate;
- no candidate may increase peak RSS by more than 5%.

If construction avoidance is high but CPU improves by less than 1.5x, retain
the virtual-result infrastructure and proceed to target-directed work: that
result would show that inference enumeration and unification, rather than
materialization, dominate.  Do not spend another tuning cycle on the slab
allocator.

## Stage 2: target-directed unit paramodulation

### 2.1 Architectural consequence: the flat hash is insufficient

The current 16-byte hash slot contains a 96-bit key and a selected hint ID.
That representation can answer whether an already known conclusion is a
target, but it cannot reveal a target's symbols, subterms, or rewrite
positions.  Target-directed inference therefore requires a structural
sidecar or a companion index built while the target generalizations are
available.

Do not attempt to reverse a 96-bit fingerprint or scan 9.8 million targets
for every given clause.

### 2.2 Preserve a compact target recipe

During generalized-hash construction, retain one reconstruction recipe for
each unique occupied target entry:

- exact target: active hint ID plus an exact marker;
- long-hint one-hole target: active hint ID plus a stable preorder subterm
  ordinal;
- small exhaustive target: compact serialized target or an explicit
  generation recipe.

Duplicate target keys retain the same preferred hint ID policy as the current
hash.  The recipe must reconstruct the pattern associated with that selected
ID; it may not point into temporary parsed hint terms.

Before committing to a representation, census:

- target entries by literal count and sign;
- unit equality versus other targets;
- term-node and depth distributions;
- exact versus one-hole versus exhaustive targets;
- unique reconstructed terms and shared subterms;
- estimated bytes for 8-, 12-, and 16-byte recipes;
- target subterm roots and frequencies;
- potential context-index records per target.

Flat expansion to one record for every subterm of every target is forbidden
unless the census demonstrates a bounded size.  With roughly 9.8 million
targets, an average of 25 positions would already mean about 245 million
records before indexing.

### 2.3 Initial target domain: unit equations

Build the first structural index only for unit equality targets.  All other
targets remain available through the existing hash and lazy gate.

Normalize variables and equality orientation exactly as the current hash
does.  Store compact term tokens or shared term handles, not `Topform *`
pointers.  The index must be immutable after construction because the current
generalized-hash mode already requires a static hint bank.

The target index should combine:

- a discrimination/code tree over normalized target equations;
- a selective entry point by replacement-side root symbol and arity;
- compact target recipe IDs at leaves;
- shared one-child tails or shared subterms where measurements justify them;
- a small mutable query workspace reused across inferences.

Full symbol-sized child arrays and a flat posting for every position are not
the default.  Use measured fan-out and selectivity to choose the compact
representation.

### 2.4 Reverse-narrowing algorithm

For an ordinary unit paramodulation:

```text
from:    l = r
into:    C[l] = u
target:  C[r] = u
```

process a newly active equality in both logical roles.

#### New clause used as the `from` parent

1. For each eligible direction `l -> r`, query the target-subterm index for
   target positions structurally compatible with `r`.
2. Treat target variables as normalized result variables, not unrestricted
   variables that may be instantiated arbitrarily.  The matcher must enforce
   variant equality of the eventual conclusion.
3. Reverse the proposed target position by replacing the target occurrence
   of `r` with `l`.  This yields a required pattern for the `into` parent.
4. Query a compact active-unit index for existing clauses compatible with
   that required parent pattern.
5. Apply the ordinary Prover9 parent tests, ordering restrictions,
   `check_instances`, and unification.
6. Validate the prospective result with the Stage-1 virtual hash lookup.
7. Materialize only a validated result and attach the ordinary parent and
   position justification.

#### New clause used as the `into` parent

Perform the symmetric incremental join against an index of older eligible
`from` equalities.  Enforce a stable parent-age/ID rule so every historical
OTTER pair is considered once rather than once from each activation event.

The active indexes may reuse compact unit-term infrastructure, but the first
version must not change the existing authoritative unit/subsumption indexes.

### 2.5 Context-with-a-hole fallback design

If reverse narrowing retrieves too many target positions, add a more
selective companion key for the unchanged part of the into clause:

```text
C[term] = u  ->  C[HOLE] = u
```

The target and into parent must have compatible outside contexts before the
replacement term matters.  A context query can therefore reject a
parent/position combination before ordinary unification.

This index must use unification-compatible structural tests; a plain exact
context hash is insufficient because the parent unifier can instantiate
variables outside the rewritten position.  Start with necessary rigid
conditions such as:

- equality-side root symbols;
- the child route to the hole;
- rigid symbols above and beside the hole;
- the root and selected rigid symbols of the unchanged equality side;
- repeated-variable relationships shared across the context.

Choose rare conditions first and confirm every survivor exactly.  Do not
materialize every possible hole context eagerly; build only measured useful
anchors or use a shared code tree.

### 2.6 Shadow comparison before authoritative pruning

In target shadow mode, continue ordinary unit paramodulation and record the
canonical key, parents, directions, and positions of every conclusion that
hits the generalized hash.  Independently run the target query for the same
given clause.

Compare ordered or explicitly sorted streams of:

```text
(target key, selected hint ID, from parent ID, into parent ID,
 from side, into position)
```

Report:

- ordinary hash-hit conclusions;
- targeted candidates;
- candidates confirmed by the virtual gate;
- missed ordinary hits;
- extra candidates and why they failed;
- duplicate inference paths;
- target and active-index nodes visited;
- candidate counts at every structural filter;
- query CPU and memory.

The recall gate is absolute: zero missed supported hash-hit conclusions.
False candidates are permitted in shadow mode but must be bounded tightly
enough to offer a speed advantage.

### 2.7 Authoritative targeted modes and fairness

Provisional review names are:

```prover9
assign(hash_targeted_inference,off).
assign(hash_targeted_inference,shadow_unit_paramod).
assign(hash_targeted_inference,targeted_only_unit_paramod).
assign(hash_targeted_inference,fair_unit_paramod).
assign(hash_target_index_kb,524288).
```

`targeted_only_unit_paramod` emits target-validated unit paramodulants and is
explicitly incomplete with respect to unhinted conclusions.  It is the first
speed/utility experiment.

`fair_unit_paramod` additionally retains a bounded ordinary inference lane.
The fairness policy must be stated in inference events, not wall time, and
must ensure that every ordinary parent pair/position is eventually visited if
the search continues indefinitely.  Its resumable iterator state must be
checkpointed.

Nonunit paramodulation, hyperresolution, possible empty clauses, conflicts,
and configured safety cases continue on the ordinary path until separately
implemented.

### 2.8 Determinism, duplicates, and proof ownership

Several target recipes can denote the same canonical conclusion, and several
parent pairs can derive it.  The implementation must define:

- deterministic target-recipe order;
- deterministic parent and position order;
- whether authoritative output is sorted by the old OTTER pair/position
  ordinal or by target priority;
- per-given duplicate suppression, if any;
- which proof justification wins when the same conclusion has several
  derivations;
- how hint degradation and match statistics count duplicate conclusions.

The safest first authoritative experiment emits confirmed candidates in the
legacy parent-ID/direction/literal/position order wherever that ordinal can be
computed without enumerating misses.  If target-priority order is later used,
it becomes an explicitly different search procedure and requires new
proof-coverage evaluation.

Every emitted clause remains an ordinary materialized Prover9 clause with
ordinary parent IDs.  No target recipe or hash entry is a proof parent.

### 2.9 Target-index resource gates

The initial resource budget is explicit:

- recipe storage target: at most 128 MiB;
- complete unit target sidecar and code tree target: at most 512 MiB;
- hard stop for the first prototype: 1 GiB additional resident memory;
- bounded reusable query workspace, reported separately;
- no per-given or per-query persistent growth.

These values are prototype gates, not final production goals.  If the target
index approaches the hard stop, stop and redesign sharing/selection rather
than increasing the budget merely because ar-2 has spare RAM.

Report actual logical bytes, allocated bytes, peak construction bytes,
resident PSS change, target nodes, shared tails/subterms, recipes, postings,
and maximum/mean candidate lists.

### 2.10 Target-directed stop/go gates

Run 1k and 3k shadow comparisons first, again with paired controls on the
same machine.  Do not start a long run until all of the following hold:

- 100% recall of supported ordinary hash-hit unit paramodulants;
- zero authoritative virtual-key or selected-hint-ID mismatches;
- all emitted proofs validate;
- deterministic repeated runs;
- at least 99% of ordinary successful unit-paramodulation events avoided
  before ordinary result enumeration; merely avoiding their `Topform`
  allocation was already Stage 1 and does not satisfy this gate;
- targeted query work, including false candidates, below 10% of ordinary
  successful-inference enumeration work;
- at least 3x search-phase CPU improvement over the Stage-1 lazy gate at 3k,
  or strong sampled evidence predicting at least 10x on the mature Josef_04
  fan-out;
- additional PSS below 512 MiB and no unbounded growth.

If recall passes but target queries are broad, implement the selective
context-with-a-hole index.  If target queries remain broad after one measured
redesign, stop the reverse-index attempt rather than tuning singleton
thresholds for Josef_04.

After the 3k gate, run a bounded 10k comparison with explicit CPU and RAM
limits.  Only then consider an hours-long Josef_04 experiment.

## Later extension: hyperresolution

Hyperresolution is 24% of the mature Josef_04 conclusion count, but it is not
part of the first targeted implementation.

The first extension should reuse the virtual gate after a clash is complete,
avoiding construction of a materialized resolvent on a target miss.  A later
target-directed version can constrain clash backtracking with the target
clause's residual literals and abandon a partial clash when no target pattern
can still match.

This work starts only if target-directed unit paramodulation passes its CPU
gate.  Otherwise the more complicated multi-parent join is unlikely to repay
its complexity.

## Failure modes to test explicitly

- A virtual key differs because equality orientation is performed after
  construction in the current path.
- Variables from the two parents are accidentally assigned the same virtual
  namespace before unification says they are equal.
- A raw miss simplifies or demodulates into a hit.
- An arbitrary keep rule retains a clause that the lazy gate skipped.
- A possible conflict or denial is pruned despite being proof-producing.
- A target variable is treated as an unrestricted matching variable rather
  than a normalized result variable.
- Duplicate target keys return a different preferred hint ID.
- Reverse narrowing misses a legal inference because of ordering,
  `para_from_small`, selected-literal, or instance restrictions.
- The same parent pair is emitted twice when a new clause is considered in
  both inference roles.
- Target recipes retain pointers into discarded hint terms.
- A structural index expands toward targets-times-positions and consumes
  several GiB.
- Sampled timing overhead changes the search materially.
- Generated-triggered actions or reports change in lazy `safe` mode because
  skipped conclusions were not counted.
- Checkpoint/resume loses an ordinary fairness iterator or target query
  cursor.

## Planned implementation commits

Each step should be committed separately with a detailed message and body.
The intended sequence is:

1. sampled inference-stage counters only;
2. unit virtual-result representation and differential key tests;
3. lazy `shadow` mode with no pruning;
4. lazy `safe` and explicitly incomplete `hit_only` policies;
5. paired 1k/3k benchmark report and lazy-gate stop/go decision;
6. target census and compact recipe representation;
7. unit target structural index in construction/shadow mode;
8. reverse-narrowing shadow comparison;
9. authoritative `targeted_only` experiment;
10. fair fallback and checkpoint support, only if earlier gates pass;
11. bounded 10k report and long-run recommendation.

No commit should combine a new representation, an authoritative behavior
change, and benchmark interpretation.  Reviewers must be able to inspect and
bisect each boundary independently.

## Review decisions requested before implementation

1. Should lazy `safe` mode require exact trace preservation, as proposed, or
   is an explicitly incomplete hit-only gate sufficient for this branch?
2. Should generated-triggered actions count virtual certified skips exactly
   as current generated conclusions, despite their lack of a `Topform`?
3. Is a 512 MiB initial target-index budget acceptable, with a 1 GiB hard
   stop?
4. Should the first authoritative target-directed experiment preserve legacy
   parent/position order, or deliberately prioritize lower target/hint IDs?
5. For the fair mode, what minimum ordinary-inference share is acceptable for
   proof-coverage experiments?

Implementation should not begin until these decisions and the overall staged
design have been reviewed.
