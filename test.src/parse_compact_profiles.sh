#!/bin/sh
set -eu

script_dir=$(CDPATH= cd -- "$(dirname -- "$0")" && pwd)
output_format=tsv

case "${1:-}" in
  --json) output_format=json; shift ;;
  --tsv) shift ;;
esac

if test "$#" -eq 0; then
  echo "usage: $0 [--tsv|--json] PROVER9.out[.gz] ..." >&2
  exit 2
fi

{
  for source in "$@"; do
    if test ! -f "$source"; then
      echo "output not found: $source" >&2
      exit 2
    fi
    printf '@@P9_FILE %s\n' "$source"
    case "$source" in
      *.gz) gzip -cd -- "$source" ;;
      *) sed -n 'p' "$source" ;;
    esac
  done
} | awk -v output_format="$output_format" -f "$script_dir/parse_compact_profiles.awk"
