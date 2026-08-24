#!/bin/sh
set -eu

script_dir=$(CDPATH= cd -- "$(dirname -- "$0")" && pwd)
prover9=${PROVER9:-"$script_dir/../provers.src/prover9"}
tmp=${TMPDIR:-/tmp}/hint-compiled-census.$$
trap 'rm -f "$tmp.out" "$tmp.err" "$tmp.path.out" "$tmp.path.err" \
  "$tmp.same.out" "$tmp.same.err"' \
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
printf '%s\n' "$table" | grep -Eq 'authoritative=0, finalized=1, active=8, additions=8, removals=0, reinsertions=0'
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

echo "hint_compiled_census_test: PASS"
