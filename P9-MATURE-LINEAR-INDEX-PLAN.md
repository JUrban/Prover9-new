# Mature-run CPU recovery: linear-space indexes, not expiring caches

Status: the first retained-linear implementation landed in `950b230` and the
global shallow sparse-path stage landed in `1f1643d`, derived from the
complete large CHAT run and the `out61`, `out71`, and `out81` measurements.
Short prefixes remain useful for correctness and profiling, but they are not
performance acceptance evidence for this work.  A new full CHAT run is still
required before the CPU gate can be claimed.

## Corrected diagnosis

The ordinary-P9 reference `chat_test.new.out1.gz` proves at 11,368 givens in
5,110.38 user seconds and 3,603,800 KiB peak RSS.  The best completed compact
run, `chat_test.new.out4`, proves at the corresponding 11,369 givens in
9,573.63 user seconds and 941,091 KiB PSS.  Its main cumulative clocks are
3,230.41 seconds in forward demodulation and 3,622.83 seconds in backward
demodulation.  Backward retrieval alone examines 20,764,898,738 posting
groups for 1,941,156 queries and 695,309 returned candidates.  A mean of more
than 10,000 posting groups is read to return 0.36 candidates.  This is an
index-amplification problem, not a constant-factor problem.

The latest configuration repairs the accidental mature unit-root scan:
`out71` and `out81` use `compact_unit_strategy=code_tree`, which performs only
293 and 279 exact conflict tests by their current endpoints.  At 2,100 user
seconds their search progress is:

| Run | Strategy | Given | Active back records | Back work | Back lookup |
|:---|:---|---:|---:|---:|---:|
| `out4` | mask8 | 7,363 | 488,014 | 3,361,412,509 | 645.798 s |
| `out71` | adaptive | 7,298 | 483,124 | 2,403,115,835 | 584.718 s |
| `out81` | mask8 | 7,097 | 464,119 | 4,000,366,150 | 676.560 s |

The adaptive route is useful at this point, but the present representation
cannot preserve that advantage:

- `out71` has already used 55.3 MiB of its fixed 64 MiB hot-tree allowance.
  Its eight admitted roots still serve 183,417 queries, but the allowance
  does not grow with the indexed population.
- All 144 exact-position features have already been demoted, and admission is
  permanently frozen.  They served only 4,724 queries.  Each feature owns a
  one-bit-per-record membership vector; 144 vectors over a growing corpus are
  `O(features * records)`, even before their sparse occurrence postings.
- `out61` records the inevitable later state.  All eight trees have been
  demoted after the fixed tree budget fills.  Tree queries stop at 200,737,
  all subsequent queries return to the mask scan, and approximately 64 MiB of
  now-unusable tree storage remains allocated until rebuilding.
- `out71` spends 22.924 seconds on backward-index maintenance by 7,298
  givens.  Its 17.1 million census visits and 12.6 million backfill visits are
  bounded by accumulated lookup work, but they construct indexes that are
  then discarded.  Amortizing construction is not enough if its result is
  not retained.
- The new forward-rewrite hot path is faster on bounded local pairs, but the
  remote mature prefixes still take more demodulation time per rewrite
  attempt than `out4`.  Only the root of its radix tree has direct rigid-child
  lookup.  Sibling fanout below the root grows with the rule corpus and is not
  currently counted.

Consequently, changing admission factors or repeating 1,500/2,000-given runs
cannot close the mature gate.  A fixed-memory selective cache must eventually
become a linear scan as an unbounded active corpus grows.

`out61` also contains a separate configuration regression.  It did not set
`compact_unit_strategy=code_tree` or `compact_nonunit_path_filter`.  At its
11,015-given endpoint, its unit root scan has performed 14,107,951,845 exact
conflict tests and accounts for 3,252.106 sampled seconds.  `out4` reaches its
proof with 39,944,680 code-tree conflict candidates and 173.981 sampled
seconds.  The missing unit option alone therefore prevents `out61` from being
a fair measurement of the then-current recommended configuration.  Its
adaptive collapse is nevertheless real and independently visible: all eight
trees demote and all 80 position features demote and freeze.

## Required invariants

The replacement design must satisfy all of these invariants.

1. **Linear retained space.** Authoritative selective metadata may grow with
   live indexed occurrences, but not with query count, elapsed time, or the
   product of feature count and record count.  Its byte allowance is expressed
   per live/base-index byte rather than as a fixed lifetime ceiling.
2. **No incomplete positive index.** A selective posting or tree is queried
   only while it contains every live occurrence in its declared scope.  A
   miss always falls back to a complete route.
3. **No silent mature fallback.** Reaching a byte allowance stops new feature
   admission or replaces a lower-benefit feature.  It must not demote every
   useful route while retaining its unreachable bytes.  Reclamation happens
   in the same bounded rebuild that changes the admitted set.
4. **Sparse exact positions.** An exact `(root, path, symbol)` feature stores
   a compressed increasing stream of occurrence references.  The rarest
   complete stream alone is a complete candidate source; additional features
   may use sparse skip/intersection data, never a full record bitmap per
   feature.
5. **Bounded admission work.** Each corpus census/backfill is charged to
   measured avoided fallback work.  A retained feature has a measured benefit
   density (saved work per retained byte); replacement uses that density and
   occurs only at a stale compaction/rebuild boundary.
6. **Broad-pattern honesty.** A variable or genuinely broad pattern may scan
   a large stream.  Reports distinguish such queries from selective queries;
   the common selective class must not scan a fixed fraction of all active
   records.
7. **Bounded correctness-neutral caches.** Direct child caches may accelerate
   a retained radix/code tree.  Collision or eviction is a cache miss followed
   by the exact ordered sibling traversal, so cache residency never changes
   search semantics.
8. **Interval acceptance.** Cumulative totals are insufficient.  Each report
   exposes interval queries, index work, candidates, maintenance, and active
   population so work/query versus population can be plotted through the
   proof endpoint.

## Implementation status

### Implemented first retained-linear stage

Commit `950b230` implements the representation and lifetime changes needed
to avoid the specific fixed-cap collapse:

- `compact_back_sparse_positions` removes per-feature record bitmaps.  The
  narrowest exact sparse stream is a complete candidate source, and admitted
  streams remain live while new records are appended.
- `compact_back_position_budget_pct` supplies a population-relative soft
  allowance when the absolute position budget is zero.  Admission reserves
  growth space and temporary early-prefix failures are retried after the live
  population doubles instead of freezing admission forever.
- `compact_back_tree_budget_pct` makes the retained-tree allowance grow with
  the complete fallback index.  Existing complete trees do not demote merely
  because a fixed 64 MiB prefix ceiling was reached.
- `compact_rewrite_deep_cache_kb` exposes a bounded internal radix-child
  cache, but defaults to zero.  Its first bounded pairs did not establish a
  CPU win after accounting for telemetry overhead, so it is deliberately not
  part of the main mature configuration.

The focused suite passes under ASan/UBSan.  The accelerated 10,000-subject
variable-prefix probe returns the same sole answer while reducing steady
fallback work from 10,000 groups/query to one sparse posting group/query;
it reports one retained sparse feature, zero bitmap bytes, and zero
demotions.  A 600-given real CHAT smoke test preserved the expected trajectory
and reported the intended code-tree unit, nonunit path-filter, sparse-position
and population-relative-tree modes.  These results validate invariants and
early overhead only; they do not replace the proof-endpoint run.

The benefit-density eviction/rebuild policy described below is not yet
implemented.  Relative allowances in `950b230` are intentionally soft for an
already-admitted complete index: correctness and retention take precedence
over enforcing a transient byte cap.  This guarantees linear rather than
product space, but a general workload with a worse tree/base ratio still
needs the density-aware rebuild to impose a tighter constant.

### Implemented global shallow sparse paths

The demand-built stage can retain a selective feature after discovering it,
but discovery still depends on a repeated query shape and may happen too late
in a mature search.  Commit `1f1643d` adds
`compact_back_eager_position_depth`.  A nonzero value constructs complete
compressed `(subject root, exact child path, rigid symbol)` posting streams as
records enter the index, through only the stated depth.  It has four important
properties:

- construction is independent of query count and query order;
- a clause is traversed once, subtree boundaries are computed once, and local
  feature/occurrence pairs are sorted and deduplicated before append;
- the number of occurrence postings is linear in serialized term occurrences
  for fixed depth and signature, with no record-sized bitmap per feature; and
- query path hashes are only a conservative selector.  The existing compact
  matcher remains authoritative, so a hash collision can add work but cannot
  remove a candidate.

Eager mode deliberately disables demand admission for deeper positions in
this first version.  It therefore cannot reintroduce the historical
`features * records` census/backfill term.  Patterns without a useful rigid
position through the configured depth continue through the complete adaptive
tree or mask route.  Compaction reconstructs the shallow definitions from the
live records, and checkpoint sidecars omit them because they are derived
state.  The new complete-clause token-slice API makes that reconstruction safe
for multi-literal clauses and repeated argument terms; the older min/max
argument span was not necessarily a well-formed prefix forest.

The option defaults to zero and rejects an inconsistent configuration.  A
nonzero depth requires:

```prolog
set(compact_back_sparse_positions).
assign(compact_back_position_budget_kb,0).
```

The 10,000-record variable-prefix stress case returns the same sole answer
while reducing steady lookup work from 10,000 groups/query to one, with zero
bitmap bytes and zero demotions.  Focused insertion, later insertion,
deletion, forced compaction, checkpoint, decreasing-order, multi-literal, and
ASan/UBSan checks pass.  The depth-16 synthetic case is intentionally a
correctness stress configuration, not a production memory recommendation.

On four same-binary 600-given CHAT runs, depths 0, 1, 2, and 3 all ended at
the identical 601-given, 497,430-generated, 16,974-kept trajectory and used
38.79--39.26 user seconds.  Depth three selected a sparse path for 4,471 of
13,571 routed lookups and reduced total backward posting groups from
3,124,571 to 1,233,135 (60.5%).  The compact back-index allocation rose from
2,319,336 to 4,724,941 bytes and peak process RSS rose by only 256 KiB at this
short endpoint.  Equal total CPU here means only that the avoided back lookup
work has not yet become dominant; the 7,000--11,000-given external interval
is the acceptance test.

A follow-up depth-4/5 guard found the useful knee.  Depth four served 5,571
of the routed lookups and reduced posting groups to 1,048,333 while increasing
the back index to 5,282,285 bytes.  Depth five served only 89 additional
lookups and reduced another 7,641 groups (0.7%), despite creating another 749
feature definitions.  The production candidate is therefore depth four, not
an arbitrarily deep index.

The paired 1,500-given depth-zero/depth-three runs and the same-binary depth-
four follow-up all preserved the exact 1,501-given, 2,947,136-generated,
66,933-kept trajectory.  Depth zero used 187.30 user seconds and 99,280 KiB
peak RSS.  Depth three used 171.65 seconds and 108,644 KiB.  Depth four used
165.27 seconds and 107,904 KiB, an 11.8% user-CPU reduction from depth zero.
Its backward posting groups fell from 43,343,242 to 10,846,020 (75.0%) and
sampled lookup time fell from 21.828 to 6.900 seconds.  This establishes a
bounded crossover and selects the parameter; it still does not close the
proof-endpoint gate.

### Forward rewrite subjects are now atom-linear

The retained back indexes do not by themselves bound forward demodulation
CPU.  The old compact query path flattened every target subtree separately
during bottom-up normalization, so a deep atom paid the sum of its subtree
sizes.  The implemented replacement converts each literal atom once to a
transient flatterm and performs exact radix matching through its `next/end`
links.  Contracta and substituted already-reduced fragments stay in that
representation until the atom reaches normal form.  A recyclable bank-owned
scratch arena bounds retained overhead by maximum concurrent subject size.

Periodic output now includes `Compact_rewrite_subjects`.  The external gate
must inspect `target_nodes / initial_nodes`: this is the measured former/new
query-preparation node ratio, not an estimate from term depth.  Normal-form,
justification, compaction, checkpoint/resume, audit, generalization, and
ASan/UBSan checks pass.  No new runtime option is required; the change is the
authoritative compact demodulation implementation.

The clean release 1,500-given gate preserved the exact trajectory and reduced
user CPU from the previous depth-four build's 165.27 seconds to 154.45
seconds (6.5%), with peak RSS moving from 107,904 to 109,008 KiB.  Its
cumulative `target_nodes / initial_nodes` ratio was 4.8x.  This remains a
bounded gate, not a mature proof-endpoint claim.

The first full CHAT validation should retain the existing compact/file store
settings and add the following complete strategy block:

```prolog
assign(compact_unit_strategy,code_tree).
set(compact_nonunit_path_filter).
assign(compact_back_demod_strategy,adaptive).
set(compact_back_sparse_positions).
assign(compact_back_position_budget_kb,0).
assign(compact_back_position_budget_pct,50).
assign(compact_back_position_build_factor,32).
assign(compact_back_eager_position_depth,4).
assign(compact_back_tree_budget_kb,65536).
assign(compact_back_tree_budget_pct,200).
assign(compact_rewrite_deep_cache_kb,0).
```

The 64 MiB tree value is an initial floor in this configuration, not the old
lifetime ceiling.  The 200% value is deliberately conservative for the first
general validation; it permits retained tree metadata to grow to twice the
non-tree/non-position back-index bytes.  That is a CPU experiment, not yet a
final RAM constant.  The sparse position allowance is 50% of the base index
and contains no record-sized per-feature bitmap.  The rewrite cache remains
off until an independent paired long interval establishes a benefit.

### Mature residual: bounded depth eventually becomes a scan

The completed 2,100-second prefixes now separate the useful short-run work
from the remaining mature defect.  At the common endpoint, the adaptive
depth-four run had processed 7,298 givens and the older mask run 7,363.  The
adaptive run was slightly faster in the final interval (1.487 versus 1.397
givens/second) and reduced mean back-index work from about 9,081 to 6,006
posting groups/query, so the retained indexes are doing useful work.  They
are not population-stable: all 144 shallow position features had been
demoted, and thousands of records still reached the exact matcher per
query.  Raising the fixed depth merely moves that failure and multiplies
ancestor/path postings.

The next index must therefore satisfy three construction invariants before
another long parameter sweep:

1. its postings are bounded by the number of rigid occurrences in the
   indexed clauses, independent of run length, query order, and query depth;
2. insertion is one traversal of a record rather than one traversal per
   historical query feature; and
3. every filtering result is checked by the existing exact compact matcher,
   so collisions or feature coarsening can add candidates but never omit a
   redex.

The planned representation is a global rigid-edge index.  For every direct
edge whose parent and child are rigid, store the key
`(parent symbol, child number, child symbol)` and append the record ID once;
duplicates within one record are removed.  A rigid pattern contributes all
such edges at arbitrary depth.  Retrieval starts with the least-populated
edge posting and may intersect other postings before scanning the surviving
records for an exact occurrence of the complete pattern.  Total posting
entries are at most the number of rigid parent-child occurrences, so this is
occurrence-linear rather than `features * records` or `nodes * indexed
depth`.

This filter is complete for every pattern that contains a non-root rigid
node: such a node necessarily has a rigid parent because pattern traversal
stops below variables.  Root-only patterns and patterns whose immediate
children are all variables are honestly broad and fall back to the existing
root/tree route.  An edge may occur elsewhere in a candidate record, so an
edge hit is deliberately only a conservative record filter.  Intersecting
several edge postings reduces those false positives without adding bitmap
space.  This does not claim answer-linear retrieval for every possible term
distribution, but it removes the current arbitrary depth boundary and the
variable-before-selective-suffix radix failure with a general linear-space
contract.

The implementation gate is: identical decreasing proof-ID answers under
audit, insertion/deletion/compaction/checkpoint correctness, an arbitrary-
depth variable-prefix stress whose lookup population stays constant as
irrelevant records grow, and a paired CHAT prefix showing both the added
linear byte slope and the avoided mature posting-group slope.  The old
depth-four index remains available for A/B comparison until that gate passes.

## Remaining implementation sequence

### 1. Count and bound mature radix fanout

Generalize the compact forward-rewrite root-child map into a bounded positive
cache for hot internal parents.  Enable a parent only after a real sibling
scan crosses a small threshold.  Report rigid sibling checks, cache
lookups/hits/misses/replacements, and enabled parents.  The cache has a fixed
byte cap and is never authoritative; this removes repeated fanout without
creating a new long-run memory slope.

### 2. Replace position bitmaps with sparse occurrence postings

Retain the existing compressed position streams and exact compact matcher,
but make the narrowest posting stream the primary complete source.  Remove
per-feature record-sized membership vectors from the production path.  Do not
perform dense intersections unless a separate shared block summary proves
cheaper and its total space is linear in postings.

The position allowance should be a fraction of the complete fallback index,
with a small initial floor.  Both numerator and denominator then grow with
the same live occurrence population.  Admission stops before the allowance is
fully committed; existing streams retain growth reserve.  At compaction, rank
features by measured saved-work/byte density, retain the best complete set,
and rebuild it once over live records.

### 3. Make hot trees retained linear-space alternatives

Replace the fixed 64 MiB lifetime ceiling with a configurable fraction of the
complete path index.  The absolute option remains available as an emergency
ceiling, not the default mature policy.  Capacity growth first triggers stale
compaction.  If the retained set still exceeds its allowance, rebuild while
evicting the lowest measured benefit-density root; never leave a demoted tree
occupying the allowance that caused its demotion.

Tree and sparse-position routes are complementary.  A rigid rare position is
preferred.  A retained tree serves variable-prefix patterns for which a
finite position feature is weak.  The complete compact mask stream remains
the last resort, not the steady route for almost every mature query.

### 4. Mature validation

Focused tests prove exact decreasing candidate IDs under insertion, deletion,
compaction, checkpoint/restart, hash collisions, cache eviction, and byte
pressure.  Synthetic longevity tests grow the active population by at least
two orders of magnitude and reject any selective-query work proportional to
that population after warmup.

The real acceptance authority is then:

- identical proof/search trajectory modulo the established two-clause compact
  representation difference;
- paired interval measurements through at least 7,365 givens, followed by the
  11,369-given proof endpoint;
- no permanent collapse in retained tree/position coverage;
- backward work/query for selective intervals does not grow in proportion to
  active records;
- maintenance remains below 5% of user CPU and no repeated full-corpus
  admission scan survives without measured repayment;
- proof-endpoint user CPU is at most 1.05 times the 5,110.38-second ordinary
  reference before CPU competitiveness is claimed; and
- peak RSS remains materially below ordinary P9.  The project-wide 80% RAM
  target remains a separate hard gate; CPU may not be purchased by silently
  reconstructing ordinary P9's pointer-heavy index.

## Evidence identity

The files used for this diagnosis are:

```text
fd7a07f0a8ec1feafacb2672b664a393368ce728d0918c5e2708c7919d4a4406  chat_test.new.out1.gz
309074bb18e63cfb98034c9524a4c21e8e2f769e56c780ab4f37b49bfcab47c6  chat_test.new.out4
d8d84965d0f7f98c0e910b710c851bf2f21ea4f25987f34dfd4848c24207ebd2  chat_test.new.out61
30385cfc520bad1e230db7cd4ab84f709e492cd309e5703df6d8e9e62598223f  chat_test.new.out71
90f6c33cd5e3dad312ee43fc8ffaeb0a8d7b2e8732939ff02e2d0302462b1187  chat_test.new.out81
```
