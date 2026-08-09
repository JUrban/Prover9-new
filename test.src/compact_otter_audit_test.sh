#!/bin/sh
set -eu

repo_dir=$(CDPATH= cd -- "$(dirname -- "$0")/.." && pwd)
test_tmp=$(mktemp -d "${TMPDIR:-/tmp}/p9-compact-otter-audit.XXXXXX")
trap 'rm -rf -- "$test_tmp"' EXIT HUP INT TERM

sed '1i\
assign(stats,all).' "$repo_dir/prover9.examples/x2.in" | \
  "$repo_dir/bin/prover9" > "$test_tmp/reference.out" 2> "$test_tmp/reference.err"

sed '1i\
set(compact_otter_audit).\
assign(stats,all).' "$repo_dir/prover9.examples/x2.in" | \
  "$repo_dir/bin/prover9" > "$test_tmp/audit.out" 2> "$test_tmp/audit.err"

sed '1i\
set(compact_otter_demodulation).\
assign(stats,all).' "$repo_dir/prover9.examples/x2.in" | \
  "$repo_dir/bin/prover9" > "$test_tmp/compact.out" 2> "$test_tmp/compact.err"

grep -q 'THEOREM PROVED' "$test_tmp/reference.out"
grep -q 'THEOREM PROVED' "$test_tmp/audit.out"
grep -q 'THEOREM PROVED' "$test_tmp/compact.out"
grep -Eq 'Compact_otter_audit: queries=[1-9][0-9]*, failures=0, current_rules=[1-9][0-9]*,' \
  "$test_tmp/audit.out"
grep -Eq 'Compact_otter_demodulation: current_rules=[1-9][0-9]*, peak_rules=[1-9][0-9]*,' \
  "$test_tmp/compact.out"
if grep -q 'compact_otter_audit: demodulation mismatch' "$test_tmp/audit.err"; then
  cat "$test_tmp/audit.err" >&2
  exit 1
fi

"$repo_dir/bin/prooftrans" parents_only < "$test_tmp/reference.out" \
  > "$test_tmp/reference.proof"
"$repo_dir/bin/prooftrans" parents_only < "$test_tmp/audit.out" \
  > "$test_tmp/audit.proof"
"$repo_dir/bin/prooftrans" parents_only < "$test_tmp/compact.out" \
  > "$test_tmp/compact.proof"
sed -n '/^% Length of proof:/,/^============================== end of proof/p' \
  "$test_tmp/reference.proof" > "$test_tmp/reference.norm"
sed -n '/^% Length of proof:/,/^============================== end of proof/p' \
  "$test_tmp/audit.proof" > "$test_tmp/audit.norm"
sed -n '/^% Length of proof:/,/^============================== end of proof/p' \
  "$test_tmp/compact.proof" > "$test_tmp/compact.norm"
cmp "$test_tmp/reference.norm" "$test_tmp/audit.norm"
cmp "$test_tmp/reference.norm" "$test_tmp/compact.norm"

for run in reference audit compact; do
  grep '^Given=' "$test_tmp/$run.out" | tail -n 1 > "$test_tmp/$run.search"
  grep '^Usable=' "$test_tmp/$run.out" | tail -n 1 >> "$test_tmp/$run.search"
done
cmp "$test_tmp/reference.search" "$test_tmp/audit.search"
cmp "$test_tmp/reference.search" "$test_tmp/compact.search"

echo 'compact_otter_audit_test: PASS'
