#!/bin/sh
set -eu

repo_dir=$(CDPATH= cd -- "$(dirname -- "$0")/.." && pwd)
test_tmp=$(mktemp -d "${TMPDIR:-/tmp}/p9-dense-passive-test.XXXXXX")
trap 'rm -rf -- "$test_tmp"' EXIT HUP INT TERM

# The focused hyperresolution problem has a deterministic proof and exercises
# repeated passive archive/activation.  Keep the resident compressed run as
# the semantic oracle, then enable the dense arena without changing any other
# search option.
"$repo_dir/bin/prover9" < "$repo_dir/test.src/collective_hyper.in" \
  > "$test_tmp/compressed.out" 2> "$test_tmp/compressed.err"

sed \
  -e 's/assign(passive_store,compressed)/assign(passive_store,dense)/' \
  -e '/assign(max_given,1000)/a\
assign(sos_limit,-1).' \
  "$repo_dir/test.src/collective_hyper.in" | \
  "$repo_dir/bin/prover9" > "$test_tmp/dense.out" 2> "$test_tmp/dense.err"

sed \
  -e 's/assign(passive_store,compressed)/assign(passive_store,dense)/' \
  -e '/assign(passive_store,dense)/a\
assign(passive_backing,file).' \
  -e '/assign(max_given,1000)/a\
assign(sos_limit,-1).' \
  "$repo_dir/test.src/collective_hyper.in" | \
  "$repo_dir/bin/prover9" > "$test_tmp/file.out" 2> "$test_tmp/file.err"

grep -q 'THEOREM PROVED' "$test_tmp/compressed.out"
grep -q 'THEOREM PROVED' "$test_tmp/dense.out"
grep -q 'THEOREM PROVED' "$test_tmp/file.out"

compressed_counts=$(grep '^Given=' "$test_tmp/compressed.out")
dense_counts=$(grep '^Given=' "$test_tmp/dense.out")
test "$compressed_counts" = "$dense_counts"
file_counts=$(grep '^Given=' "$test_tmp/file.out")
test "$compressed_counts" = "$file_counts"

grep -Eq 'Search_loop: mode=discount, frontier=collective, active_indexed=[0-9]+, passive_indexed=0, delayed_demodulators=[0-9]+\.' \
  "$test_tmp/dense.out"
# The historical hyper frontier can consume every live passive before the
# proof boundary, so records may be zero even though the dedicated selector
# and arena were both exercised and retain nonzero backing capacity.
grep -Eq 'Dense_passive: backing=mmap, records=[0-9]+, record_bytes=[1-9][0-9]*, heap_bytes=[1-9][0-9]*, arena_records=[1-9][0-9]*, arena_record_bytes=[1-9][0-9]*, arena_backing=[1-9][0-9]*, arena_physical=[0-9]+,' \
  "$test_tmp/dense.out"
grep -Eq 'Dense_passive_gc: arena_materialized=[1-9][0-9]*, validation_failures=0, compactions=[0-9]+, records_reclaimed=[0-9]+, arena_bytes_reclaimed=[0-9]+, file_reads=0 \(0 bytes\), file_writes=0 \(0 bytes\)\.' \
  "$test_tmp/dense.out"
grep -Eq 'Dense_passive: backing=file, records=[0-9]+, record_bytes=[1-9][0-9]*, heap_bytes=[1-9][0-9]*, arena_records=[1-9][0-9]*, arena_record_bytes=[1-9][0-9]*, arena_backing=[1-9][0-9]*, arena_physical=[1-9][0-9]*,' \
  "$test_tmp/file.out"
grep -Eq 'Dense_passive_gc: arena_materialized=[1-9][0-9]*, validation_failures=0, compactions=[0-9]+, records_reclaimed=[0-9]+, arena_bytes_reclaimed=[0-9]+, file_reads=[1-9][0-9]* \([1-9][0-9]* bytes\), file_writes=[1-9][0-9]* \([1-9][0-9]* bytes\)\.' \
  "$test_tmp/file.out"
grep -Eq 'avl_node \(  32\)[[:space:]]+0[[:space:]]+0[[:space:]]+0' \
  "$test_tmp/dense.out"

if grep -q 'Fatal error' "$test_tmp/dense.err"; then
  cat "$test_tmp/dense.err" >&2
  exit 1
fi
if grep -q 'Fatal error' "$test_tmp/file.err"; then
  cat "$test_tmp/file.err" >&2
  exit 1
fi

"$repo_dir/bin/prooftrans" parents_only < "$test_tmp/compressed.out" \
  > "$test_tmp/compressed-proof.out"
"$repo_dir/bin/prooftrans" parents_only < "$test_tmp/dense.out" \
  > "$test_tmp/dense-proof.out"
"$repo_dir/bin/prooftrans" parents_only < "$test_tmp/file.out" \
  > "$test_tmp/file-proof.out"
sed -n '/^% Length of proof:/,/^============================== end of proof/p' \
  "$test_tmp/compressed-proof.out" > "$test_tmp/compressed-proof.norm"
sed -n '/^% Length of proof:/,/^============================== end of proof/p' \
  "$test_tmp/dense-proof.out" > "$test_tmp/dense-proof.norm"
sed -n '/^% Length of proof:/,/^============================== end of proof/p' \
  "$test_tmp/file-proof.out" > "$test_tmp/file-proof.norm"
cmp "$test_tmp/compressed-proof.norm" "$test_tmp/dense-proof.norm"
cmp "$test_tmp/compressed-proof.norm" "$test_tmp/file-proof.norm"

echo 'dense_passive_test: PASS'
