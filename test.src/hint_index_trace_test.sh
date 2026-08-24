#!/bin/sh
set -eu

repo_dir=$(CDPATH= cd -- "$(dirname -- "$0")/.." && pwd)
prover9=${PROVER9:-"$repo_dir/bin/prover9"}
test_tmp=$(mktemp -d "${TMPDIR:-/tmp}/p9-hint-index-test.XXXXXX")
trap 'rm -rf -- "$test_tmp"' EXIT HUP INT TERM

sed '/assign(hint_index,compact)./a set(hint_trace).' \
  "$repo_dir/test.src/discount_loop.in" |
  "$prover9" > "$test_tmp/compact.out" 2> "$test_tmp/compact.err"

sed '/assign(hint_index,compact)./a assign(hint_index,fpa).\
assign(hints_fpa_depth,10).\
set(hint_trace).' "$repo_dir/test.src/discount_loop.in" |
  "$prover9" > "$test_tmp/fpa.out" 2> "$test_tmp/fpa.err"

sed '/assign(hint_index,compact)./a assign(hint_index,packed).\
set(hint_trace).' "$repo_dir/test.src/discount_loop.in" |
  "$prover9" > "$test_tmp/packed.out" 2> "$test_tmp/packed.err"

sed '/assign(hint_index,compact)./a assign(hint_index,packed_fast).\
set(hint_trace).' "$repo_dir/test.src/discount_loop.in" |
  "$prover9" > "$test_tmp/packed-fast.out" \
                           2> "$test_tmp/packed-fast.err"

sed '/assign(hint_index,compact)./a assign(hint_index,packed_compiled_shadow).\
set(hint_trace).' "$repo_dir/test.src/discount_loop.in" |
  "$prover9" > "$test_tmp/packed-compiled-shadow.out" \
               2> "$test_tmp/packed-compiled-shadow.err"

sed '/assign(hint_index,compact)./a assign(hint_index,packed_compiled).\
set(hint_trace).' "$repo_dir/test.src/discount_loop.in" |
  "$prover9" > "$test_tmp/packed-compiled.out" \
               2> "$test_tmp/packed-compiled.err"

sed '/assign(hint_index,compact)./a assign(hint_index,packed_compiled_lazy).\
set(hint_trace).' "$repo_dir/test.src/discount_loop.in" |
  "$prover9" > "$test_tmp/packed-compiled-lazy.out" \
               2> "$test_tmp/packed-compiled-lazy.err"

sed '/assign(hint_index,compact)./a assign(hint_index,packed_compiled_paths).\
set(hint_trace).' "$repo_dir/test.src/discount_loop.in" |
  "$prover9" > "$test_tmp/packed-compiled-paths.out" \
               2> "$test_tmp/packed-compiled-paths.err"

sed '/assign(hint_index,compact)./a assign(hint_index,hybrid).\
set(hint_trace).' "$repo_dir/test.src/discount_loop.in" |
  "$prover9" > "$test_tmp/hybrid.out" 2> "$test_tmp/hybrid.err"

sed '/assign(hint_index,compact)./a assign(hint_index,packed_legacy).\
set(hint_trace).' "$repo_dir/test.src/discount_loop.in" |
  "$prover9" > "$test_tmp/packed-legacy.out" \
                           2> "$test_tmp/packed-legacy.err"

grep '^HINT_TRACE ' "$test_tmp/compact.out" > "$test_tmp/compact.trace"
grep '^HINT_TRACE ' "$test_tmp/fpa.out" > "$test_tmp/fpa.trace"
grep '^HINT_TRACE ' "$test_tmp/packed.out" > "$test_tmp/packed.trace"
grep '^HINT_TRACE ' "$test_tmp/packed-fast.out" \
  > "$test_tmp/packed-fast.trace"
grep '^HINT_TRACE ' "$test_tmp/packed-compiled-shadow.out" \
  > "$test_tmp/packed-compiled-shadow.trace"
grep '^HINT_TRACE ' "$test_tmp/packed-compiled.out" \
  > "$test_tmp/packed-compiled.trace"
grep '^HINT_TRACE ' "$test_tmp/packed-compiled-lazy.out" \
  > "$test_tmp/packed-compiled-lazy.trace"
grep '^HINT_TRACE ' "$test_tmp/packed-compiled-paths.out" \
  > "$test_tmp/packed-compiled-paths.trace"
grep '^HINT_TRACE ' "$test_tmp/hybrid.out" > "$test_tmp/hybrid.trace"
grep '^HINT_TRACE ' "$test_tmp/packed-legacy.out" \
  > "$test_tmp/packed-legacy.trace"
test -s "$test_tmp/compact.trace"
diff -u "$test_tmp/fpa.trace" "$test_tmp/compact.trace"
diff -u "$test_tmp/fpa.trace" "$test_tmp/packed.trace"
diff -u "$test_tmp/fpa.trace" "$test_tmp/packed-fast.trace"
diff -u "$test_tmp/fpa.trace" "$test_tmp/packed-compiled-shadow.trace"
diff -u "$test_tmp/fpa.trace" "$test_tmp/packed-compiled.trace"
diff -u "$test_tmp/fpa.trace" "$test_tmp/packed-compiled-lazy.trace"
diff -u "$test_tmp/fpa.trace" "$test_tmp/packed-compiled-paths.trace"
diff -u "$test_tmp/fpa.trace" "$test_tmp/hybrid.trace"
diff -u "$test_tmp/fpa.trace" "$test_tmp/packed-legacy.trace"
grep -q 'THEOREM PROVED' "$test_tmp/compact.out"
grep -q 'THEOREM PROVED' "$test_tmp/fpa.out"
grep -q 'THEOREM PROVED' "$test_tmp/packed.out"
grep -q 'THEOREM PROVED' "$test_tmp/packed-fast.out"
grep -q 'THEOREM PROVED' "$test_tmp/packed-compiled-shadow.out"
grep -q 'THEOREM PROVED' "$test_tmp/packed-compiled.out"
grep -q 'THEOREM PROVED' "$test_tmp/packed-compiled-lazy.out"
grep -q 'THEOREM PROVED' "$test_tmp/packed-compiled-paths.out"
grep -q 'THEOREM PROVED' "$test_tmp/hybrid.out"
grep -q 'THEOREM PROVED' "$test_tmp/packed-legacy.out"
for hint_op in equivalence match flipped_match back_demod; do
  grep -q "^Packed_hint_operation: op=$hint_op," "$test_tmp/packed.out"
done
# Ordinary matching scans only the rarest exact structural posting.  A
# conservative per-hint fingerprint filters the remaining query features,
# and exact subsumption remains the final decision.
match_line=$(grep '^Packed_hint_operation: op=match,' "$test_tmp/packed.out")
printf '%s\n' "$match_line" | grep -Eq \
  'seconds=[0-9]+\.[0-9]+, candidate_seconds=[0-9]+\.[0-9]+, compiled_filter_seconds=[0-9]+\.[0-9]+, confirmation_seconds=[0-9]+\.[0-9]+,'
match_queries=$(printf '%s\n' "$match_line" | \
  sed 's/.*queries=\([0-9][0-9]*\),.*/\1/')
match_postings=$(printf '%s\n' "$match_line" | \
  sed 's/.*posting_lists=\([0-9][0-9]*\),.*/\1/')
test "$match_postings" -le "$match_queries"
grep -Eq '^Better_packed_postings: .*fingerprint_bytes=[1-9][0-9]*,' \
  "$test_tmp/packed.out"
grep -q '^Better_packed_postings:' "$test_tmp/hybrid.out"
grep -q '^Better_packed_postings:' "$test_tmp/packed.out"
grep -q '^Packed_fast_cache:' "$test_tmp/packed-fast.out"
grep -Eq '^Compiled_hint_term_table: authoritative=0, shadow=1, filter=0, finalized=1, active=[1-9][0-9]*,' \
  "$test_tmp/packed-compiled-shadow.out"
grep -Eq '^Compiled_hint_term_table: authoritative=0, shadow=0, filter=1, finalized=1, active=[1-9][0-9]*,' \
  "$test_tmp/packed-compiled.out"
grep -Eq '^Compiled_hint_term_table: authoritative=0, shadow=0, filter=1, finalized=0, active=0,.*base_nodes=0,.*total_bytes=0, lazy=1, lazy_root_requests=0, lazy_root_builds=0,' \
  "$test_tmp/packed-compiled-lazy.out"
grep -q '^Compiled_hint_same_filter:' "$test_tmp/packed-compiled.out"
grep -q '^Compiled_hint_rigid_filter:' "$test_tmp/packed-compiled.out"
grep -q '^Compiled_hint_same_cache:' "$test_tmp/packed-compiled.out"
grep -q '^Compiled_hint_path_index:' \
  "$test_tmp/packed-compiled-paths.out"

"$prover9" -f "$repo_dir/test.src/hint_anyconst.in" \
  > "$test_tmp/anyconst-compact.out" 2> "$test_tmp/anyconst-compact.err"
for hint_mode in packed packed_fast packed_compiled_shadow packed_compiled packed_compiled_lazy packed_compiled_paths hybrid packed_legacy; do
  sed "/assign(hint_index,compact)./a assign(hint_index,$hint_mode)." \
    "$repo_dir/test.src/hint_anyconst.in" |
    "$prover9" > "$test_tmp/anyconst-$hint_mode.out" \
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
diff -u "$test_tmp/anyconst-compact.trace" \
  "$test_tmp/anyconst-packed_compiled_shadow.trace"
diff -u "$test_tmp/anyconst-compact.trace" \
  "$test_tmp/anyconst-packed_compiled.trace"
diff -u "$test_tmp/anyconst-compact.trace" \
  "$test_tmp/anyconst-packed_compiled_lazy.trace"
diff -u "$test_tmp/anyconst-compact.trace" \
  "$test_tmp/anyconst-packed_compiled_paths.trace"
diff -u "$test_tmp/anyconst-compact.trace" "$test_tmp/anyconst-hybrid.trace"
diff -u "$test_tmp/anyconst-compact.trace" \
  "$test_tmp/anyconst-packed_legacy.trace"
grep -Eq '^Better_packed_postings: .*anyconst_references=[1-9][0-9]*,' \
  "$test_tmp/anyconst-packed_fast.out"

sed 's/packed_compiled/packed_fast/' \
  "$repo_dir/test.src/hint_compiled_rigid_cache.in" |
  "$prover9" > "$test_tmp/rigid-control.out" \
               2> "$test_tmp/rigid-control.err" || rigid_control_status=$?
rigid_control_status=${rigid_control_status:-0}
"$prover9" < "$repo_dir/test.src/hint_compiled_rigid_cache.in" \
  > "$test_tmp/rigid-compiled.out" \
  2> "$test_tmp/rigid-compiled.err" || rigid_compiled_status=$?
rigid_compiled_status=${rigid_compiled_status:-0}
sed 's/packed_compiled/packed_compiled_lazy/' \
  "$repo_dir/test.src/hint_compiled_rigid_cache.in" |
  "$prover9" > "$test_tmp/rigid-lazy.out" \
               2> "$test_tmp/rigid-lazy.err" || rigid_lazy_status=$?
rigid_lazy_status=${rigid_lazy_status:-0}
if [ "$rigid_control_status" -ne 2 ] || \
   [ "$rigid_compiled_status" -ne 2 ] || [ "$rigid_lazy_status" -ne 2 ]; then
  echo "hint_index_trace_test: rigid statuses control=$rigid_control_status compiled=$rigid_compiled_status lazy=$rigid_lazy_status" >&2
  exit 1
fi
grep '^HINT_TRACE ' "$test_tmp/rigid-control.out" \
  > "$test_tmp/rigid-control.trace"
grep '^HINT_TRACE ' "$test_tmp/rigid-compiled.out" \
  > "$test_tmp/rigid-compiled.trace"
grep '^HINT_TRACE ' "$test_tmp/rigid-lazy.out" \
  > "$test_tmp/rigid-lazy.trace"
test -s "$test_tmp/rigid-control.trace"
diff -u "$test_tmp/rigid-control.trace" \
  "$test_tmp/rigid-compiled.trace"
diff -u "$test_tmp/rigid-control.trace" \
  "$test_tmp/rigid-lazy.trace"
grep -Eq '^Compiled_hint_same_cache: .*rigid_built=1,.*rigid_hits=[1-9][0-9]*,' \
  "$test_tmp/rigid-compiled.out"
grep -Eq '^Compiled_hint_program: queries=1, conditions=2, maximum_conditions=2,.*word_operations=[1-9][0-9]*,' \
  "$test_tmp/rigid-compiled.out"
grep -Eq '^Compiled_hint_term_table: .*finalized=1, active=6,.*base_nodes=[1-9][0-9]*,.*hash_bytes=0,.*lazy=1, lazy_root_requests=6, lazy_root_builds=6, lazy_root_deferred=0, lazy_stream_builds=6, lazy_materializations=0,' \
  "$test_tmp/rigid-lazy.out"
grep -Eq '^Compiled_hint_program: queries=1, conditions=2, maximum_conditions=2,.*word_operations=[1-9][0-9]*,' \
  "$test_tmp/rigid-lazy.out"

sed 's/packed_compiled/packed_fast/' \
  "$repo_dir/test.src/hint_compiled_program.in" |
  "$prover9" > "$test_tmp/program-control.out" \
               2> "$test_tmp/program-control.err" || program_control_status=$?
program_control_status=${program_control_status:-0}
"$prover9" < "$repo_dir/test.src/hint_compiled_program.in" \
  > "$test_tmp/program-compiled.out" \
  2> "$test_tmp/program-compiled.err" || program_compiled_status=$?
program_compiled_status=${program_compiled_status:-0}
if [ "$program_control_status" -ne 2 ] || \
   [ "$program_compiled_status" -ne 2 ]; then
  echo "hint_index_trace_test: program statuses control=$program_control_status compiled=$program_compiled_status" >&2
  exit 1
fi
grep '^HINT_TRACE ' "$test_tmp/program-control.out" \
  > "$test_tmp/program-control.trace"
grep '^HINT_TRACE ' "$test_tmp/program-compiled.out" \
  > "$test_tmp/program-compiled.trace"
test -s "$test_tmp/program-control.trace"
diff -u "$test_tmp/program-control.trace" \
  "$test_tmp/program-compiled.trace"
grep -Eq '^Compiled_hint_program: queries=2, conditions=3, maximum_conditions=2,.*word_operations=[1-9][0-9]*,' \
  "$test_tmp/program-compiled.out"

sed 's/packed_compiled/packed_fast/' \
  "$repo_dir/test.src/hint_compiled_navigation.in" |
  "$prover9" > "$test_tmp/navigation-control.out" \
               2> "$test_tmp/navigation-control.err" || navigation_control_status=$?
navigation_control_status=${navigation_control_status:-0}
"$prover9" < "$repo_dir/test.src/hint_compiled_navigation.in" \
  > "$test_tmp/navigation-compiled.out" \
  2> "$test_tmp/navigation-compiled.err" || navigation_compiled_status=$?
navigation_compiled_status=${navigation_compiled_status:-0}
sed 's/packed_compiled/packed_compiled_lazy/' \
  "$repo_dir/test.src/hint_compiled_navigation.in" |
  "$prover9" > "$test_tmp/navigation-lazy.out" \
               2> "$test_tmp/navigation-lazy.err" || navigation_lazy_status=$?
navigation_lazy_status=${navigation_lazy_status:-0}
if [ "$navigation_control_status" -ne 2 ] || \
   [ "$navigation_compiled_status" -ne 2 ] || \
   [ "$navigation_lazy_status" -ne 2 ]; then
  echo "hint_index_trace_test: navigation statuses control=$navigation_control_status compiled=$navigation_compiled_status lazy=$navigation_lazy_status" >&2
  exit 1
fi
for navigation_mode in control compiled lazy; do
  grep '^HINT_TRACE ' "$test_tmp/navigation-$navigation_mode.out" \
    > "$test_tmp/navigation-$navigation_mode.trace"
done
test -s "$test_tmp/navigation-control.trace"
diff -u "$test_tmp/navigation-control.trace" \
  "$test_tmp/navigation-compiled.trace"
diff -u "$test_tmp/navigation-control.trace" \
  "$test_tmp/navigation-lazy.trace"
grep -Eq '^Compiled_hint_same_filter: queries=1, query_tests=1, candidates_before=2, candidates_after=1, candidates_rejected=1, candidate_tests=2, navigation_rejects=1,' \
  "$test_tmp/navigation-compiled.out"
grep -Eq '^Compiled_hint_same_filter: queries=1, query_tests=1, candidates_before=2, candidates_after=1, candidates_rejected=1, candidate_tests=2, navigation_rejects=1,' \
  "$test_tmp/navigation-lazy.out"

echo 'hint_index_trace_test: PASS'
