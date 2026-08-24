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

sed '/assign(hint_index,compact)./a assign(hint_index,packed_compiled_blocks).\
set(hint_trace).' "$repo_dir/test.src/discount_loop.in" |
  "$prover9" > "$test_tmp/packed-compiled-blocks.out" \
               2> "$test_tmp/packed-compiled-blocks.err"

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
grep '^HINT_TRACE ' "$test_tmp/packed-compiled-blocks.out" \
  > "$test_tmp/packed-compiled-blocks.trace"
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
diff -u "$test_tmp/fpa.trace" "$test_tmp/packed-compiled-blocks.trace"
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
grep -q 'THEOREM PROVED' "$test_tmp/packed-compiled-blocks.out"
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
grep -Eq '^Compiled_hint_blocks: enabled=1, queries=[0-9]+, activations=[0-9]+, prefix_candidates=[0-9]+, early_rejects=[0-9]+,' \
  "$test_tmp/packed-compiled-blocks.out"
grep -Eq '^Compiled_hint_term_table: authoritative=0, shadow=0, filter=1, finalized=0, active=0,.*base_nodes=0,.*total_bytes=0, lazy=1, lazy_root_requests=0, lazy_root_builds=0,' \
  "$test_tmp/packed-compiled-lazy.out"
grep -q '^Compiled_hint_same_filter:' "$test_tmp/packed-compiled.out"
grep -q '^Compiled_hint_rigid_filter:' "$test_tmp/packed-compiled.out"
grep -q '^Compiled_hint_same_cache:' "$test_tmp/packed-compiled.out"
grep -q '^Compiled_hint_path_index:' \
  "$test_tmp/packed-compiled-paths.out"

"$prover9" -f "$repo_dir/test.src/hint_anyconst.in" \
  > "$test_tmp/anyconst-compact.out" 2> "$test_tmp/anyconst-compact.err"
for hint_mode in packed packed_fast packed_compiled_shadow packed_compiled packed_compiled_blocks packed_compiled_lazy packed_compiled_paths hybrid packed_legacy; do
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
  "$test_tmp/anyconst-packed_compiled_blocks.trace"
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
sed 's/packed_compiled/packed_compiled_blocks/' \
  "$repo_dir/test.src/hint_compiled_navigation.in" |
  "$prover9" > "$test_tmp/navigation-blocks.out" \
               2> "$test_tmp/navigation-blocks.err" || navigation_blocks_status=$?
navigation_blocks_status=${navigation_blocks_status:-0}
if [ "$navigation_control_status" -ne 2 ] || \
   [ "$navigation_compiled_status" -ne 2 ] || \
   [ "$navigation_lazy_status" -ne 2 ] || \
   [ "$navigation_blocks_status" -ne 2 ]; then
  echo "hint_index_trace_test: navigation statuses control=$navigation_control_status compiled=$navigation_compiled_status lazy=$navigation_lazy_status blocks=$navigation_blocks_status" >&2
  exit 1
fi
for navigation_mode in control compiled lazy blocks; do
  grep '^HINT_TRACE ' "$test_tmp/navigation-$navigation_mode.out" \
    > "$test_tmp/navigation-$navigation_mode.trace"
done
test -s "$test_tmp/navigation-control.trace"
diff -u "$test_tmp/navigation-control.trace" \
  "$test_tmp/navigation-compiled.trace"
diff -u "$test_tmp/navigation-control.trace" \
  "$test_tmp/navigation-lazy.trace"
diff -u "$test_tmp/navigation-control.trace" \
  "$test_tmp/navigation-blocks.trace"
grep -Eq '^Compiled_hint_same_filter: queries=1, query_tests=1, candidates_before=2, candidates_after=1, candidates_rejected=1, candidate_tests=2, navigation_rejects=1,' \
  "$test_tmp/navigation-compiled.out"
grep -Eq '^Compiled_hint_same_filter: queries=1, query_tests=1, candidates_before=2, candidates_after=1, candidates_rejected=1, candidate_tests=2, navigation_rejects=1,' \
  "$test_tmp/navigation-lazy.out"
grep -Eq '^Compiled_hint_same_filter: queries=1, query_tests=1, candidates_before=2, candidates_after=1, candidates_rejected=1, candidate_tests=2, navigation_rejects=1,' \
  "$test_tmp/navigation-blocks.out"
grep -Eq '^Compiled_hint_blocks: enabled=1, queries=[1-9][0-9]*, activations=[1-9][0-9]*, prefix_candidates=0, early_rejects=1,' \
  "$test_tmp/navigation-blocks.out"

sed 's/packed_compiled_blocks/packed_fast/' \
  "$repo_dir/test.src/hint_compiled_blocks_cache.in" |
  "$prover9" > "$test_tmp/blocks-cache-control.out" \
               2> "$test_tmp/blocks-cache-control.err" || blocks_cache_control_status=$?
blocks_cache_control_status=${blocks_cache_control_status:-0}
"$prover9" < "$repo_dir/test.src/hint_compiled_blocks_cache.in" \
  > "$test_tmp/blocks-cache-blocks.out" \
  2> "$test_tmp/blocks-cache-blocks.err" || blocks_cache_status=$?
blocks_cache_status=${blocks_cache_status:-0}
if [ "$blocks_cache_control_status" -ne 2 ] || \
   [ "$blocks_cache_status" -ne 2 ]; then
  echo "hint_index_trace_test: blocks cache statuses control=$blocks_cache_control_status blocks=$blocks_cache_status" >&2
  exit 1
fi
for blocks_cache_mode in control blocks; do
  grep '^HINT_TRACE ' "$test_tmp/blocks-cache-$blocks_cache_mode.out" \
    > "$test_tmp/blocks-cache-$blocks_cache_mode.trace"
done
test -s "$test_tmp/blocks-cache-control.trace"
diff -u "$test_tmp/blocks-cache-control.trace" \
  "$test_tmp/blocks-cache-blocks.trace"
grep -Eq '^Packed_fast_cache: .*hits=[1-9][0-9]*,' \
  "$test_tmp/blocks-cache-blocks.out"

# Force two co-occurring SAME conditions to mature together on a bank large
# enough for packed_fast's dense-word collector.  The third identical query
# must use the two learned masks before enumerating stable IDs.
blocks_batch_input="$test_tmp/blocks-batch.in"
{
  printf '%s\n' \
    'clear(auto_denials).' \
    'clear(auto_inference).' \
    'clear(predicate_elim).' \
    'clear(print_initial_clauses).' \
    'clear(print_given).' \
    'clear(print_kept).' \
    'clear(back_demod).' \
    'clear(back_demod_hints).' \
    'assign(search_loop,discount).' \
    'assign(passive_store,compressed).' \
    'assign(hint_index,packed_compiled_blocks).' \
    'assign(hint_compiled_min_candidates,0).' \
    'assign(hint_compiled_cache_build_factor,0).' \
    'assign(hint_conjunction_kb,1).' \
    'assign(ancestor_store,memory).' \
    'assign(stats,all).' \
    'assign(max_given,3).' \
    'set(process_initial_sos).' \
    'set(hint_trace).' \
    'formulas(hints).'
  batch_i=1
  while [ "$batch_i" -le 600 ]; do
    case $((batch_i % 4)) in
      0) printf '  p(k%u,f(a,a),g(c,c)).\n' "$batch_i" ;;
      1) printf '  p(k%u,f(a,b),g(c,c)).\n' "$batch_i" ;;
      2) printf '  p(k%u,f(a,a),g(c,d)).\n' "$batch_i" ;;
      3) printf '  p(k%u,f(a,b),g(c,d)).\n' "$batch_i" ;;
    esac
    batch_i=$((batch_i + 1))
  done
  printf '%s\n' \
    'end_of_list.' \
    'formulas(sos).' \
    '  p(z,f(x,x),g(y,y)).' \
    '  p(z,f(x,x),g(y,y)).' \
    '  p(z,f(x,x),g(y,y)).' \
    'end_of_list.'
} > "$blocks_batch_input"
sed 's/packed_compiled_blocks/packed_fast/' "$blocks_batch_input" |
  "$prover9" > "$test_tmp/blocks-batch-control.out" \
               2> "$test_tmp/blocks-batch-control.err" || blocks_batch_control_status=$?
blocks_batch_control_status=${blocks_batch_control_status:-0}
"$prover9" < "$blocks_batch_input" \
  > "$test_tmp/blocks-batch-blocks.out" \
  2> "$test_tmp/blocks-batch-blocks.err" || blocks_batch_status=$?
blocks_batch_status=${blocks_batch_status:-0}
if [ "$blocks_batch_control_status" -ne 2 ] || \
   [ "$blocks_batch_status" -ne 2 ]; then
  echo "hint_index_trace_test: blocks batch statuses control=$blocks_batch_control_status blocks=$blocks_batch_status" >&2
  exit 1
fi
for blocks_batch_mode in control blocks; do
  grep '^HINT_TRACE ' "$test_tmp/blocks-batch-$blocks_batch_mode.out" \
    > "$test_tmp/blocks-batch-$blocks_batch_mode.trace"
done
diff -u "$test_tmp/blocks-batch-control.trace" \
  "$test_tmp/blocks-batch-blocks.trace"
grep -Eq '^Compiled_hint_same_cache: .*builds=2, .*build_batches=1, batch_conditions=2, maximum_batch=2,' \
  "$test_tmp/blocks-batch-blocks.out"
grep -Eq '^Compiled_hint_blocks: .*block_words=[1-9][0-9]*, block_rejects=[1-9][0-9]*,' \
  "$test_tmp/blocks-batch-blocks.out"

# Exercise a mixed two-instruction program in the before-emission path.
# Positions 7 and 28 of k/65 collide in the established shallow 64-bit path
# mask.  One independent cohort violates the query's repeated x at positions
# 0/1 and another has h instead of g at position 7.  The first broad query
# must therefore retain both a SAME and a RIGID instruction; later identical
# queries reuse the normalized two-instruction program.  Mask construction is
# deliberately kept out of this fixture so it tests policy reuse itself; the
# preceding and following fixtures cover learned dense/sparse masks.
emit_rigid_atom()
{
  rigid_pred=$1
  rigid_bad=$2
  rigid_query=$3
  rigid_tag=$4
  rigid_group=${5:-}
  printf '  %s(f(k(' "$rigid_pred"
  rigid_pos=0
  while [ "$rigid_pos" -lt 65 ]; do
    if [ "$rigid_pos" -ne 0 ]; then
      printf ','
    fi
    if [ "$rigid_query" -eq 1 ] && \
       { [ "$rigid_pos" -eq 0 ] || [ "$rigid_pos" -eq 1 ]; }; then
      printf 'x'
    elif [ $((rigid_bad & 1)) -ne 0 ] && [ "$rigid_pos" -eq 1 ]; then
      printf 'h'
    elif [ $((rigid_bad & 2)) -ne 0 ] && [ "$rigid_pos" -eq 7 ]; then
      printf 'h'
    else
      printf 'g'
    fi
    rigid_pos=$((rigid_pos + 1))
  done
  if [ -n "$rigid_group" ] && [ "$rigid_query" -eq 1 ]; then
    printf ')),%s,z).\n' "$rigid_group"
  elif [ -n "$rigid_group" ]; then
    printf ')),%s,c%s).\n' "$rigid_group" "$rigid_tag"
  elif [ "$rigid_query" -eq 1 ]; then
    printf ')),y,y).\n'
  else
    printf ')),c%s,c%s).\n' "$rigid_tag" "$rigid_tag"
  fi
}

blocks_rigid_input="$test_tmp/blocks-rigid.in"
{
  printf '%s\n' \
    'clear(auto_denials).' \
    'clear(auto_inference).' \
    'clear(predicate_elim).' \
    'clear(print_initial_clauses).' \
    'clear(print_given).' \
    'clear(print_kept).' \
    'clear(back_demod).' \
    'clear(back_demod_hints).' \
    'assign(search_loop,discount).' \
    'assign(passive_store,compressed).' \
    'assign(hint_index,packed_compiled_blocks).' \
    'assign(hint_compiled_min_candidates,128).' \
    'assign(hint_compiled_cache_build_factor,100).' \
    'assign(hint_conjunction_kb,1).' \
    'assign(ancestor_store,memory).' \
    'assign(stats,all).' \
    'assign(max_given,3).' \
    'set(process_initial_sos).' \
    'set(hint_trace).' \
    'formulas(hints).'
  rigid_i=1
  while [ "$rigid_i" -le 600 ]; do
    emit_rigid_atom p $((rigid_i % 4)) 0 "$rigid_i"
    rigid_i=$((rigid_i + 1))
  done
  printf '%s\n' 'end_of_list.' 'formulas(sos).'
  emit_rigid_atom p 0 1 0
  emit_rigid_atom p 0 1 0
  emit_rigid_atom p 0 1 0
  printf '%s\n' 'end_of_list.'
} > "$blocks_rigid_input"
sed 's/packed_compiled_blocks/packed_fast/' "$blocks_rigid_input" |
  "$prover9" > "$test_tmp/blocks-rigid-control.out" \
               2> "$test_tmp/blocks-rigid-control.err" || blocks_rigid_control_status=$?
blocks_rigid_control_status=${blocks_rigid_control_status:-0}
"$prover9" < "$blocks_rigid_input" \
  > "$test_tmp/blocks-rigid-blocks.out" \
  2> "$test_tmp/blocks-rigid-blocks.err" || blocks_rigid_status=$?
blocks_rigid_status=${blocks_rigid_status:-0}
if [ "$blocks_rigid_control_status" -ne 2 ] || \
   [ "$blocks_rigid_status" -ne 2 ]; then
  echo "hint_index_trace_test: blocks rigid statuses control=$blocks_rigid_control_status blocks=$blocks_rigid_status" >&2
  exit 1
fi
for blocks_rigid_mode in control blocks; do
  grep '^HINT_TRACE ' "$test_tmp/blocks-rigid-$blocks_rigid_mode.out" \
    > "$test_tmp/blocks-rigid-$blocks_rigid_mode.trace"
done
diff -u "$test_tmp/blocks-rigid-control.trace" \
  "$test_tmp/blocks-rigid-blocks.trace"
if ! grep -Eq '^Compiled_hint_plan: .*same_conditions=[1-9][0-9]*, rigid_conditions=[1-9][0-9]*,.*mixed_queries=[1-9][0-9]*,' \
     "$test_tmp/blocks-rigid-blocks.out" || \
   ! grep -Eq '^Compiled_hint_same_cache: .*same_entries=2, rigid_entries=1, same_built=0, rigid_built=0,.*builds=0,' \
     "$test_tmp/blocks-rigid-blocks.out" || \
   ! grep -Eq '^Compiled_hint_query_program_cache: .*hits=[1-9][0-9]*,.*stores=[1-9][0-9]*,.*sample_tests_avoided=[1-9][0-9]*, instructions=[1-9][0-9]*, same_instructions=[1-9][0-9]*, rigid_instructions=[1-9][0-9]*, mixed_hits=[1-9][0-9]*, maximum_width=2,' \
     "$test_tmp/blocks-rigid-blocks.out"; then
  grep '^Compiled_hint_' "$test_tmp/blocks-rigid-blocks.out" >&2
  exit 1
fi

# Repeat with three 200-ID cohorts.  The fixed group symbol is covered by the
# ordinary shallow index, while the selected deep g position is shared by all
# three query plans.  A 200-ID seed is below FAST_DENSE_MIN_POSTING, so the
# third cohort must exercise learned-mask rejection in the sparse vector path.
blocks_sparse_input="$test_tmp/blocks-sparse.in"
{
  printf '%s\n' \
    'clear(auto_denials).' \
    'clear(auto_inference).' \
    'clear(predicate_elim).' \
    'clear(print_initial_clauses).' \
    'clear(print_given).' \
    'clear(print_kept).' \
    'clear(back_demod).' \
    'clear(back_demod_hints).' \
    'assign(search_loop,discount).' \
    'assign(passive_store,compressed).' \
    'assign(hint_index,packed_compiled_blocks).' \
    'assign(hint_compiled_min_candidates,128).' \
    'assign(hint_compiled_cache_build_factor,0).' \
    'assign(hint_conjunction_kb,1).' \
    'assign(ancestor_store,memory).' \
    'assign(stats,all).' \
    'assign(max_given,3).' \
    'set(process_initial_sos).' \
    'set(hint_trace).' \
    'formulas(hints).'
  rigid_tag=1
  for rigid_group in s1 s2 s3; do
    rigid_i=1
    while [ "$rigid_i" -le 200 ]; do
      emit_rigid_atom p $((rigid_i % 2)) 0 "$rigid_tag" "$rigid_group"
      rigid_i=$((rigid_i + 1))
      rigid_tag=$((rigid_tag + 1))
    done
  done
  printf '%s\n' 'end_of_list.' 'formulas(sos).'
  emit_rigid_atom p 0 1 0 s1
  emit_rigid_atom p 0 1 0 s2
  emit_rigid_atom p 0 1 0 s3
  printf '%s\n' 'end_of_list.'
} > "$blocks_sparse_input"
sed 's/packed_compiled_blocks/packed_fast/' "$blocks_sparse_input" |
  "$prover9" > "$test_tmp/blocks-sparse-control.out" \
               2> "$test_tmp/blocks-sparse-control.err" || blocks_sparse_control_status=$?
blocks_sparse_control_status=${blocks_sparse_control_status:-0}
"$prover9" < "$blocks_sparse_input" \
  > "$test_tmp/blocks-sparse-blocks.out" \
  2> "$test_tmp/blocks-sparse-blocks.err" || blocks_sparse_status=$?
blocks_sparse_status=${blocks_sparse_status:-0}
if [ "$blocks_sparse_control_status" -ne 2 ] || \
   [ "$blocks_sparse_status" -ne 2 ]; then
  echo "hint_index_trace_test: blocks sparse statuses control=$blocks_sparse_control_status blocks=$blocks_sparse_status" >&2
  exit 1
fi
for blocks_sparse_mode in control blocks; do
  grep '^HINT_TRACE ' "$test_tmp/blocks-sparse-$blocks_sparse_mode.out" \
    > "$test_tmp/blocks-sparse-$blocks_sparse_mode.trace"
done
diff -u "$test_tmp/blocks-sparse-control.trace" \
  "$test_tmp/blocks-sparse-blocks.trace"
if ! grep -Eq '^Compiled_hint_blocks: .*sparse_mask_tests=[1-9][0-9]*, sparse_mask_rejects=[1-9][0-9]*,' \
     "$test_tmp/blocks-sparse-blocks.out"; then
  grep '^Compiled_hint_' "$test_tmp/blocks-sparse-blocks.out" >&2
  exit 1
fi

# Use the compact six-hint RIGID fixture with threshold-zero diagnostics so a
# 1 MiB conjunction budget can retain every profile.  The learned stable-ID
# mask cannot be ANDed directly with profile positions, but it must reject
# mapped IDs before literal-count checks and packed candidate insertion.
sed -e 's/hint_index,packed_compiled/hint_index,packed_fast/' \
    -e '/assign(hint_compiled_cache_build_factor,0)./a assign(hint_conjunction_kb,1024).' \
    "$repo_dir/test.src/hint_compiled_rigid_cache.in" |
  "$prover9" > "$test_tmp/blocks-conjunction-control.out" \
               2> "$test_tmp/blocks-conjunction-control.err" || blocks_conjunction_control_status=$?
blocks_conjunction_control_status=${blocks_conjunction_control_status:-0}
sed -e 's/hint_index,packed_compiled/hint_index,packed_compiled_blocks/' \
    -e '/assign(hint_compiled_cache_build_factor,0)./a assign(hint_conjunction_kb,1024).' \
    "$repo_dir/test.src/hint_compiled_rigid_cache.in" |
  "$prover9" > "$test_tmp/blocks-conjunction-blocks.out" \
               2> "$test_tmp/blocks-conjunction-blocks.err" || blocks_conjunction_status=$?
blocks_conjunction_status=${blocks_conjunction_status:-0}
if [ "$blocks_conjunction_control_status" -ne 2 ] || \
   [ "$blocks_conjunction_status" -ne 2 ]; then
  echo "hint_index_trace_test: blocks conjunction statuses control=$blocks_conjunction_control_status blocks=$blocks_conjunction_status" >&2
  exit 1
fi
for blocks_conjunction_mode in control blocks; do
  grep '^HINT_TRACE ' \
    "$test_tmp/blocks-conjunction-$blocks_conjunction_mode.out" \
    > "$test_tmp/blocks-conjunction-$blocks_conjunction_mode.trace"
done
diff -u "$test_tmp/blocks-conjunction-control.trace" \
  "$test_tmp/blocks-conjunction-blocks.trace"
if ! grep -Eq '^Packed_fast_conjunction: enabled=yes,' \
     "$test_tmp/blocks-conjunction-blocks.out" || \
   ! grep -Eq '^Compiled_hint_blocks: .*conjunction_mask_tests=[1-9][0-9]*, conjunction_mask_rejects=[1-9][0-9]*,' \
     "$test_tmp/blocks-conjunction-blocks.out"; then
  grep -E '^(Packed_fast_conjunction|Compiled_hint_)' \
    "$test_tmp/blocks-conjunction-blocks.out" >&2
  exit 1
fi

echo 'hint_index_trace_test: PASS'
