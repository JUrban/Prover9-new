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
printf '%s\n' "$line" | grep -Eq 'profiled_units=[1-9][0-9]*'
printf '%s\n' "$line" | grep -Eq 'exact_matches=[1-9][0-9]*'
printf '%s\n' "$line" | grep -Eq 'repeated_rejects=[1-9][0-9]*'
printf '%s\n' "$line" | grep -Eq 'repeated_only=[1-9][0-9]*'
printf '%s\n' "$line" | grep -Eq 'rigid_rejects=[0-9]+'
printf '%s\n' "$line" | grep -Eq 'combined=[0-9]+'
printf '%s\n' "$line" | grep -Eq 'skipped_subterms=[1-9][0-9]*'
grep -Eq '^Compiled_hint_census_interval: op=match,' "$tmp.out"

echo "hint_compiled_census_test: PASS"
