/* Bounded synthetic measurement for Phase 4 record/handle accounting. */

#include "../ladr/ladr.h"
#include <errno.h>
#include <limits.h>

static size_t requested_count(int argc, char **argv)
{
  unsigned long long n = 20000;
  char *end = NULL;
  if (argc > 1) {
    errno = 0;
    n = strtoull(argv[1], &end, 10);
    if (errno != 0 || end == argv[1] || *end != '\0' || n < 100 ||
        n > INT_MAX) {
      fprintf(stderr, "usage: %s [record_count: 100..%d]\n", argv[0],
              INT_MAX);
      exit(2);
    }
  }
  return (size_t) n;
}

int main(int argc, char **argv)
{
  size_t n = requested_count(argc, argv), i;
  Clause_store_archive_mode mode =
    argc > 2 && strcmp(argv[2], "mmap") == 0 ?
      CLAUSE_STORE_ARCHIVE_MMAP :
    argc > 2 && strcmp(argv[2], "file") == 0 ?
      CLAUSE_STORE_ARCHIVE_FILE : CLAUSE_STORE_ARCHIVE_MEMORY;
  Clause_store store;
  struct clause_store_stats records;
  struct clause_id_table_stats ids, final_ids;
  unsigned long long resident, total, legacy;

  init_standard_ladr();
  clear_clause_id_tab();
  set_clause_id_count(0);
  store = clause_store_init("ancestor-scale");
  if (argc > 2 && strcmp(argv[2], "memory") != 0 &&
      strcmp(argv[2], "mmap") != 0 && strcmp(argv[2], "file") != 0) {
    fprintf(stderr, "usage: %s [record_count] [memory|mmap|file]\n", argv[0]);
    return 2;
  }
  if (!clause_store_enable_archive(store, mode)) {
    fprintf(stderr, "ancestor_store_scale_test: cannot initialize store\n");
    return 1;
  }
  for (i = 0; i < n; i++) {
    Topform c = parse_clause_from_string("scale_atom(f(x),a).");
    c->justification = input_just();
    assign_clause_id(c);
    clause_store_append(store, c);
    if (!clause_store_archive_clause(store, c)) {
      fprintf(stderr, "ancestor_store_scale_test: append failed at %llu\n",
              (unsigned long long) i);
      return 1;
    }
  }
  if (mode == CLAUSE_STORE_ARCHIVE_FILE && !clause_store_sync(store)) {
    fprintf(stderr, "ancestor_store_scale_test: final flush failed\n");
    return 1;
  }
  records = clause_store_get_stats(store);
  ids = clause_id_table_get_stats();
  resident = records.handle_bytes + ids.allocated_bytes;
  total = resident + records.record_bytes;
  legacy = (unsigned long long) n *
    (sizeof(struct topform) + sizeof(struct literals) +
     sizeof(struct term) + 2 * sizeof(Term) + sizeof(struct just) +
     sizeof(struct clist_pos) + sizeof(struct plist));
  printf("ancestor_store_scale_test: records=%llu record_bytes=%llu "
         "backing_bytes=%llu handle_bytes=%llu id_bytes=%llu "
         "physical_bytes=%llu "
         "resident_bytes=%llu total_logical_bytes=%llu legacy_estimate=%llu "
         "resident_per_record=%.3f total_per_record=%.3f "
         "legacy_per_record=%.3f mmap_evictions=%llu "
         "mmap_eviction_bytes=%llu io_buffer=%llu "
         "write_buffer=%llu "
         "file_reads=%llu file_read_bytes=%llu "
         "file_writes=%llu file_write_bytes=%llu "
         "file_write_calls=%llu file_write_call_bytes=%llu\n",
         records.records, records.record_bytes, records.backing_bytes,
         records.handle_bytes, ids.allocated_bytes, records.physical_bytes,
         resident, total, legacy,
         (double) resident / n, (double) total / n, (double) legacy / n,
         records.mmap_eviction_passes, records.mmap_eviction_bytes,
         records.io_buffer_bytes, records.write_buffer_bytes,
         records.file_reads, records.file_read_bytes,
         records.file_writes, records.file_write_bytes,
         records.file_write_calls, records.file_write_call_bytes);
  printf("ancestor_store_file_cache: evictions=%llu eviction_bytes=%llu "
         "syncs=%llu failures=%llu\n",
         records.file_cache_eviction_passes,
         records.file_cache_eviction_bytes, records.file_syncs,
         records.file_cache_eviction_failures);
  if (records.records != n || ids.entries != n || resident >= legacy ||
      total >= legacy ||
      (mode == CLAUSE_STORE_ARCHIVE_MMAP && records.record_bytes >=
         16U * 1024U * 1024U && records.mmap_eviction_passes == 0) ||
      (mode == CLAUSE_STORE_ARCHIVE_FILE &&
       (records.io_buffer_bytes == 0 || records.write_buffer_bytes == 0 ||
        records.file_writes != n ||
        records.file_write_bytes != records.record_bytes ||
        records.file_write_calls >= records.file_writes ||
        records.file_write_call_bytes != records.file_write_bytes))) {
    fprintf(stderr, "ancestor_store_scale_test: accounting check failed\n");
    return 1;
  }
  clause_store_delete_clauses(store);
  final_ids = clause_id_table_get_stats();
  if (final_ids.entries != 0 || final_ids.pages != 0) {
    fprintf(stderr, "ancestor_store_scale_test: teardown check failed\n");
    return 1;
  }
  set_clause_id_count(0);
  printf("ancestor_store_scale_test: PASS\n");
  return 0;
}
