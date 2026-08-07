# Prover9 DISCOUNT/Waldmeister memory project

Date: 2026-08-07 (Europe/Berlin)

## Objective

Add a proof-complete search mode that reduces peak resident memory by 80--90%
on large AIM/Osborn-class searches.  The mode must stop treating every passive
clause as an active, pointer-rich index participant.  It must preserve the
meaning of Prover9 hint matching, including matching-hint identity, adjusted
weight, labels, degradation, and `hint_age`; it is not required to reproduce
the Otter loop's given-clause sequence.

The existing Otter-style loop remains the compatibility mode until the new
mode passes the proof, fairness, checkpoint, and memory gates below.

## Evidence and architectural consequence

The bounded Osborn measurements use the exact 310,153-hint input and the
proof-complete mmap ancestor store.  At 500 givens in a deterministic 10%
hint sample, Prover9 retains 423 usable clauses, 23,084 SOS clauses, and
17,610 demodulators.  FPA plus discrimination/DI indexes explain about 67% of
the 100-to-500-given live-memory growth.  In the archived 4,000-given run,
there are only 2,823 usable clauses but 588,252 SOS clauses and 489,224
demodulators.

The current loop indexes every kept clause for forward/back subsumption and
back demodulation while it is in limbo, makes eligible clauses demodulators,
then moves them to SOS.  Selection therefore does not separate passive from
active state.  Compressing disabled clauses cannot correct this growth law.

The full-hint 100-given measurement also shows a separate fixed problem:
FPA structures use 167.6 MiB and terms/argument arrays use 126.4 MiB.  The
additional 90% of hints add about 309.7 MiB of logical live allocation and
386.9 MiB of RSS.  A DISCOUNT loop and a compact hint representation are both
required for the final target.

## Semantic contract

### Search procedure

`search_loop=otter` retains the current procedure and search trace.

`search_loop=discount` has these invariants:

1. Only selected/active clauses participate in inference, forward
   simplification, forward subsumption, back subsumption, demodulation, and
   back-demodulation indexes.
2. A newly generated candidate is transiently simplified against the active
   state, weighed, hint-matched, filtered, assigned proof parents, and placed
   in the passive selector.  It is not inserted into active indexes.
3. A selected passive is refreshed against the current active simplifier
   epoch before activation.  If refresh changes or deletes it, its proof and
   selector metadata are updated and selection continues fairly.
4. A clause becomes a demodulator and generates inferences only after it is
   activated.
5. Every retained proof parent remains reconstructible through the existing
   ancestor store or the passive store.

The new procedure is judged by sound proofs, fairness/completeness of its
specified calculus, and solved-problem coverage.  It deliberately does not
promise the old given-clause sequence because the old sequence depends on
eager passive back simplification.

### Hint behavior

For every candidate committed to the passive frontier, the new mode must
produce the same hint tuple as the current matcher when invoked on the same
normalized clause and hint state:

```
(clause fingerprint, matcher hint ID, raw weight, adjusted weight,
 copied labels, degradation count, hint state epoch)
```

Matching includes subsumption/equivalence, flipped equality matching,
`bsub_hint_wt`, `breadth_first_hints`, `degrade_hints`,
`limit_hint_matchers`, `hint_match_once`, and stable hint IDs used by
`hint_age`.  A compact passive record stores the matcher ID, not a raw hint
pointer.  Hint degradation is committed at the same logical event as keeping
the candidate.  Back-demodulated or expired hints advance a hint epoch.

Collective inference batches cannot in general reproduce the Otter loop's
global hint-prioritized order without enumerating every conclusion: an unseen
conclusion may match a high-priority hint.  The collective mode therefore
preserves exact matching semantics when a candidate is materialized and uses
a fair, hint-aware scheduler; it does not claim trace identity.

## Target architecture

### Active state

- Selected clauses have materialized clause bodies and ordinary proof data.
- Only active clauses occur in inference, literal, unit, demodulation, and
  back-demodulation indexes.
- The initial mutable implementation can use current index code.  Packed
  immutable/delta indexes replace it after the ownership boundary is proven.

### Passive state

The first complete passive store uses one dense record per kept candidate:

- stable clause ID and proof-parent/justification reference;
- canonical literal/term-bank root IDs;
- raw and adjusted weights, matcher hint ID, hint epoch, and selector keys;
- compact subsumption/tautology features;
- simplifier epoch and state flags.

Selector queues contain record IDs.  Clause bodies live in append-only
compressed or mmap pages and are materialized into a bounded cache for
refresh, activation, printing, checkpointing, or proof reconstruction.  No
active index may retain a pointer into an evictable passive page.

### Hints

Hints use immutable canonical term roots, dense mutable sidecars, and a packed
matching index.  A sidecar contains stable hint ID, degradation count,
last-matched given, labels/attribute references, active/redundant state, and
epoch.  Matcher answer ordering must be defined and regression-tested because
the existing code selects the first equivalent or last subsumed hint returned
by its index.

### Collective frontier

After the dense cold-passive mode is correct, replace most individual passive
conclusions with resumable inference-batch descriptors:

- active parent IDs and inference rule;
- partner-ID range and literal/position/substitution cursors;
- simplifier and hint epochs;
- a conservative selection-key lower bound;
- proof-construction data.

A bounded candidate cache contains materialized promising conclusions.
Scheduling expands a batch when its lower bound can beat the cache or through
a fair round-robin budget.  Hint patterns get a dedicated discovery channel,
but ordinary fair expansion remains mandatory so hints cannot destroy
completeness.

## Delivery plan and gates

### Phase 1: establish the DISCOUNT ownership boundary

- Add an explicit `search_loop` option with `otter` as the default.
- In DISCOUNT mode, do not index or promote passive clauses during
  `cl_process`/`limbo_process`.
- At selection, activate the clause by installing active literal,
  back-demodulation, clashable, and demodulator indexes before inference.
- Report active/passive index membership and delayed-demodulator counts.

Gate: compatibility mode remains regression-identical; DISCOUNT mode proves
the basic proof suite and reports zero passive entries in active indexes.

### Phase 2: refresh semantics and exact hint trace

- Add simplifier and hint epochs.
- Lazily re-simplify a selected passive before activation and requeue a
  changed nonempty clause with a complete justification.
- Add optional hint-trace output and a differential trace harness.
- Define handling for back subsumption, back unit deletion, CAC processing,
  and hint degradation under lazy refresh.

Gate: every committed candidate in paired reference/new runs has an identical
hint tuple; all emitted proofs pass `prooftrans` and `directproof`.

### Phase 3: cold compact passive store

- Replace passive `Topform *` ownership with dense record IDs and mmap-backed
  serialized bodies.
- Convert given selectors to record IDs and materialize through a bounded
  cache.
- Extend checkpoint/resume to serialize passive records, selector state,
  epochs, and cache-independent hashes.

Gate: no raw passive pointer is retained by a selector or active index;
checkpoint/resume is deterministic within DISCOUNT mode; passive resident
bytes are at least 80% below the Phase 1 representation.

### Phase 4: compact hint bank and packed active indexes

- Publish hints and kept active terms in an immutable hash-consed term bank.
- Replace hint FPA and then active FPA/discrimination trees with packed
  immutable segments plus small mutable deltas and tombstones.
- Preserve matcher answer ordering explicitly.

Gate: full 310,153-hint Osborn preprocessing uses at least 80% less RSS and
returns identical matcher IDs/weights on the hint-trace corpus.  Active-index
bytes fall by at least 75% with no more than 10% throughput loss.

### Phase 5: Waldmeister collective batches

- Introduce resumable descriptors and a bounded promising-candidate cache.
- Add hint-aware lower bounds/discovery and a fair fallback schedule.
- Retain historical simplifier epochs needed for deferred conclusions.

Gate: the scheduler is demonstrably fair, proof reconstruction is complete,
and bounded-suite proof coverage has no unexplained regressions under matched
resources.

### Phase 6: radical acceptance

Run at least three AIM/Osborn-class searches whose compatibility-mode peak
RSS exceeds 100 MB.  Require:

- at least 80% lower externally measured peak RSS, with 90% as the stretch
  target;
- proof validation for every success;
- allocator/index/term/hint/frontier accounting explaining at least 95% of
  the RSS delta;
- no unbounded cache, descriptor, epoch, or proof-parent growth hidden from
  the accounting.

## Immediate implementation sequence

Implementation status on 2026-08-07:

- Phase 1 is implemented behind the default-off `search_loop=discount`
  option.  A focused proof/hint test confirms zero passive clauses in active
  indexes and validates the emitted proof with both `prooftrans` and
  `directproof`.
- The first Phase 2/3 increment adds `passive_store=compressed`: selector
  metadata remains resident, but cold literal/term bodies are serialized and
  materialized only for selection, disabling, printing, or checkpointing.
- The 500-given, 10%-hint Osborn run reduced passive body storage from
  38,835,944 to 3,573,597 bytes (90.8%) and peak RSS from about 145 MiB to
  114.7 MiB.  This is a 21% total-RSS reduction, not the final target.
- Epoch-based activation refresh is implemented: a stale selected clause is
  re-simplified against the current active state and is either requeued with a
  copy justification, discarded by active forward subsumption, or activated.
  Epochs and refresh counters are checkpoint metadata.
- A semantics-preserving `hint_index=compact` mode uses FPA depth 2.  FPA
  back-subsumption already sorts exact answers by stable clause ID, so matcher
  choice is independent of path depth.  On the 100-given, 31,014-hint sample,
  depths 1, 2, 3, 4, and 10 produced identical generated/kept/SOS counts,
  hint IDs/counts, and refresh counts.  Depth 2 reduced peak RSS from 72.2 MiB
  to 44.6 MiB and CPU time from 5.3 to 4.4 seconds; depth 1 saved only another
  3.6 MiB but took 18.7 seconds, so depth 2 is the selected knee.
- A default-off `hint_index=packed` mode now removes active hint term trees
  and the hint FPA entirely.  It keeps compressed hint bodies, dense mutable
  side tables, 128 path/symbol feature bitsets, and a 64-bit rewrite-symbol
  filter.  Candidates are materialized for the ordinary exact subsumption
  test and are sorted by decreasing stable hint ID, reproducing the old
  first-equivalent/last-subsumed choice.  Back-demodulation uses a conservative
  filter followed by an exact rewrite-direction/order check.
- The differential harness compares packed, compact depth-2, and legacy
  depth-10 modes.  Matcher ID, raw/adjusted weight, labels, degradation,
  epoch, proof outcome, and the entire trace agree.
- On the bounded 31,014-hint/100-given Osborn sample, packed and compact modes
  both report `Given=101`, `Generated=9554`, `Kept=2513`, `Sos=2344`, 1,354
  initially redundant hints, 29,050 active hints at exit, and 160 matches.
  Packed mode stores 31,012 compressed hint bodies, uses 0.50 MiB of feature
  bitsets plus 1.31 MiB of dense tables, and peaks at 32.2 MiB RSS versus
  43.0 MiB for compact FPA.  The run took 11.3 seconds and stayed within the
  explicit 30-second/256-MiB/100-given limits.
- Hint bodies are precompressed before packed-index construction, so the
  parsed term forest and packed index do not overlap.  Hint input
  justifications are discarded (hints are not proof ancestors).  `Topform`
  is now 96 bytes instead of 120 through mutually exclusive clause/formula
  storage, field ordering, and bit flags.
- The first Phase 5 increment is implemented behind
  `inference_frontier=collective` (and requires DISCOUNT plus a memory or
  mmap ancestor store).  Each activated given creates constant-size queued
  descriptor instead of eagerly enumerating its complete paramodulation and
  positive/negative-hyper sets.  The descriptor retains a paramodulation cursor
  into an append-only activation history and expands one historical partner
  at a time.  Hyper descriptors are one-shot sets evaluated against a
  temporary FPA index that reconstructs the active set at the descriptor's
  creation epoch.  Every concrete conclusion still
  enters the unchanged `cl_process` path, so normalization, exact hint
  matching, weight/degradation updates, filtering, and proof construction are
  performed before it can enter SOS.
- By default the scheduler alternates a given activation with one descriptor
  expansion and rotates incomplete descriptors; the explicit ratio described
  below can reduce the expansion rate.  It records activation
  and deactivation epochs in paged dense tables, materializes disabled parents
  from the ancestor archive, accounts for descriptors/history separately,
  and serializes the mixed descriptor queue in checkpoints.  One clashable
  eligibility bit per activation distinguishes active restricted denials from
  clauses that were actually in the historical resolution index.  A new
  `collective_trace` option reports each expansion, its parent IDs, generated
  and kept counts, and archive materializations.  Per-rule generation counters
  identified hyperresolution, rather than paramodulation, as the later
  candidate explosion on the sampled AIM prefix.
- On the bounded 31,014-hint/250-given Osborn run, keeping paramodulation
  collective but executing hyperresolution eagerly produced 34,709 clauses
  (33,431 from hyper) and left 4,846 SOS clauses.  The mixed collective
  frontier produced 6,641 clauses (5,625 from 98 expanded hyper batches) and
  left 1,364 SOS clauses: 80.9% fewer generated clauses and 71.9% fewer
  resident passives at the same given limit.  That first two-record prototype
  had 398 pending descriptors using 15,920 bytes, and activation history used
  24,832 bytes.  Peak RSS was 32.3
  MiB in both runs because packed-hint preprocessing, not the frontier, is the
  bounded run's peak; current RSS in the mixed run was 24.1 MiB.  The run
  stayed within 30 seconds and 256 MiB.
- `collective_given_ratio=N` makes the fair expansion rate explicit (default
  1).  Hyper and paramodulation state for an activation are now combined in
  one descriptor.  The first `N=4` implementation evaluated a delayed hyper
  batch against the *current* active index.  At 250 givens it generated 4,907
  clauses and retained 777 SOS clauses, but that index incorrectly admitted
  activations newer than the descriptor and lost clauses disabled after its
  creation.  Those figures remain useful as attribution data, not as the
  result of the corrected calculus.
- The corrected historical-hyper implementation uses the descriptor's
  activation limit and simplifier epoch.  It excludes future clauses and
  includes clauses active at creation even if they have since been disabled.
  The first correct scaffold rebuilt a temporary FPA index in activation
  order and materialized later-disabled parents for each expansion.  A focused
  regression back-rewrites both a nucleus and a required satellite before
  expansion and still requires the historical conclusion that a live-index
  lookup misses.
- The scaffold is now replaced by a reusable persistent historical index.
  Each activation contributes one immutable inference-only clause clone and,
  when hyper-clashable, indexes that clone once.  Paramodulation reads clones
  by activation position; hyperresolution filters index hits by activation and
  deactivation epoch.  Thus this state grows with selected givens (11,242 in
  the archived full Osborn proof), not with the millions of generated,
  passive, or disabled clauses.  Collective-hyper-only runs no longer maintain
  a duplicate live clashable index.  The later-disabled regression now uses
  zero ancestor materializations.  If SOS empties, the scheduler still drains
  descriptors, so the ratio changes fair interleaving rather than imposing a
  hard candidate cap.
- With historical snapshots, the same bounded 31,014-hint/250-given `N=4`
  Osborn run generated 830 clauses, kept 587, and retained only 2 SOS clauses,
  with 199 usable clauses and 248 pending constant-size descriptors.  It
  expanded 11 paramodulation pairs and 51 hyper batches.  The temporary
  snapshot scaffold indexed 1,315 historical clauses in total and at most 50
  at once.  Against
  the eager-hyper attribution baseline, this is 97.6% fewer generated clauses
  and 99.96% fewer resident passives at the boundary.  Against the flawed
  live-index collective prototype, it is 83.1% fewer generated and 99.7%
  fewer passive clauses.  The persistent implementation retains 250 history
  clauses occupying an estimated 88,976 body/header bytes, performs zero
  ancestor materializations, and reports 504 rejected future index hits.  It
  uses 15.44 CPU seconds and 33,092 KiB peak RSS versus 16.48 seconds and
  33,088 KiB for repeated snapshot rebuilding; the peak remains the packed-
  hint preprocessing floor.  These finite-boundary
  figures include deferred fair work and must not be extrapolated as a
  week-long reduction until the long-run acceptance experiment is run.
- The first hint-aware scheduler increment is an opt-in bounded descriptor
  probe, enabled by `collective_hint_probes`.  When a selected given has an exact
  `matching_hint`, its newly appended combined descriptor may move to the
  queue head to advance one eligible paramodulation pair.  One-shot hyper
  expansion is explicitly ineligible because one such call can emit an
  unbounded conclusion burst; it remains on the ordinary queue.  The probe
  consumes a single credit, and only a subsequent ordinary FIFO expansion
  restores it.  The marker is cleared before an incomplete descriptor rotates
  to the tail.  Consequently there is at most one probe between ordinary turns,
  so an unlimited stream of hint matches cannot starve older descriptors.
  This spends no memory proportional to generated clauses and never
  substitutes an approximate matcher for the unchanged exact `cl_process`
  path.  It is an early feedback channel, not yet the promised lower-bound or
  promising-pair cache for unseen conclusions.  The reason it is default-off
  is measured rather than hypothetical: on the 250-given/10%-hint Osborn
  boundary, an initial version that allowed one-shot hyper as a probe emitted
  2,842 hyper conclusions and left 502 SOS clauses, versus zero and two with
  probes disabled.  Restricting probes to one paramodulation pair reduced this
  to 907 total generated and 21 SOS, close to the disabled run's 830 and two;
  it used 15.52 versus 18.06 CPU seconds and the same 33,092-KiB peak RSS.
  This is a useful bounded experiment, but it does not justify accepting more
  frontier growth by default before solved-problem coverage is measured.
- Focused equality and hyperresolution examples both prove in collective mode;
  equality proofs pass `prooftrans` and `directproof`, and the predicate-hyper
  proof passes `prooftrans`.  Mixed queues, activation/deactivation history,
  the scheduler ratio/cursor, and all rule bits are checkpointed.  Collective
  resume deliberately rebuilds active FPA indexes because the existing
  serialized clashable trie lost leaf multiplicity needed for identical raw
  hyper enumeration.  Historical batches instead rebuild their persistent
  filtered index from activation history during resume.  The remaining
  deterministic-resume divergence was
  traced to `symbols.txt`: the old whitespace-token parser stopped at the
  first quoted symbol name containing a space, so restored raw symbol numbers
  changed secondary equality orientation.  The reader is now line-based and
  verifies every restored symbol number.  Collective binary checkpoint format
  `P9COLL5` first added clashable eligibility for each activation.
  `P9COLL6` additionally preserves the bounded hint-probe scheduler credit
  and its optional queue-head marker; the reader still accepts `P9COLL5`.
  A fresh persistent-history checkpoint at given 92 passed all 18 integrity
  hashes; its resumed and uninterrupted 200-given runs both ended at exactly 695
  generated, 566 kept, 162 usable, 161 SOS, 9 paramodulation pairs, 41 hyper
  batches, 200 history clauses, and zero archive materializations.  A fresh
  `P9COLL6` checkpoint at given 109 captured consumed probe credit, passed all
  18 hashes, and resumed to the exact uninterrupted 250-given probe boundary:
  907 generated, 605 kept, 21 SOS, 47 paramodulation pairs, 40 hyper batches,
  and 41 scheduled/expanded probes.  The same reader also resumed the old
  `P9COLL5` given-92 checkpoint to its original 200-given counts with 18/18
  hashes.
- `passive_store=dense` now removes passive ownership from `Topform`,
  `Clist_pos`, selector AVL nodes, active indexes, and the live clause-ID
  table.  Given-selection rules and semantics are evaluated once while the
  candidate is resident; stable ID, matcher hint ID, exact double weight,
  simplifier epoch, delayed-demodulator bit, and a selector-membership mask
  are retained in a dense record.  Per-selector binary heaps contain 32-bit
  record indexes and use lazy deletion.  Selection reconstructs and registers
  one clause before the ordinary refresh/activation path, so exact hint
  matching still occurs before archival and `hint_age` uses the saved stable
  matcher ID.
- The first dense prototype reused the proof-ancestor archive and thereby paid
  its general 96-byte header and retained passives as ancestor handles.  It
  served as a correctness scaffold, not the final memory representation.
  Dense passives now use a dedicated memory/mmap arena with a 40-byte
  checksummed record containing the already-packed body, justification,
  attributes, and only the Topform flags needed at activation.  Passive IDs
  are detached from the live ID table and registered again only on activation;
  the general archive is reserved for actual disabled proof ancestors.
- Before the historical-hyper correction, the bounded 31,014-hint/250-given
  Osborn run showed dedicated-arena mode search-identical to compressed
  DISCOUNT: 4,907 generated, 1,247 kept,
  198 usable, 777 SOS, 145 demodulators, 31,014 hints, and 28,564 active hints.
  It reports zero passive active-index entries and 630 delayed demodulators.
  Live passive payload is 42,517 bytes (26,187 body and 16,330 justification
  bytes); allocated selector-record, heap, and arena capacities are 114,688,
  12,544, and 131,072 bytes after 1,247 total insertions.  The proof-ancestor
  archive falls from 1,588 records / 245,414 used bytes in the shared-store
  scaffold to 341 records / 46,267 used bytes.  Peak RSS is 32,964 KiB versus
  33,088 KiB because packed-hint preprocessing is the fixed peak here.
- Dense checkpointing visits active records in clause-ID order, writes them as
  SOS, restores matching-hint IDs and selector cycle state, and excludes
  obsolete archive versions from disabled clauses.  A SIGUSR2 checkpoint at
  given 63 resumed to the exact 250-given search counts and all 18 integrity
  hashes passed.  As in the older format, ID-zero pre-elimination scratch
  clauses are intentionally not serialized, so their diagnostic disabled
  count is not a semantic restart invariant.
- Dense record and heap arrays now grow by 1.5x instead of 2x.  When at least
  1,024 historical records exist and inactive records reach half the active
  population, the selector compacts to active records, rebuilds every binary
  heap, and copies only live immutable bodies into a fresh memory/mmap arena.
  Clause IDs, selector cycle counters, exact double weights, hint IDs, and
  epochs do not change.  In the bounded checkpoint-resume run, one compaction
  reclaimed 349 selector records and 30,477 bytes of dead arena records; the
  run retained exact search counts and passed all checkpoint hashes.

The allocation table explains why body compression alone cannot be radical.
At 200 givens with 31,014 hints, hint-dominated FPA structures occupy about
41 MiB, while compressed passive bodies occupy about 0.48 MiB.  The packed
hint bank removes that fixed prefix cost, and the collective frontier attacks
the search-time growth rate rather than merely shrinking each generated
clause.

The full 310,153-hint acceptance run is intentionally not being executed on
the current low-RAM host.  The 10% sample is an implementation/regression
gate, not a claim that the Phase 4 80% full-input gate has already passed.
The Phase 5 gate is not yet claimed.  Hyper batches now query a persistent
versioned historical index and the focused later-disabled-parent regression
closes the known coverage bug.  The current index retains ordinary cloned
term trees per given; a packed/hash-consed representation and a compact sparse
deactivation map remain desirable before a week-long scale gate.  Phase 5
also still needs a bounded promising-pair cache, useful lower bounds, and a
hint-discovery channel for unmaterialized conclusions.  Exact matches on
selected givens now provide a starvation-safe one-turn descriptor probe, and
every emitted candidate passes the unchanged exact hint matcher and all
ordinary passive selection heuristics.  However, unseen conclusions still
cannot influence priority before their descriptor is expanded.  The bounded
checkpoint trace gate is now closed for both the
combined frontier and dense passive store, and dense inactive state is bounded
by automatic compaction rather than growing for the life of the process.
Phase 3 is not yet declared complete because the required Phase-1-to-dense
per-passive 80% result still needs a bounded synthetic/million-record
measurement.  Phase 5 still requires hint-aware discovery/lower-bound
scheduling, compact history storage, and long-run completeness/performance
gates.
