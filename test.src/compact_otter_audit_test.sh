#!/bin/sh
set -eu

repo_dir=$(CDPATH= cd -- "$(dirname -- "$0")/.." && pwd)
test_tmp=$(mktemp -d "${TMPDIR:-/tmp}/p9-compact-otter-audit.XXXXXX")
trap 'rm -rf -- "$test_tmp"' EXIT HUP INT TERM

"$repo_dir/bin/prover9" -f "$repo_dir/prover9.examples/x2.in" \
  < /dev/null > "$test_tmp/reference.out" 2> "$test_tmp/reference.err"

sed '1i\
set(compact_otter_audit).\
assign(stats,all).' "$repo_dir/prover9.examples/x2.in" | \
  "$repo_dir/bin/prover9" > "$test_tmp/audit.out" 2> "$test_tmp/audit.err"

grep -q 'THEOREM PROVED' "$test_tmp/reference.out"
grep -q 'THEOREM PROVED' "$test_tmp/audit.out"
grep -Eq 'Compact_otter_audit: queries=[1-9][0-9]*, failures=0, current_rules=[1-9][0-9]*,' \
  "$test_tmp/audit.out"
if grep -q 'compact_otter_audit: demodulation mismatch' "$test_tmp/audit.err"; then
  cat "$test_tmp/audit.err" >&2
  exit 1
fi

"$repo_dir/bin/prooftrans" parents_only < "$test_tmp/reference.out" \
  > "$test_tmp/reference.proof"
"$repo_dir/bin/prooftrans" parents_only < "$test_tmp/audit.out" \
  > "$test_tmp/audit.proof"
sed -n '/^% Length of proof:/,/^============================== end of proof/p' \
  "$test_tmp/reference.proof" > "$test_tmp/reference.norm"
sed -n '/^% Length of proof:/,/^============================== end of proof/p' \
  "$test_tmp/audit.proof" > "$test_tmp/audit.norm"
cmp "$test_tmp/reference.norm" "$test_tmp/audit.norm"

echo 'compact_otter_audit_test: PASS'
