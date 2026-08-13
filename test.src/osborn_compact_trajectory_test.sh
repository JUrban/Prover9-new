#!/bin/sh
set -eu

repo_dir=$(CDPATH= cd -- "$(dirname -- "$0")/.." && pwd)
test_tmp=$(mktemp -d "${TMPDIR:-/tmp}/p9-osborn-trajectory.XXXXXX")
trap 'rm -rf -- "$test_tmp"' EXIT HUP INT TERM

explicit_input=no
if [ "$#" -gt 0 ]; then
  osborn_input=$1
  explicit_input=yes
elif [ -n "${OSBORN_INPUT:-}" ]; then
  osborn_input=$OSBORN_INPUT
  explicit_input=yes
elif [ -f "$repo_dir/../bob/rr_osbe.in.gz" ]; then
  osborn_input=$repo_dir/../bob/rr_osbe.in.gz
elif [ -f "$repo_dir/bob/rr_osbe.in.gz" ]; then
  osborn_input=$repo_dir/bob/rr_osbe.in.gz
else
  echo 'osborn_compact_trajectory_test: SKIP (set OSBORN_INPUT)' >&2
  exit 0
fi

if [ ! -f "$osborn_input" ]; then
  if [ "$explicit_input" = yes ]; then
    echo "osborn_compact_trajectory_test: missing $osborn_input" >&2
    exit 1
  fi
  echo 'osborn_compact_trajectory_test: SKIP (input not found)' >&2
  exit 0
fi

prover=${P9_PROVER:-$repo_dir/bin/prover9}
wall_seconds=${P9_OSBORN_WALL_SECONDS:-1200}
max_seconds=${P9_OSBORN_MAX_SECONDS:-900}
max_megs=${P9_OSBORN_MAX_MEGS:-4096}
max_given=${P9_OSBORN_MAX_GIVEN:-510}

case "$max_given" in
  *[!0-9]*|'')
    echo 'osborn_compact_trajectory_test: P9_OSBORN_MAX_GIVEN must be an integer' >&2
    exit 1
    ;;
esac
if [ "$max_given" -lt 510 ]; then
  echo 'osborn_compact_trajectory_test: P9_OSBORN_MAX_GIVEN must be at least 510' >&2
  exit 1
fi

read_osborn_input()
{
  case "$osborn_input" in
    *.gz) gzip -dc -- "$osborn_input" ;;
    *) sed -n '1,$p' "$osborn_input" ;;
  esac
}

invoke_prover()
{
  if command -v timeout >/dev/null 2>&1; then
    timeout "$wall_seconds" "$prover"
  else
    "$prover"
  fi
}

run_case()
{
  name=$1
  options=$2
  out=$test_tmp/$name.out
  err=$test_tmp/$name.err
  set +e
  {
    printf '%s\n' \
      "assign(max_given,$max_given)." \
      'clear(echo_input).' \
      'clear(print_initial_clauses).'
    printf '%s\n' "$options"
    read_osborn_input
    printf '%s\n' \
      'clear(print_gen).' \
      'clear(print_kept).' \
      'set(print_given).' \
      'set(search_event_trace).' \
      "assign(max_seconds,$max_seconds)." \
      "assign(max_megs,$max_megs)."
  } | invoke_prover > "$out" 2> "$err"
  status=$?
  set -e
  if [ "$status" -ne 5 ]; then
    echo "osborn_compact_trajectory_test: $name exited $status, expected max_given (5)" >&2
    tail -40 "$err" >&2 || true
    return 1
  fi
}

reference_options='assign(search_loop,otter).
assign(passive_store,full).
assign(hint_index,packed_fast).
assign(inference_frontier,clauses).
assign(sos_limit,-1).'

compact_options='assign(search_loop,otter).
assign(passive_store,dense).
assign(passive_directory,file).
assign(passive_selector_store,file).
assign(passive_selector_buffer,65536).
assign(hint_index,packed_fast).
assign(inference_frontier,clauses).
assign(ancestor_store,file).
assign(sos_limit,-1).
set(process_initial_sos).
set(back_demod).
set(back_demod_hints).
clear(unit_deletion).
clear(ancestor_subsume).
clear(eval_rewrite).
clear(compress_disabled).
set(compact_otter_demodulation).
set(compact_otter_unit_index).
set(compact_otter_back_demod_index).
set(compact_otter_nonunit_index).
assign(compact_unit_strategy,code_tree).
set(compact_nonunit_path_filter).
assign(compact_back_demod_strategy,adaptive32).
set(compact_back_sparse_positions).
assign(compact_back_position_budget_kb,0).
assign(compact_back_position_budget_pct,50).
assign(compact_back_position_build_factor,32).
assign(compact_back_eager_position_depth,4).
assign(compact_back_tree_budget_kb,65536).
assign(compact_back_tree_budget_pct,200).
clear(compact_back_edge_filter).
assign(compact_rewrite_deep_cache_kb,0).
assign(compact_passive_cache,0).
assign(compact_index_stale_pct,25).
assign(compact_term_reclaim_kb,8192).'

run_case reference "$reference_options" &
reference_pid=$!
run_case compact "$compact_options" &
compact_pid=$!
reference_status=0
compact_status=0
wait "$reference_pid" || reference_status=$?
wait "$compact_pid" || compact_status=$?
if [ "$reference_status" -ne 0 ] || [ "$compact_status" -ne 0 ]; then
  exit 1
fi

python3 - "$test_tmp/reference.out" "$test_tmp/compact.out" "$max_given" <<'PY'
import hashlib
import re
import sys

given_re = re.compile(r'^given #(\d+) \([^)]*\): \d+ (.*?)(?:  \[.*)$')
stats_re = re.compile(
    r'^Given=(\d+)\. Generated=(\d+)\. Kept=(\d+)\. proofs=(\d+)\.$')
expected_suffix = '28641f7c2ba46869c5b51d33d9980008d07adf8b68846cd3a0236caa68036ea1'
expected_stats = (511, 390236, 25140, 0)
max_given = int(sys.argv[3])

def read_run(path):
    givens = []
    stats = []
    with open(path, encoding='utf-8', errors='replace') as stream:
        for line in stream:
            match = given_re.match(line.rstrip())
            if match:
                givens.append((int(match.group(1)), match.group(2)))
            match = stats_re.match(line.rstrip())
            if match:
                stats.append(tuple(map(int, match.groups())))
    if [number for number, _ in givens] != list(range(1, max_given + 1)):
        raise SystemExit(
            f'{path}: expected exactly given clauses 1..{max_given}')
    if not stats:
        raise SystemExit(f'{path}: final statistics are missing')
    if max_given == 510 and stats[-1] != expected_stats:
        raise SystemExit(
            f'{path}: endpoint {stats[-1] if stats else None}, '
            f'expected {expected_stats}')
    if max_given != 510 and (stats[-1][0] != max_given + 1 or stats[-1][3] != 0):
        raise SystemExit(
            f'{path}: unexpected extended-prefix endpoint {stats[-1]}')
    return [body for _, body in givens]

reference = read_run(sys.argv[1])
compact = read_run(sys.argv[2])
if reference != compact:
    at = next(i for i, pair in enumerate(zip(reference, compact), 1)
              if pair[0] != pair[1])
    raise SystemExit(
        f'given trajectory diverged at {at}:\n'
        f'  reference: {reference[at-1]}\n'
        f'  compact:   {compact[at-1]}')

suffix = ''.join(body + '\n' for body in reference[194:510]).encode()
digest = hashlib.sha256(suffix).hexdigest()
if digest != expected_suffix:
    raise SystemExit(
        f'givens 195..510 do not match the 2021 outaH prefix: {digest}')
PY

reference_cpu=$(grep '^User_CPU=' "$test_tmp/reference.out" | tail -1)
compact_cpu=$(grep '^User_CPU=' "$test_tmp/compact.out" | tail -1)
echo "osborn_compact_trajectory_test: reference $reference_cpu"
echo "osborn_compact_trajectory_test: compact   $compact_cpu"
echo "osborn_compact_trajectory_test: PASS ($max_given givens; outaH suffix preserved)"
