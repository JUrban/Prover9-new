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

sed '1i\
set(compact_otter_demodulation).\
set(compact_unit_subsumption_audit).\
assign(stats,all).' "$repo_dir/prover9.examples/x2.in" | \
  "$repo_dir/bin/prover9" > "$test_tmp/unit-audit.out" 2> "$test_tmp/unit-audit.err"

sed '1i\
assign(compact_unit_strategy,code_tree).\
set(compact_otter_demodulation).\
set(compact_unit_subsumption_audit).\
assign(stats,all).' "$repo_dir/prover9.examples/x2.in" | \
  "$repo_dir/bin/prover9" > "$test_tmp/unit-tree-audit.out" 2> "$test_tmp/unit-tree-audit.err"

sed '1i\
set(compact_otter_demodulation).\
set(compact_otter_unit_index).\
assign(stats,all).' "$repo_dir/prover9.examples/x2.in" | \
  "$repo_dir/bin/prover9" > "$test_tmp/unit-compact.out" 2> "$test_tmp/unit-compact.err"

sed '1i\
set(compact_otter_demodulation).\
set(compact_otter_unit_index).\
set(compact_back_demod_audit).\
assign(stats,all).' "$repo_dir/prover9.examples/x2.in" | \
  "$repo_dir/bin/prover9" > "$test_tmp/back-demod-audit.out" 2> "$test_tmp/back-demod-audit.err"

sed '1i\
assign(compact_back_demod_strategy,signature32).\
set(compact_otter_demodulation).\
set(compact_otter_unit_index).\
set(compact_back_demod_audit).\
assign(stats,all).' "$repo_dir/prover9.examples/x2.in" | \
  "$repo_dir/bin/prover9" > "$test_tmp/back-signature-audit.out" 2> "$test_tmp/back-signature-audit.err"

sed '1i\
assign(compact_back_demod_strategy,mask32).\
set(compact_otter_demodulation).\
set(compact_otter_unit_index).\
set(compact_back_demod_audit).\
assign(stats,all).' "$repo_dir/prover9.examples/x2.in" | \
  "$repo_dir/bin/prover9" > "$test_tmp/back-mask32-audit.out" 2> "$test_tmp/back-mask32-audit.err"

sed '1i\
assign(compact_back_demod_strategy,code_tree).\
set(compact_otter_demodulation).\
set(compact_otter_unit_index).\
set(compact_back_demod_audit).\
assign(stats,all).' "$repo_dir/prover9.examples/x2.in" | \
  "$repo_dir/bin/prover9" > "$test_tmp/back-tree-audit.out" 2> "$test_tmp/back-tree-audit.err"

sed '1i\
assign(compact_back_demod_strategy,hybrid_tree).\
assign(compact_back_tree_min_tokens,4).\
assign(compact_back_tree_budget_kb,65536).\
set(compact_otter_demodulation).\
set(compact_otter_unit_index).\
set(compact_back_demod_audit).\
assign(stats,all).' "$repo_dir/prover9.examples/x2.in" | \
  "$repo_dir/bin/prover9" > "$test_tmp/back-hybrid-audit.out" 2> "$test_tmp/back-hybrid-audit.err"

sed '1i\
assign(compact_back_demod_strategy,hot_root_tree).\
assign(compact_back_tree_admit_work,1).\
assign(compact_back_tree_budget_kb,65536).\
set(compact_otter_demodulation).\
set(compact_otter_unit_index).\
set(compact_back_demod_audit).\
assign(stats,all).' "$repo_dir/prover9.examples/x2.in" | \
  "$repo_dir/bin/prover9" > "$test_tmp/back-hot-root-audit.out" 2> "$test_tmp/back-hot-root-audit.err"

sed '1i\
assign(compact_back_demod_strategy,position).\
assign(compact_back_tree_admit_work,1).\
assign(compact_back_tree_budget_kb,65536).\
set(compact_back_position_admission).\
set(compact_otter_demodulation).\
set(compact_otter_unit_index).\
set(compact_back_demod_audit).\
assign(stats,all).' "$repo_dir/prover9.examples/x2.in" | \
  "$repo_dir/bin/prover9" > "$test_tmp/back-position-audit.out" 2> "$test_tmp/back-position-audit.err"

sed '1i\
assign(compact_back_demod_strategy,adaptive).\
set(compact_back_edge_filter).\
set(compact_otter_demodulation).\
set(compact_otter_unit_index).\
set(compact_back_demod_audit).\
assign(stats,all).' "$repo_dir/prover9.examples/x2.in" | \
  "$repo_dir/bin/prover9" > "$test_tmp/back-edge-audit.out" 2> "$test_tmp/back-edge-audit.err"

sed '1i\
set(compact_otter_demodulation).\
set(compact_otter_unit_index).\
set(compact_otter_back_demod_index).\
assign(stats,all).' "$repo_dir/prover9.examples/x2.in" | \
  "$repo_dir/bin/prover9" > "$test_tmp/back-demod-compact.out" 2> "$test_tmp/back-demod-compact.err"

grep -q 'THEOREM PROVED' "$test_tmp/reference.out"
grep -q 'THEOREM PROVED' "$test_tmp/audit.out"
grep -q 'THEOREM PROVED' "$test_tmp/compact.out"
grep -q 'THEOREM PROVED' "$test_tmp/unit-audit.out"
grep -q 'THEOREM PROVED' "$test_tmp/unit-tree-audit.out"
grep -q 'THEOREM PROVED' "$test_tmp/unit-compact.out"
grep -q 'THEOREM PROVED' "$test_tmp/back-demod-audit.out"
grep -q 'THEOREM PROVED' "$test_tmp/back-signature-audit.out"
grep -q 'THEOREM PROVED' "$test_tmp/back-tree-audit.out"
grep -q 'THEOREM PROVED' "$test_tmp/back-hybrid-audit.out"
grep -q 'THEOREM PROVED' "$test_tmp/back-hot-root-audit.out"
grep -q 'THEOREM PROVED' "$test_tmp/back-position-audit.out"
grep -q 'THEOREM PROVED' "$test_tmp/back-edge-audit.out"
grep -q 'THEOREM PROVED' "$test_tmp/back-demod-compact.out"
grep -Eq 'Compact_otter_audit: queries=[1-9][0-9]*, failures=0, current_rules=[1-9][0-9]*,' \
  "$test_tmp/audit.out"
grep -Eq 'Compact_otter_demodulation: current_rules=[1-9][0-9]*, peak_rules=[1-9][0-9]*,' \
  "$test_tmp/compact.out"
grep -Eq 'Compact_unit_index: mode=audit, strategy=root_scan, failures=0, active=[1-9][0-9]*, peak=[1-9][0-9]*,' \
  "$test_tmp/unit-audit.out"
grep -Eq 'Compact_unit_index: mode=audit, strategy=code_tree, failures=0, active=[1-9][0-9]*, peak=[1-9][0-9]*,' \
  "$test_tmp/unit-tree-audit.out"
grep -Eq 'Compact_unit_index: mode=authoritative, strategy=root_scan, failures=0, active=[1-9][0-9]*, peak=[1-9][0-9]*,' \
  "$test_tmp/unit-compact.out"
grep -Eq 'Compact_back_demod: mode=audit, strategy=mask8, failures=0, active=[1-9][0-9]*, peak=[1-9][0-9]*,' \
  "$test_tmp/back-demod-audit.out"
grep -Eq 'Compact_back_demod: mode=audit, strategy=signature32, failures=0, active=[1-9][0-9]*, peak=[1-9][0-9]*,' \
  "$test_tmp/back-signature-audit.out"
grep -Eq 'Compact_back_demod: mode=audit, strategy=mask32, failures=0, active=[1-9][0-9]*, peak=[1-9][0-9]*,' \
  "$test_tmp/back-mask32-audit.out"
grep -Eq 'Compact_back_demod: mode=audit, strategy=code_tree, failures=0, active=[1-9][0-9]*, peak=[1-9][0-9]*,' \
  "$test_tmp/back-tree-audit.out"
grep -Eq 'Compact_back_demod: mode=audit, strategy=hybrid_tree, failures=0, active=[1-9][0-9]*, peak=[1-9][0-9]*,' \
  "$test_tmp/back-hybrid-audit.out"
grep -Eq 'Compact_back_demod: mode=audit, strategy=hot_root_tree, failures=0, active=[1-9][0-9]*, peak=[1-9][0-9]*,' \
  "$test_tmp/back-hot-root-audit.out"
grep -Eq 'Compact_back_demod: mode=audit, strategy=position, failures=0, active=[1-9][0-9]*, peak=[1-9][0-9]*,' \
  "$test_tmp/back-position-audit.out"
grep -Eq 'Compact_back_demod: mode=audit, strategy=adaptive, failures=0, active=[1-9][0-9]*, peak=[1-9][0-9]*,' \
  "$test_tmp/back-edge-audit.out"
grep -Eq 'Compact_back_edge: enabled=yes, features=[1-9][0-9]*, postings=[1-9][0-9]*,' \
  "$test_tmp/back-edge-audit.out"
grep -Eq 'Compact_back_demod: mode=authoritative, strategy=mask8, failures=0, active=[1-9][0-9]*, peak=[1-9][0-9]*,' \
  "$test_tmp/back-demod-compact.out"
if grep -q 'compact_otter_audit: demodulation mismatch' "$test_tmp/audit.err"; then
  cat "$test_tmp/audit.err" >&2
  exit 1
fi
if grep -q 'compact_back_demod_audit: mismatch' "$test_tmp/back-demod-audit.err"; then
  cat "$test_tmp/back-demod-audit.err" >&2
  exit 1
fi
if grep -q 'compact_back_demod_audit: mismatch' "$test_tmp/back-signature-audit.err"; then
  cat "$test_tmp/back-signature-audit.err" >&2
  exit 1
fi
if grep -q 'compact_back_demod_audit: mismatch' "$test_tmp/back-mask32-audit.err"; then
  cat "$test_tmp/back-mask32-audit.err" >&2
  exit 1
fi
if grep -q 'compact_back_demod_audit: mismatch' "$test_tmp/back-tree-audit.err"; then
  cat "$test_tmp/back-tree-audit.err" >&2
  exit 1
fi
if grep -q 'compact_back_demod_audit: mismatch' "$test_tmp/back-hybrid-audit.err"; then
  cat "$test_tmp/back-hybrid-audit.err" >&2
  exit 1
fi
if grep -q 'compact_back_demod_audit: mismatch' "$test_tmp/back-hot-root-audit.err"; then
  cat "$test_tmp/back-hot-root-audit.err" >&2
  exit 1
fi
if grep -q 'compact_back_demod_audit: mismatch' "$test_tmp/back-position-audit.err"; then
  cat "$test_tmp/back-position-audit.err" >&2
  exit 1
fi
if grep -q 'compact_back_demod_audit: mismatch' "$test_tmp/back-edge-audit.err"; then
  cat "$test_tmp/back-edge-audit.err" >&2
  exit 1
fi

"$repo_dir/bin/prooftrans" parents_only < "$test_tmp/reference.out" \
  > "$test_tmp/reference.proof"
"$repo_dir/bin/prooftrans" parents_only < "$test_tmp/audit.out" \
  > "$test_tmp/audit.proof"
"$repo_dir/bin/prooftrans" parents_only < "$test_tmp/compact.out" \
  > "$test_tmp/compact.proof"
"$repo_dir/bin/prooftrans" parents_only < "$test_tmp/unit-audit.out" \
  > "$test_tmp/unit-audit.proof"
"$repo_dir/bin/prooftrans" parents_only < "$test_tmp/unit-tree-audit.out" \
  > "$test_tmp/unit-tree-audit.proof"
"$repo_dir/bin/prooftrans" parents_only < "$test_tmp/unit-compact.out" \
  > "$test_tmp/unit-compact.proof"
"$repo_dir/bin/prooftrans" parents_only < "$test_tmp/back-demod-audit.out" \
  > "$test_tmp/back-demod-audit.proof"
"$repo_dir/bin/prooftrans" parents_only < "$test_tmp/back-signature-audit.out" \
  > "$test_tmp/back-signature-audit.proof"
"$repo_dir/bin/prooftrans" parents_only < "$test_tmp/back-mask32-audit.out" \
  > "$test_tmp/back-mask32-audit.proof"
"$repo_dir/bin/prooftrans" parents_only < "$test_tmp/back-tree-audit.out" \
  > "$test_tmp/back-tree-audit.proof"
"$repo_dir/bin/prooftrans" parents_only < "$test_tmp/back-hybrid-audit.out" \
  > "$test_tmp/back-hybrid-audit.proof"
"$repo_dir/bin/prooftrans" parents_only < "$test_tmp/back-hot-root-audit.out" \
  > "$test_tmp/back-hot-root-audit.proof"
"$repo_dir/bin/prooftrans" parents_only < "$test_tmp/back-edge-audit.out" \
  > "$test_tmp/back-edge-audit.proof"
"$repo_dir/bin/prooftrans" parents_only < "$test_tmp/back-demod-compact.out" \
  > "$test_tmp/back-demod-compact.proof"
sed -n '/^% Length of proof:/,/^============================== end of proof/p' \
  "$test_tmp/reference.proof" > "$test_tmp/reference.norm"
sed -n '/^% Length of proof:/,/^============================== end of proof/p' \
  "$test_tmp/audit.proof" > "$test_tmp/audit.norm"
sed -n '/^% Length of proof:/,/^============================== end of proof/p' \
  "$test_tmp/compact.proof" > "$test_tmp/compact.norm"
sed -n '/^% Length of proof:/,/^============================== end of proof/p' \
  "$test_tmp/unit-audit.proof" > "$test_tmp/unit-audit.norm"
sed -n '/^% Length of proof:/,/^============================== end of proof/p' \
  "$test_tmp/unit-tree-audit.proof" > "$test_tmp/unit-tree-audit.norm"
sed -n '/^% Length of proof:/,/^============================== end of proof/p' \
  "$test_tmp/unit-compact.proof" > "$test_tmp/unit-compact.norm"
sed -n '/^% Length of proof:/,/^============================== end of proof/p' \
  "$test_tmp/back-demod-audit.proof" > "$test_tmp/back-demod-audit.norm"
sed -n '/^% Length of proof:/,/^============================== end of proof/p' \
  "$test_tmp/back-signature-audit.proof" > "$test_tmp/back-signature-audit.norm"
sed -n '/^% Length of proof:/,/^============================== end of proof/p' \
  "$test_tmp/back-mask32-audit.proof" > "$test_tmp/back-mask32-audit.norm"
sed -n '/^% Length of proof:/,/^============================== end of proof/p' \
  "$test_tmp/back-tree-audit.proof" > "$test_tmp/back-tree-audit.norm"
sed -n '/^% Length of proof:/,/^============================== end of proof/p' \
  "$test_tmp/back-hybrid-audit.proof" > "$test_tmp/back-hybrid-audit.norm"
sed -n '/^% Length of proof:/,/^============================== end of proof/p' \
  "$test_tmp/back-hot-root-audit.proof" > "$test_tmp/back-hot-root-audit.norm"
sed -n '/^% Length of proof:/,/^============================== end of proof/p' \
  "$test_tmp/back-edge-audit.proof" > "$test_tmp/back-edge-audit.norm"
sed -n '/^% Length of proof:/,/^============================== end of proof/p' \
  "$test_tmp/back-demod-compact.proof" > "$test_tmp/back-demod-compact.norm"
cmp "$test_tmp/reference.norm" "$test_tmp/audit.norm"
cmp "$test_tmp/reference.norm" "$test_tmp/compact.norm"
cmp "$test_tmp/reference.norm" "$test_tmp/unit-audit.norm"
cmp "$test_tmp/reference.norm" "$test_tmp/unit-tree-audit.norm"
cmp "$test_tmp/reference.norm" "$test_tmp/unit-compact.norm"
cmp "$test_tmp/reference.norm" "$test_tmp/back-demod-audit.norm"
cmp "$test_tmp/reference.norm" "$test_tmp/back-signature-audit.norm"
cmp "$test_tmp/reference.norm" "$test_tmp/back-mask32-audit.norm"
cmp "$test_tmp/reference.norm" "$test_tmp/back-tree-audit.norm"
cmp "$test_tmp/reference.norm" "$test_tmp/back-hybrid-audit.norm"
cmp "$test_tmp/reference.norm" "$test_tmp/back-hot-root-audit.norm"
cmp "$test_tmp/reference.norm" "$test_tmp/back-edge-audit.norm"
cmp "$test_tmp/reference.norm" "$test_tmp/back-demod-compact.norm"

for run in reference audit compact unit-audit unit-tree-audit unit-compact back-demod-audit back-mask32-audit back-signature-audit back-tree-audit back-hybrid-audit back-hot-root-audit back-edge-audit back-demod-compact; do
  grep '^Given=' "$test_tmp/$run.out" | tail -n 1 > "$test_tmp/$run.search"
  grep '^Usable=' "$test_tmp/$run.out" | tail -n 1 >> "$test_tmp/$run.search"
done
cmp "$test_tmp/reference.search" "$test_tmp/audit.search"
cmp "$test_tmp/reference.search" "$test_tmp/compact.search"
cmp "$test_tmp/reference.search" "$test_tmp/unit-audit.search"
cmp "$test_tmp/reference.search" "$test_tmp/unit-tree-audit.search"
cmp "$test_tmp/reference.search" "$test_tmp/unit-compact.search"
cmp "$test_tmp/reference.search" "$test_tmp/back-demod-audit.search"
cmp "$test_tmp/reference.search" "$test_tmp/back-signature-audit.search"
cmp "$test_tmp/reference.search" "$test_tmp/back-mask32-audit.search"
cmp "$test_tmp/reference.search" "$test_tmp/back-tree-audit.search"
cmp "$test_tmp/reference.search" "$test_tmp/back-hybrid-audit.search"
cmp "$test_tmp/reference.search" "$test_tmp/back-hot-root-audit.search"
cmp "$test_tmp/reference.search" "$test_tmp/back-edge-audit.search"
cmp "$test_tmp/reference.search" "$test_tmp/back-demod-compact.search"

# x2 itself is unit-only.  Add one harmless nonunit equality clause and turn
# automatic setup off so the feature-index audit and authoritative paths are
# both exercised without enabling unsupported unit deletion.
awk 'BEGIN { added=0 }
     !added && /^end_of_list\.$/ {
       print "  x * y = y * x | x = e."
       added=1
     }
     { print }' "$repo_dir/prover9.examples/x2.in" > "$test_tmp/nonunit.in"

sed '1i\
clear(auto).\
clear(auto_setup).\
clear(auto_inference).\
clear(unit_deletion).\
set(paramodulation).\
assign(stats,all).' "$test_tmp/nonunit.in" | \
  "$repo_dir/bin/prover9" > "$test_tmp/nonunit-reference.out" 2> "$test_tmp/nonunit-reference.err"

sed '1i\
clear(auto).\
clear(auto_setup).\
clear(auto_inference).\
clear(unit_deletion).\
set(paramodulation).\
set(compact_otter_demodulation).\
set(compact_otter_unit_index).\
set(compact_otter_back_demod_index).\
set(compact_nonunit_subsumption_audit).\
set(compact_nonunit_path_filter).\
assign(stats,all).' "$test_tmp/nonunit.in" | \
  "$repo_dir/bin/prover9" > "$test_tmp/nonunit-audit.out" 2> "$test_tmp/nonunit-audit.err"

sed '1i\
clear(auto).\
clear(auto_setup).\
clear(auto_inference).\
clear(unit_deletion).\
set(paramodulation).\
set(compact_otter_demodulation).\
set(compact_otter_unit_index).\
set(compact_otter_back_demod_index).\
set(compact_otter_nonunit_index).\
set(compact_nonunit_path_filter).\
assign(stats,all).' "$test_tmp/nonunit.in" | \
  "$repo_dir/bin/prover9" > "$test_tmp/nonunit-compact.out" 2> "$test_tmp/nonunit-compact.err"

sed '1i\
clear(auto).\
clear(auto_setup).\
clear(auto_inference).\
clear(unit_deletion).\
set(paramodulation).\
assign(search_loop,otter).\
assign(inference_frontier,clauses).\
assign(passive_store,dense).\
assign(ancestor_store,mmap).\
assign(sos_limit,-1).\
set(compact_otter_demodulation).\
set(compact_otter_unit_index).\
set(compact_otter_back_demod_index).\
set(compact_otter_nonunit_index).\
set(compact_nonunit_path_filter).\
assign(compact_passive_cache,4).\
assign(stats,all).' "$test_tmp/nonunit.in" | \
  "$repo_dir/bin/prover9" > "$test_tmp/nonunit-archive.out" 2> "$test_tmp/nonunit-archive.err"

for run in nonunit-reference nonunit-audit nonunit-compact nonunit-archive; do
  grep -q 'THEOREM PROVED' "$test_tmp/$run.out"
  "$repo_dir/bin/prooftrans" parents_only < "$test_tmp/$run.out" | \
    sed -n '/^% Length of proof:/,/^============================== end of proof/p' \
    > "$test_tmp/$run.norm"
  grep '^Given=' "$test_tmp/$run.out" | tail -n 1 > "$test_tmp/$run.search"
  grep '^Usable=' "$test_tmp/$run.out" | tail -n 1 >> "$test_tmp/$run.search"
done
grep -Eq 'Compact_nonunit_index: mode=audit, failures=0, active=[1-9][0-9]*,' \
  "$test_tmp/nonunit-audit.out"
grep -Eq 'forward_path_rejects=[1-9][0-9]*,' \
  "$test_tmp/nonunit-audit.out"
grep -Eq 'Compact_nonunit_index: mode=authoritative, failures=0, active=[1-9][0-9]*,' \
  "$test_tmp/nonunit-compact.out"
grep -Eq 'Dense_passive: backing=ancestor-mmap, .*records=[1-9][0-9]*,' \
  "$test_tmp/nonunit-archive.out"
grep -Eq 'Ancestor_store: records=[1-9][0-9]*, .*validation_failures=0,' \
  "$test_tmp/nonunit-archive.out"
grep -Eq 'Compact_passive_cache: budget=4194304, .*hits=[0-9]+, .*misses=[0-9]+, ' \
  "$test_tmp/nonunit-archive.out"
cmp "$test_tmp/nonunit-reference.norm" "$test_tmp/nonunit-audit.norm"
cmp "$test_tmp/nonunit-reference.norm" "$test_tmp/nonunit-compact.norm"
cmp "$test_tmp/nonunit-reference.norm" "$test_tmp/nonunit-archive.norm"
cmp "$test_tmp/nonunit-reference.search" "$test_tmp/nonunit-audit.search"
cmp "$test_tmp/nonunit-reference.search" "$test_tmp/nonunit-compact.search"
cmp "$test_tmp/nonunit-reference.search" "$test_tmp/nonunit-archive.search"

# Exercise the terminal compact lifetime split with a real packed hint bank.
# The proof is found during preprocessing, after hint matching has run but
# before any given clause.  This catches releasing the packed index too early,
# failing to empty it before destruction, or losing the retained hint clauses
# needed by proof/result reconstruction.
awk '
BEGIN {
  print "clear(auto)."
  print "clear(auto_setup)."
  print "assign(search_loop,otter)."
  print "assign(passive_store,dense)."
  print "assign(hint_index,packed_fast)."
  print "assign(inference_frontier,clauses)."
  print "assign(ancestor_store,file)."
  print "set(compact_otter_demodulation)."
  print "set(compact_otter_unit_index)."
  print "set(compact_otter_back_demod_index)."
  print "set(compact_otter_nonunit_index)."
  print "assign(compact_passive_cache,0)."
  print "assign(stats,all)."
}
/^assign\((search_loop|passive_store|hint_index|inference_frontier|ancestor_store|stats|max_seconds),/ { next }
{ print }
' "$repo_dir/test.src/hint_anyconst.in" > "$test_tmp/terminal-hints.in"

P9_COMPACT_HEAP=1 "$repo_dir/bin/prover9" \
  < "$test_tmp/terminal-hints.in" \
  > "$test_tmp/terminal-hints.out" 2> "$test_tmp/terminal-hints.err"
grep -q 'THEOREM PROVED' "$test_tmp/terminal-hints.out"
grep -q '^Given=0\.' "$test_tmp/terminal-hints.out"
grep -Eq 'Packed_hint_index: nodes=[1-9][0-9]*, references=[1-9][0-9]*,' \
  "$test_tmp/terminal-hints.out"
if grep -q 'ERROR: Hints index not empty!' "$test_tmp/terminal-hints.out"; then
  cat "$test_tmp/terminal-hints.out" >&2
  exit 1
fi
"$repo_dir/bin/prooftrans" parents_only < "$test_tmp/terminal-hints.out" | \
  sed -n '/^% Length of proof:/,/^============================== end of proof/p' \
  > "$test_tmp/terminal-hints.norm"
grep -q 'label(repeated_anyconst)' "$test_tmp/terminal-hints.norm"

echo 'compact_otter_audit_test: PASS'
