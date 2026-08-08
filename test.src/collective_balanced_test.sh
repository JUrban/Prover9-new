#!/bin/sh
set -eu

repo_dir=$(CDPATH= cd -- "$(dirname -- "$0")/.." && pwd)
test_tmp=$(mktemp -d "${TMPDIR:-/tmp}/p9-balanced-test.XXXXXX")
trap 'rm -rf -- "$test_tmp"' EXIT HUP INT TERM

"$repo_dir/bin/prover9" < "$repo_dir/test.src/collective_balanced.in" \
  > "$test_tmp/balanced.out" 2> "$test_tmp/balanced.err" || true

if grep -q 'Fatal error' "$test_tmp/balanced.err"; then
  cat "$test_tmp/balanced.err" >&2
  exit 1
fi
grep -q 'Collective_scheduler: policy=balanced_hint' "$test_tmp/balanced.out"
grep -Eq 'Collective_backlog: paramod=[0-9]+, pos_hyper=[0-9]+, neg_hyper=[0-9]+' \
  "$test_tmp/balanced.out"
grep -Eq 'Collective_rule_descriptors: created_paramod=[1-9][0-9]*, created_pos_hyper=[1-9][0-9]*, created_neg_hyper=[1-9][0-9]*, completed_paramod=[1-9][0-9]*, completed_pos_hyper=[1-9][0-9]*, completed_neg_hyper=[1-9][0-9]*\.' \
  "$test_tmp/balanced.out"
grep -Eq 'Collective_balanced: lane_paramod=[1-9][0-9]*, lane_pos_hyper=[1-9][0-9]*, lane_neg_hyper=[1-9][0-9]*, lane_turns=[1-9][0-9]*, oldest_turns=[1-9][0-9]*, drain_entries=[1-9][0-9]*, drain_exits=[1-9][0-9]*, givens_withheld=[1-9][0-9]*, paramod_from_turns=[1-9][0-9]*, paramod_into_turns=[1-9][0-9]*\.' \
  "$test_tmp/balanced.out"

peak=$(sed -n 's/.*Collective_frontier:.* peak=\([0-9][0-9]*\),.*/\1/p' \
  "$test_tmp/balanced.out" | tail -1)
test -n "$peak"
test "$peak" -le 8

# Both independent paramodulation directions must be observable in the trace.
sed '1i set(collective_trace).' "$repo_dir/test.src/collective_balanced.in" \
  > "$test_tmp/trace.in"
"$repo_dir/bin/prover9" < "$test_tmp/trace.in" \
  > "$test_tmp/trace.out" 2> "$test_tmp/trace.err" || true
grep -q 'COLLECTIVE_TRACE kind=paramod_from' "$test_tmp/trace.out"
grep -q 'COLLECTIVE_TRACE kind=paramod_into' "$test_tmp/trace.out"
grep -q 'COLLECTIVE_TRACE kind=pos_hyper' "$test_tmp/trace.out"
grep -q 'COLLECTIVE_TRACE kind=neg_hyper' "$test_tmp/trace.out"

# The checkpoint policy byte and scheduler state are required for fail-closed
# resume.  Compare the terminal aggregate state with an uninterrupted run.
sed '1i assign(checkpoint_given,5).\nset(checkpoint_exit).\nset(checkpoint_verify).' \
  "$repo_dir/test.src/collective_balanced.in" > "$test_tmp/checkpoint.in"
checkpoint_case="$test_tmp/checkpoint-case"
mkdir "$checkpoint_case"
(
  cd "$checkpoint_case"
  "$repo_dir/bin/prover9" < "$test_tmp/checkpoint.in" \
    > before.out 2> before.err || true
)
checkpoint_dir=$(find "$checkpoint_case" -maxdepth 1 -type d \
  -name 'prover9_*_ckpt_5' -print)
test -n "$checkpoint_dir"
"$repo_dir/bin/prover9" -r "$checkpoint_dir" < /dev/null \
  > "$test_tmp/resumed.out" 2> "$test_tmp/resumed.err" || true
grep -Eq '^%   Verification: [0-9]+ passed, 0 failed\.$' \
  "$test_tmp/resumed.out"
grep -E '^(Given=|Collective_(frontier|rule_descriptors|work|balanced|chunks):)' \
  "$test_tmp/balanced.out" | tail -6 > "$test_tmp/control.stats"
grep -E '^(Given=|Collective_(frontier|rule_descriptors|work|balanced|chunks):)' \
  "$test_tmp/resumed.out" | tail -6 > "$test_tmp/resumed.stats"
diff -u "$test_tmp/control.stats" "$test_tmp/resumed.stats"

echo 'collective_balanced_test: PASS'
