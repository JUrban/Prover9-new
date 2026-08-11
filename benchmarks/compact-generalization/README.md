# Compact-index generalization benchmarks

This directory freezes the development and holdout cases for
`P9-GENERAL-COMPACT-INDEXING-PLAN.md`.

The manifest deliberately separates:

- `training`: cases whose measurements may guide algorithms and parameters;
- `holdout`: cases examined only at a phase promotion gate; and
- `control`: small correctness cases that are not performance evidence.

An absolute source path records where the current shared-workspace artifact was
found.  The SHA-256 digest, not that mutable path, identifies its contents.
Before a run, the harness must fail if the source digest differs.

## Embedded large input

The 11,000-given input is echoed inside
`/project/bob/chat_test.new.out3.gz`.  Reconstruct it with:

```sh
test.src/extract_prover9_input.sh \
  /project/bob/chat_test.new.out3.gz \
  /path/to/frozen-chat-test-new.in \
  2b7ab154323801bd541962d2314a23a5861def3fde53271330e4ebd2a54d340e \
  dfd5df473b9b1dcae073e324297cdcbb11c0535e838ca86422bdefc75d8ea84e
```

The reconstructed file has 18,400 lines and 901,624 bytes.  The extraction
removes the two Prover9 section-marker lines and a Prover9-generated `WARNING,`
line emitted after the final input list but before the closing marker.  That
diagnostic was not part of the original source and is not a parseable term.

## Holdout discipline

Do not use holdout CPU, candidate distributions, or memory to choose index
features, strategy thresholds, or cache sizes.  Holdout cases may be run for a
promotion gate only after the candidate commit and configuration are recorded.
If a holdout case exposes a defect, retain it permanently as a regression case
and freeze a new holdout partition before another tuning cycle.

## Timing discipline

Correctness-only cases may run in parallel.  Promotion CPU measurements run
sequentially on a quiet machine, pinned to one real core, with binary and input
digests, `/usr/bin/time -v`, and the complete Prover9 statistics recorded.

## Bounded runner

The generalization runner uses only training/control cases unless holdout access
is explicitly authorized.  Its defaults are deliberately small: one Osborn
training case, 100 givens, 120 CPU seconds, and 2 GiB per variant.

```sh
P9_MATRIX_CASES='osborn-chat chat-new-11k nil3-1k mbol-nil3-a' \
P9_MATRIX_VARIANTS='legacy packed_only compact_full compact_dense_file' \
P9_MATRIX_MAX_GIVEN=300 \
P9_MATRIX_MAX_SECONDS=600 \
P9_MATRIX_MAX_MEGS=4096 \
  test.src/compact_generalization_matrix.sh /path/to/results
```

Results include the complete generated input, binary/input hashes,
`/usr/bin/time -v`, prooftrans output for completed proofs, `summary.tsv`, and
machine-readable `profiles.tsv`/`profiles.json`.  Component-isolation variants
are `compact_demod_only`, `compact_unit_only`, `compact_back_only`, and
`compact_nonunit_only`; they retain full passive bodies so the other legacy
indexes remain valid.  `compact_nonunit_path` enables the experimental rigid
path prefilter for the isolated nonunit index.  The paired full-index variants
are `compact_full_nonunit_path` and `compact_dense_file_nonunit_path`; their
unsuffixed counterparts explicitly clear the filter and are the controls.
The product-checkpoint compositions
`compact_full_code_tree_nonunit_path` and
`compact_dense_file_code_tree_nonunit_path` additionally select the shared
unit code-tree traversal while retaining the recommended complete `mask8`
backward-demodulation fallback.
The filter stores a 64-bit Bloom summary of signed rigid symbols at exact term
paths.  A subset failure is a safe pre-materialization rejection; collisions
only retain extra candidates, and ordinary exact subsumption remains the final
authority.

`P9_MATRIX_CLOCKS=0` disables high-frequency CPU clocks while retaining the
logical work counters and external `/usr/bin/time` measurement.  Use it only
as a paired instrumentation-overhead diagnostic; operation-level time fields
are then zero and cannot support component attribution.  Promotion profiles
retain the default value 1 unless a lower-overhead timing implementation has
been validated.

Packed-hint operation timing now samples a deterministic hash-selected 1/64
of authoritative queries with microsecond user-CPU readings and scales the
sample to the exact timed-query count.  The statistics line labels
`timing=sampled` and reports eligible and sampled counts.  Candidate, posting,
stale, materialization, match, and histogram counters remain exact; only
`seconds` is estimated.  Read-only previews are excluded as before.  This
replaces two `getrusage` calls on every hint query, which became a large
source of avoidable system calls on hint-heavy AIM searches.  Maintenance
rebuild time remains exact because rebuilds are infrequent.

The experimental position-compatible unit retrieval is
selected by `compact_unit_position`, `compact_full_position`, or
`compact_dense_file_position`; the corresponding variants without the suffix
remain root-scan controls.  This first prototype is intentionally not the
default: at 100 givens it reduced unit-unification exact tests from 5,935 to 53
on the Osborn training case and from 4,620 to 76 on nil3, but increased the
reported unit-index bytes by 94% and 29%, respectively.  It establishes a
selectivity oracle while failing the 20% index-metadata promotion gate.
The `compact_unit_code_tree`, `compact_full_code_tree`, and
`compact_dense_file_code_tree` variants instead traverse the already-owned
radix-compressed unit term tree and allocate no position-posting table.
The harness emits an explicit strategy assignment for root-scan controls too.
String-option values enter Prover9's symbol table; omitting the control value
would shift later problem symbol numbers and change collision rates in the
legacy 64-bit unit-instance prefilter, making exact-test counts incomparable
even when ordered generated-clause traces are identical.

In the corrected optimized 100-given matrix, code-tree versus explicit
root-scan reduced unit-unification exact tests from 5,935 to 11 on Osborn and
from 4,620 to 3 on nil3.  The paired unit-instance counts and unit-index bytes
were identical; x2 retained the same 12-given proof.  End-to-end CPU at this
small boundary remains noise-dominated because unit unification accounts for
only milliseconds, so larger gates—not this prefix—decide promotion.
The same strategy also traverses the shared tree for stored-instance retrieval
(backward unit subsumption), using a distinct compatibility rule and retaining
the direct repeated-variable matcher at terminal postings.  On the 100-given
cases this reduced exact instance tests from 100,042 to 114 on Osborn and from
1,769 to 17 on nil3.  The Osborn legacy-index audit reported zero answer/order
mismatches; the added persistent state is three 64-bit reporting counters.

Backward-demodulation experiments use `compact_back_signature`,
`compact_full_signature`, or `compact_full_code_tree_signature` (and matching
dense-file names).  These select `compact_back_demod_strategy=signature32`;
controls explicitly select `mask8` so option parsing cannot perturb problem
symbol numbers.  `signature32` records two hashed bits for every rigid
path/symbol fact at arbitrary depth.  Collisions only add work because direct
compact matching remains authoritative.

The finer signature initially exposed a general packing defect: every sparse
path bucket paid for a 64-byte posting block.  Posting blocks now use a
16-byte payload and the first group is stored inline in its bucket; the
high-cardinality signature directory also grows by bounded 25% increments.
On the corrected 100-given Osborn pair, signature32 reduced examined groups
from 18,566 to 2,687 and occurrences from 14,762 to 2,139 with the same 278
candidates.  Back-index bytes rose from 209,144 to 248,520 (18.83%), within
the 20% replacement gate; whole-process RSS was unchanged within measurement
noise.  The next 300-given gate rejected the prototype: it reduced examined
groups from 371,423 to 22,746 and occurrences from 304,611 to 18,093, with the
same 1,148 candidates, but back-index bytes rose from 560,773 to 750,485
(33.83%).  User CPU changed from 16.50 to 16.15 seconds, which is too small to
interpret as a stable speedup at this boundary.  The extra exact-signature
buckets scale faster than the work they replace, so `signature32` remains a
diagnostic strategy rather than a production candidate.  Phase 3 must replace
the mask/signature directory with compact structural retrieval instead of
adding a wider per-occurrence signature.

The first structural replacement is available explicitly as
`compact_back_demod_strategy=code_tree`, through the matrix variants
`compact_back_tree`, `compact_full_back_tree`, and
`compact_full_code_tree_back_tree` (with matching dense-file variants).  It
radix-compresses complete serialized subterms, stores occurrence references at
terminal nodes, traverses variables with one-way matching semantics, and
retains the direct repeated-variable matcher plus decreasing proof-ID sort as
the final authority.  Deletion, forced rebuild, term-pool rebasing, and audit
mode use the same strategy.

This uncompressed structural prototype is also diagnostic, not promoted.  On
the corrected 100-given Osborn comparison it preserved the same 278 candidates
and reduced examined groups from 18,566 (`mask8`) and 2,687 (`signature32`) to
1,988.  It used 324,504 back-index bytes, versus 209,224 and 248,600 bytes,
respectively, and its 3,708 tree nodes plus 2,491 terminal descriptors made
construction more expensive.  Whole-run user CPU was 11.39 seconds versus
8.70 for `mask8`, although the 0.009-second tree lookup clock shows that this
small-prefix timing is dominated elsewhere and needs repetition before being
attributed.  The result justifies structural traversal but
rejects a full occurrence tree as the production representation; the next
prototype must compress terminal postings and/or admit the tree only where a
complete, budgeted structural partition can beat the compact fallback.

The follow-up representation removes the raw prototype's per-occurrence
stream.  Complete serialized terms are prefix-free, so a 16-byte node can use
one flagged word for either its first child or terminal posting list.  Each
16-byte terminal stores a representative token offset for one exact match,
one inline clause ID, and a tail.  A terminal's second clause ID is encoded
directly in the otherwise unused tail-head word; only a third clause allocates
a posting block.  Later IDs are absolute varints, avoiding persistent
last-record state.  One direct match of the representative enforces repeated
variables for every clause in the terminal; the ordinary rewritability test
remains the final search-level authority.

On Osborn at 100 given, the packed exact tree returned the same 278 candidates,
examined 428 posting groups and 1,430 representative terminals, and used
246,080 bytes versus 209,224 for `mask8` (+17.62%).  User CPU was 7.95 seconds
in that tree run; nearby paired controls ranged from 8.31 to 8.70 seconds, so
the safe claim is no measured slowdown rather than a speedup.  At 300 given it
returned the same 1,148 candidates, reduced posting groups from 371,423 to
1,958 (189.69x), examined 9,126 representative terminals instead of 304,611
occurrence offsets, and used 671,128 bytes versus 560,773 (+19.68%).  User CPU
was 16.44 seconds versus 16.50 in the recorded control.  Thus it passes the
frozen 20% metadata gate and the two-orders-of-magnitude posting-group portion
at 300.  Representative terminal matches fall 33.38x rather than 100x relative
to old occurrence-offset examinations, so Phase 3 is not yet promoted;
`code_tree` remains explicit pending the other training cases and the
1,000-given gate.

A tested alternative that added a second full inline word to every terminal
was rejected: at 300 given it increased bytes from 696,008 to 710,016 because
sparse terminals outnumbered saved blocks.  The flagged direct-second encoding
gets the sparse benefit without taxing singleton terminals.  The no-demodulation
nil3/mbol controls remained inert at 100 given, and x2 retained the same proof
and candidate answers under legacy audit.

The required 1,000-given Osborn gate rejects promotion of the full packed tree.
It returned the same 6,755 candidates and cut posting groups from 9,556,812 to
10,977 (870.62x) and occurrence/terminal matches from 7,960,042 to 55,041
(144.62x).  Lookup time improved only from 3.386 to 2.993 seconds because tree
traversal visited 26.099 million nodes.  More importantly, the back index grew
to 4,847,144 bytes versus 3,078,680 for `mask8` (+57.44%); tree nodes and
terminals alone occupied 2,773,920 bytes.  Whole user CPU was 80.82 versus
77.14 seconds (+4.77%).  Peak process RSS differed by only 564 KiB at this
small absolute scale because the large hint set dominates, but the linear
unique-term metadata would become material in a long search.

This is the deliberate generalization failure the staged gates are intended
to expose: passing 100/300 does not make a representation production-safe.
The next prototype must keep a complete compact fallback for every occurrence
and place structural retrieval behind an explicit byte budget.  A partially
populated tree can never be queried as if complete.  Safe choices are either
(1) a length-partitioned tree that indexes every subject above a current
cutoff and uses the fallback below it, rebuilding deterministically when the
budget raises the cutoff, or (2) sealed structural generations plus fallback
retrieval for every uncovered generation.  Budget exhaustion must degrade to
the complete fallback, never silently omit candidates.

The first safe partition is exposed as
`compact_back_demod_strategy=hybrid_tree`.  It always builds the complete
`mask8` fallback and additionally indexes every subject subterm with at least
`compact_back_tree_min_tokens` serialized tokens.  A query uses the tree only
when its own minimum token count reaches that cutoff; every matching subject
must then belong to the complete tree partition.  Otherwise it uses `mask8`.
`compact_back_tree_budget_kb` (default 65,536) bounds conservatively estimated
persistent tree metadata.  A clause that could exceed the budget is not added
to the tree, `tree_complete` becomes false, and all subsequent queries use the
fallback.  The partial tree is never queried.  Statistics report the cutoff,
budget, estimated bytes, completeness state, and exhaustion count.

This mechanism is correct and bounded but token length alone is rejected as
the final admission policy.  On the current-build 1,000-given Osborn trio,
`mask8` used 3,078,736 bytes and examined 9,556,812 groups.  Cutoff 12 used
4,215,984 bytes (+36.94%) and examined 3,764,171 groups (-60.61%).  Cutoff 14
used 3,545,184 bytes (+15.15%) but still examined 6,669,165 groups (-30.22%).
User CPU in the parallel bounded runs was 81.35, 79.34, and 79.28 seconds,
respectively; those timings show no gross regression but are not quiet-machine
promotion evidence.  The tradeoff is too weak for radical improvement.

The next structural partition should therefore be by complete root symbol,
not length.  Keep `mask8` for every root; measure fallback work per queried
root; admit a root only after its cumulative false-candidate work justifies a
backfill; backfill every active occurrence of that root before marking it
complete; and index all later occurrences for admitted roots.  Budget failure
leaves that root on `mask8`.  This preserves completeness independently for
each root while directing structural bytes toward the actual hot query
families rather than every unique constant and subterm in the problem.

That hot-root policy is now available diagnostically as
`compact_back_demod_strategy=hot_root_tree`, with
`compact_back_tree_admit_work` setting the deterministic cumulative fallback
group threshold.  Admission first counts and budget-checks the complete root,
then backfills every active occurrence before the root is marked usable.  The
complete `mask8` index remains authoritative if admission is rejected or the
global structural budget is exhausted.  Forced and materialized compaction
copy the per-root decision state, and the focused and legacy-audit tests cover
admission, later insertion, deletion, rebuilding, and exact answer order.

The 100/300-given Osborn gates reject this policy as the Phase-3 production
answer.  At 100 given, thresholds 256, 1,024, 4,096, and 16,384 used 337,808,
304,008, 288,392, and 215,480 bytes, respectively, versus 209,224 bytes for
`mask8`; only the last threshold admitted no roots.  At 300 given, thresholds
4,096, 16,384, and 65,536 admitted 7, 3, and 2 roots and used 992,917,
882,133, and 829,829 bytes, versus 560,773 bytes for `mask8`.  The most
conservative active policy therefore cost 47.99% extra, outside the 20% gate,
and user CPU was not improved.  Root heat identifies where lookup work occurs,
but a complete root tree still duplicates too much fallback storage.  The next
Phase-3 candidate must be the planned exact-position posting index, where one
occurrence participates only in selected structural facts and queries intersect
the rarest safe facts rather than materializing a second complete term index.

The first such candidate is exposed as
`compact_back_demod_strategy=position`.  The complete `mask8` occurrence index
remains authoritative.  When one fallback query examines at least
`compact_back_tree_admit_work` groups, the index performs one sequential scan
of compact records and counts every rigid `(root, relative-path, symbol)` fact
from that query.  It admits only the rarest fact when its clause posting is at
least four times smaller than the observed fallback work.  A 64-bit hash
represents an arbitrary-depth child path; collisions merge postings and can
only add final compact matches.  The admitted posting stores each matching
clause plus the exact root-occurrence offsets carrying the fact, so subsequent
queries do not rescan the complete clause.  Later clauses, deletion, encoded
and materialized rebuilding, and decreasing proof-ID order retain the same
semantics.

Position metadata is bounded by the stricter of
`compact_back_tree_budget_kb` and 20% of the complete fallback index bytes.
Admission is conservatively preflighted.  If later growth would cross that
cap, the position partition is marked incomplete and every query returns to
the complete fallback; a partial posting is never queried.  The focused test
covers a rare arbitrary-depth fact, multiple same-feature occurrences in one
clause, later insertion, deletion, forced compaction, and budget rejection;
the whole-search legacy audit reports zero mismatches.

This is a checkpoint rather than a promoted policy.  At 100 given, threshold
256 admitted three features and used 217,200 bytes versus 216,688 for an inert
position control and 209,520 for `mask8`; only one later query reused a
feature.  At 300 given, threshold 1,024 admitted 13 features, used 588,870
bytes, and reduced examined groups from 351,722 to 301,769 and occurrences
from 295,055 to 258,461 relative to the same-strategy inert run.  Its 81,948
record examinations for census, backfill, and feature lookup made the short
run slower (17.32 versus 15.90 user seconds in parallel measurements).  The
next gate asks whether reuse amortizes that deterministic construction cost at
1,000 given; otherwise admission needs a bounded repeated-use probation stage,
not another problem-specific threshold.

The raw one-query admission rule fails that gate.  At 1,000 given, threshold
2,048 admitted 198 features, reduced groups from 8.422 million to 7.155
million and occurrences from 7.044 million to 6.187 million, and used 3.364 MB
versus 3.087 MB (+8.95%).  But 3.708 million backfill-record scans raised user
CPU from 77.68 to 81.51 seconds in the bounded parallel comparison.  The
representation is compact enough; speculative construction is the deeper
problem.

Position admission therefore now has two workload-independent gates.  A
byte-bounded two-choice heavy-hitter table first accumulates fallback work for
exact query features.  A feature must occur in at least two broad queries and
its cumulative work must cover eight complete live-record passes before any
census.  The table receives at most one quarter of the auxiliary metadata
budget, capped at 128 KiB, and its state survives index rebuilding.  The
subsequent census retains the fourfold selectivity and total-byte gates.
`compact_back_position_admission` explicitly enables this probation policy;
it is clear by default.  Matrix variants ending in `_back_position_off` provide
the same-strategy, same-option-value control.

The fair 1,000-given Osborn comparison keeps the policy diagnostic rather than
promoting it.  With threshold 1,024, admission-off and admission-on had
identical fallback structure and examined exactly 7,955,664 groups and
6,580,140 occurrences.  Probation admitted no features, attempted three
censuses that failed selectivity, added 8,192 bytes, and used 84.46 versus
82.15 user seconds.  At 300 given on the corrected `chat-new-11k` training
case, it performed only 17 counter updates, admitted nothing, and added 16 KiB;
the search work was identical.  The no-demodulation nil3/mbol training cases
allocated no probation index.  Thus exact positions remain a correct,
byte-bounded research path, but the recommended backward-demodulation strategy
remains `mask8` until a representation can eliminate construction overhead on
more than one rewrite-heavy training workload.

Phase 4 begins with the opt-in `compact_nonunit_path_filter`.  It augments the
complete numerical feature tree with one 64-bit Bloom mask per physical
nonunit record.  The mask represents signed rigid symbols at ordered argument
paths.  Before an ID leaves the feature tree, forward retrieval requires the
stored mask to be a subset of the query mask; backward retrieval applies the
reverse subset.  These are necessary conditions for the ordinary ordered
one-way matcher used by `feature_subsumes_raw()`.  Collisions can only retain
extra candidates, and the ordinary exact routine is still authoritative.

The first corrected 100-given training comparison is encouraging but not a
promotion result.  On Osborn, forward exact tests fell from 4,561 to 2,286 and
backward tests from 833 to 504 while nonunit-index bytes rose from 60,104 to
62,152 (+3.41%).  On `chat-new-11k`, the corresponding changes were
3,821 to 2,263, 638 to 362, and 57,032 to 59,080 bytes (+3.59%).  Nil3 changed
229 to 145 forward tests, 99 to 73 backward tests, and 142,536 to 143,560
bytes (+0.72%).  The full-passive isolation variants materialize no archived
clauses, so the next bounded dense-file comparison must determine whether
these candidate reductions translate into fewer archive reconstructions.
End-to-end times at this prefix are recorded but treated as noise; the paired
Osborn values were 11.25 and 9.57 user seconds, whereas `chat-new-11k` and
nil3 moved slightly in the opposite direction.

The matrix recipes now enable the required compact unit index for both
nonunit-isolation variants.  Before that repair they terminated during option
validation and therefore supplied no performance evidence.

The 300-given dense-file comparison confirms that the first mask acts at the
intended archive boundary, but it does not yet pass the Phase-4 gate.  Osborn
archive cache misses fell from 21,322 to 11,490 (-46.11%), materialization
clock time from 0.231 to 0.124 seconds, and nonunit-index bytes rose from
193,224 to 201,416 (+4.24%); user CPU was 18.25 versus 17.03 seconds.
`chat-new-11k` misses fell from 16,483 to 7,106 (-56.89%), materialization
time from 0.078 to 0.033 seconds, and bytes changed by the same 8,192; user CPU
was 4.70 versus 4.89 seconds.  The 100-given mbol diversity check reduced
misses from 343 to 271 but was timing-neutral at this scale.  Since the safe
rigid facts remove only about half the reconstructions, the next prototype
must address variable consistency and literal compatibility rather than tune
the Bloom hash to one problem.

That semantic extension stores a separate 32-bit mask of equality constraints
between every pair of paths carrying the same pattern variable.  A target
clause stores a 32-bit mask of path pairs whose resident subterms are
identical.  The two masks are deliberately independent of the rigid mask:
their collisions can retain extra candidates but cannot satisfy a missing
rigid fact.  Predicate/sign/path locations omit literal ordinal because
Prover9's nonunit subsumption is allowed to reuse one target literal.  The
100-given Osborn dual-index audit compared 1,535 forward and 1,235 backward
queries with the legacy tree and reported zero answer/order failures.

The repeated-variable layer passes the 300-given dense Phase-4 materialization
gate on all three measured training problems.  Osborn nonunit materializations
fell from 15,750 to 328 (-97.92%), `chat-new-11k` from 14,443 to 239
(-98.35%), and the 100-given mbol check from 191 to 8 (-95.81%).  Osborn and
chat nonunit-index bytes rose from 193,248 to 209,120 (+8.21%); mbol rose from
145,120 to 147,168 (+1.41%).  The paired end-to-end user times were
17.51/17.39, 4.77/4.87, and 2.67/2.87 seconds respectively, so this prefix
supports the materialization and memory claims but is too small and noisy for
a CPU-speedup claim.  An explicit summary clock reports only 0.014 seconds on
Osborn, 0.005 on chat, and below 0.001 on nil3 at 100 givens.

The path filter remains opt-in pending larger training checkpoints and the
holdout phase.  Its rules and fixed bit budgets come from matching semantics,
not from Osborn symbols, hint IDs, or fitted work thresholds.

The combined product variant passes its 300-given four-category training
gate.  Each enabled/disabled pair used dense passive storage, unit code-tree
retrieval, complete `mask8` backward demodulation, and packed-fast hints; all
four pairs had identical given/generated/kept boundaries.  Nonunit
materializations fell from 15,750 to 336 on Osborn (-97.87%) and from 14,443
to 249 on `chat-new-11k` (-98.28%), with nonunit-index growth from 193,248 to
209,120 bytes (+8.21%).  On nil3 they fell from 506 to 128 and on mbol from
398 to 72.  Those percentage reductions are smaller because the final exact
sets are nearly irreducible: nil3 performed 406 forward exact tests for 390
successes and no backward exact tests; mbol performed 252 for 250 and none.
Their index growth was only 0.74% and 1.43%.

Single-run user CPU for control/filter was 16.07/17.76 seconds on Osborn,
4.79/4.72 on chat, 8.79/9.18 on nil3, and 16.30/14.41 on mbol.  The directions
are mixed and index-level work is a small fraction of these short runs, so no
CPU-speedup claim is made.  Peak RSS changed by at most 132 KiB.  These
results authorize the bounded 1,000-given training checkpoint; they do not
open the holdout suite.

Phase 5 removes the single-workload constants from the `packed_fast` match
cache.  The historical `chat_test.new.out3.gz` cache occupied 2,228,224 bytes,
accepted at most eight structural keys, rejected 399,668 longer profiles, and
still avoided 2.63 billion posting candidates.  The cache is therefore useful,
but the compiled-in shape was not a general policy.

`hint_cache_kb` now supplies an explicit total cache budget (default 2,048;
zero disables it).  The allocation contains a power-of-two direct table and a
bounded ring of complete variable-length profile keys.  Each ring cell records
its owning table slot.  On wrap, overwriting a key range invalidates only live
entries that overlap that range; it does not flush the whole cache.  A profile
larger than the entire arena follows the unchanged posting path.  Candidate
vectors remain capped at eight because broad result sets are deliberately not
cached; this affects speed only, never authoritative matching.  Statistics
report the exact budget/allocation, key distribution, wraps, overlap
invalidations, and both overflow reasons.

The focused test constructs a profile longer than the former eight-key cap,
checks that it is cached completely with no key overflow, verifies the match,
and checks a 64-KiB allocation against the reported byte total.  The complete
FPA/packed/packed-fast/hybrid trace suite, equality flips, `_AnyConst`, and
side-effect-free preview tests agree.

At 300 givens, both Osborn and `chat-new-11k` reached nine-key profiles.  With
a 512-KiB ring, chat used 524,284 bytes, hit 35.93% of 37,694 queries, wrapped
five times, and invalidated 2,151 overlapping entries; Osborn hit 34.62% of
34,872 queries with three wraps and 1,323 overlap invalidations.  The earlier
same-build budget sweep measured 37.75% and 34.97% hit rates at 2 MiB, only
about two and 0.4 percentage points above 512 KiB respectively.  End-to-end
times at this prefix remain noise-dominated.  The 2-MiB default is retained
for the long training checkpoint because the historical run had a much larger
working set; users can now make the RAM/CPU tradeoff explicitly rather than
recompile a CHAT-derived table.

Long-lived packed indexes now also rebuild from measured wasted lookup work.
The previous maintenance rule considered only the number of stale references
stored in the index.  That misses a small stale posting traversed millions of
times: `chat_test.new.out3.gz`, for example, reports about 832 million stale
skips even though the stored garbage was not large enough to force timely
maintenance.  Every authoritative packed operation now charges skipped stale
IDs to a saturating counter.  Once at least 65,536 skips have accumulated, a
rebuild is allowed when the skipped work reaches
`hint_rebuild_scan_ratio` times a conservative live-index cost estimate.  The
default ratio is 8; zero disables this observed-work trigger while retaining
the original storage-volume trigger.

The scale estimate includes the stable-ID table capacity, live structural and
equivalence references, and live `_AnyConst` references.  It therefore adapts
to the actual hint population instead of a problem name, symbol vocabulary,
or fixed hint count.  Rebuilding remains safe: stale references only admit
extra exact checks, and the rebuilt index is produced from all active hints,
materializing at most one compressed hint at a time.  Read-only scheduler
previews do not charge the counter or trigger maintenance.  The
`Better_packed_maintenance` statistics line reports the configured ratio,
total and current stale scans, the peak between rebuilds, trigger reasons, and
rebuild CPU time.  The focused mutation test crosses the production floor,
observes a scan-triggered rebuild, and verifies that no stale feature
references remain.

For controlled comparisons, the matrix accepts
`P9_MATRIX_HINT_REBUILD_SCAN_RATIO` and records it in `run.conf`.  A useful
attribution pair is the default value 8 versus 0.  Do not interpret a bounded
run with zero triggers as evidence for either setting; this control matters
only after hint expiry or back-demodulation has created stale postings and
subsequent queries repeatedly encounter them.

The first 300-given attribution pair used `packed_only` on the two rewrite
training cases.  Ratio 0 and the default ratio 8 had identical generated,
kept, and non-time hint-work counters.  Ratio 8 did not fire at this boundary.
On Osborn both traversed 6,374,542 stale IDs and performed the same three
storage-triggered rebuilds; user CPU was 16.51/17.48 seconds and peak RSS was
90,416 KiB in both runs.  On `chat-new-11k`, both traversed 452,964 stale IDs
without a rebuild; user CPU was 4.98/5.06 seconds and peak RSS was 26,248 KiB.
These are single short measurements, so the time variation is noise evidence,
not a promotion result.

A ratio-2 diagnostic deliberately made the work trigger observable on the
same Osborn prefix.  One of the three rebuilds fired from scan work earlier
than the storage rule, reducing stale skips to 5,981,463 (-6.17%) while
preserving `Generated=120793` and `Kept=5737`; rebuild CPU was 0.925 seconds
and total user CPU was 16.98 seconds.  It remained inert on `chat-new-11k` and
again preserved `Generated=129756` and `Kept=5039`.  Ratio 2 is diagnostic,
not a proposed default.  The conservative default 8 remains frozen for the
1,000-given and longevity gates.

Setting `P9_MATRIX_ALLOW_HOLDOUT=1` is required even when a holdout case is
named explicitly.  Do this only for a recorded phase-promotion commit, never
while selecting features or thresholds.
