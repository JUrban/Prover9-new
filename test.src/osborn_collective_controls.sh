#!/bin/sh
set -eu

repo_dir=$(CDPATH= cd -- "$(dirname -- "$0")/.." && pwd)
input=${1:-"$repo_dir/../Osborn-instrumentation-2026-08-07/rr_osbe-hints10pct.in"}
output_dir=${2:-"$repo_dir/osborn-collective-controls"}
max_given=${3:-100}
max_seconds=${4:-120}
max_megs=${5:-2048}
prover="$repo_dir/bin/prover9"

if test ! -f "$input"; then
  echo "input not found: $input" >&2
  exit 2
fi
if test ! -x "$prover"; then
  echo "prover not built: $prover" >&2
  exit 2
fi

mkdir -p "$output_dir"
sha256sum "$input" "$prover" > "$output_dir/hashes.txt"
{
  echo "max_given=$max_given"
  echo "max_seconds=$max_seconds"
  echo "max_megs=$max_megs"
} > "$output_dir/limits.txt"

filtered_input="$output_dir/base-filtered.in"
awk '
  /^assign\(max_(given|seconds|megs),/ { next }
  /^(set|clear)\(print_(gen|kept|given|initial_clauses)\)\./ { next }
  /^assign\((search_loop|passive_store|hint_index|inference_frontier|ancestor_store|collective_[a-z_]+),/ { next }
  /^(set|clear)\(collective_[a-z_]+\)\./ { next }
  { print }
' "$input" > "$filtered_input"

make_case()
{
  name=$1
  policy=$2
  case_input="$output_dir/$name.in"
  {
    echo 'clear(print_gen).'
    echo 'clear(print_kept).'
    echo 'clear(print_given).'
    echo 'clear(print_initial_clauses).'
    echo 'set(clocks).'
    echo 'assign(stats,all).'
    echo 'assign(report,60).'
    echo "assign(max_given,$max_given)."
    echo "assign(max_seconds,$max_seconds)."
    echo "assign(max_megs,$max_megs)."
    printf '%s\n' "$policy"
    cat "$filtered_input"
  } > "$case_input"
}

make_case otter_fpa '
assign(search_loop,otter).
assign(passive_store,full).
assign(hint_index,fpa).
assign(inference_frontier,clauses).
assign(ancestor_store,off).'

make_case discount_clauses '
assign(search_loop,discount).
assign(passive_store,dense).
assign(hint_index,compact).
assign(inference_frontier,clauses).
assign(ancestor_store,mmap).'

make_case collective_conservative '
assign(search_loop,discount).
assign(passive_store,dense).
assign(hint_index,compact).
assign(inference_frontier,collective).
assign(ancestor_store,mmap).
assign(collective_given_ratio,4).
clear(collective_hint_probes).
clear(collective_promising_candidates).
clear(collective_promising_scheduler).'

make_case collective_aids '
assign(search_loop,discount).
assign(passive_store,dense).
assign(hint_index,compact).
assign(inference_frontier,collective).
assign(ancestor_store,mmap).
assign(collective_given_ratio,1).
set(collective_hint_probes).
set(collective_promising_candidates).
set(collective_promising_scheduler).'

for name in otter_fpa discount_clauses collective_conservative collective_aids
do
  status=0
  if /usr/bin/time -v -o "$output_dir/$name.time" \
       "$prover" < "$output_dir/$name.in" \
       > "$output_dir/$name.out" 2> "$output_dir/$name.err"; then
    status=0
  else
    status=$?
  fi
  echo "$status" > "$output_dir/$name.status"
done

echo "bounded Osborn controls written to $output_dir"
