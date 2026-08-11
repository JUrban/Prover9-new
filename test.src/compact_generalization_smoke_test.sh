#!/bin/sh
set -eu

repo_dir=$(CDPATH= cd -- "$(dirname -- "$0")/.." && pwd)
test_tmp=$(mktemp -d "${TMPDIR:-/tmp}/p9-general-matrix-smoke.XXXXXX")
trap 'rm -rf -- "$test_tmp"' EXIT HUP INT TERM

P9_MATRIX_CASES=x2-control \
P9_MATRIX_VARIANTS='legacy compact_full compact_dense_file' \
P9_MATRIX_MAX_GIVEN=100 \
P9_MATRIX_MAX_SECONDS=30 \
P9_MATRIX_MAX_MEGS=512 \
P9_MATRIX_WALL_SECONDS=45 \
  "$repo_dir/test.src/compact_generalization_matrix.sh" "$test_tmp" \
  > "$test_tmp/matrix.log"

awk -F '\t' '
  NR == 1 { next }
  $3 != 0 || $4 != "yes" || $5 != 12 { bad=1 }
  END { exit bad }
' "$test_tmp/summary.tsv"

test "$(awk -F '\t' 'NR > 1 { n++ } END { print n+0 }' \
  "$test_tmp/profiles.tsv")" -eq 12
grep -q '"component":"unit"' "$test_tmp/profiles.json"
grep -q '"component":"back_demod"' "$test_tmp/profiles.json"
grep -q '"component":"nonunit"' "$test_tmp/profiles.json"
grep -q '^x2-control[[:space:]]control[[:space:]]' "$test_tmp/cases.tsv"

echo 'compact_generalization_smoke_test: PASS'
