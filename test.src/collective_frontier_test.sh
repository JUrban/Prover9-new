#!/bin/sh
set -eu

repo_dir=$(CDPATH= cd -- "$(dirname -- "$0")/.." && pwd)
test_tmp=$(mktemp -d "${TMPDIR:-/tmp}/p9-collective-test.XXXXXX")
trap 'rm -rf -- "$test_tmp"' EXIT HUP INT TERM

"$repo_dir/bin/prover9" < "$repo_dir/test.src/collective_frontier.in" \
  > "$test_tmp/prover.out" 2> "$test_tmp/prover.err"

grep -q 'THEOREM PROVED' "$test_tmp/prover.out"
grep -Eq 'Search_loop: mode=discount, frontier=collective, active_indexed=[0-9]+, passive_indexed=0, delayed_demodulators=[1-9][0-9]*\.' \
  "$test_tmp/prover.out"
grep -Eq 'Collective_frontier: batches_created=[1-9][0-9]*, completed=[1-9][0-9]*, pending=[1-9][0-9]*, peak=[1-9][0-9]*, ratio=1, skipped=[0-9]+, parent_materializations=[1-9][0-9]*, activations=[1-9][0-9]*\.' \
  "$test_tmp/prover.out"
grep -Eq 'Collective_work: paramod_pairs=[1-9][0-9]*, hyper_batches=[0-9]+\.' \
  "$test_tmp/prover.out"
grep -Eq 'Collective_memory: descriptor_bytes=[1-9][0-9]*, history_bytes=[1-9][0-9]*\.' \
  "$test_tmp/prover.out"
grep -Eq 'Hint_index: mode=packed, fpa_depth=0, epoch=[1-9][0-9]*\.' \
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

"$repo_dir/bin/prover9" < "$repo_dir/test.src/collective_hyper.in" \
  > "$test_tmp/hyper.out" 2> "$test_tmp/hyper.err"
grep -q 'THEOREM PROVED' "$test_tmp/hyper.out"
grep -Eq 'Generated_by_rule: binary=0, hyper=[1-9][0-9]*, ur=0, paramod=0, other=[0-9]+\.' \
  "$test_tmp/hyper.out"
grep -Eq 'Collective_work: paramod_pairs=0, hyper_batches=[1-9][0-9]*\.' \
  "$test_tmp/hyper.out"
"$repo_dir/bin/prooftrans" parents_only < "$test_tmp/hyper.out" \
  > "$test_tmp/hyper-parents.out"
grep -q 'end of proof' "$test_tmp/hyper-parents.out"

echo 'collective_frontier_test: PASS'
