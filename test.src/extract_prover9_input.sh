#!/bin/sh
set -eu

if test "$#" -ne 4; then
  echo "usage: $0 OUTPUT.gz DEST EXPECTED_OUTPUT_SHA256 EXPECTED_INPUT_SHA256" >&2
  exit 2
fi

source_gz=$1
destination=$2
expected_source=$3
expected_input=$4

if test ! -f "$source_gz"; then
  echo "source output not found: $source_gz" >&2
  exit 2
fi

actual_source=$(sha256sum "$source_gz" | awk '{print $1}')
if test "$actual_source" != "$expected_source"; then
  echo "source digest mismatch: expected $expected_source, got $actual_source" >&2
  exit 1
fi

destination_dir=$(dirname -- "$destination")
if test ! -d "$destination_dir"; then
  echo "destination directory not found: $destination_dir" >&2
  exit 2
fi

temporary=$(mktemp "$destination_dir/.prover9-input.XXXXXX")
trap 'rm -f -- "$temporary"' EXIT HUP INT TERM

gzip -cd -- "$source_gz" |
  sed -n '/^============================== INPUT /,/^============================== end of input /p' |
  sed '1d;$d' |
  # Prover9 writes parser/option diagnostics before closing its echoed INPUT
  # section.  They were not present in the source file and are not terms, so
  # retaining them makes an otherwise exact historical input unparsable.
  sed '/^WARNING, /d' > "$temporary"

if test ! -s "$temporary"; then
  echo "no echoed Prover9 input found in: $source_gz" >&2
  exit 1
fi

actual_input=$(sha256sum "$temporary" | awk '{print $1}')
if test "$actual_input" != "$expected_input"; then
  echo "input digest mismatch: expected $expected_input, got $actual_input" >&2
  exit 1
fi

mv -- "$temporary" "$destination"
trap - EXIT HUP INT TERM
printf '%s  %s\n' "$actual_input" "$destination"
