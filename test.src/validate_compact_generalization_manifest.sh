#!/bin/sh
set -eu

script_dir=$(CDPATH= cd -- "$(dirname -- "$0")" && pwd)
repository_dir=$(CDPATH= cd -- "$script_dir/.." && pwd)
manifest=${1:-$repository_dir/benchmarks/compact-generalization/manifest.tsv}

if test ! -f "$manifest"; then
  echo "manifest not found: $manifest" >&2
  exit 2
fi

expected_header=$(printf 'case_id\trole\ttier\tcategory\tsource_kind\tsource_path\tsource_sha256\tinput_sha256\tmax_given\tmax_seconds\tmax_megs\tnotes')
IFS= read -r actual_header < "$manifest"
if test "$actual_header" != "$expected_header"; then
  echo "unexpected manifest header: $manifest" >&2
  exit 1
fi

if ! awk -F '\t' '
    NR > 1 && NF != 12 {
      print "manifest line " NR " has " NF " fields; expected 12" > "/dev/stderr"
      bad = 1
    }
    END { exit bad }
  ' "$manifest"; then
  exit 1
fi

validation_tmp=$(mktemp -d "${TMPDIR:-/tmp}/p9-compact-manifest.XXXXXX")
trap 'rm -rf -- "$validation_tmp"' EXIT HUP INT TERM

status=0
tab=$(printf '\t')
rows=$validation_tmp/manifest.rows
awk -F '\t' 'NR > 1 { print $1 "\t" $5 "\t" $6 "\t" $7 "\t" $8 }' "$manifest" > "$rows"

while IFS="$tab" read -r case_id source_kind source_path source_sha input_sha; do
  case "$source_path" in
    /*) resolved_source=$source_path ;;
    *) resolved_source=$repository_dir/$source_path ;;
  esac

  if test ! -f "$resolved_source"; then
    echo "$case_id: source not found: $resolved_source" >&2
    status=1
    continue
  fi

  actual_source=$(sha256sum "$resolved_source" | awk '{print $1}')
  if test "$actual_source" != "$source_sha"; then
    echo "$case_id: source digest mismatch: expected $source_sha, got $actual_source" >&2
    status=1
    continue
  fi

  case "$source_kind" in
    direct)
      if test "$actual_source" != "$input_sha"; then
        echo "$case_id: direct input digest mismatch: expected $input_sha, got $actual_source" >&2
        status=1
        continue
      fi
      ;;
    echoed-gzip)
      extracted=$validation_tmp/$case_id.in
      if ! "$script_dir/extract_prover9_input.sh" \
          "$resolved_source" "$extracted" "$source_sha" "$input_sha" \
          >/dev/null; then
        status=1
        continue
      fi
      ;;
    *)
      echo "$case_id: unknown source kind: $source_kind" >&2
      status=1
      continue
      ;;
  esac

  echo "$case_id: verified ($source_kind)"
done < "$rows"

exit "$status"
