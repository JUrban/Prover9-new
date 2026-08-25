#!/bin/sh
set -eu

test_dir=$(CDPATH= cd -- "$(dirname -- "$0")" && pwd)
repo_dir=$(CDPATH= cd -- "$test_dir/.." && pwd)
prover=${PROVER9:-$repo_dir/provers.src/prover9}
scratch_root=${TMPDIR:-/tmp}
work=$(mktemp -d "$scratch_root/proof-parent-guidance.XXXXXX")
trap 'rm -rf "$work"' EXIT HUP INT TERM

run_paramod_mode()
{
  mode=$1
  expected_status=$2
  sed "s/proof_parent_guidance,authoritative/proof_parent_guidance,$mode/" \
    "$test_dir/proof_parent_guidance.in" |
    "$prover" > "$work/$mode.out" 2> "$work/$mode.err" || status=$?
  status=${status:-0}
  if [ "$status" -ne "$expected_status" ]; then
    cat "$work/$mode.err" >&2
    echo "proof-parent $mode returned $status, expected $expected_status" >&2
    exit 1
  fi
  unset status
}

run_paramod_mode off 2
run_paramod_mode shadow 2
run_paramod_mode authoritative 2

grep -q 'Given=1. Generated=1. Kept=1. proofs=0.' "$work/off.out"
grep -q 'Given=1. Generated=1. Kept=1. proofs=0.' "$work/shadow.out"
grep -q 'para_pair_tests=2 (accepted=1, rejected=1)' "$work/shadow.out"
grep -q 'sparse_para_queries=1, raw_partners=1, unique_partners=1' \
  "$work/authoritative.out"

"$prover" < "$test_dir/proof_parent_hyper_guidance.in" \
  > "$work/hyper.out" 2> "$work/hyper.err"
grep -q 'THEOREM PROVED' "$work/hyper.out"
grep -q 'hyper_pair_tests=3 (accepted=2, rejected=1)' "$work/hyper.out"

python3 "$test_dir/proof_parent_guide_extract_test.py"
echo 'proof_parent_guidance_test: PASS'
