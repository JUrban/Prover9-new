#!/bin/sh
set -eu

script_dir=$(CDPATH= cd -- "$(dirname -- "$0")" && pwd)
prover9=${PROVER9:-"$script_dir/../provers.src/prover9"}
tmp=${TMPDIR:-/tmp}/hint-compiled-census.$$
trap 'rm -f "$tmp.out" "$tmp.err"' EXIT HUP INT TERM

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
printf '%s\n' "$bank" | grep -Eq 'finalized=1, retained=9, units=8, ordinary_units=8, anyconst_units=0, nonunits=1'
printf '%s\n' "$bank" | grep -Eq 'positive_units=6, negative_units=2, equations=1, disequations=1'
printf '%s\n' "$bank" | grep -Eq 'repeated_variable_occurrences=2, units_with_repeated_variables=2'
printf '%s\n' "$bank" | grep -Eq 'subterm_occurrences=[1-9][0-9]*, subterm_fingerprint_distinct=[1-9][0-9]*'
printf '%s\n' "$line" | grep -Eq 'profiled_units=[1-9][0-9]*'
printf '%s\n' "$line" | grep -Eq 'exact_matches=[1-9][0-9]*'
printf '%s\n' "$line" | grep -Eq 'repeated_rejects=[1-9][0-9]*'
printf '%s\n' "$line" | grep -Eq 'repeated_only=[1-9][0-9]*'
printf '%s\n' "$line" | grep -Eq 'rigid_rejects=[0-9]+'
printf '%s\n' "$line" | grep -Eq 'combined=[0-9]+'
printf '%s\n' "$line" | grep -Eq 'skipped_subterms=[1-9][0-9]*'
grep -Eq '^Compiled_hint_census_interval: op=match,' "$tmp.out"

echo "hint_compiled_census_test: PASS"
