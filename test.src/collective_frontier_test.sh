#!/bin/sh
set -eu

repo_dir=$(CDPATH= cd -- "$(dirname -- "$0")/.." && pwd)
test_tmp=$(mktemp -d "${TMPDIR:-/tmp}/p9-collective-test.XXXXXX")
trap 'rm -rf -- "$test_tmp"' EXIT HUP INT TERM

"$repo_dir/bin/prover9" < "$repo_dir/test.src/collective_frontier.in" \
  > "$test_tmp/prover.out" 2> "$test_tmp/prover.err"

grep -q 'THEOREM PROVED' "$test_tmp/prover.out"
grep -Eq 'Search_loop: mode=discount, frontier=collective, active_indexed=[0-9]+, passive_indexed=0, delayed_demodulators=[0-9]+\.' \
  "$test_tmp/prover.out"
grep -Eq 'Collective_frontier: batches_created=[1-9][0-9]*, completed=[1-9][0-9]*, pending=[1-9][0-9]*, peak=[1-9][0-9]*, ratio=1, skipped=[0-9]+, parent_materializations=0, activations=[1-9][0-9]*\.' \
  "$test_tmp/prover.out"
grep -Eq 'Collective_work: paramod_pairs=[1-9][0-9]*, hyper_batches=[0-9]+\.' \
  "$test_tmp/prover.out"
grep -Eq 'Collective_hint_probes: scheduled=[1-9][0-9]*, expanded=[1-9][0-9]*, credit=(available|consumed)\.' \
  "$test_tmp/prover.out"
grep -Eq 'Collective_memory: descriptor_bytes=[1-9][0-9]*, history_bytes=[1-9][0-9]*\.' \
  "$test_tmp/prover.out"
grep -Eq 'Collective_history_index: clauses=[1-9][0-9]*, indexed=[0-9]+, shared=[0-9]+, retained=[0-9]+, retained_clause_bytes=[0-9]+, queries=[0-9]+, candidates=[0-9]+, future_rejected=[0-9]+, inactive_rejected=[0-9]+\.' \
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

# The optimization is independently switchable.  Turning it off must retain
# exact hint matching/proof behavior while scheduling no early descriptors.
sed 's/set(collective_hint_probes)\./clear(collective_hint_probes)./' \
  "$repo_dir/test.src/collective_frontier.in" > "$test_tmp/no-probes.in"
"$repo_dir/bin/prover9" < "$test_tmp/no-probes.in" \
  > "$test_tmp/no-probes.out" 2> "$test_tmp/no-probes.err"
grep -q 'THEOREM PROVED' "$test_tmp/no-probes.out"
grep -Eq 'Collective_hint_probes: scheduled=0, expanded=0, credit=available\.' \
  "$test_tmp/no-probes.out"
grep -Eq 'total=2, redundant=[0-9]+, active=[0-9]+, matched=[1-9][0-9]*' \
  "$test_tmp/no-probes.out"

"$repo_dir/bin/prover9" < "$repo_dir/test.src/collective_hyper.in" \
  > "$test_tmp/hyper.out" 2> "$test_tmp/hyper.err"
grep -q 'THEOREM PROVED' "$test_tmp/hyper.out"
grep -Eq 'Generated_by_rule: binary=0, hyper=[1-9][0-9]*, ur=0, paramod=0, other=[0-9]+\.' \
  "$test_tmp/hyper.out"
grep -Eq 'Collective_work: paramod_pairs=0, hyper_batches=[1-9][0-9]*\.' \
  "$test_tmp/hyper.out"
grep -Eq 'Collective_history_index: clauses=[1-9][0-9]*, indexed=[1-9][0-9]*, shared=[0-9]+, retained=[0-9]+, retained_clause_bytes=[0-9]+, queries=[1-9][0-9]*, candidates=[1-9][0-9]*, future_rejected=[1-9][0-9]*, inactive_rejected=[0-9]+\.' \
  "$test_tmp/hyper.out"
"$repo_dir/bin/prooftrans" parents_only < "$test_tmp/hyper.out" \
  > "$test_tmp/hyper-parents.out"
grep -q 'end of proof' "$test_tmp/hyper-parents.out"

if "$repo_dir/bin/prover9" < \
     "$repo_dir/test.src/collective_historical_hyper.in" \
     > "$test_tmp/historical-hyper.out" \
     2> "$test_tmp/historical-hyper.err"; then
  echo 'historical hyper fixture unexpectedly completed successfully' >&2
  exit 1
fi
grep -q 'SEARCH FAILED' "$test_tmp/historical-hyper.out"
grep -Eq 'COLLECTIVE_TRACE kind=pos_hyper given=[0-9]+ epoch=[0-9]+ history_candidates=2 future_rejected=0 inactive_rejected=0 generated=1 kept=1 hint_probe=0\.' \
  "$test_tmp/historical-hyper.out"
grep -Eq 'Generated_by_rule: binary=0, hyper=1, ur=0, paramod=0, other=[0-9]+\.' \
  "$test_tmp/historical-hyper.out"
grep -Eq 'Collective_frontier: .*parent_materializations=0, activations=[0-9]+\.' \
  "$test_tmp/historical-hyper.out"
grep -Eq 'Collective_history_index: clauses=6, indexed=6, shared=4, retained=2, retained_clause_bytes=[1-9][0-9]*, queries=1, candidates=2, future_rejected=0, inactive_rejected=0\.' \
  "$test_tmp/historical-hyper.out"
grep -Eq 'New_demodulators=1 .*Back_demodulated=2\.' \
  "$test_tmp/historical-hyper.out"

echo 'collective_frontier_test: PASS'
