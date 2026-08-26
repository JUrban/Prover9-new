#!/bin/sh
set -eu

test_dir=$(CDPATH= cd -- "$(dirname -- "$0")" && pwd)
repo_dir=$(CDPATH= cd -- "$test_dir/.." && pwd)
prover=${PROVER9:-$repo_dir/provers.src/prover9}
scratch_root=${TMPDIR:-/tmp}
work=$(mktemp -d "$scratch_root/proof-recipe-replay.XXXXXX")
trap 'rm -rf "$work"' EXIT HUP INT TERM

"$prover" < "$test_dir/proof_recipe_replay.in" \
  > "$work/control.out" 2> "$work/control.err"
grep -q 'THEOREM PROVED' "$work/control.out"
grep -q 'target_nodes=6, verified_nodes=6' "$work/control.out"
grep -q 'ordinary_search_indexes=not_built, hint_index=not_built' \
  "$work/control.out"
grep -q 'Length of proof: 6' "$work/control.out"

status=0
sed 's/proof_recipe_max_nodes,-1/proof_recipe_max_nodes,5/' \
  "$test_dir/proof_recipe_replay.in" |
  "$prover" > "$work/prefix.out" 2> "$work/prefix.err" || status=$?
test "$status" -eq 2
grep -q 'prefix completed: 5 of 6 nodes verified' "$work/prefix.out"
grep -q 'target_nodes=5, verified_nodes=5' "$work/prefix.out"
if grep -q 'THEOREM PROVED' "$work/prefix.out"; then
  echo 'bounded recipe prefix incorrectly reported a proof' >&2
  exit 1
fi

status=0
sed 's/AQYAAQICAgICAgIA/AQYAAQICAgICAgQA/' \
  "$test_dir/proof_recipe_replay.in" |
  "$prover" > "$work/corrupt.out" 2> "$work/corrupt.err" || status=$?
test "$status" -eq 1
grep -q 'Proof recipe node 5 failed at primary' "$work/corrupt.err"
grep -q 'checked proof recipe diverged' "$work/corrupt.err"

echo 'proof_recipe_replay_integration_test: PASS'
