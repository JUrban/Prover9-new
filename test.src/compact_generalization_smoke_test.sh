#!/bin/sh
set -eu

repo_dir=$(CDPATH= cd -- "$(dirname -- "$0")/.." && pwd)
test_tmp=$(mktemp -d "${TMPDIR:-/tmp}/p9-general-matrix-smoke.XXXXXX")
trap 'rm -rf -- "$test_tmp"' EXIT HUP INT TERM

P9_MATRIX_CASES='x2-control nil3-1k' \
P9_MATRIX_VARIANTS='legacy compact_full compact_dense_file' \
P9_MATRIX_MAX_GIVEN=100 \
P9_MATRIX_MAX_SECONDS=30 \
P9_MATRIX_MAX_MEGS=512 \
P9_MATRIX_WALL_SECONDS=45 \
  "$repo_dir/test.src/compact_generalization_matrix.sh" "$test_tmp" \
  > "$test_tmp/matrix.log"

awk -F '\t' '
  NR == 1 { next }
  $1 == "x2-control" && ($3 != 0 || $4 != "yes" || $5 != 12) { bad=1 }
  $1 == "nil3-1k" && ($3 != 5 || $4 != "no" || $5 != 101) { bad=1 }
  END { exit bad }
' "$test_tmp/summary.tsv"

test "$(awk -F '\t' 'NR > 1 { n++ } END { print n+0 }' \
  "$test_tmp/profiles.tsv")" -eq 22
grep -q '"component":"unit"' "$test_tmp/profiles.json"
grep -q '"component":"back_demod"' "$test_tmp/profiles.json"
grep -q '"component":"nonunit"' "$test_tmp/profiles.json"
grep -q '^x2-control[[:space:]]control[[:space:]]' "$test_tmp/cases.tsv"
grep -q '^nil3-1k[[:space:]]training[[:space:]]' "$test_tmp/cases.tsv"
grep -q 'Compact_index_inactive: component=back_demod, reason=back_demod_disabled.' \
  "$test_tmp/nil3-1k.compact_full.out"
grep -q 'Compact_index_inactive: component=back_demod, reason=back_demod_disabled.' \
  "$test_tmp/nil3-1k.compact_dense_file.out"

# Exercise input generation for the external old-P9 control without requiring
# an old binary in CI.  The current binary is a compatible stand-in here; the
# assertion is that new-only tuning options never enter the old control input.
old_tmp=$test_tmp/old-control
P9_MATRIX_CASES='x2-control' \
P9_MATRIX_VARIANTS='old_p9' \
P9_MATRIX_OLD_PROVER="$repo_dir/bin/prover9" \
P9_MATRIX_MAX_GIVEN=100 \
P9_MATRIX_MAX_SECONDS=30 \
P9_MATRIX_MAX_MEGS=512 \
P9_MATRIX_WALL_SECONDS=45 \
  "$repo_dir/test.src/compact_generalization_matrix.sh" "$old_tmp" \
  > "$old_tmp.log"
test "$(awk -F '\t' 'NR == 2 { print $3 }' "$old_tmp/summary.tsv")" -eq 0
if grep -Eq '^assign\((hint_cache_kb|hint_rebuild_scan_ratio),' \
     "$old_tmp/x2-control.old_p9.in"; then
  echo 'new-only hint controls leaked into old_p9 input' >&2
  exit 1
fi

echo 'compact_generalization_smoke_test: PASS'
