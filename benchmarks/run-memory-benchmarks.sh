#!/bin/sh
# Bounded memory/correctness benchmark driver for Phases 0--4.

set -eu

repo=$(CDPATH= cd -- "$(dirname -- "$0")/.." && pwd)
prover="$repo/bin/prover9"
aim_dir=${AIM_DIR:-"$repo/../AIM_REDONE/AIM_REDONE"}
suite=${1:-smoke}
cap_seconds=${CAP_SECONDS:-30}

if [ ! -x "$prover" ]; then
  echo "Build bin/prover9 first (make all)." >&2
  exit 1
fi

summarize()
{
  label=$1
  out=$2
  err=$3
  status=$4
  echo "===== $label (status=$status, external_cap=${cap_seconds}s) ====="
  grep -E '^(Given|Usable|Disabled_compression|Clause_body_bytes|Ancestor_store|Bookkeeping_bytes|Allocator_bytes)' "$out" || true
  grep -E 'User time|System time|Elapsed .* time|Maximum resident set size' "$err" || true
}

settings_for_mode()
{
  case "$1" in
    off)        printf '%s\n' 'clear(compress_disabled).' 'assign(ancestor_store,off).' ;;
    compressed|on)
                printf '%s\n' 'set(compress_disabled).' 'assign(ancestor_store,off).' ;;
    memory)     printf '%s\n' 'clear(compress_disabled).' 'assign(ancestor_store,memory).' ;;
    mmap)       printf '%s\n' 'clear(compress_disabled).' 'assign(ancestor_store,mmap).' ;;
    *) echo "unknown mode: $1" >&2; return 1 ;;
  esac
}

run_file()
{
  label=$1
  input=$2
  mode=$3
  max_given=$4
  tmp=$(mktemp -d "${TMPDIR:-/tmp}/p9-memory-bench.XXXXXX")
  set +e
  {
    sed -n 'p' "$input"
    settings_for_mode "$mode"
    printf '%s\n' "assign(max_given,$max_given)." \
      'assign(max_seconds,20).' 'clear(print_proofs).'
  } | /usr/bin/time -v timeout "$cap_seconds" "$prover" \
      > "$tmp/out" 2> "$tmp/err"
  status=$?
  set -e
  summarize "$label/$mode" "$tmp/out" "$tmp/err" "$status"
  case "$status" in 0|2|5) ;; *) cat "$tmp/err" >&2; rm -r "$tmp"; return 1;; esac
  rm -r "$tmp"
}

run_x2()
{
  mode=$1
  tmp=$(mktemp -d "${TMPDIR:-/tmp}/p9-memory-x2.XXXXXX")
  {
    sed -n 'p' "$repo/prover9.examples/x2.in"
    settings_for_mode "$mode"
    printf '%s\n' 'assign(max_seconds,20).'
  } | /usr/bin/time -v timeout "$cap_seconds" "$prover" \
      > "$tmp/out" 2> "$tmp/err"
  timeout "$cap_seconds" "$repo/bin/prooftrans" parents_only \
      < "$tmp/out" > "$tmp/checked" 2> "$tmp/check.err"
  grep -q 'end of proof' "$tmp/checked"
  summarize "x2-proof/$mode" "$tmp/out" "$tmp/err" 0
  rm -r "$tmp"
}

run_disabled_heavy()
{
  mode=$1
  tmp=$(mktemp -d "${TMPDIR:-/tmp}/p9-memory-disabled.XXXXXX")
  set +e
  {
    printf '%s\n' 'clear(auto).' 'set(binary_resolution).' \
      'set(back_subsume).' 'clear(print_initial_clauses).' \
      'clear(print_given).' 'clear(print_proofs).' \
      'assign(max_given,1).' 'assign(max_seconds,10).'
    settings_for_mode "$mode"
    printf '%s\n' \
      'formulas(sos).'
    i=1
    while [ "$i" -le 1000 ]; do
      printf 'p(f(f(f(c%d)))).\n' "$i"
      i=$((i + 1))
    done
    printf '%s\n' 'p(x).' 'end_of_list.'
  } | /usr/bin/time -v timeout "$cap_seconds" "$prover" \
      > "$tmp/out" 2> "$tmp/err"
  status=$?
  set -e
  summarize "disabled-heavy/$mode" "$tmp/out" "$tmp/err" "$status"
  case "$status" in 0|2|5) ;; *) cat "$tmp/err" >&2; rm -r "$tmp"; return 1;; esac
  rm -r "$tmp"
}

case "$suite" in
  smoke|all)
    timeout "$cap_seconds" make -C "$repo" memory-tests
    run_x2 off
    run_x2 compressed
    run_x2 memory
    run_x2 mmap
    run_disabled_heavy off
    run_disabled_heavy compressed
    run_disabled_heavy memory
    run_disabled_heavy mmap
    ;;
  aim|bookkeeping) ;;
  *) echo "usage: $0 [smoke|aim|bookkeeping|all]" >&2; exit 2;;
esac

case "$suite" in
  bookkeeping|all)
    timeout "$cap_seconds" make -C "$repo/test.src" bookkeeping_lifecycle_test
    /usr/bin/time -v timeout "$cap_seconds" \
      "$repo/test.src/bookkeeping_lifecycle_test" 1000000
    ;;
esac

case "$suite" in
  aim|all)
    run_file LCC_to_aK1 "$aim_dir/LCC_to_aK1.in" off 500
    run_file LCC_to_aK1 "$aim_dir/LCC_to_aK1.in" compressed 500
    run_file LCC_to_aK1 "$aim_dir/LCC_to_aK1.in" memory 500
    run_file LCC_to_aK1 "$aim_dir/LCC_to_aK1.in" mmap 500
    run_file aK1_nil3_a "$aim_dir/aK1_nil3_a.in" off 200
    run_file aK1_nil3_a "$aim_dir/aK1_nil3_a.in" compressed 200
    run_file aK1_nil3_a "$aim_dir/aK1_nil3_a.in" memory 200
    run_file aK1_nil3_a "$aim_dir/aK1_nil3_a.in" mmap 200
    ;;
esac
