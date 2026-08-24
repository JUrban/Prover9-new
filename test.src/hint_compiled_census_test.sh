#!/bin/sh
set -eu

script_dir=$(CDPATH= cd -- "$(dirname -- "$0")" && pwd)
prover9=${PROVER9:-"$script_dir/../provers.src/prover9"}
tmp=${TMPDIR:-/tmp}/hint-compiled-census.$$
trap 'rm -f "$tmp.out" "$tmp.err" "$tmp.path.out" "$tmp.path.err" \
  "$tmp.same.out" "$tmp.same.err" "$tmp.cache.out" "$tmp.cache.err" \
  "$tmp.deny.out" "$tmp.deny.err" "$tmp.rigid.out" "$tmp.rigid.err" \
  "$tmp.program.out" "$tmp.program.err"' \
  EXIT HUP INT TERM

"$prover9" < "$script_dir/hint_compiled_census.in" \
  > "$tmp.out" 2> "$tmp.err" || status=$?
status=${status:-0}
if [ "$status" -ne 2 ] && [ "$status" -ne 5 ]; then
  cat "$tmp.err" >&2
  echo "hint_compiled_census_test: expected sos_empty/max_given, got $status" >&2
  exit 1
fi

line=$(grep '^Compiled_hint_census: op=match,' "$tmp.out")
bank=$(grep '^Compiled_hint_bank_census:' "$tmp.out")
table=$(grep '^Compiled_hint_term_table:' "$tmp.out")
grep -Eq '^Hint_index: mode=packed_compiled_shadow,' "$tmp.out"
printf '%s\n' "$bank" | grep -Eq 'finalized=1, retained=9, units=8, ordinary_units=8, anyconst_units=0, nonunits=1'
printf '%s\n' "$bank" | grep -Eq 'positive_units=6, negative_units=2, equations=1, disequations=1'
printf '%s\n' "$bank" | grep -Eq 'repeated_variable_occurrences=2, units_with_repeated_variables=2'
printf '%s\n' "$bank" | grep -Eq 'subterm_occurrences=[1-9][0-9]*, subterm_fingerprint_distinct=[1-9][0-9]*'
printf '%s\n' "$table" | grep -Eq 'authoritative=0, shadow=1, filter=0, finalized=1, active=8, additions=8, removals=0, reinsertions=0'
printf '%s\n' "$table" | grep -Eq 'base_nodes=[1-9][0-9]*, base_children=[1-9][0-9]*'
printf '%s\n' "$table" | grep -Eq 'hash_bytes=0, hash_peak_bytes=[1-9][0-9]*, scratch_bytes=0, total_bytes=[1-9][0-9]*'
printf '%s\n' "$table" | grep -Eq 'match_attempts=[1-9][0-9]*, match_successes=[1-9][0-9]*'
printf '%s\n' "$table" | grep -Eq 'match_repeated_tests=[1-9][0-9]*, match_repeated_rejects=[1-9][0-9]*'
printf '%s\n' "$line" | grep -Eq 'profiled_units=[1-9][0-9]*'
printf '%s\n' "$line" | grep -Eq 'exact_matches=[1-9][0-9]*'
printf '%s\n' "$line" | grep -Eq 'repeated_rejects=[1-9][0-9]*'
printf '%s\n' "$line" | grep -Eq 'repeated_only=[1-9][0-9]*'
printf '%s\n' "$line" | grep -Eq 'rigid_rejects=[0-9]+'
printf '%s\n' "$line" | grep -Eq 'combined=[0-9]+'
printf '%s\n' "$line" | grep -Eq 'skipped_subterms=[1-9][0-9]*'
grep -Eq '^Compiled_hint_census_interval: op=match,' "$tmp.out"

"$prover9" < "$script_dir/hint_compiled_path.in" \
  > "$tmp.path.out" 2> "$tmp.path.err" || path_status=$?
path_status=${path_status:-0}
if [ "$path_status" -ne 2 ] && [ "$path_status" -ne 5 ]; then
  cat "$tmp.path.err" >&2
  echo "hint_compiled_census_test: path run returned $path_status" >&2
  exit 1
fi
path_line=$(grep '^Compiled_hint_path_index:' "$tmp.path.out")
printf '%s\n' "$path_line" | grep -Eq \
  'keys=[1-9][0-9]*, references=[1-9][0-9]*, references_added=[1-9][0-9]*'
printf '%s\n' "$path_line" | grep -Eq \
  'queries=[1-9][0-9]*, query_keys=[1-9][0-9]*,'
printf '%s\n' "$path_line" | grep -Eq \
  'candidates_before=[1-9][0-9]*, candidates_after=[0-9]+, candidates_rejected=[1-9][0-9]*'

sed 's/packed_compiled_shadow/packed_compiled/' \
  "$script_dir/hint_compiled_census.in" |
  "$prover9" > "$tmp.same.out" 2> "$tmp.same.err" || same_status=$?
same_status=${same_status:-0}
if [ "$same_status" -ne 2 ] && [ "$same_status" -ne 5 ]; then
  cat "$tmp.same.err" >&2
  echo "hint_compiled_census_test: SAME run returned $same_status" >&2
  exit 1
fi
same_line=$(grep '^Compiled_hint_same_filter:' "$tmp.same.out")
printf '%s\n' "$same_line" | grep -Eq \
  'queries=[1-9][0-9]*, query_tests=[1-9][0-9]*,'
printf '%s\n' "$same_line" | grep -Eq \
  'candidates_before=[1-9][0-9]*, candidates_after=[0-9]+, candidates_rejected=[1-9][0-9]*'

"$prover9" < "$script_dir/hint_compiled_cache.in" \
  > "$tmp.cache.out" 2> "$tmp.cache.err" || cache_status=$?
cache_status=${cache_status:-0}
if [ "$cache_status" -ne 2 ] && [ "$cache_status" -ne 5 ]; then
  cat "$tmp.cache.err" >&2
  echo "hint_compiled_census_test: cache run returned $cache_status" >&2
  exit 1
fi
cache_line=$(grep '^Compiled_hint_same_cache:' "$tmp.cache.out")
printf '%s\n' "$cache_line" | grep -Eq \
  'entries=1, built=1, same_entries=1, rigid_entries=0, same_built=1, rigid_built=0, build_factor=0, lookups=[1-9][0-9]*,'
printf '%s\n' "$cache_line" | grep -Eq \
  'cache_hits=[1-9][0-9]*, same_hits=[1-9][0-9]*, rigid_hits=0, builds=1, build_scans=[1-9][0-9]*, build_matches=[1-9][0-9]*,'
printf '%s\n' "$cache_line" | grep -Eq \
  'dense_keys=1, dense_bit_bytes=[1-9][0-9]*,.*dense_denials=0,'

sed '/assign(hint_compiled_cache_build_factor,0)./a assign(hint_compiled_cache_kb,0).' \
  "$script_dir/hint_compiled_cache.in" |
  "$prover9" > "$tmp.deny.out" 2> "$tmp.deny.err" || deny_status=$?
deny_status=${deny_status:-0}
if [ "$deny_status" -ne 2 ] && [ "$deny_status" -ne 5 ]; then
  cat "$tmp.deny.err" >&2
  echo "hint_compiled_census_test: cache-denial run returned $deny_status" >&2
  exit 1
fi
deny_line=$(grep '^Compiled_hint_same_cache:' "$tmp.deny.out")
printf '%s\n' "$deny_line" | grep -Eq \
  'entries=1, built=0, same_entries=1, rigid_entries=0, same_built=0, rigid_built=0, build_factor=0,.*cache_hits=0,.*builds=0,'
printf '%s\n' "$deny_line" | grep -Eq \
  'denied_entries=1, dense_keys=0, dense_bit_bytes=0, dense_budget_bytes=0, dense_denials=1,'

"$prover9" < "$script_dir/hint_compiled_rigid_cache.in" \
  > "$tmp.rigid.out" 2> "$tmp.rigid.err" || rigid_status=$?
rigid_status=${rigid_status:-0}
if [ "$rigid_status" -ne 2 ] && [ "$rigid_status" -ne 5 ]; then
  cat "$tmp.rigid.err" >&2
  echo "hint_compiled_census_test: rigid-cache run returned $rigid_status" >&2
  exit 1
fi
rigid_line=$(grep '^Compiled_hint_rigid_filter:' "$tmp.rigid.out")
rigid_program=$(grep '^Compiled_hint_program:' "$tmp.rigid.out")
rigid_cache=$(grep '^Compiled_hint_same_cache:' "$tmp.rigid.out")
printf '%s\n' "$rigid_line" | grep -Eq \
  'queries=[1-9][0-9]*, sampled_queries=[1-9][0-9]*,.*candidates_rejected=[1-9][0-9]*, candidate_tests=[1-9][0-9]*,'
printf '%s\n' "$rigid_cache" | grep -Eq \
  'entries=2, built=2, same_entries=1, rigid_entries=1, same_built=1, rigid_built=1, build_factor=0,'
printf '%s\n' "$rigid_cache" | grep -Eq \
  'cache_hits=2, same_hits=1, rigid_hits=1, builds=2,'
printf '%s\n' "$rigid_program" | grep -Eq \
  'queries=1, conditions=2, maximum_conditions=2,.*word_operations=[1-9][0-9]*,'

"$prover9" < "$script_dir/hint_compiled_program.in" \
  > "$tmp.program.out" 2> "$tmp.program.err" || program_status=$?
program_status=${program_status:-0}
if [ "$program_status" -ne 2 ] && [ "$program_status" -ne 5 ]; then
  cat "$tmp.program.err" >&2
  echo "hint_compiled_census_test: program run returned $program_status" >&2
  exit 1
fi
program_line=$(grep '^Compiled_hint_program:' "$tmp.program.out")
program_cache=$(grep '^Compiled_hint_same_cache:' "$tmp.program.out")
printf '%s\n' "$program_line" | grep -Eq \
  'queries=2, conditions=3, maximum_conditions=2, maximum_allowed=8, word_operations=[1-9][0-9]*, candidate_membership_tests=0, scratch_words=[1-9][0-9]*, scratch_bytes=[1-9][0-9]*\.'
printf '%s\n' "$program_cache" | grep -Eq \
  'entries=3, built=2, same_entries=3, rigid_entries=0, same_built=2, rigid_built=0,.*cache_hits=3, same_hits=3, rigid_hits=0, builds=2,'

echo "hint_compiled_census_test: PASS"
