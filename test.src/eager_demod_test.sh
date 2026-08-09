#!/bin/sh
set -eu

repo_dir=$(CDPATH= cd -- "$(dirname -- "$0")/.." && pwd)
test_tmp=$(mktemp -d "${TMPDIR:-/tmp}/p9-eager-demod-test.XXXXXX")
trap 'rm -rf -- "$test_tmp"' EXIT HUP INT TERM

"$repo_dir/bin/prover9" < "$repo_dir/test.src/eager_demod.in" \
  > "$test_tmp/prover.out" 2> "$test_tmp/prover.err"

grep -q 'THEOREM PROVED' "$test_tmp/prover.out"
grep -Eq 'Discount_demodulation: policy=eager_legacy, candidates=[1-9][0-9]*, oriented=[1-9][0-9]*, lex=[0-9]+, rewrite_only_admitted=[1-9][0-9]*, retired=0, selected=[0-9]+, current=[0-9]+, peak=[1-9][0-9]*, bytes=[1-9][0-9]*, peak_bytes=[1-9][0-9]*\.' \
  "$test_tmp/prover.out"
grep -Eq 'New_demodulators=[1-9][0-9]* \([0-9]+ lex\), Back_demodulated=[1-9][0-9]*\.' \
  "$test_tmp/prover.out"
grep -Eq 'Packed_hint_operation: op=back_demod, .*exact_positive=[1-9][0-9]*, rewrites=[1-9][0-9]*, reindexes=[1-9][0-9]*' \
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

# The compact policy must preserve the eager oracle's proof and hint behavior
# while replacing full discrimination-index clones with encoded rules plus
# compressed proof shells.
sed 's/assign(discount_demodulation,eager_legacy)\./assign(discount_demodulation,eager_interreduced)./' \
  "$repo_dir/test.src/eager_demod.in" | \
  "$repo_dir/bin/prover9" > "$test_tmp/compact.out" \
  2> "$test_tmp/compact.err"
grep -q 'THEOREM PROVED' "$test_tmp/compact.out"
grep -Eq 'Discount_demodulation: policy=eager_interreduced, .*rewrite_only_admitted=[1-9][0-9]*, .*selected=[1-9][0-9]*' \
  "$test_tmp/compact.out"
grep -Eq 'Compact_rewrite: current=[1-9][0-9]*, peak=[1-9][0-9]*, retired=0, .*attempts=[1-9][0-9]*, rewrites=[1-9][0-9]*' \
  "$test_tmp/compact.out"
grep -Eq 'Packed_hint_operation: op=back_demod, .*exact_positive=[1-9][0-9]*, rewrites=[1-9][0-9]*, reindexes=[1-9][0-9]*' \
  "$test_tmp/compact.out"
"$repo_dir/bin/prooftrans" parents_only < "$test_tmp/compact.out" \
  > "$test_tmp/compact-parents.out"
"$repo_dir/bin/directproof" < "$test_tmp/compact.out" \
  > "$test_tmp/compact-direct.out"
grep -q 'end of proof' "$test_tmp/compact-parents.out"
grep -q 'Directproof did' "$test_tmp/compact-direct.out"

# Save after preprocessing, while both rewrite-only rules and their dense
# passive proof bodies exist.  Resume must reconstruct the index using clone
# terms, transfer IDs during bulk archival, and reach identical core totals.
checkpoint_case="$test_tmp/checkpoint-case"
mkdir "$checkpoint_case"
(
  cd "$checkpoint_case"
  sed '1i assign(checkpoint_given,0).\
set(checkpoint_exit).\
set(checkpoint_verify).' "$repo_dir/test.src/eager_demod.in" | \
    "$repo_dir/bin/prover9" > before.out 2> before.err || true
)
checkpoint_dir=$(find "$checkpoint_case" -maxdepth 1 -type d \
  -name 'prover9_*_ckpt_0' -print)
test -n "$checkpoint_dir"
"$repo_dir/bin/prover9" -r "$checkpoint_dir" < /dev/null \
  > "$test_tmp/resumed.out" 2> "$test_tmp/resumed.err" || true
grep -Eq '^%   Verification: [0-9]+ passed, 0 failed\.$' \
  "$test_tmp/resumed.out"
grep -q 'THEOREM PROVED' "$test_tmp/resumed.out"
grep -E '^(Given=|Discount_demodulation:|New_demodulators=)' \
  "$test_tmp/prover.out" | tail -3 > "$test_tmp/control.stats"
grep -E '^(Given=|Discount_demodulation:|New_demodulators=)' \
  "$test_tmp/resumed.out" | tail -3 > "$test_tmp/resumed.stats"
diff -u "$test_tmp/control.stats" "$test_tmp/resumed.stats"

# Compact checkpoints rebuild the live bank in proof-ID order before dense
# archival transfers IDs to compressed proof shells.
compact_checkpoint_case="$test_tmp/compact-checkpoint-case"
mkdir "$compact_checkpoint_case"
(
  cd "$compact_checkpoint_case"
  sed 's/assign(discount_demodulation,eager_legacy)\./assign(discount_demodulation,eager_interreduced)./; 1i assign(checkpoint_given,0).\
set(checkpoint_exit).\
set(checkpoint_verify).' "$repo_dir/test.src/eager_demod.in" | \
    "$repo_dir/bin/prover9" > before.out 2> before.err || true
)
compact_checkpoint_dir=$(find "$compact_checkpoint_case" -maxdepth 1 -type d \
  -name 'prover9_*_ckpt_0' -print)
test -n "$compact_checkpoint_dir"
"$repo_dir/bin/prover9" -r "$compact_checkpoint_dir" < /dev/null \
  > "$test_tmp/compact-resumed.out" 2> "$test_tmp/compact-resumed.err" || true
grep -Eq '^%   Verification: [0-9]+ passed, 0 failed\.$' \
  "$test_tmp/compact-resumed.out"
grep -q 'THEOREM PROVED' "$test_tmp/compact-resumed.out"
grep -E '^(Given=|Discount_demodulation:|Compact_rewrite:|New_demodulators=)' \
  "$test_tmp/compact.out" | tail -4 > "$test_tmp/compact-control.stats"
grep -E '^(Given=|Discount_demodulation:|Compact_rewrite:|New_demodulators=)' \
  "$test_tmp/compact-resumed.out" | tail -4 > "$test_tmp/compact-resumed.stats"
diff -u "$test_tmp/compact-control.stats" "$test_tmp/compact-resumed.stats"

# The production target combines eager rewriting with deferred balanced
# collective descriptors.  Ensure rewrite-only clauses never leak into an
# inference lane while their later selected transitions remain valid.
sed '/assign(passive_store,dense)\./a\
assign(discount_demodulation,eager_legacy).\
set(back_demod_hints).' "$repo_dir/test.src/collective_balanced.in" | \
  "$repo_dir/bin/prover9" > "$test_tmp/balanced.out" \
  2> "$test_tmp/balanced.err"
grep -q 'THEOREM PROVED' "$test_tmp/balanced.out"
grep -Eq 'Discount_demodulation: policy=eager_legacy, .*rewrite_only_admitted=[1-9][0-9]*, .*selected=[1-9][0-9]*' \
  "$test_tmp/balanced.out"
grep -Eq 'Collective_frontier: batches_created=[1-9][0-9]*, completed=[1-9][0-9]*' \
  "$test_tmp/balanced.out"
"$repo_dir/bin/prooftrans" parents_only < "$test_tmp/balanced.out" \
  > "$test_tmp/balanced-proof.out"
grep -q 'end of proof' "$test_tmp/balanced-proof.out"

sed '/assign(passive_store,dense)\./a\
assign(discount_demodulation,eager_interreduced).\
set(back_demod_hints).' "$repo_dir/test.src/collective_balanced.in" | \
  "$repo_dir/bin/prover9" > "$test_tmp/compact-balanced.out" \
  2> "$test_tmp/compact-balanced.err"
grep -q 'THEOREM PROVED' "$test_tmp/compact-balanced.out"
grep -Eq 'Discount_demodulation: policy=eager_interreduced, .*rewrite_only_admitted=[1-9][0-9]*, .*selected=[1-9][0-9]*' \
  "$test_tmp/compact-balanced.out"
grep -Eq 'Collective_frontier: batches_created=[1-9][0-9]*, completed=[1-9][0-9]*' \
  "$test_tmp/compact-balanced.out"
"$repo_dir/bin/prooftrans" parents_only < "$test_tmp/compact-balanced.out" \
  > "$test_tmp/compact-balanced-proof.out"
grep -q 'end of proof' "$test_tmp/compact-balanced-proof.out"

# Fair background repair must expose a hint matcher that exists only after a
# later rewrite rule, without selecting the stale body first.
"$repo_dir/bin/prover9" < "$repo_dir/test.src/rewrite_refresh.in" \
  > "$test_tmp/refresh.out" 2> "$test_tmp/refresh.err" || true
grep -Eq 'Rewrite_refresh: .*materialized=[1-9][0-9]*, rewritten=[1-9][0-9]*' \
  "$test_tmp/refresh.out"
grep -Eq 'Hint match stats:|matched=[1-9][0-9]*' "$test_tmp/refresh.out"
grep -Eq 'matched=[1-9][0-9]*' "$test_tmp/refresh.out"

# An unaffected stale simplifier exercises the reversible extraction path:
# compact rule suspension, proof-ID transfer, and selector reactivation.
"$repo_dir/bin/prover9" < "$repo_dir/test.src/rewrite_refresh_unchanged.in" \
  > "$test_tmp/refresh-unchanged.out" 2> "$test_tmp/refresh-unchanged.err" || true
grep -Eq 'Rewrite_refresh: .*materialized=[1-9][0-9]*, rewritten=0, unchanged=[1-9][0-9]*' \
  "$test_tmp/refresh-unchanged.out"
grep -Eq 'Compact_rewrite: current=[1-9][0-9]*, .*retired=0' \
  "$test_tmp/refresh-unchanged.out"

# Sequential rule-first repair is the bounded interreduction pass.  Exercise
# both composition into a replacement rule and collapse to a tautology.
"$repo_dir/bin/prover9" < "$repo_dir/test.src/rewrite_interreduce.in" \
  > "$test_tmp/interreduce.out" 2> "$test_tmp/interreduce.err" || true
grep -Eq 'Discount_demodulation: policy=eager_interreduced, .*retired=[1-9][0-9]*' \
  "$test_tmp/interreduce.out"
grep -Eq 'Rewrite_interreduce: rule_turns=[1-9][0-9]*, rule_changed=[1-9][0-9]*' \
  "$test_tmp/interreduce.out"
grep -Eq 'Rewrite_interreduce: .*debt_peak=[1-9][0-9]*, .*drain_entries=[1-9][0-9]*, drain_exits=[1-9][0-9]*, drain_turns=[1-9][0-9]*' \
  "$test_tmp/interreduce.out"
grep -q 'THEOREM PROVED' "$test_tmp/interreduce.out"
"$repo_dir/bin/prooftrans" parents_only < "$test_tmp/interreduce.out" \
  > "$test_tmp/interreduce-proof.out"
grep -q 'end of proof' "$test_tmp/interreduce-proof.out"

# High-water drain is a priority boost, never an exclusive phase.  With two
# dirty rules and a one-turn burst, the theorem requires a forced scheduler
# yield before all rewrite debt has been repaired.
"$repo_dir/bin/prover9" < "$repo_dir/test.src/rewrite_drain_liveness.in" \
  > "$test_tmp/drain-liveness.out" 2> "$test_tmp/drain-liveness.err" || true
grep -Eq 'Rewrite_interreduce: .*debt_peak=2, .*drain_burst=1, .*drain_yields=[1-9][0-9]*' \
  "$test_tmp/drain-liveness.out"
grep -q 'THEOREM PROVED' "$test_tmp/drain-liveness.out"
"$repo_dir/bin/prooftrans" parents_only < "$test_tmp/drain-liveness.out" \
  > "$test_tmp/drain-liveness-proof.out"
grep -q 'end of proof' "$test_tmp/drain-liveness-proof.out"

# A composed replacement is installed and back-demodulates hints, but it
# cannot recursively launch another rule-only composition wave.  This is the
# bound that prevents Osborn's one-out/one-in dirty-debt cascade.
"$repo_dir/bin/prover9" < "$repo_dir/test.src/rewrite_cascade_bound.in" \
  > "$test_tmp/cascade-bound.out" 2> "$test_tmp/cascade-bound.err" || true
grep -Eq 'Rewrite_interreduce: rule_turns=1, rule_changed=1, .*overlap_visits=1, dirty_marks=1, cascade_suppressed=[1-9][0-9]*,' \
  "$test_tmp/cascade-bound.out"
grep -q 'THEOREM PROVED' "$test_tmp/cascade-bound.out"
"$repo_dir/bin/prooftrans" parents_only < "$test_tmp/cascade-bound.out" \
  > "$test_tmp/cascade-bound-proof.out"
grep -q 'end of proof' "$test_tmp/cascade-bound-proof.out"

# Save immediately before the bounded repair work.  Compact-bank counters,
# the logical next-record cursor IDs, and all semantic drain outcomes must
# survive dense-store reconstruction.  The physical scanned-record count is
# deliberately excluded: checkpoint reconstruction discards tombstones.
interreduce_checkpoint_case="$test_tmp/interreduce-checkpoint-case"
mkdir "$interreduce_checkpoint_case"
(
  cd "$interreduce_checkpoint_case"
  sed '1i assign(checkpoint_given,1).\
set(checkpoint_exit).\
set(checkpoint_verify).' "$repo_dir/test.src/rewrite_interreduce.in" | \
    "$repo_dir/bin/prover9" > before.out 2> before.err || true
)
interreduce_checkpoint_dir=$(find "$interreduce_checkpoint_case" \
  -maxdepth 1 -type d -name 'prover9_*_ckpt_1' -print)
test -n "$interreduce_checkpoint_dir"
grep -Eq '^rewrite_interreduce_cursor_id [1-9][0-9]*$' \
  "$interreduce_checkpoint_dir/metadata.txt"
grep -q 'rewrite_rule_dirty' "$interreduce_checkpoint_dir/clause_data.txt"
"$repo_dir/bin/prover9" -r "$interreduce_checkpoint_dir" < /dev/null \
  > "$test_tmp/interreduce-resumed.out" \
  2> "$test_tmp/interreduce-resumed.err" || true
grep -Eq '^%   Verification: [0-9]+ passed, 0 failed\.$' \
  "$test_tmp/interreduce-resumed.out"
grep -q 'THEOREM PROVED' "$test_tmp/interreduce-resumed.out"
grep -E '^(Given=|Discount_demodulation:|Compact_rewrite:|Rewrite_interreduce:|New_demodulators=)' \
  "$test_tmp/interreduce.out" | tail -5 \
  > "$test_tmp/interreduce-control.stats"
grep -E '^(Given=|Discount_demodulation:|Compact_rewrite:|Rewrite_interreduce:|New_demodulators=)' \
  "$test_tmp/interreduce-resumed.out" | tail -5 \
  > "$test_tmp/interreduce-resumed.stats"
diff -u "$test_tmp/interreduce-control.stats" \
  "$test_tmp/interreduce-resumed.stats"

"$repo_dir/bin/prover9" < "$repo_dir/test.src/rewrite_collapse.in" \
  > "$test_tmp/collapse.out" 2> "$test_tmp/collapse.err" || true
grep -Eq 'Compact_rewrite: current=[1-9][0-9]*, .*retired=[1-9][0-9]*' \
  "$test_tmp/collapse.out"
grep -Eq 'Rewrite_interreduce: rule_turns=[1-9][0-9]*, .*rule_collapsed=[1-9][0-9]*' \
  "$test_tmp/collapse.out"

# Eager operation is intentionally tied to dense ownership.  Reject a label
# that cannot provide the independent clone/archive lifecycle.
sed 's/assign(passive_store,dense)\./assign(passive_store,compressed)./' \
  "$repo_dir/test.src/eager_demod.in" | \
  "$repo_dir/bin/prover9" > "$test_tmp/invalid.out" \
  2> "$test_tmp/invalid.err" || true
grep -q 'discount_demodulation=eager_legacy requires passive_store=dense' \
  "$test_tmp/invalid.err"

sed 's/assign(discount_demodulation,eager_legacy)\./assign(discount_demodulation,eager_interreduced)./; s/assign(passive_store,dense)\./assign(passive_store,compressed)./' \
  "$repo_dir/test.src/eager_demod.in" | \
  "$repo_dir/bin/prover9" > "$test_tmp/compact-invalid.out" \
  2> "$test_tmp/compact-invalid.err" || true
grep -q 'discount_demodulation=eager_interreduced requires passive_store=dense' \
  "$test_tmp/compact-invalid.err"

# Hysteresis must have a nonempty band, otherwise drain mode can chatter at a
# single debt value.
sed '/assign(rewrite_refresh_low_water,0)\./c\
assign(rewrite_refresh_low_water,1).' \
  "$repo_dir/test.src/rewrite_interreduce.in" | \
  "$repo_dir/bin/prover9" > "$test_tmp/watermark-invalid.out" \
  2> "$test_tmp/watermark-invalid.err" || true
grep -q 'rewrite_refresh_low_water must be below high_water' \
  "$test_tmp/watermark-invalid.err"

echo 'eager_demod_test: PASS'
