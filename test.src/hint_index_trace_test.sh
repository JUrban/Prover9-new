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

sed '/assign(hint_index,compact)./a assign(hint_index,packed_fast).\
set(hint_trace).' "$repo_dir/test.src/discount_loop.in" |
  "$repo_dir/bin/prover9" > "$test_tmp/packed-fast.out" \
                           2> "$test_tmp/packed-fast.err"

sed '/assign(hint_index,compact)./a assign(hint_index,hybrid).\
set(hint_trace).' "$repo_dir/test.src/discount_loop.in" |
  "$repo_dir/bin/prover9" > "$test_tmp/hybrid.out" 2> "$test_tmp/hybrid.err"

sed '/assign(hint_index,compact)./a assign(hint_index,packed_legacy).\
set(hint_trace).' "$repo_dir/test.src/discount_loop.in" |
  "$repo_dir/bin/prover9" > "$test_tmp/packed-legacy.out" \
                           2> "$test_tmp/packed-legacy.err"

grep '^HINT_TRACE ' "$test_tmp/compact.out" > "$test_tmp/compact.trace"
grep '^HINT_TRACE ' "$test_tmp/fpa.out" > "$test_tmp/fpa.trace"
grep '^HINT_TRACE ' "$test_tmp/packed.out" > "$test_tmp/packed.trace"
grep '^HINT_TRACE ' "$test_tmp/packed-fast.out" \
  > "$test_tmp/packed-fast.trace"
grep '^HINT_TRACE ' "$test_tmp/hybrid.out" > "$test_tmp/hybrid.trace"
grep '^HINT_TRACE ' "$test_tmp/packed-legacy.out" \
  > "$test_tmp/packed-legacy.trace"
test -s "$test_tmp/compact.trace"
diff -u "$test_tmp/fpa.trace" "$test_tmp/compact.trace"
diff -u "$test_tmp/fpa.trace" "$test_tmp/packed.trace"
diff -u "$test_tmp/fpa.trace" "$test_tmp/packed-fast.trace"
diff -u "$test_tmp/fpa.trace" "$test_tmp/hybrid.trace"
diff -u "$test_tmp/fpa.trace" "$test_tmp/packed-legacy.trace"
grep -q 'THEOREM PROVED' "$test_tmp/compact.out"
grep -q 'THEOREM PROVED' "$test_tmp/fpa.out"
grep -q 'THEOREM PROVED' "$test_tmp/packed.out"
grep -q 'THEOREM PROVED' "$test_tmp/packed-fast.out"
grep -q 'THEOREM PROVED' "$test_tmp/hybrid.out"
grep -q 'THEOREM PROVED' "$test_tmp/packed-legacy.out"
for hint_op in equivalence match flipped_match back_demod; do
  grep -q "^Packed_hint_operation: op=$hint_op," "$test_tmp/packed.out"
done
# Ordinary matching scans only the rarest exact structural posting.  A
# conservative per-hint fingerprint filters the remaining query features,
# and exact subsumption remains the final decision.
match_line=$(grep '^Packed_hint_operation: op=match,' "$test_tmp/packed.out")
match_queries=$(printf '%s\n' "$match_line" | \
  sed 's/.*queries=\([0-9][0-9]*\),.*/\1/')
match_postings=$(printf '%s\n' "$match_line" | \
  sed 's/.*posting_lists=\([0-9][0-9]*\),.*/\1/')
test "$match_postings" -le "$match_queries"
grep -Eq '^Better_packed_postings: .*fingerprint_bytes=[1-9][0-9]*,' \
  "$test_tmp/packed.out"
grep -q '^Better_packed_postings:' "$test_tmp/hybrid.out"
grep -q '^Better_packed_postings:' "$test_tmp/packed.out"

"$repo_dir/bin/prover9" -f "$repo_dir/test.src/hint_anyconst.in" \
  > "$test_tmp/anyconst-compact.out" 2> "$test_tmp/anyconst-compact.err"
for hint_mode in packed packed_fast hybrid packed_legacy; do
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
diff -u "$test_tmp/anyconst-compact.trace" \
  "$test_tmp/anyconst-packed_fast.trace"
diff -u "$test_tmp/anyconst-compact.trace" "$test_tmp/anyconst-hybrid.trace"
diff -u "$test_tmp/anyconst-compact.trace" \
  "$test_tmp/anyconst-packed_legacy.trace"

echo 'hint_index_trace_test: PASS'
