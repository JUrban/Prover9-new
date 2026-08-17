#!/bin/sh
set -eu

repo_dir=$(CDPATH= cd -- "$(dirname -- "$0")/.." && pwd)
prover=${P9_TEST_PROVER:-$repo_dir/bin/prover9}
test_tmp=$(mktemp -d "${TMPDIR:-/tmp}/p9-compact-checkpoint.XXXXXX")
trap 'rm -rf -- "$test_tmp"' EXIT HUP INT TERM

# x2 is otherwise unit-only.  The harmless disjunction makes the test cover
# the authoritative nonunit feature index as well as rewrite, unit, and
# back-demodulation reconstruction.
awk 'BEGIN { added=0 }
     !added && /^end_of_list\.$/ {
       print "  x * y = y * x | x = e."
       added=1
     }
     { print }' "$repo_dir/prover9.examples/x2.in" > "$test_tmp/problem.in"

make_input()
{
  checkpoint_given=$1
  sed "1i\\
assign(checkpoint_given,$checkpoint_given).\\
set(checkpoint_exit).\\
set(checkpoint_verify).\\
clear(auto).\\
clear(auto_setup).\\
clear(auto_inference).\\
clear(unit_deletion).\\
set(paramodulation).\\
assign(search_loop,otter).\\
assign(inference_frontier,clauses).\\
assign(passive_store,dense).\\
assign(passive_directory,file).\\
assign(passive_selector_store,file).\\
assign(passive_selector_buffer,1024).\\
assign(ancestor_store,file).\\
assign(sos_limit,-1).\\
set(compact_otter_demodulation).\\
set(compact_otter_unit_index).\\
set(compact_otter_back_demod_index).\\
set(compact_otter_nonunit_index).\\
set(search_event_trace).\\
set(print_given).\\
assign(stats,all)." "$test_tmp/problem.in"
}

make_input -1 | "$prover" > "$test_tmp/control.out" \
  2> "$test_tmp/control.err"
grep -q 'THEOREM PROVED' "$test_tmp/control.out"
grep -E '^(CANDIDATE_TRACE|KEPT_TRACE|given #)' \
  "$test_tmp/control.out" > "$test_tmp/control.trace"
{
  grep '^Given=' "$test_tmp/control.out" | tail -n 1
  grep '^Usable=' "$test_tmp/control.out" | tail -n 1
} > "$test_tmp/control.search"
"$repo_dir/bin/prooftrans" parents_only < "$test_tmp/control.out" | \
  sed -n '/^% Length of proof:/,/^============================== end of proof/p' \
  > "$test_tmp/control.proof"

# Boundary zero validates a frontier made entirely by preprocessing; boundary
# two also validates retirement and counter state after search has advanced.
for checkpoint_given in 0 2; do
  case_dir="$test_tmp/case-$checkpoint_given"
  mkdir "$case_dir"
  (
    cd "$case_dir"
    make_input "$checkpoint_given" | "$prover" \
      > before.out 2> before.err || true
  )
  checkpoint_dir=$(find "$case_dir" -maxdepth 1 -type d \
    -name "prover9_*_ckpt_$checkpoint_given" -print)
  test -n "$checkpoint_dir"
  test -f "$checkpoint_dir/verify.txt"

  "$prover" -r "$checkpoint_dir" < /dev/null \
    > "$case_dir/resumed.out" 2> "$case_dir/resumed.err"
  grep -q 'THEOREM PROVED' "$case_dir/resumed.out"
  grep -Eq '^%   Verification: [0-9]+ passed, 0 failed\.$' \
    "$case_dir/resumed.out"
  grep -Eq 'Compact_otter_demodulation: current_rules=[1-9][0-9]*,' \
    "$case_dir/resumed.out"
  grep -Eq 'Compact_unit_index: mode=authoritative, strategy=root_scan, failures=0,' \
    "$case_dir/resumed.out"
  grep -Eq 'Compact_back_demod: mode=authoritative, strategy=mask8, failures=0,' \
    "$case_dir/resumed.out"
  grep -Eq 'Compact_nonunit_index: mode=authoritative, failures=0,' \
    "$case_dir/resumed.out"

  {
    grep -E '^(CANDIDATE_TRACE|KEPT_TRACE|given #)' \
      "$case_dir/before.out" || true
    grep -E '^(CANDIDATE_TRACE|KEPT_TRACE|given #)' \
      "$case_dir/resumed.out" || true
  } > "$case_dir/combined.trace"
  cmp "$test_tmp/control.trace" "$case_dir/combined.trace"

  {
    grep '^Given=' "$case_dir/resumed.out" | tail -n 1
    grep '^Usable=' "$case_dir/resumed.out" | tail -n 1
  } > "$case_dir/resumed.search"
  if ! cmp "$test_tmp/control.search" "$case_dir/resumed.search"; then
    diff -u "$test_tmp/control.search" "$case_dir/resumed.search" >&2 || true
    exit 1
  fi

  "$repo_dir/bin/prooftrans" parents_only < "$case_dir/resumed.out" | \
    sed -n '/^% Length of proof:/,/^============================== end of proof/p' \
    > "$case_dir/resumed.proof"
  cmp "$test_tmp/control.proof" "$case_dir/resumed.proof"
done

# Checkpoint at the exact archive-lifetime boundary covered by
# terminal_cold_unit.in.  The resumed run discovers its proof while compact
# unit-conflict owns a cold passive candidate.  The safe-point finalizer must
# preserve the complete proof and release every cache pin after restore too.
terminal_dir="$test_tmp/terminal-cold"
mkdir "$terminal_dir"
(
  cd "$terminal_dir"
  sed '1i\
assign(checkpoint_given,0).\
set(checkpoint_exit).\
set(checkpoint_verify).' "$repo_dir/test.src/terminal_cold_unit.in" | \
    "$prover" > before.out 2> before.err || true
)
terminal_checkpoint=$(find "$terminal_dir" -maxdepth 1 -type d \
  -name 'prover9_*_ckpt_0' -print)
test -n "$terminal_checkpoint"
"$prover" -r "$terminal_checkpoint" < /dev/null \
  > "$terminal_dir/resumed.out" 2> "$terminal_dir/resumed.err"
grep -q 'THEOREM PROVED' "$terminal_dir/resumed.out"
grep -Eq '^%   Verification: [0-9]+ passed, 0 failed\.$' \
  "$terminal_dir/resumed.out"
grep -q '^3 -q(a)\.  \[assumption\]\.$' "$terminal_dir/resumed.out"
grep -q '^5 \$F\.  \[resolve(4,a,3,a)\]\.$' "$terminal_dir/resumed.out"
grep -Eq 'Compact_passive_cache: .*misses=1,' \
  "$terminal_dir/resumed.out"
if grep -Eq 'Fatal error|closure failure|pinned clause' \
     "$terminal_dir/resumed.err"; then
  cat "$terminal_dir/resumed.err" >&2
  exit 1
fi
"$repo_dir/bin/prooftrans" parents_only < "$terminal_dir/resumed.out" | \
  sed -n '/^% Length of proof:/,/^============================== end of proof/p' \
  > "$terminal_dir/resumed.proof"
grep -q '^3 -q(a)\.  \[\]\.$' "$terminal_dir/resumed.proof"
grep -q '^5 \$F\.  \[4,3\]\.$' "$terminal_dir/resumed.proof"

echo 'compact_otter_checkpoint_test: PASS'
