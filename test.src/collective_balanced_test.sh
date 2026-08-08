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

# Force a one-position native paramodulation budget on the equality proof.
# No raw prefix is regenerated, both directions still advance, and the
# authoritative proof path remains translatable.
sed -e '1i assign(sos_limit,-1).\nassign(collective_raw_work_budget,1).\nassign(collective_candidate_chunk,1).' \
    -e 's/assign(passive_store,compressed)\./assign(passive_store,dense)./' \
    -e 's/set(collective_hint_probes)\./clear(collective_hint_probes)./' \
    -e '/assign(inference_frontier,collective)\./a assign(collective_scheduler,balanced_hint).' \
  "$repo_dir/test.src/collective_frontier.in" > "$test_tmp/paramod.in"
"$repo_dir/bin/prover9" < "$test_tmp/paramod.in" \
  > "$test_tmp/paramod.out" 2> "$test_tmp/paramod.err"
grep -q 'THEOREM PROVED' "$test_tmp/paramod.out"
grep -Eq 'Collective_iterators: raw_budget=1, raw_steps=[1-9][0-9]*, candidates=[1-9][0-9]*, completions=[1-9][0-9]*, invalidations=0, raw_turn_peak=1, path_bytes=[0-9]+, hyper_raw_steps=0, hyper_candidates=0, hyper_completions=0, hyper_choice_bytes=0\.' \
  "$test_tmp/paramod.out"
grep -Eq 'Collective_chunks: limit=1, emitted=[1-9][0-9]*, replayed=0, raw_seen=[1-9][0-9]*, deferred_turns=[1-9][0-9]*, raw_peak=1\.' \
  "$test_tmp/paramod.out"
"$repo_dir/bin/prooftrans" expand < "$test_tmp/paramod.out" \
  > "$test_tmp/paramod-proof.out"
grep -q 'end of proof' "$test_tmp/paramod-proof.out"

# The same unit raw-work bound applies inside nested hyperresolution clashes.
# In particular, a large clash cannot hide the paramodulation lane behind one
# opaque generator call.
sed -e '1i assign(sos_limit,-1).\nassign(collective_raw_work_budget,1).\nassign(collective_candidate_chunk,1).' \
    -e 's/assign(passive_store,compressed)\./assign(passive_store,dense)./' \
    -e '/assign(inference_frontier,collective)\./a assign(collective_scheduler,balanced_hint).' \
  "$repo_dir/test.src/collective_hyper.in" > "$test_tmp/hyper.in"
"$repo_dir/bin/prover9" < "$test_tmp/hyper.in" \
  > "$test_tmp/hyper.out" 2> "$test_tmp/hyper.err"
grep -q 'THEOREM PROVED' "$test_tmp/hyper.out"
grep -Eq 'Collective_iterators: raw_budget=1, raw_steps=[1-9][0-9]*, candidates=[1-9][0-9]*, completions=[1-9][0-9]*, invalidations=0, raw_turn_peak=1, path_bytes=0, hyper_raw_steps=[1-9][0-9]*, hyper_candidates=[1-9][0-9]*, hyper_completions=[1-9][0-9]*, hyper_choice_bytes=[1-9][0-9]*\.' \
  "$test_tmp/hyper.out"
grep -Eq 'Collective_chunks: limit=1, emitted=[1-9][0-9]*, replayed=0, raw_seen=[1-9][0-9]*, deferred_turns=[1-9][0-9]*, raw_peak=1\.' \
  "$test_tmp/hyper.out"
"$repo_dir/bin/prooftrans" expand < "$test_tmp/hyper.out" \
  > "$test_tmp/hyper-proof.out"
grep -q 'end of proof' "$test_tmp/hyper-proof.out"

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

# Checkpoint with live raw candidates in the global bounded pool.  The pool
# body/metadata file and per-descriptor raw ordinal must reproduce the same
# final advisory/authoritative accounting as the uninterrupted run.
sed '1i assign(checkpoint_candidate_pool,1).\nset(checkpoint_exit).\nset(checkpoint_verify).' \
  "$repo_dir/test.src/collective_balanced.in" > "$test_tmp/pool-checkpoint.in"
pool_case="$test_tmp/pool-checkpoint-case"
mkdir "$pool_case"
(
  cd "$pool_case"
  "$repo_dir/bin/prover9" < "$test_tmp/pool-checkpoint.in" \
    > before.out 2> before.err || true
)
pool_checkpoint_dir=$(find "$pool_case" -maxdepth 1 -type d \
  -name 'prover9_*_ckpt_*' -print)
test -n "$pool_checkpoint_dir"
pool_count=$(awk 'NR == 1 { print $2 }' \
  "$pool_checkpoint_dir/collective_candidates.txt")
test "$pool_count" -ge 1
"$repo_dir/bin/prover9" -r "$pool_checkpoint_dir" < /dev/null \
  > "$test_tmp/pool-resumed.out" 2> "$test_tmp/pool-resumed.err" || true
grep -Eq '^%   Verification: [0-9]+ passed, 0 failed\.$' \
  "$test_tmp/pool-resumed.out"
grep -E '^(Given=|Collective_(frontier|rule_descriptors|work|balanced|chunks|candidate_cache|preview):)' \
  "$test_tmp/balanced.out" | tail -8 > "$test_tmp/pool-control.stats"
grep -E '^(Given=|Collective_(frontier|rule_descriptors|work|balanced|chunks|candidate_cache|preview):)' \
  "$test_tmp/pool-resumed.out" | tail -8 > "$test_tmp/pool-resumed.stats"
diff -u "$test_tmp/pool-control.stats" "$test_tmp/pool-resumed.stats"

# Exact hint preview is advisory but must agree with authoritative matching
# on stable candidates, while the configured count and byte bounds remain
# hard even when several rule lanes contribute to one shared window.
"$repo_dir/bin/prover9" < "$repo_dir/test.src/collective_preview.in" \
  > "$test_tmp/preview.out" 2> "$test_tmp/preview.err" || true
grep -Eq 'Collective_preview: calls=[1-9][0-9]*, predicted_hints=[1-9][0-9]*, authoritative_hints=[1-9][0-9]*, false_positives=0, changed_hint_ids=0, stale_refreshes=[0-9]+\.' \
  "$test_tmp/preview.out"
grep -Eq 'Collective_candidate_pool: .*commits=[1-9][0-9]*, priority_commits=[1-9][0-9]*, fair_commits=[1-9][0-9]*\.' \
  "$test_tmp/preview.out"
preview_peak=$(sed -n 's/.*Collective_candidate_cache:.* peak=\([0-9][0-9]*\),.*/\1/p' \
  "$test_tmp/preview.out" | tail -1)
test -n "$preview_peak"
test "$preview_peak" -le 4

echo 'collective_balanced_test: PASS'
