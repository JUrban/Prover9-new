#!/bin/sh
set -eu

repo_dir=$(CDPATH= cd -- "$(dirname -- "$0")/.." && pwd)
input=${1:-/project/bob/chat_test.in}
output_dir=${2:-"$repo_dir/chat-test-results"}
max_given=${3:-300}
max_seconds=${4:-120}
max_megs=${5:-2048}
wall_seconds=${6:-$((max_seconds + 60))}
report_seconds=${CHAT_REPORT_SECONDS:-30}
cpu=${CHAT_CPU:-0}
trace=${CHAT_TRACE:-0}
compact_term_reclaim_kb=${CHAT_COMPACT_TERM_RECLAIM_KB:-8192}
compact_index_stale_pct=${CHAT_COMPACT_INDEX_STALE_PCT:-25}
compact_back_position_build_factor=${CHAT_COMPACT_BACK_POSITION_BUILD_FACTOR:-32}
compact_back_position_budget_kb=${CHAT_COMPACT_BACK_POSITION_BUDGET_KB:-16384}
compact_back_position_budget_pct=${CHAT_COMPACT_BACK_POSITION_BUDGET_PCT:-50}
compact_back_eager_position_depth=${CHAT_COMPACT_BACK_EAGER_POSITION_DEPTH:-3}
compact_back_tree_budget_kb=${CHAT_COMPACT_BACK_TREE_BUDGET_KB:-65536}
compact_back_tree_budget_pct=${CHAT_COMPACT_BACK_TREE_BUDGET_PCT:-200}
passive_selector_buffer=${CHAT_PASSIVE_SELECTOR_BUFFER:-65536}
new_prover=${CHAT_NEW_PROVER:-"$repo_dir/bin/prover9"}
old_prover=${CHAT_OLD_PROVER:-/project/Prover9-old-LADR-2026-6A/bin/prover9}
all_cases='old_otter new_otter_fpa new_otter_packed new_otter_packed_fast new_otter_compact_full new_otter_compact_packed_fast new_otter_compact_file_mask new_otter_compact_file_signature new_otter_compact_file_heap new_otter_compact_file_runs new_otter_compact_file_linear discount_clauses_selected discount_clauses_eager collective_balanced_selected collective_balanced_legacy collective_balanced_eager'
selected_cases=${CHAT_CASES:-$all_cases}
reference_output=${CHAT_REFERENCE_OUTPUT:-}
compare_max_cpu_ratio=${CHAT_COMPARE_MAX_CPU_RATIO:-1.25}
compare_min_ram_saving_pct=${CHAT_COMPARE_MIN_RAM_SAVING_PCT:-80}
compare_max_back_slope_ratio=${CHAT_COMPARE_MAX_BACK_SLOPE_RATIO:-1.25}
cgroup_accounting=${CHAT_CGROUP_ACCOUNTING:-0}

if test ! -f "$input"; then
  echo "input not found: $input" >&2
  exit 2
fi
if test ! -x "$new_prover"; then
  echo "new prover not found: $new_prover" >&2
  exit 2
fi
case " $selected_cases " in
  *" old_otter "*)
    if test ! -x "$old_prover"; then
      echo "old prover not found: $old_prover" >&2
      exit 2
    fi
    need_old_prover=yes
    ;;
  *) need_old_prover=no ;;
esac
if test -n "$reference_output" && test ! -f "$reference_output"; then
  echo "reference output not found: $reference_output" >&2
  exit 2
fi

mkdir -p "$output_dir/tmp"
case "$cgroup_accounting" in
  0) ;;
  1)
    if ! "$repo_dir/test.src/cgroup_job_memory.sh" \
         "$output_dir/tmp/cgroup-probe.cgroup" /bin/true; then
      echo "CHAT_CGROUP_ACCOUNTING requested but unavailable" >&2
      exit 77
    fi
    rm -f "$output_dir/tmp/cgroup-probe.cgroup"
    ;;
  *) echo "CHAT_CGROUP_ACCOUNTING must be 0 or 1" >&2; exit 2 ;;
esac
{
  sha256sum "$input" "$new_prover"
  if test "$need_old_prover" = yes; then
    sha256sum "$old_prover"
  fi
  if test -n "$reference_output"; then
    sha256sum "$reference_output"
  fi
} > "$output_dir/hashes.txt"
{
  echo "max_given=$max_given"
  echo "max_seconds=$max_seconds"
  echo "max_megs=$max_megs"
  echo "wall_seconds=$wall_seconds"
  echo "report_seconds=$report_seconds"
  echo "cpu=$cpu"
  echo "trace=$trace"
  echo "compact_term_reclaim_kb=$compact_term_reclaim_kb"
  echo "compact_index_stale_pct=$compact_index_stale_pct"
  echo "compact_back_position_build_factor=$compact_back_position_build_factor"
  echo "compact_back_position_budget_kb=$compact_back_position_budget_kb"
  echo "compact_back_position_budget_pct=$compact_back_position_budget_pct"
  echo "compact_back_eager_position_depth=$compact_back_eager_position_depth"
  echo "compact_back_tree_budget_kb=$compact_back_tree_budget_kb"
  echo "compact_back_tree_budget_pct=$compact_back_tree_budget_pct"
  echo "passive_selector_buffer=$passive_selector_buffer"
  echo "cases=$selected_cases"
  echo "reference_output=${reference_output:-none}"
  echo "compare_max_cpu_ratio=$compare_max_cpu_ratio"
  echo "compare_min_ram_saving_pct=$compare_min_ram_saving_pct"
  echo "compare_max_back_slope_ratio=$compare_max_back_slope_ratio"
  echo "cgroup_accounting=$cgroup_accounting"
  echo "new_prover=$new_prover"
  echo "old_prover=$old_prover"
} > "$output_dir/limits.txt"

# Preserve every logical input item and hint.  Remove only experiment-level
# controls so each generated case has one authoritative setting for limits,
# reporting, storage, search loop, inference frontier, and rewrite policy.
base_input="$output_dir/base-filtered.in"
awk '
  /^assign\((max_given|max_seconds|max_minutes|max_hours|max_days|max_megs|report|stats),/ { next }
  /^assign\((search_loop|passive_store|passive_directory|passive_selector_store|discount_demodulation|hint_index|inference_frontier|collective_scheduler|ancestor_store),/ { next }
  /^assign\(passive_selector_buffer,/ { next }
  /^assign\(compact_term_reclaim_kb,/ { next }
  /^assign\(compact_index_stale_pct,/ { next }
  /^assign\(compact_rewrite_deep_cache_kb,/ { next }
  /^assign\(compact_(unit_strategy|back_demod_strategy),/ { next }
  /^assign\(compact_back_(position_(build_factor|budget_kb|budget_pct)|eager_position_depth),/ { next }
  /^assign\(compact_back_tree_(budget_kb|budget_pct),/ { next }
  /^assign\((collective_[a-z_]+|rewrite_refresh_[a-z_]+),/ { next }
  /^(set|clear)\(collective_[a-z_]+\)\./ { next }
  /^(set|clear)\(compact_otter_[a-z_]+\)\./ { next }
  /^(set|clear)\(compact_back_sparse_positions\)\./ { next }
  /^(set|clear)\(compact_nonunit_path_filter\)\./ { next }
  /^(set|clear)\(print_(gen|kept|given|initial_clauses)\)\./ { next }
  { print }
' "$input" > "$base_input"

write_case()
{
  name=$1
  policy=$2
  case " $selected_cases " in
    *" $name "*) ;;
    *) return ;;
  esac
  case_input="$output_dir/$name.in"
  {
    echo 'clear(print_gen).'
    if test "$trace" = 1; then
      echo 'set(print_kept).'
      echo 'set(print_given).'
      echo 'set(hint_trace).'
    else
      echo 'clear(print_kept).'
      echo 'clear(print_given).'
    fi
    echo 'clear(print_initial_clauses).'
    echo 'set(clocks).'
    echo 'set(hint_match_stats).'
    echo 'set(back_demod_hints).'
    echo 'assign(stats,all).'
    echo "assign(report,$report_seconds)."
    echo "assign(max_given,$max_given)."
    echo "assign(max_seconds,$max_seconds)."
    echo "assign(max_megs,$max_megs)."
    printf '%s\n' "$policy"
    case "$name" in
      new_otter_compact_file_linear)
        echo "assign(compact_term_reclaim_kb,$compact_term_reclaim_kb)."
        echo "assign(compact_index_stale_pct,$compact_index_stale_pct)."
        echo "assign(compact_back_position_build_factor,$compact_back_position_build_factor)."
        echo 'assign(compact_back_position_budget_kb,0).'
        echo "assign(compact_back_position_budget_pct,$compact_back_position_budget_pct)."
        echo "assign(compact_back_eager_position_depth,$compact_back_eager_position_depth)."
        echo "assign(compact_back_tree_budget_kb,$compact_back_tree_budget_kb)."
        echo "assign(compact_back_tree_budget_pct,$compact_back_tree_budget_pct)."
        ;;
      new_otter_compact_*)
        echo "assign(compact_term_reclaim_kb,$compact_term_reclaim_kb)."
        echo "assign(compact_index_stale_pct,$compact_index_stale_pct)."
        echo "assign(compact_back_position_build_factor,$compact_back_position_build_factor)."
        echo "assign(compact_back_position_budget_kb,$compact_back_position_budget_kb)."
        ;;
    esac
    cat "$base_input"
  } > "$case_input"
}

write_case old_otter ''

write_case new_otter_fpa '
assign(search_loop,otter).
assign(passive_store,full).
assign(hint_index,fpa).
assign(inference_frontier,clauses).
assign(ancestor_store,off).'

write_case new_otter_packed '
assign(search_loop,otter).
assign(passive_store,full).
assign(hint_index,packed).
assign(inference_frontier,clauses).
assign(ancestor_store,off).'

write_case new_otter_packed_fast '
assign(search_loop,otter).
assign(passive_store,full).
assign(hint_index,packed_fast).
assign(inference_frontier,clauses).
assign(ancestor_store,off).'

write_case new_otter_compact_full '
assign(search_loop,otter).
assign(passive_store,full).
assign(hint_index,packed_fast).
assign(inference_frontier,clauses).
assign(ancestor_store,off).
set(compact_otter_demodulation).
set(compact_otter_unit_index).
set(compact_otter_back_demod_index).
set(compact_otter_nonunit_index).'

write_case new_otter_compact_packed_fast '
assign(search_loop,otter).
assign(passive_store,dense).
assign(hint_index,packed_fast).
assign(inference_frontier,clauses).
assign(ancestor_store,mmap).
set(compact_otter_demodulation).
set(compact_otter_unit_index).
set(compact_otter_back_demod_index).
set(compact_otter_nonunit_index).'

write_case new_otter_compact_file_mask '
assign(search_loop,otter).
assign(passive_store,dense).
assign(passive_directory,file).
assign(passive_selector_store,heap).
assign(hint_index,packed_fast).
assign(inference_frontier,clauses).
assign(ancestor_store,file).
set(compact_otter_demodulation).
set(compact_otter_unit_index).
set(compact_otter_back_demod_index).
set(compact_otter_nonunit_index).
assign(compact_back_demod_strategy,mask8).'

write_case new_otter_compact_file_signature '
assign(search_loop,otter).
assign(passive_store,dense).
assign(passive_directory,file).
assign(passive_selector_store,heap).
assign(hint_index,packed_fast).
assign(inference_frontier,clauses).
assign(ancestor_store,file).
set(compact_otter_demodulation).
set(compact_otter_unit_index).
set(compact_otter_back_demod_index).
set(compact_otter_nonunit_index).
assign(compact_back_demod_strategy,signature32).'

write_case new_otter_compact_file_heap '
assign(search_loop,otter).
assign(passive_store,dense).
assign(passive_directory,file).
assign(passive_selector_store,heap).
assign(hint_index,packed_fast).
assign(inference_frontier,clauses).
assign(ancestor_store,file).
set(compact_otter_demodulation).
set(compact_otter_unit_index).
set(compact_otter_back_demod_index).
set(compact_otter_nonunit_index).
assign(compact_back_demod_strategy,adaptive).'

write_case new_otter_compact_file_runs '
assign(search_loop,otter).
assign(passive_store,dense).
assign(passive_directory,file).
assign(passive_selector_store,file).
assign(passive_selector_buffer,'"$passive_selector_buffer"').
assign(hint_index,packed_fast).
assign(inference_frontier,clauses).
assign(ancestor_store,file).
set(compact_otter_demodulation).
set(compact_otter_unit_index).
set(compact_otter_back_demod_index).
set(compact_otter_nonunit_index).
assign(compact_back_demod_strategy,adaptive).'

write_case new_otter_compact_file_linear '
assign(search_loop,otter).
assign(passive_store,dense).
assign(passive_directory,file).
assign(passive_selector_store,file).
assign(passive_selector_buffer,65536).
assign(hint_index,packed_fast).
assign(inference_frontier,clauses).
assign(ancestor_store,file).
set(compact_otter_demodulation).
set(compact_otter_unit_index).
set(compact_otter_back_demod_index).
set(compact_otter_nonunit_index).
assign(compact_unit_strategy,code_tree).
set(compact_nonunit_path_filter).
assign(compact_back_demod_strategy,adaptive).
set(compact_back_sparse_positions).'

write_case discount_clauses_selected '
assign(search_loop,discount).
assign(passive_store,dense).
assign(discount_demodulation,selected).
assign(hint_index,packed).
assign(inference_frontier,clauses).
assign(ancestor_store,mmap).'

write_case discount_clauses_eager '
assign(search_loop,discount).
assign(passive_store,dense).
assign(discount_demodulation,eager_interreduced).
assign(hint_index,packed).
assign(inference_frontier,clauses).
assign(ancestor_store,mmap).
assign(rewrite_refresh_drain_burst,64).'

write_case collective_balanced_selected '
assign(search_loop,discount).
assign(passive_store,dense).
assign(discount_demodulation,selected).
assign(hint_index,packed).
assign(inference_frontier,collective).
assign(collective_scheduler,balanced_hint).
set(collective_hint_discovery).
clear(collective_hint_probes).
clear(collective_promising_candidates).
clear(collective_promising_scheduler).
assign(ancestor_store,mmap).'

write_case collective_balanced_legacy '
assign(search_loop,discount).
assign(passive_store,dense).
assign(discount_demodulation,eager_legacy).
assign(hint_index,packed).
assign(inference_frontier,collective).
assign(collective_scheduler,balanced_hint).
set(collective_hint_discovery).
clear(collective_hint_probes).
clear(collective_promising_candidates).
clear(collective_promising_scheduler).
assign(ancestor_store,mmap).'

write_case collective_balanced_eager '
assign(search_loop,discount).
assign(passive_store,dense).
assign(discount_demodulation,eager_interreduced).
assign(hint_index,packed).
assign(inference_frontier,collective).
assign(collective_scheduler,balanced_hint).
set(collective_hint_discovery).
clear(collective_hint_probes).
clear(collective_promising_candidates).
clear(collective_promising_scheduler).
assign(ancestor_store,mmap).
assign(rewrite_refresh_drain_burst,64).'

run_case()
{
  name=$1
  case "$name" in
    old_otter) prover=$old_prover ;;
    *) prover=$new_prover ;;
  esac
  case " $selected_cases " in
    *" $name "*) ;;
    *) return ;;
  esac
  status=0
  if test "$cgroup_accounting" = 1; then
    if TMPDIR="$output_dir/tmp" "$repo_dir/test.src/cgroup_job_memory.sh" \
         "$output_dir/$name.cgroup" \
         /usr/bin/time -v -o "$output_dir/$name.time" \
         timeout --signal=TERM --kill-after=10 "$wall_seconds" \
         taskset -c "$cpu" "$prover" < "$output_dir/$name.in" \
         > "$output_dir/$name.out" 2> "$output_dir/$name.err"; then
      status=0
    else
      status=$?
    fi
  else
    if TMPDIR="$output_dir/tmp" /usr/bin/time -v -o "$output_dir/$name.time" \
         timeout --signal=TERM --kill-after=10 "$wall_seconds" \
         taskset -c "$cpu" "$prover" < "$output_dir/$name.in" \
         > "$output_dir/$name.out" 2> "$output_dir/$name.err"; then
      status=0
    else
      status=$?
    fi
  fi
  echo "$status" > "$output_dir/$name.status"
}

for name in $selected_cases
do
  case " $all_cases " in
    *" $name "*) run_case "$name" ;;
    *)
      echo "unknown chat_test case: $name" >&2
      exit 2
      ;;
  esac
done

summary="$output_dir/summary.tsv"
printf 'case\tstatus\tproved\tgiven\tuser_cpu\twall\tmax_rss_kb\tcgroup_peak_kb\tcgroup_file_end_kb\n' > "$summary"
for name in $selected_cases
do
  out="$output_dir/$name.out"
  time_file="$output_dir/$name.time"
  status=$(cat "$output_dir/$name.status")
  if grep -q 'THEOREM PROVED' "$out"; then proved=yes; else proved=no; fi
  given=$(sed -n 's/^Given=\([0-9][0-9]*\).*/\1/p' "$out" | tail -1)
  user_cpu=$(sed -n 's/^[[:space:]]*User time (seconds):[[:space:]]*//p' "$time_file")
  wall=$(sed -n 's/^[[:space:]]*Elapsed (wall clock) time (h:mm:ss or m:ss):[[:space:]]*//p' "$time_file")
  max_rss=$(sed -n 's/^[[:space:]]*Maximum resident set size (kbytes):[[:space:]]*//p' "$time_file")
  cgroup_file="$output_dir/$name.cgroup"
  if test -f "$cgroup_file"; then
    cgroup_peak=$(awk -F= '$1 == "memory_peak_bytes" {printf "%.0f", $2 / 1024}' "$cgroup_file")
    cgroup_file_end=$(awk -F= '$1 == "file_current_bytes" {printf "%.0f", $2 / 1024}' "$cgroup_file")
  else
    cgroup_peak=NA
    cgroup_file_end=NA
  fi
  printf '%s\t%s\t%s\t%s\t%s\t%s\t%s\t%s\t%s\n' \
    "$name" "$status" "$proved" "${given:-NA}" "${user_cpu:-NA}" \
    "${wall:-NA}" "${max_rss:-NA}" "${cgroup_peak:-NA}" \
    "${cgroup_file_end:-NA}" >> "$summary"
done

set --
if test -n "$reference_output"; then
  set -- "$@" "$reference_output"
fi
for name in $selected_cases
do
  set -- "$@" "$output_dir/$name.out"
done
python3 "$repo_dir/test.src/compact_long_run_report.py" --format tsv "$@" \
  > "$output_dir/long-run-slopes.tsv"
if test "$#" -gt 1; then
  python3 "$repo_dir/test.src/compact_long_run_report.py" --summary-only \
    --compare-to-first --max-cpu-ratio "$compare_max_cpu_ratio" \
    --min-ram-saving-pct "$compare_min_ram_saving_pct" \
    --max-back-slope-ratio "$compare_max_back_slope_ratio" "$@" \
    > "$output_dir/long-run-summary.md"
else
  python3 "$repo_dir/test.src/compact_long_run_report.py" --summary-only "$@" \
    > "$output_dir/long-run-summary.md"
fi

echo "chat_test matrix written to $output_dir"
cat "$summary"
