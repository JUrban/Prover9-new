#!/bin/sh
set -eu

repo_dir=$(CDPATH= cd -- "$(dirname -- "$0")/.." && pwd)
manifest=${P9_MATRIX_MANIFEST:-$repo_dir/benchmarks/compact-generalization/manifest.tsv}
new_prover=${P9_MATRIX_PROVER:-$repo_dir/bin/prover9}
old_prover=${P9_MATRIX_OLD_PROVER:-/project/Prover9-old-LADR-2026-6A/bin/prover9}
case_ids=${P9_MATRIX_CASES:-osborn-chat}
variants=${P9_MATRIX_VARIANTS:-legacy compact_full compact_dense_file}
max_given=${P9_MATRIX_MAX_GIVEN:-100}
max_seconds=${P9_MATRIX_MAX_SECONDS:-120}
max_megs=${P9_MATRIX_MAX_MEGS:-2048}
wall_seconds=${P9_MATRIX_WALL_SECONDS:-$((max_seconds + 30))}
report_seconds=${P9_MATRIX_REPORT_SECONDS:-30}
back_tree_min_tokens=${P9_MATRIX_BACK_TREE_MIN_TOKENS:-8}
back_tree_budget_kb=${P9_MATRIX_BACK_TREE_BUDGET_KB:-65536}
detected_cpu=$(taskset -pc $$ 2>/dev/null | sed 's/^.*: //;s/,.*//;s/-.*//' || true)
cpu=${P9_MATRIX_CPU:-${detected_cpu:-0}}
allow_holdout=${P9_MATRIX_ALLOW_HOLDOUT:-0}
output_dir=${1:-}

if test -z "$output_dir"; then
  echo "usage: $0 OUTPUT_DIR" >&2
  echo "configure cases/variants with P9_MATRIX_CASES and P9_MATRIX_VARIANTS" >&2
  exit 2
fi
if test ! -x "$new_prover"; then
  echo "new prover not executable: $new_prover" >&2
  exit 2
fi

"$repo_dir/test.src/validate_compact_generalization_manifest.sh" "$manifest"
mkdir -p "$output_dir/inputs" "$output_dir/tmp"

case " $variants " in
  *" old_p9 "*)
    if test ! -x "$old_prover"; then
      echo "old prover not executable: $old_prover" >&2
      exit 2
    fi
    ;;
esac

case_row()
{
  awk -F '\t' -v wanted="$1" 'NR > 1 && $1 == wanted { print; found=1 }
    END { if (!found) exit 1 }' "$manifest"
}

prepare_input()
{
  case_id=$1
  row=$(case_row "$case_id") || {
    echo "case not found in manifest: $case_id" >&2
    exit 2
  }
  role=$(printf '%s\n' "$row" | awk -F '\t' '{print $2}')
  source_kind=$(printf '%s\n' "$row" | awk -F '\t' '{print $5}')
  source_path=$(printf '%s\n' "$row" | awk -F '\t' '{print $6}')
  source_sha=$(printf '%s\n' "$row" | awk -F '\t' '{print $7}')
  input_sha=$(printf '%s\n' "$row" | awk -F '\t' '{print $8}')

  if test "$role" = holdout && test "$allow_holdout" != 1; then
    echo "refusing holdout case without P9_MATRIX_ALLOW_HOLDOUT=1: $case_id" >&2
    exit 2
  fi
  case "$source_path" in
    /*) resolved_source=$source_path ;;
    *) resolved_source=$repo_dir/$source_path ;;
  esac
  raw_input=$output_dir/inputs/$case_id.raw.in
  case "$source_kind" in
    direct)
      awk '{ print }' "$resolved_source" > "$raw_input"
      ;;
    echoed-gzip)
      "$repo_dir/test.src/extract_prover9_input.sh" \
        "$resolved_source" "$raw_input" "$source_sha" "$input_sha" \
        >/dev/null
      ;;
    *)
      echo "unsupported source kind for $case_id: $source_kind" >&2
      exit 2
      ;;
  esac
  actual=$(sha256sum "$raw_input" | awk '{print $1}')
  if test "$actual" != "$input_sha"; then
    echo "$case_id: prepared input digest mismatch" >&2
    exit 1
  fi

  # Remove only experiment controls.  All logical clauses, inference rules,
  # weights, and hints remain byte-for-byte in their original order.
  awk '
    /^assign\((max_given|max_seconds|max_minutes|max_hours|max_days|max_megs|report|stats),/ { next }
    /^assign\((search_loop|passive_store|hint_index|inference_frontier|ancestor_store),/ { next }
    /^assign\((compact_term_reclaim_kb|compact_index_stale_pct|compact_passive_cache|compact_back_tree_min_tokens|compact_back_tree_budget_kb),/ { next }
    /^assign\(compact_unit_strategy,/ { next }
    /^assign\(compact_back_demod_strategy,/ { next }
    /^(set|clear)\(compact_otter_[a-z_]+\)\./ { next }
    /^(set|clear)\((clocks|hint_match_stats|print_gen|print_kept|print_given|print_initial_clauses)\)\./ { next }
    { print }
  ' "$raw_input" > "$output_dir/inputs/$case_id.base.in"
  printf '%s\t%s\t%s\n' "$case_id" "$role" "$input_sha" \
    >> "$output_dir/cases.tsv"
}

emit_common()
{
  echo 'clear(print_gen).'
  echo 'clear(print_kept).'
  echo 'clear(print_given).'
  echo 'clear(print_initial_clauses).'
  echo 'set(clocks).'
  echo 'set(hint_match_stats).'
  echo 'assign(stats,all).'
  echo "assign(report,$report_seconds)."
  echo "assign(max_given,$max_given)."
  echo "assign(max_seconds,$max_seconds)."
  echo "assign(max_megs,$max_megs)."
}

emit_variant()
{
  case "$1" in
    old_p9)
      ;;
    legacy)
      echo 'assign(search_loop,otter).'
      echo 'assign(passive_store,full).'
      echo 'assign(hint_index,fpa).'
      echo 'assign(inference_frontier,clauses).'
      echo 'assign(ancestor_store,off).'
      ;;
    packed_only)
      echo 'assign(search_loop,otter).'
      echo 'assign(passive_store,full).'
      echo 'assign(hint_index,packed_fast).'
      echo 'assign(inference_frontier,clauses).'
      echo 'assign(ancestor_store,off).'
      ;;
    compact_demod_only)
      emit_variant packed_only
      echo 'set(compact_otter_demodulation).'
      ;;
    compact_unit_only)
      emit_variant packed_only
      echo 'assign(compact_unit_strategy,root_scan).'
      echo 'set(compact_otter_unit_index).'
      ;;
    compact_unit_position)
      emit_variant packed_only
      echo 'assign(compact_unit_strategy,position).'
      echo 'set(compact_otter_unit_index).'
      ;;
    compact_unit_code_tree)
      emit_variant packed_only
      echo 'assign(compact_unit_strategy,code_tree).'
      echo 'set(compact_otter_unit_index).'
      ;;
    compact_back_only)
      emit_variant packed_only
      echo 'assign(compact_back_demod_strategy,mask8).'
      echo 'set(compact_otter_back_demod_index).'
      ;;
    compact_back_signature)
      emit_variant packed_only
      echo 'assign(compact_back_demod_strategy,signature32).'
      echo 'set(compact_otter_back_demod_index).'
      ;;
    compact_back_tree)
      emit_variant packed_only
      echo 'assign(compact_back_demod_strategy,code_tree).'
      echo 'set(compact_otter_back_demod_index).'
      ;;
    compact_back_hybrid)
      emit_variant packed_only
      echo 'assign(compact_back_demod_strategy,hybrid_tree).'
      echo "assign(compact_back_tree_min_tokens,$back_tree_min_tokens)."
      echo "assign(compact_back_tree_budget_kb,$back_tree_budget_kb)."
      echo 'set(compact_otter_back_demod_index).'
      ;;
    compact_nonunit_only)
      emit_variant packed_only
      echo 'set(compact_otter_nonunit_index).'
      ;;
    compact_full|compact_dense_file|compact_full_position|compact_dense_file_position|compact_full_code_tree|compact_dense_file_code_tree|compact_full_signature|compact_dense_file_signature|compact_full_code_tree_signature|compact_dense_file_code_tree_signature|compact_full_back_tree|compact_dense_file_back_tree|compact_full_code_tree_back_tree|compact_dense_file_code_tree_back_tree|compact_full_back_hybrid|compact_dense_file_back_hybrid|compact_full_code_tree_back_hybrid|compact_dense_file_code_tree_back_hybrid)
      echo 'assign(search_loop,otter).'
      case "$1" in
        compact_dense_file*)
          echo 'assign(passive_store,dense).'
          echo 'assign(ancestor_store,file).'
          ;;
        *)
          echo 'assign(passive_store,full).'
          echo 'assign(ancestor_store,off).'
          ;;
      esac
      case "$1" in
        *_position) echo 'assign(compact_unit_strategy,position).' ;;
        *_code_tree*) echo 'assign(compact_unit_strategy,code_tree).' ;;
        *) echo 'assign(compact_unit_strategy,root_scan).' ;;
      esac
      case "$1" in
        *_signature) echo 'assign(compact_back_demod_strategy,signature32).' ;;
        *_back_tree) echo 'assign(compact_back_demod_strategy,code_tree).' ;;
        *_back_hybrid)
          echo 'assign(compact_back_demod_strategy,hybrid_tree).'
          echo "assign(compact_back_tree_min_tokens,$back_tree_min_tokens)."
          echo "assign(compact_back_tree_budget_kb,$back_tree_budget_kb)."
          ;;
        *) echo 'assign(compact_back_demod_strategy,mask8).' ;;
      esac
      echo 'assign(hint_index,packed_fast).'
      echo 'assign(inference_frontier,clauses).'
      echo 'set(compact_otter_demodulation).'
      echo 'set(compact_otter_unit_index).'
      echo 'set(compact_otter_back_demod_index).'
      echo 'set(compact_otter_nonunit_index).'
      echo 'assign(compact_passive_cache,0).'
      echo 'assign(compact_term_reclaim_kb,2048).'
      echo 'assign(compact_index_stale_pct,25).'
      ;;
    *)
      echo "unknown matrix variant: $1" >&2
      exit 2
      ;;
  esac
}

run_one()
{
  case_id=$1
  variant=$2
  run_id=$case_id.$variant
  input=$output_dir/$run_id.in
  base=$output_dir/inputs/$case_id.base.in
  {
    emit_common
    emit_variant "$variant"
    awk '{ print }' "$base"
    case "$variant" in
      compact_dense_file*) echo 'assign(sos_limit,-1).' ;;
    esac
  } > "$input"
  case "$variant" in
    old_p9) prover=$old_prover ;;
    *) prover=$new_prover ;;
  esac
  sha256sum "$input" "$prover" > "$output_dir/$run_id.sha256"
  status=0
  if TMPDIR="$output_dir/tmp" P9_COMPACT_HEAP=1 \
       /usr/bin/time -v -o "$output_dir/$run_id.time" \
       timeout --signal=TERM --kill-after=10 "$wall_seconds" \
       taskset -c "$cpu" "$prover" < "$input" \
       > "$output_dir/$run_id.out" 2> "$output_dir/$run_id.err"; then
    status=0
  else
    status=$?
  fi
  printf '%s\n' "$status" > "$output_dir/$run_id.status"
  if grep -q 'THEOREM PROVED' "$output_dir/$run_id.out"; then
    "$repo_dir/bin/prooftrans" parents_only < "$output_dir/$run_id.out" \
      > "$output_dir/$run_id.proof" 2> "$output_dir/$run_id.proof.err"
  fi
}

printf 'case\trole\tinput_sha256\n' > "$output_dir/cases.tsv"
for case_id in $case_ids; do
  prepare_input "$case_id"
done

{
  echo "commit=$(git -C "$repo_dir" rev-parse HEAD 2>/dev/null || echo unknown)"
  echo "new_prover=$new_prover"
  echo "old_prover=$old_prover"
  echo "cases=$case_ids"
  echo "variants=$variants"
  echo "max_given=$max_given"
  echo "max_seconds=$max_seconds"
  echo "max_megs=$max_megs"
  echo "wall_seconds=$wall_seconds"
  echo "back_tree_min_tokens=$back_tree_min_tokens"
  echo "back_tree_budget_kb=$back_tree_budget_kb"
  echo "cpu=$cpu"
  echo "allow_holdout=$allow_holdout"
  uname -a
} > "$output_dir/run.conf"

for case_id in $case_ids; do
  for variant in $variants; do
    run_one "$case_id" "$variant"
  done
done

summary=$output_dir/summary.tsv
printf 'case\tvariant\tstatus\tproved\tgiven\tuser_cpu\twall\tmax_rss_kb\n' > "$summary"
outputs=
for case_id in $case_ids; do
  for variant in $variants; do
    run_id=$case_id.$variant
    out=$output_dir/$run_id.out
    time_file=$output_dir/$run_id.time
    status=$(awk 'NR == 1 { print; exit }' "$output_dir/$run_id.status")
    if grep -q 'THEOREM PROVED' "$out"; then proved=yes; else proved=no; fi
    given=$(sed -n 's/^Given=\([0-9][0-9]*\).*/\1/p' "$out" | tail -n 1)
    user_cpu=$(sed -n 's/^[[:space:]]*User time (seconds):[[:space:]]*//p' "$time_file")
    wall=$(sed -n 's/^[[:space:]]*Elapsed (wall clock) time (h:mm:ss or m:ss):[[:space:]]*//p' "$time_file")
    max_rss=$(sed -n 's/^[[:space:]]*Maximum resident set size (kbytes):[[:space:]]*//p' "$time_file")
    printf '%s\t%s\t%s\t%s\t%s\t%s\t%s\t%s\n' \
      "$case_id" "$variant" "$status" "$proved" "${given:-NA}" \
      "${user_cpu:-NA}" "${wall:-NA}" "${max_rss:-NA}" >> "$summary"
    outputs="$outputs $out"
  done
done

# The parser reads only committed machine-readable statistics and can be run
# again on compressed long-run outputs without rerunning Prover9.
# shellcheck disable=SC2086
"$repo_dir/test.src/parse_compact_profiles.sh" --tsv $outputs \
  > "$output_dir/profiles.tsv"
# shellcheck disable=SC2086
"$repo_dir/test.src/parse_compact_profiles.sh" --json $outputs \
  > "$output_dir/profiles.json"

echo "compact generalization matrix written to $output_dir"
awk '{ print }' "$summary"
