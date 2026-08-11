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
  30528567ef7aacc51b438ff638672a7b26afa63ab7b670017f8da021750b2dcf
```

The reconstructed file has 18,401 lines and 901,712 bytes.  The extraction
removes only the two Prover9 section-marker lines surrounding the echoed input.

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
indexes remain valid.  The experimental position-compatible unit retrieval is
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

Setting `P9_MATRIX_ALLOW_HOLDOUT=1` is required even when a holdout case is
named explicitly.  Do this only for a recorded phase-promotion commit, never
while selecting features or thresholds.
