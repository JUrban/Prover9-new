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

Setting `P9_MATRIX_ALLOW_HOLDOUT=1` is required even when a holdout case is
named explicitly.  Do this only for a recorded phase-promotion commit, never
while selecting features or thresholds.
