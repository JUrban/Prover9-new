#!/bin/sh
set -eu

script_dir=$(CDPATH= cd -- "$(dirname -- "$0")" && pwd)
prover9=${PROVER9:-"$script_dir/../provers.src/prover9"}
tmp=${TMPDIR:-/tmp}/hash-inference-gate.$$
trap 'rm -f "$tmp".*' EXIT HUP INT TERM

run_mode()
{
  mode=$1
  sed "s/assign(hash_inference_gate,off)./assign(hash_inference_gate,$mode)./" \
    "$script_dir/hash_inference_gate.in" |
    "$prover9" > "$tmp.$mode.out" 2> "$tmp.$mode.err" || status=$?
  status=${status:-0}
  if [ "$status" -ne 2 ] && [ "$status" -ne 5 ]; then
    cat "$tmp.$mode.err" >&2
    echo "hash_inference_gate_test: $mode returned $status" >&2
    exit 1
  fi
  unset status
}

run_mode off
run_mode shadow
run_mode safe
run_mode hit_only

for mode in shadow safe; do
  grep '^given #' "$tmp.off.out" > "$tmp.off.given"
  grep '^given #' "$tmp.$mode.out" > "$tmp.$mode.given"
  diff -u "$tmp.off.given" "$tmp.$mode.given"
  grep '^Given=' "$tmp.off.out" > "$tmp.off.summary"
  grep '^Given=' "$tmp.$mode.out" > "$tmp.$mode.summary"
  diff -u "$tmp.off.summary" "$tmp.$mode.summary"
done

shadow=$(grep '^Hash_inference_gate: mode=shadow,' "$tmp.shadow.out")
safe=$(grep '^Hash_inference_gate: mode=safe,' "$tmp.safe.out")
hit=$(grep '^Hash_inference_gate: mode=hit_only,' "$tmp.hit_only.out")
printf '%s\n' "$shadow" | grep -Eq \
  'virtual_hits=[1-9][0-9]*, virtual_misses=[1-9][0-9]*,.*false_misses=0, false_hits=0, changed_hint_ids=0,'
printf '%s\n' "$safe" | grep -Eq \
  'materialized_hits=[1-9][0-9]*,.*certified_skips=[1-9][0-9]*,.*false_misses=0, false_hits=0, changed_hint_ids=0,'
printf '%s\n' "$hit" | grep -Eq \
  'materialized_hits=[1-9][0-9]*,.*hit_only_skips=[1-9][0-9]*,'
grep -Eq '^Hash_inference_stages: candidates=[1-9][0-9]*, materialized=[1-9][0-9]*,' \
  "$tmp.safe.out"
grep -Eq 'sampled_construction_allocations=[1-9][0-9]*,' \
  "$tmp.safe.out"
grep -q 'intentionally incomplete' "$tmp.hit_only.err"

echo "hash_inference_gate_test: PASS"
