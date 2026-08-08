#!/bin/sh
set -eu

repo_dir=$(CDPATH= cd -- "$(dirname -- "$0")/.." && pwd)
test_tmp=$(mktemp -d "${TMPDIR:-/tmp}/p9-hint-index-test.XXXXXX")
trap 'rm -rf -- "$test_tmp"' EXIT HUP INT TERM

sed '/assign(hint_index,compact)./a set(hint_trace).' \
  "$repo_dir/test.src/discount_loop.in" |
  "$repo_dir/bin/prover9" > "$test_tmp/compact.out" 2> "$test_tmp/compact.err"

sed '/assign(hint_index,compact)./a assign(hint_index,fpa).\
assign(hints_fpa_depth,10).\
set(hint_trace).' "$repo_dir/test.src/discount_loop.in" |
  "$repo_dir/bin/prover9" > "$test_tmp/fpa.out" 2> "$test_tmp/fpa.err"

sed '/assign(hint_index,compact)./a assign(hint_index,packed).\
set(hint_trace).' "$repo_dir/test.src/discount_loop.in" |
  "$repo_dir/bin/prover9" > "$test_tmp/packed.out" 2> "$test_tmp/packed.err"

sed '/assign(hint_index,compact)./a assign(hint_index,hybrid).\
set(hint_trace).' "$repo_dir/test.src/discount_loop.in" |
  "$repo_dir/bin/prover9" > "$test_tmp/hybrid.out" 2> "$test_tmp/hybrid.err"

grep '^HINT_TRACE ' "$test_tmp/compact.out" > "$test_tmp/compact.trace"
grep '^HINT_TRACE ' "$test_tmp/fpa.out" > "$test_tmp/fpa.trace"
grep '^HINT_TRACE ' "$test_tmp/packed.out" > "$test_tmp/packed.trace"
grep '^HINT_TRACE ' "$test_tmp/hybrid.out" > "$test_tmp/hybrid.trace"
test -s "$test_tmp/compact.trace"
diff -u "$test_tmp/fpa.trace" "$test_tmp/compact.trace"
diff -u "$test_tmp/fpa.trace" "$test_tmp/packed.trace"
diff -u "$test_tmp/fpa.trace" "$test_tmp/hybrid.trace"
grep -q 'THEOREM PROVED' "$test_tmp/compact.out"
grep -q 'THEOREM PROVED' "$test_tmp/fpa.out"
grep -q 'THEOREM PROVED' "$test_tmp/packed.out"
grep -q 'THEOREM PROVED' "$test_tmp/hybrid.out"
for hint_op in equivalence match flipped_match back_demod; do
  grep -q "^Packed_hint_operation: op=$hint_op," "$test_tmp/packed.out"
done
grep -q '^Better_packed_postings:' "$test_tmp/hybrid.out"

"$repo_dir/bin/prover9" -f "$repo_dir/test.src/hint_anyconst.in" \
  > "$test_tmp/anyconst-compact.out" 2> "$test_tmp/anyconst-compact.err"
for hint_mode in packed hybrid; do
  sed "/assign(hint_index,compact)./a assign(hint_index,$hint_mode)." \
    "$repo_dir/test.src/hint_anyconst.in" |
    "$repo_dir/bin/prover9" > "$test_tmp/anyconst-$hint_mode.out" \
                             2> "$test_tmp/anyconst-$hint_mode.err"
  grep '^HINT_TRACE ' "$test_tmp/anyconst-$hint_mode.out" \
    > "$test_tmp/anyconst-$hint_mode.trace"
  grep -q 'THEOREM PROVED' "$test_tmp/anyconst-$hint_mode.out"
done
grep '^HINT_TRACE ' "$test_tmp/anyconst-compact.out" \
  > "$test_tmp/anyconst-compact.trace"
test -s "$test_tmp/anyconst-compact.trace"
diff -u "$test_tmp/anyconst-compact.trace" "$test_tmp/anyconst-packed.trace"
diff -u "$test_tmp/anyconst-compact.trace" "$test_tmp/anyconst-hybrid.trace"

echo 'hint_index_trace_test: PASS'
