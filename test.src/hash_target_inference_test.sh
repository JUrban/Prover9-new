#!/bin/sh
set -eu

script_dir=$(CDPATH= cd -- "$(dirname -- "$0")" && pwd)
repo_dir=$(CDPATH= cd -- "$script_dir/.." && pwd)
prover9=${PROVER9:-"$repo_dir/provers.src/prover9"}
scratch_root=${TMPDIR:-/tmp}
tmp="$scratch_root/hash-target-inference.$$"
trap 'rm -f "$tmp".*' EXIT HUP INT TERM

run_mode()
{
  mode=$1
  {
    printf '%s\n' "assign(hash_targeted_inference,$mode)."
    printf '%s\n' 'assign(hash_target_index_kb,1024).'
    cat "$script_dir/hash_inference_gate_proof.in"
  } | "$prover9" > "$tmp.$mode.out" 2> "$tmp.$mode.err"
}

run_mode shadow_unit_paramod
run_mode targeted_only_unit_paramod

for mode in shadow_unit_paramod targeted_only_unit_paramod; do
  grep -q 'THEOREM PROVED' "$tmp.$mode.out"
  grep -Eq '^Hash_target_index: targets=[1-9][0-9]*,.*feature_keys=[1-9][0-9]*,' \
    "$tmp.$mode.out"
  "$repo_dir/bin/prooftrans" parents_only < "$tmp.$mode.out" \
    > "$tmp.$mode.parents"
  "$repo_dir/bin/directproof" < "$tmp.$mode.out" \
    > "$tmp.$mode.direct"
  grep -q 'end of proof' "$tmp.$mode.parents"
  grep -q 'Directproof did' "$tmp.$mode.direct"
done

shadow_line=$(grep '^Hash_target_inference:' \
  "$tmp.shadow_unit_paramod.out")
printf '%s\n' "$shadow_line" | grep -Eq \
  'ordinary_hash_hits=[1-9][0-9]*, covered_hash_hits=[1-9][0-9]*, missed_hash_hits=0,'
targeted_line=$(grep '^Hash_target_inference:' \
  "$tmp.targeted_only_unit_paramod.out")
printf '%s\n' "$targeted_line" | grep -Eq \
  'requirements=0,.*symmetric_requirements=disabled, omitted_symmetric=[1-9][0-9]*,'

shadow_generated=$(sed -n 's/^Given=.* Generated=\([0-9][0-9]*\).*/\1/p' \
  "$tmp.shadow_unit_paramod.out" | tail -1)
targeted_generated=$(sed -n 's/^Given=.* Generated=\([0-9][0-9]*\).*/\1/p' \
  "$tmp.targeted_only_unit_paramod.out" | tail -1)
test "$targeted_generated" -lt "$shadow_generated"
grep -q 'intentionally incomplete' "$tmp.targeted_only_unit_paramod.err"

{
  printf '%s\n' \
    'assign(hash_targeted_inference,targeted_only_unit_paramod).' \
    'assign(hash_target_index_kb,1024).'
  cat "$script_dir/hash_target_nonunit_proof.in"
} | "$prover9" > "$tmp.nonunit.out" 2> "$tmp.nonunit.err"
grep -q 'THEOREM PROVED' "$tmp.nonunit.out"
"$repo_dir/bin/prooftrans" parents_only < "$tmp.nonunit.out" \
  > "$tmp.nonunit.parents"
grep -q 'end of proof' "$tmp.nonunit.parents"

echo "hash_target_inference_test: PASS"
