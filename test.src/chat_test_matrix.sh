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
new_prover=${CHAT_NEW_PROVER:-"$repo_dir/bin/prover9"}
old_prover=${CHAT_OLD_PROVER:-/project/Prover9-old-LADR-2026-6A/bin/prover9}
all_cases='old_otter new_otter_fpa new_otter_packed discount_clauses_selected discount_clauses_eager collective_balanced_selected collective_balanced_legacy collective_balanced_eager'
selected_cases=${CHAT_CASES:-$all_cases}

if test ! -f "$input"; then
  echo "input not found: $input" >&2
  exit 2
fi
if test ! -x "$new_prover"; then
  echo "new prover not found: $new_prover" >&2
  exit 2
fi
if test ! -x "$old_prover"; then
  echo "old prover not found: $old_prover" >&2
  exit 2
fi

mkdir -p "$output_dir/tmp"
sha256sum "$input" "$new_prover" "$old_prover" > "$output_dir/hashes.txt"
{
  echo "max_given=$max_given"
  echo "max_seconds=$max_seconds"
  echo "max_megs=$max_megs"
  echo "wall_seconds=$wall_seconds"
  echo "report_seconds=$report_seconds"
  echo "cases=$selected_cases"
  echo "new_prover=$new_prover"
  echo "old_prover=$old_prover"
} > "$output_dir/limits.txt"

# Preserve every logical input item and hint.  Remove only experiment-level
# controls so each generated case has one authoritative setting for limits,
# reporting, storage, search loop, inference frontier, and rewrite policy.
base_input="$output_dir/base-filtered.in"
awk '
  /^assign\((max_given|max_seconds|max_minutes|max_hours|max_days|max_megs|report|stats),/ { next }
  /^assign\((search_loop|passive_store|discount_demodulation|hint_index|inference_frontier|collective_scheduler|ancestor_store),/ { next }
  /^assign\((collective_[a-z_]+|rewrite_refresh_[a-z_]+),/ { next }
  /^(set|clear)\(collective_[a-z_]+\)\./ { next }
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
    echo 'clear(print_kept).'
    echo 'clear(print_given).'
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
  if TMPDIR="$output_dir/tmp" /usr/bin/time -v -o "$output_dir/$name.time" \
       timeout --signal=TERM --kill-after=10 "$wall_seconds" \
       taskset -c 0 "$prover" < "$output_dir/$name.in" \
       > "$output_dir/$name.out" 2> "$output_dir/$name.err"; then
    status=0
  else
    status=$?
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
printf 'case\tstatus\tproved\tgiven\tuser_cpu\twall\tmax_rss_kb\n' > "$summary"
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
  printf '%s\t%s\t%s\t%s\t%s\t%s\t%s\n' \
    "$name" "$status" "$proved" "${given:-NA}" "${user_cpu:-NA}" \
    "${wall:-NA}" "${max_rss:-NA}" >> "$summary"
done

echo "chat_test matrix written to $output_dir"
cat "$summary"
