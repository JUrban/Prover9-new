#!/bin/sh

# Run one command in a fresh delegated cgroup-v2 child and retain whole-job
# memory accounting.  Exit 77 means the caller has no usable delegation; it is
# intentionally not a silent RSS fallback because file cache is part of the
# long-run RAM acceptance gate.

set -u

if test "$#" -lt 2; then
  echo "usage: $0 REPORT.cgroup COMMAND [ARG ...]" >&2
  exit 2
fi

report=$1
shift
cgroup_root=${P9_CGROUP_ROOT:-/sys/fs/cgroup}

if test "$(stat -f -c '%T' "$cgroup_root" 2>/dev/null || true)" != cgroup2fs; then
  echo "cgroup_job_memory: cgroup v2 is not mounted at $cgroup_root" >&2
  exit 77
fi

relative=$(awk -F: '$1 == "0" { print $3; found=1 } END { if (!found) exit 1 }' \
  /proc/self/cgroup) || {
  echo "cgroup_job_memory: cannot determine the current cgroup" >&2
  exit 77
}
parent=$cgroup_root$relative

if test ! -d "$parent" || test ! -w "$parent" || \
   test ! -w "$parent/cgroup.procs"; then
  echo "cgroup_job_memory: current cgroup is not delegated: $parent" >&2
  exit 77
fi
if ! grep -qw memory "$parent/cgroup.subtree_control" 2>/dev/null; then
  echo "cgroup_job_memory: memory controller is not delegated below $parent" >&2
  exit 77
fi

child=$parent/prover9-memory-$$
if ! mkdir "$child" 2>/dev/null; then
  echo "cgroup_job_memory: cannot create $child" >&2
  exit 77
fi

in_child=no
cleanup()
{
  if test "$in_child" = yes; then
    printf '%s\n' "$$" > "$parent/cgroup.procs" 2>/dev/null || true
    in_child=no
  fi
  if test -e "$child/cgroup.kill"; then
    printf '1\n' > "$child/cgroup.kill" 2>/dev/null || true
  fi
  rmdir "$child" 2>/dev/null || true
}
trap cleanup EXIT
trap 'exit 129' HUP
trap 'exit 130' INT
trap 'exit 143' TERM

if ! printf '%s\n' "$$" > "$child/cgroup.procs"; then
  echo "cgroup_job_memory: cannot enter $child" >&2
  exit 77
fi
in_child=yes

status=0
"$@" || status=$?

# Exclude this small wrapper from the end-of-job breakdown.  memory.peak is
# monotone for the lifetime of the fresh child and is unaffected by the move.
if ! printf '%s\n' "$$" > "$parent/cgroup.procs"; then
  echo "cgroup_job_memory: cannot leave $child" >&2
  exit 77
fi
in_child=no
if test -e "$child/cgroup.kill"; then
  printf '1\n' > "$child/cgroup.kill" 2>/dev/null || true
fi

stat_value()
{
  awk -v wanted="$1" '$1 == wanted { print $2; found=1; exit }
    END { if (!found) print 0 }' "$child/memory.stat"
}

memory_peak=$(cat "$child/memory.peak")
memory_current=$(cat "$child/memory.current")
anon_current=$(stat_value anon)
file_current=$(stat_value file)
shmem_current=$(stat_value shmem)
file_mapped_current=$(stat_value file_mapped)
if test -r "$child/memory.swap.peak"; then
  swap_peak=$(cat "$child/memory.swap.peak")
else
  swap_peak=0
fi

for value in "$memory_peak" "$memory_current" "$anon_current" \
             "$file_current" "$shmem_current" "$file_mapped_current" \
             "$swap_peak"; do
  case "$value" in
    ''|*[!0-9]*)
      echo "cgroup_job_memory: nonnumeric memory counter: $value" >&2
      exit 74
      ;;
  esac
done

if ! {
  echo "format=1"
  echo "memory_peak_bytes=$memory_peak"
  echo "memory_current_bytes=$memory_current"
  echo "anon_current_bytes=$anon_current"
  echo "file_current_bytes=$file_current"
  echo "shmem_current_bytes=$shmem_current"
  echo "file_mapped_current_bytes=$file_mapped_current"
  echo "swap_peak_bytes=$swap_peak"
  echo "command_status=$status"
} > "$report"; then
  echo "cgroup_job_memory: cannot write report: $report" >&2
  exit 74
fi

exit "$status"
