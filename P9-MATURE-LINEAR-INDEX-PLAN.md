# Mature-run CPU recovery: linear-space indexes, not expiring caches

Status: the first retained-linear implementation landed in `950b230`, derived
from the complete large CHAT run and the `out61`, `out71`, and `out81`
measurements.  Short prefixes remain useful for correctness and profiling,
but they are not performance acceptance evidence for this work.  A new full
CHAT run is still required before the CPU gate can be claimed.

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
