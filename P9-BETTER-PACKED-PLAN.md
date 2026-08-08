# Better packed hint indexing

Date: 2026-08-08 (Europe/Berlin)

Branch: `better-packed`

## Objective

Keep the RAM advantage of `assign(hint_index,packed)` while removing its
large CPU penalty.  The result must preserve the observable hint behavior of
the current compact depth-2 FPA implementation, including hint identity,
weights, labels, degradation, equality flipping, back-demodulation, and
checkpoint/resume behavior.

This work is deliberately isolated from the running 4,000-given comparison.
The executable and configuration used by that run are frozen baselines.  New
code will initially be exposed under a separate experimental hint-index mode;
the meanings of `packed` and `compact` will not change until all differential
gates pass.

## Implementation outcome (2026-08-08)

The bounded development gates passed and the improved implementation is now
the meaning of `assign(hint_index,packed)`.  `hybrid` remains an exact alias
for command files made during development.  The former broad hashed candidate
bank is available only as `assign(hint_index,packed_legacy)` for diagnostic
reproduction; `compact` and ordinary FPA modes are unchanged.

The delivered index has:

- compressed hint bodies and dense mutable sidecars;
- stable 32-bit hint-ID postings with exact symbol/path keys;
- occurrence-correlated back-demodulation keys;
- selective ordinary, flipped, and equivalence candidate retrieval;
- conservative stale references with a 65,536-reference/25%-of-live rebuild
  threshold;
- compact equivalence buckets indexed by safe literal-count ranges;
- exact final `subsumes`/`rewritable_clause_type` decisions and decreasing-ID
  candidate order;
- generic and numbered `AnyConst` handling;
- deterministic derived-index reconstruction after checkpoints; and
- operation-specific candidate, materialization, rewrite, stale, and byte
  statistics.

Measured bounded evidence is:

| Workload | Mode/result | User CPU | Peak RSS |
| --- | --- | ---: | ---: |
| 31,014 hints, 100 givens | compact reference | 6.13 s | about 43.0 MiB |
| 31,014 hints, 100 givens | old packed | 10.88 s | about 32.2 MiB |
| 31,014 hints, 100 givens | improved packed | 3.98 s | 35,600 KiB |
| 310,153 hints, 100 givens | improved packed | 71.90--81.03 s | 295,808--296,064 KiB |

The full-hint improved run produced the exact first-100 compact given trace
(`620c7df29deac1f5cff1f5855570ca27c9a3a5594984f4bd742da18923e57ea2`),
`Generated=499`, and `Kept=436`.  It performed 912,856 total exact hint-body
materializations, including 548,856 back-demodulation candidates and 51,182
actual rewrites.  The full-hint peak is essentially the old packed peak
(295,680 KiB) and is 41.6% below the old-P9 507,108 KiB artifact.

The full 1,000-given improved run was deliberately not launched on this
low-memory/busy host.  The user-run 4,000-given campaign and a suitable-host
1,000-given run remain the final performance acceptance gates; therefore the
79-second 1,000-given CPU target is not yet claimed as measured.

## Evidence from the 1,000-given Osborn runs

The full input contains 310,153 hints.  The four relevant results under
`../bob/` are:

| Mode | CPU seconds | Peak RSS | Search result |
| --- | ---: | ---: | --- |
| Current legacy | 113.44 | 507,108 KiB | Reference search |
| Radical compact depth-2 FPA | 68.33 | 371,904 KiB | Radical reference |
| Radical packed | 441.39 | 295,680 KiB | Exact radical trace |
| Packed with `clear(back_demod_hints)` | 92.79 | 295,680 KiB | Diverges at given 181 |

The normal packed and compact runs have the same given-clause trace hash and
the same generated, kept, and SOS statistics.  Packed is therefore a sound
memory-saving representation in this test, but it is about 6.5 times slower
than compact.

The run with back-demodulation of hints disabled is not semantically
acceptable, but it is useful attribution evidence: packed CPU falls from
441.39 to 92.79 seconds.  Approximately 79% of packed's elapsed CPU is thus
associated with its current back-demodulation path.

Additional packed counters explain the mechanism:

- normal packed: 64,231,383 candidate checks, 159,442,356 materializations,
  64,231,269 recompressions, and approximately 115 GB of allocator traffic;
- packed without hint back-demodulation: approximately 15.4 million candidate
  checks, 15.7 million materializations, and 16.4 GB of allocator traffic.

The first target is therefore candidate selectivity, especially for hint
back-demodulation.  General allocator tuning or a decompressed-hint cache
would treat the symptom and risk surrendering the RAM saving.

## Constraint discovered in the current FPA implementation

The compact depth-2 FPA index cannot simply be retained after hint bodies are
compressed.

FPA posting arrays contain live `Term *` values.  Retrieval subsequently
performs exact match or unification operations against those terms, and
back-demodulation uses each term's `container` pointer to recover its clause.
Consequently, freeing a compressed hint's term tree would leave dangling
pointers in the ordinary FPA index.

The improved design therefore needs a candidate index whose durable values
are stable hint IDs, not pointers into evictable term trees.  Existing exact
subsumption and rewriting procedures remain the final authorities after a
small candidate set has been materialized.

## Semantic invariants

The implementation must preserve all of the following:

1. Candidate filters are conservative: they may admit false positives but
   must not reject a possible exact match or rewrite.
2. Exact matching, mutual subsumption, and `rewritable_clause_type` make the
   final decision using the existing code.
3. Candidate answers are visited in the order required by current semantics.
   In particular, stable decreasing hint-ID order must preserve the existing
   first-equivalent and last-subsumed choices.
4. Positive and negative literals, flipped equalities, theory terms,
   variables, `AnyConst`, and lex-dependent demodulators retain their current
   behavior.
5. `bsub_hint_wt`, `breadth_first_hints`, `degrade_hints`,
   `limit_hint_matchers`, `hint_match_once`, and `hint_age` retain their exact
   matcher identity and state transitions.
6. Rewritten, redundant, expired, or reactivated hints cannot leave visible
   stale index entries.
7. A checkpoint reconstructs the same logical candidate index and resumes
   with the same hint state and subsequent search trace.
8. The ordinary `packed`, `compact`, and legacy modes remain unchanged while
   the experimental implementation is developed.

## Target architecture

### Compressed hint bank and mutable sidecars

Retain the current packed compressed bodies and dense mutable hint metadata.
Do not restore persistent `Topform`, literal, or term graphs for all hints.
One hint may be materialized temporarily for exact checking, rewriting,
printing, or checkpoint processing and then recompressed.

### ID-based structural postings

While a hint is temporarily materialized, extract conservative shallow
features for its literals and for every subterm that may be rewritten.  Store
postings as dense 32-bit hint IDs.  Feature keys should use exact symbol IDs,
arities, literal signs, and short paths rather than a small hashed presence
mask that can collide.

A first implementation should prefer simple contiguous posting vectors with
explicit active/version checks.  Dense bitmaps are appropriate for very
common keys only if measurements show that their fixed footprint and
intersection speed are superior.  Pointer-rich per-occurrence structures are
out of scope.

Queries choose the rarest safe feature and, where profitable, intersect two
or more postings.  The resulting unique hint IDs are sorted in the required
decreasing order.  Only these survivors are materialized for the existing
exact operation.

### Back-demodulation index

For every nonvariable hint subterm that is eligible for rewriting, record a
small structural signature.  A demodulator-left-side query derives only
features guaranteed to be present in a match and retrieves candidate hint
IDs from the rarest compatible postings.

The query flow is:

```text
demodulator left side
        |
        v
safe exact structural features
        |
        v
rarest posting / posting intersection
        |
        v
unique hint IDs in semantic order
        |
        v
materialize survivor and run rewritable_clause_type
        |
        v
rewrite, recompress, and update postings when necessary
```

The structural filter must handle variable-root demodulators and
lex-dependent orientation conservatively.  If no useful safe feature exists,
it may fall back to a broader candidate set; correctness may never depend on
an optimistic feature.

Index mutation needs a bounded stale-entry policy.  The initial preference is
to remove and reinsert the rewritten hint's memberships directly.  If lazy
generations prove materially cheaper, obsolete postings must be counted and
periodically compacted so a week-long run cannot accumulate unbounded stale
IDs.

### Ordinary hint-matching index

Use the same ID-posting infrastructure for initial equivalence checks and
online hint matching.  Safe keys include:

- literal count and literal polarity profile;
- literal root symbol and arity;
- shallow symbol-at-path features;
- equality/theory shape features that do not exclude a valid flipped match;
- stronger invariant features for mutual-subsumption equivalence queries.

The present packed theory-term query can collapse almost to a root-symbol
test, which is especially broad for the Osborn equality hints.  Multiple
exact shallow keys and posting intersections should eliminate most of these
false candidates without changing the exact matcher.

Direct exact matching against serialized bodies is a possible later phase,
but it is not the first implementation.  Better candidate selectivity should
remove most decompressions while retaining the mature exact matching code.

## Delivery phases

### Phase 0: freeze baselines and improve attribution

- Record the baseline commit, binary identity, commands, and input hashes.
- Add separate clocks and counters for initial equivalence, ordinary/flipped
  matching, and hint back-demodulation.
- For each path report queries, posting candidates, unique candidates,
  materializations, exact positives, rewrites, reindexes, and stale skips.
- Report mean, maximum, and useful distribution buckets for candidates per
  query.
- Keep the instrumentation cheap and default-off where per-query detail would
  perturb normal runs.

Gate: on the existing 1,000-given artifacts or a bounded reproduction, the
new accounting explains at least 95% of packed hint time and agrees with the
existing aggregate counters.

### Phase 1: reusable ID-posting primitives

- Define stable feature keys and a compact posting representation.
- Implement insertion, deletion/versioning, rarest-key selection,
  intersection, uniqueness, and decreasing-ID answer order.
- Add focused tests for variables, repeated symbols, equality orientation,
  `AnyConst`, deletion, rewrite/reinsert, and posting compaction.
- Add exact logical-byte reporting for keys, postings, temporary query state,
  and stale entries.

Gate: unit and sanitizer tests pass, answer ordering is deterministic, and a
mutation stress test shows bounded resident and stale storage.

### Phase 2: packed back-demodulation

- Populate structural occurrence postings during packed hint construction.
- Replace the all-active-hint scan and 64-bit rewrite-symbol prefilter in the
  experimental mode.
- Preserve `rewritable_clause_type` and the existing rewrite/reindex path for
  final candidates.
- Time candidate lookup, exact checking, rewriting, recompression, and index
  maintenance separately.

Gate: the compact reference and experimental mode produce identical hint
rewrite events, active/redundant state, matcher results, and given-clause
trace through 1,000 givens.  Average materialized candidates per demodulator
should fall by at least two orders of magnitude from the present broad scan.

### Phase 3: equivalence and online matching

- Replace the current single-bit/feature-bank candidate choice with safe
  multi-key posting intersections.
- Reuse one decoded candidate within a query when several exact checks need
  it, without installing an unbounded decoded-hint cache.
- Preserve the exact candidate ordering and equality-flip behavior.

Gate: all hint differential tests and the complete 1,000-given trace agree
with compact; total materializations fall below the acceptance threshold.

### Phase 4: checkpoint and longevity hardening

- Rebuild the derived structural index deterministically from compressed
  bodies and sidecars on restore, unless measured restore cost justifies a
  separately checksummed index section.
- Test checkpoints before, during, and after hint rewrite/reindex activity.
- Stress repeated rewrite, disable, and restore cycles for stale posting
  growth.
- Validate produced proofs with both `prooftrans` and `directproof`.

Gate: uninterrupted and resumed runs have identical post-checkpoint traces,
and index storage remains bounded by live hints plus an explicitly limited
maintenance overhead.

### Phase 5: promote and document

- Compare the experimental mode against `packed`, compact depth-2 FPA, and
  legacy on the bounded Osborn matrix.
- Run the final 4,000-given/full-size comparison only on the suitable
  high-memory machine after the smaller gates pass.
- If all acceptance criteria pass, make the improved implementation the
  meaning of `hint_index=packed`; retain the previous algorithm under an
  explicitly named diagnostic mode only if it remains useful.
- Update the Markdown/LaTeX report, command examples, option reference, and
  memory forecast with measured rather than extrapolated results.

## Validation matrix

Development on the current low-memory machine must start with small or random
hint subsets and only hundreds of given clauses.  Existing output artifacts
are the source for baseline measurements; expensive historical cases do not
need to be rerun merely to reproduce their statistics.

The progression is:

1. focused synthetic matching and rewrite cases;
2. deterministic small/random Osborn hint samples and at most a few hundred
   givens;
3. full 310,153 hints with bounded given and time/RAM limits;
4. the established full-hint 1,000-given comparison;
5. the user-run 4,000-given comparison on the appropriate machine;
6. other large AIM cases after Osborn closes the gates.

Each differential run records:

- the ordered given-clause trace and its digest;
- generated, kept, disabled, SOS, and active-hint counts;
- matched hint IDs, weights, labels, and degradation state;
- hint rewrite/redundancy events and hint epoch;
- proof validation where a proof is found;
- internal logical bytes, allocator traffic, peak RSS, CPU, and wall time;
- candidate and materialization attribution by hint operation.

## Acceptance targets

For the existing 1,000-given full-hint Osborn comparison:

- exact radical-compact search trace and hint state;
- peak RSS at or near the current packed result, with 320 MiB as an initial
  upper bound and 295 MiB as the desired result;
- CPU no more than 15% above compact's 68.33 seconds, approximately 79
  seconds, after accounting for normal measurement variance;
- fewer than 1--2 million total packed hint materializations, compared with
  the current 159.4 million;
- at least a 100-fold reduction in average back-demodulation
  materializations per query;
- no unbounded decoded cache, stale posting growth, or pointer-rich duplicate
  hint representation.

The targets may be refined by Phase 0 counters, but they may not be relaxed
merely to accommodate an implementation that trades most of the packed RAM
saving back for speed.

## Risks and fallback choices

- A shallow index may remain broad for highly variable terms.  Add safe
  deeper path features incrementally, selected from measured candidate
  distributions.
- Frequent rewritten-hint mutations may make vector deletion costly.  Use
  generations plus bounded rebuilds only after measuring direct removal.
- Large posting intersections may allocate excessive temporary memory.  Use
  reusable bounded scratch arrays or streaming intersections.
- A proxy-term FPA is an alternative, but ordinary FPA retrieval still wants
  live candidate terms for exact matching.  It should be considered only if
  an explicit candidate-only FPA API can be introduced without retaining the
  complete hint term forest.
- A decoded-hint cache can improve repeated checks but obscures the RAM
  bound.  Any later cache must have an explicit byte limit and be reported in
  statistics.
- Term sharing is valuable for live active/history clauses, but it is not the
  first packed-hint remedy: compressed bodies already avoid the full live
  hint forest, while the measured packed problem is repeated decompression
  caused by poor filtering.

## Commit discipline

Keep changes reviewable and bisectable.  Planned commit boundaries are:

1. this design and benchmark baseline;
2. operation-specific packed instrumentation;
3. ID-posting primitives and focused tests;
4. back-demodulation integration and differential tests;
5. equivalence/ordinary matching integration and differential tests;
6. checkpoint/longevity hardening;
7. measured results and user documentation.

Each implementation commit should explain the ownership model, preserved
semantic invariants, relevant counter changes, commands run, and known
limitations in its commit body.
