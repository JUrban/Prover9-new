# Packed-fast hint indexing plan

Date: 2026-08-09 (Europe/Berlin)

Branch: `packed-fast`

## Objective

Make the dense-passive, mmap-backed DISCOUNT configuration comparable in CPU
time to old OTTER/FPA while retaining its radical whole-process RAM saving.
The work is not complete merely when one microbenchmark improves: the final
gate is a proof-producing full `chat_test.in` run with exact hint behavior,
bounded memory, and CPU/wall time in the old-OTTER range.

Development is isolated behind a new `assign(hint_index,packed_fast)` option.
The meanings of `packed`, `fpa`, and the other existing modes stay frozen
until all differential and performance gates pass.

## Authoritative baseline

The input is `/project/bob/chat_test.in`, SHA256
`9781ee07691bc62e01f67534208620ca0be3f026f55248a227e9161f1ed17e6c`,
with 88,494 hints.

| Mode | Result | Given | User CPU | Wall | Peak RSS |
| --- | --- | ---: | ---: | ---: | ---: |
| Old P9 OTTER/FPA | proof | 2,945 | 765.69 s | 14:16 | 550,400 KiB |
| Current OTTER/FPA | same proof | 2,945 | 778.16 s | 14:28 | 445,576 KiB |
| Current OTTER/packed | same proof | 2,945 | 2,119.65 s | 37:11 | 444,080 KiB |
| Dense DISCOUNT/selected/packed, post-fix | no proof by wall guard | last report 2,268 | 2,312.71 s | 40:03 | 114,012 KiB |

The current 64-byte dense record was committed after the guarded long run and
is expected to save another roughly 3 MiB at that state.  The RAM objective
is therefore already approximately achieved; this branch must recover CPU
without restoring persistent full hint or passive term forests.

## CPU attribution and required speedup

At the last selected report:

```text
User_CPU                         2280.08 s
clock hints                     1977.71 s
ordinary packed match           1725.547 s
flipped packed match             239.660 s
equivalence                        7.950 s
packed hint back-demodulation      12.255 s
```

Hint work accounts for about 87% of user CPU.  Ordinary matching alone made
2,513,076 queries, scanned 6,234,457,893 posting IDs, and rejected
5,738,050,753 of them with the 64-bit fingerprint.  It materialized only
2,527,873 exact candidates, about one per query.  The front-end scan, not
exact candidate decoding, is the principal `chat_test` bottleneck.

The non-hint remainder is roughly 300--335 seconds.  To approach old P9's
765.69 seconds, packed hint work must fall from about 1,978 seconds to at most
400--430 seconds: a required speedup of approximately 4.5--5 times.  A 10%
matcher improvement is not success.

## Evidence that narrows the design

- Dense DISCOUNT with full FPA hints reaches the exact 300-given state in
  27.72 seconds and 151,056 KiB, versus packed's 50.03 seconds and 90,636
  KiB.  It is an immediately usable 1.8-times speed compromise, but does not
  meet the final CPU or RAM target.
- A serialized exact matcher cut materializations by about 90% but improved
  paired CPU by only 0.6%.  Serialized matching is not Phase 1.
- Literal-local composite postings cut posting visits about tenfold at 300
  givens but reduced materializations little and made total CPU worse.
  Adding more pointer/vector posting maintenance is not sufficient.
- Match feature depth three also regressed.  The new design must change the
  broad-candidate execution model, not merely add another shallow key.
- Eager interreduction peaks at 211,412 KiB and changes the historical hint
  trajectory.  Selected demodulation remains the baseline.
- On this input back-demodulation is a minor clock.  It remains semantically
  mandatory, but ordinary and flipped matching come first.  Large Osborn
  inputs need a later dedicated back-demodulation phase because their cost
  distribution differs.

## Semantic invariants

Every experimental implementation must preserve:

1. Conservative candidate retrieval: no possible exact match, equivalence,
   flipped match, or rewrite may be omitted.
2. Existing `subsumes` and `rewritable_clause_type` decisions remain the
   final authorities until a separately validated direct matcher replaces
   them.
3. Candidate IDs are enumerated in the established decreasing-ID order so
   first-equivalent and last-proper-subsumee tie breaking is unchanged.
4. Literal sign, polarity counts, equality flipping, AC/commutative theory
   handling, variables, generic and numbered `AnyConst`, and nonunit hints
   retain their behavior.
5. Labels, `bsub_hint_wt`, degradation, `hint_match_once`, expiry,
   `hint_age`, and matching-hint identity remain exact.
6. Rewritten, expired, redundant, and reactivated hints update the fast index
   without unsafe omissions or unbounded stale storage.
7. Preview queries remain side-effect free and do not contaminate
   authoritative operation accounting.
8. Checkpoint restore reconstructs identical derived state and reproduces the
   uninterrupted trace.
9. Packed storage remains stable-ID based; no index retains pointers into
   compressed or evictable term trees.

## Target architecture

### Phase 0: exact workload instrumentation

Add inexpensive aggregate counters for:

- exact feature-key counts and density distribution;
- seed posting length and distinct query-feature profiles;
- how often the same feature profile repeats between hint-index mutations;
- unit/nonunit, equality, theory, variable-root, and `AnyConst` query/hint
  populations;
- bitset words/containers that would be visited by an adaptive query plan.

This decides from measured data whether a dense bitmap, hierarchical bitmap,
or substitution trie is the correct fast path.  It also creates a replayable
bounded query corpus if that can be done without omitting mutation events.

### Phase 1: density-adaptive exact-feature sets

The current better packed index has about 804 exact structural keys.  A full
88,494-bit row is about 11 KiB; all 804 rows are about 8.5 MiB, comparable to
the current exact postings plus fingerprint arrays.  Replace rather than
blindly duplicate those structures.

Each key uses a representation chosen from its density:

- sorted 32-bit IDs for rare keys;
- dense or hierarchical bitsets for broad keys;
- a top-level nonempty-block summary so queries do not sweep empty regions.

An ordinary/flipped query intersects the rarest safe exact feature sets and
the active-hint set, applies polarity/profile masks, and enumerates surviving
IDs from high to low.  Broad vector scans and fingerprint rejection should
disappear.  Equivalence buckets stay unchanged initially because they consume
less than one percent of the measured hint CPU.

Mutation initially uses exact bit set/clear operations.  This avoids stale
IDs and rebuild spikes.  If sparse-vector mutation is retained, its stale
budget must remain explicitly bounded.

### Phase 2: unit-equation substitution trie

If adaptive feature sets do not reach the 4.5-times gate, add a compact fast
path for the measured dominant unit-equation population:

- flatten immutable terms into symbol/arity/variable tokens;
- share common prefixes in a discrimination or substitution trie;
- store stable terminal hint IDs in semantic order;
- traverse constant and variable branches directly for one-way matching;
- route nonunit, theory-sensitive, and `AnyConst` cases through the exact
  conservative fallback.

The trie must reduce asymptotic query work, not just move candidate vectors
into another container.

### Phase 3: Waldmeister-style shared term arena

Instrument parsed term-node count versus unique hash-consed node/edge count.
If sharing is substantial, replace separately encoded hint bodies with an
immutable DAG of 32-bit term-node IDs and compact literal/root arrays.
Matching uses separate contexts, so immutable variable and function nodes can
be shared safely.

The shared arena should support:

- allocation-free exact matching over node IDs;
- substitution-trie construction without persistent `Term *` forests;
- direct occurrence indexing for later back-demodulation;
- deterministic serialization/checkpoint reconstruction; and
- exact byte accounting.

This phase revisits the direct matcher only after candidate scanning is no
longer dominant.  It should replace packed bodies/index bytes, not become an
unbounded decoded cache.

### Phase 4: packed back-demodulation

Use the shared term IDs or a density-adaptive occurrence index to retrieve
only hint subterms compatible with a demodulator left side.  Preserve
`rewritable_clause_type`, rewrite/reindex ordering, and all hint events.
Optimize this phase against both `chat_test` and the existing 310,153-hint
Osborn artifacts; the two workloads have different dominant operations.

### Phase 5: proof-trajectory decision

Run fast selected DISCOUNT to proof or a substantially larger given bound.
If per-generated-clause CPU is competitive but the old proof is still not
found near the old given range, classify the remaining gap as scheduling,
not indexing.

The next design is then an OTTER-compatible compact frontier: preserve old
given selection and inference ordering while storing passive bodies in the
mmap/shared-term arena and using stable-node indexes to enumerate inference
partners on demand.  Do not hide a trajectory problem with more collective
scheduler tuning.

## Delivery and commit sequence

1. Commit this plan and frozen baseline identities.
2. Add `packed_fast` option plumbing and default-off instrumentation.
3. Commit focused density-adaptive set primitives with memory accounting and
   mutation tests.
4. Integrate ordinary/flipped candidate retrieval and commit exact trace
   tests.
5. Measure 300 givens; keep, revise, or remove the prototype based on total
   CPU, not a favorable internal counter.
6. Measure 1,000 givens only after the small gates pass.
7. Add the substitution trie/shared arena only if the measured remaining
   cost requires them.
8. Run the full proof/time/RSS comparison and update both the implementation
   report and user-facing options.

Commits should contain detailed bodies explaining representation ownership,
correctness invariants, measured tradeoffs, rejected alternatives, and test
evidence.

## Phase 0/1 checkpoint: epoch-scoped profile cache

The first `packed_fast` prototype adds a fixed 32,768-entry, direct-mapped
cache of final structural candidate vectors.  A key contains the complete
canonical shallow-feature profile, literal-polarity counts, first-literal
mask, and hint-state epoch; hash equality alone is never trusted.  Hits still
run the authoritative exact matcher in the established candidate order.
Vectors longer than eight IDs are deliberately not cached, which bounds the
table at 5 MiB and avoids an unaccounted variable-size arena.

On the exact selected-DISCOUNT 300-given boundary it preserved all search and
hint counters and changed the measurements as follows:

| Index | User CPU | Peak RSS | Ordinary + flipped posting IDs |
| --- | ---: | ---: | ---: |
| `packed` | 50.02 s | 90,636 KiB | 152,591,234 |
| `packed_fast` cache | 41.54 s | 90,624 KiB | 111,284,483 |

The cache hit 15,841 of 48,568 eligible queries (32.62%) and avoided
41,306,751 posting-ID visits.  This is a real 16.9% total-CPU improvement at
no observed RSS increase, but it misses the 32-second gate.  Epoch changes
and 3,723 candidate-vector overflows bound its reach.  It is therefore a
useful bounded first layer, not the primary Phase 1 solution; exact adaptive
feature-set intersection remains necessary for cache misses.

The next prototype lazily promotes only broad feature postings (at least
1,024 references) to exact ID bitsets.  A small summary bitmap marks nonempty
64-word blocks, and queries intersect summaries before touching data words.
At 300 givens only 51 keys were promoted, costing 835,584 bytes of data bits
and 13,056 bytes of summaries.  Ordinary/flipped posting-ID visits fell from
111.3 million in the cache-only run to 3.15 million.  Dependency-scoped cache
validity then allowed reuse across unrelated hint mutations: an entry is
invalidated only if its rare required feature, the `AnyConst` population, or
the complete posting index changes.  The resulting exact run used 36.04
seconds and 90,492 KiB peak RSS.

Thus Phase 1 currently saves 28.0% of total CPU versus `packed`, with slightly
lower measured RSS, while preserving the exact 300-given state.  It still
misses the 32-second gate.  The remaining ordinary-match clock is 18.55
seconds despite only 2.96 million counted posting candidates, so subsequent
work must separately attribute bitset planning/intersection, exact candidate
decoding, and subsumption rather than assuming posting counts remain the sole
bottleneck.

### Phase 1 completion candidate

Two further changes close most of the remaining prefix gap:

- Sparse profiles now scan their rarest posting but test every other required
  feature by exact bit membership.  This removes the lossy fingerprint from
  `packed_fast` ordinary/flipped misses while retaining the 512-reference
  break-even threshold for wordwise intersection.
- Unit clauses are matched directly against the bounds-checked version-2
  compressed stream in both subsumption directions.  Repeated variables,
  target variables, polarity, and private-flag semantics are handled exactly;
  unsupported/nonunit/`AnyConst` cases retain materialize-and-`subsumes`.

At the exact 300-given selected-DISCOUNT boundary the best run is now:

| Index | User CPU | Peak RSS | Ordinary materializations |
| --- | ---: | ---: | ---: |
| `packed` | 50.02 s | 90,636 KiB | 522,181 |
| `packed_fast` Phase 1 | 32.61 s | 90,492 KiB | 4,313 |

This is a 34.8% total-CPU reduction versus packed and only 17.6% slower than
the 27.72-second FPA result, with no measured RAM regression.  It misses the
aggressive 32-second target by 0.61 seconds but satisfies the more important
"comparable CPU" interpretation at this prefix.  A trace-enabled rerun emitted
15,039 records byte-identical to FPA and packed, SHA256
`505085c14b0576018a363cdad16beafddbf04aa9a03b6d2c0cf8f1cd2deda60c`.

Measured rejects were removed rather than accumulated: feature depth three
increased candidate/bitset work; a four-way 65,536-entry cache spent about
6 MiB for only 59 additional hits; eager clearing across every promoted row
saved too few candidates for its mutation cost; and a 256-reference dense
threshold regressed versus 512.  The next decision must come from the
1,000-given scaling gate and ultimately the proof run, not another small
constant tweak.

## Acceptance gates

### Correctness

- Existing hint-index, AnyConst, checkpoint, dense-passive, DISCOUNT,
  collective, eager, proof, and iterator tests pass.
- `HINT_TRACE` is byte-identical to FPA and current packed at bounded exact
  states.
- Generated, Kept, selector state, active/redundant/matched hints, matcher ID,
  weights, labels, degradation, and back-demodulation events agree.
- Checkpoint/resume after a hint rewrite agrees with uninterrupted execution.
- Produced proofs pass `prooftrans`; use `directproof` where its unit-
  paramodulation domain permits.

### Performance

At the exact 300-given `chat_test` boundary:

- target user CPU: at most 32 seconds;
- hard regression ceiling: current packed's approximately 50 seconds;
- target peak RSS: at most 105 MiB;
- exact search and hint traces are mandatory.

At 1,000 givens:

- ordinary plus flipped hint clocks improve by at least four times, with a
  stretch target of five times;
- total CPU is within 25% of dense FPA;
- peak RSS remains at most 120 MiB on the current machine.

For the final run:

- produce a proof, not merely a faster prefix;
- target user CPU at most 1.25 times old P9 (about 957 seconds);
- target wall time at most 18 minutes on the measured machine;
- target peak RSS at most 125 MiB, with a stretch target near 115 MiB;
- report both proof-boundary and guarded-prefix comparisons honestly.

Failure of a gate is evidence to redesign or remove the prototype.  It is not
permission to promote a merely 5--10% improvement or silently weaken hint
semantics.

## Reproduction discipline

- Pin one prover per physical CPU and record input/binary hashes, commit,
  limits, `/usr/bin/time -v`, and output status.
- Use hundreds of givens first on this host.  Do not rerun expensive old
  baselines whose authoritative artifacts already exist.
- Prefer three simultaneous long cases at most on the four-core machine.
- Inspect `/proc/<pid>/smaps` for anonymous versus passive-mmap residency;
  Prover9's historical `Megabytes` value is not whole-process RSS.
- Keep generated artifacts outside Git.  Commit source, tests, harnesses,
  plans, and reports only.
