#!/bin/sh
set -eu

repo_dir=$(CDPATH= cd -- "$(dirname -- "$0")/.." && pwd)
test_tmp=$(mktemp -d "${TMPDIR:-/tmp}/p9-hint-checkpoint-test.XXXXXX")
trap 'rm -rf -- "$test_tmp"' EXIT HUP INT TERM

make_input()
{
  checkpoint_given=$1
  sed "/assign(hint_index,compact)./a assign(hint_index,hybrid).\
assign(max_proofs,-1).\
assign(max_given,12).\
assign(checkpoint_given,$checkpoint_given).\
set(checkpoint_exit).\
set(checkpoint_verify).\
set(hint_trace).\
list(given_selection).\
part(A,low,age,all) = 1.\
end_of_list." "$repo_dir/test.src/discount_loop.in"
}

make_input -1 | "$repo_dir/bin/prover9" \
  > "$test_tmp/control.out" 2> "$test_tmp/control.err" || true
grep '^HINT_TRACE ' "$test_tmp/control.out" > "$test_tmp/control.trace"
grep -q '^Given=13\. Generated=142\. Kept=63\. proofs=0\.$' \
  "$test_tmp/control.out"

for checkpoint_given in 0 2 6; do
  case_dir="$test_tmp/case-$checkpoint_given"
  mkdir "$case_dir"
  (
    cd "$case_dir"
    make_input "$checkpoint_given" | "$repo_dir/bin/prover9" \
      > before.out 2> before.err || true
  )
  checkpoint_dir=$(find "$case_dir" -maxdepth 1 -type d \
    -name "prover9_*_ckpt_$checkpoint_given" -print)
  test -n "$checkpoint_dir"
  test -f "$checkpoint_dir/verify.txt"
  "$repo_dir/bin/prover9" -r "$checkpoint_dir" < /dev/null \
    > "$case_dir/resumed.out" 2> "$case_dir/resumed.err" || true
  {
    grep '^HINT_TRACE ' "$case_dir/before.out" || true
    grep '^HINT_TRACE ' "$case_dir/resumed.out" || true
  } > "$case_dir/combined.trace"
  diff -u "$test_tmp/control.trace" "$case_dir/combined.trace"
  grep -Eq '^%   Verification: [0-9]+ passed, 0 failed\.$' \
    "$case_dir/resumed.out"
  grep -q '^Given=13\. Generated=142\. Kept=63\. proofs=0\.$' \
    "$case_dir/resumed.out"
  grep -q '^Better_packed_postings:' "$case_dir/resumed.out"
done

echo 'hint_checkpoint_test: PASS'
