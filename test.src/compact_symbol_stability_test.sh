#!/bin/sh
set -eu

repo_dir=$(CDPATH= cd -- "$(dirname -- "$0")/.." && pwd)
prover=${P9_TEST_PROVER:-$repo_dir/bin/prover9}
test_tmp=$(mktemp -d "${TMPDIR:-/tmp}/p9-symbol-stability.XXXXXX")
trap 'rm -rf -- "$test_tmp"' EXIT HUP INT TERM

make_input()
{
  selector_store=$1
  sed "1i\\
clear(auto).\\
clear(auto_setup).\\
clear(auto_inference).\\
clear(unit_deletion).\\
set(paramodulation).\\
assign(search_loop,otter).\\
assign(inference_frontier,clauses).\\
assign(passive_store,dense).\\
assign(passive_directory,file).\\
assign(passive_selector_store,$selector_store).\\
assign(ancestor_store,file).\\
assign(sos_limit,-1).\\
set(compact_otter_demodulation).\\
set(compact_otter_unit_index).\\
set(compact_otter_back_demod_index).\\
set(compact_otter_nonunit_index).\\
assign(compact_back_demod_strategy,adaptive).\\
assign(max_given,40).\\
assign(max_seconds,30).\\
assign(stats,all)." "$repo_dir/prover9.examples/x2.in"
}

for mode in heap file; do
  make_input "$mode" | "$prover" > "$test_tmp/$mode.out" \
    2> "$test_tmp/$mode.err" || true
  grep -q '^Compact_back_demod:' "$test_tmp/$mode.out"
  sed -n 's/^Compact_back_demod:.*queries=\([0-9][0-9]*\), candidates=\([0-9][0-9]*\), exact_tests=[0-9][0-9]*, posting_groups=\([0-9][0-9]*\), path_buckets=\([0-9][0-9]*\),.*tree_root_admissions=\([0-9][0-9]*\),.*position_admissions=\([0-9][0-9]*\),.*groups_examined=\([0-9][0-9]*\), occurrences_examined=\([0-9][0-9]*\), path_checks=\([0-9][0-9]*\), path_rejects=\([0-9][0-9]*\), query_input=\([0-9a-f][0-9a-f]*\), query_output=\([0-9a-f][0-9a-f]*\),.*/queries=\1 candidates=\2 posting_groups=\3 path_buckets=\4 tree_admissions=\5 position_admissions=\6 groups=\7 occurrences=\8 path_checks=\9 path_rejects=\10 input=\11 output=\12/p' \
    "$test_tmp/$mode.out" > "$test_tmp/$mode.normalized"
  test -s "$test_tmp/$mode.normalized"
done

if ! cmp "$test_tmp/heap.normalized" "$test_tmp/file.normalized"; then
  diff -u "$test_tmp/heap.normalized" "$test_tmp/file.normalized" >&2 || true
  exit 1
fi

echo 'compact_symbol_stability_test: PASS'
