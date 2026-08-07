#!/bin/sh
set -eu

repo_dir=$(CDPATH= cd -- "$(dirname -- "$0")/.." && pwd)
test_tmp=$(mktemp -d "${TMPDIR:-/tmp}/p9-discount-test.XXXXXX")
trap 'rm -rf -- "$test_tmp"' EXIT HUP INT TERM

"$repo_dir/bin/prover9" < "$repo_dir/test.src/discount_loop.in" \
  > "$test_tmp/prover.out" 2> "$test_tmp/prover.err"

grep -q 'THEOREM PROVED' "$test_tmp/prover.out"
grep -Eq 'Search_loop: mode=discount, frontier=clauses, active_indexed=[0-9]+, passive_indexed=0, delayed_demodulators=[1-9][0-9]*\.' \
  "$test_tmp/prover.out"
grep -Eq 'Hint_index: mode=compact, fpa_depth=2, epoch=[1-9][0-9]*\.' "$test_tmp/prover.out"
grep -Eq 'Passive_refresh: epoch=[1-9][0-9]*, checks=[1-9][0-9]*, requeued=[1-9][0-9]*, subsumed=[0-9]+\.' \
  "$test_tmp/prover.out"
grep -Eq 'Passive_store: compressed=[1-9][0-9]*, body_bytes=[1-9][0-9]*, justification_bytes=[1-9][0-9]*, total_bytes=[1-9][0-9]*, estimated_full=[1-9][0-9]*\.' \
  "$test_tmp/prover.out"
grep -Eq 'total=2, redundant=[0-9]+, active=[0-9]+, matched=[1-9][0-9]*' \
  "$test_tmp/prover.out"
if grep -q 'Fatal error' "$test_tmp/prover.err"; then
  cat "$test_tmp/prover.err" >&2
  exit 1
fi

"$repo_dir/bin/prooftrans" parents_only < "$test_tmp/prover.out" \
  > "$test_tmp/parents.out"
"$repo_dir/bin/directproof" < "$test_tmp/prover.out" \
  > "$test_tmp/direct.out"
grep -q 'end of proof' "$test_tmp/parents.out"
grep -q 'Directproof did' "$test_tmp/direct.out"

echo 'discount_loop_test: PASS'
