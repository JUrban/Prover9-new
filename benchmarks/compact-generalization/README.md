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
indexes remain valid.

Setting `P9_MATRIX_ALLOW_HOLDOUT=1` is required even when a holdout case is
named explicitly.  Do this only for a recorded phase-promotion commit, never
while selecting features or thresholds.
