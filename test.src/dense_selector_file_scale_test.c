/* Accelerated resident-memory probe for the file-backed dense directory. */

#include "../ladr/ladr.h"
#include "../provers.src/giv_select.h"

#include <errno.h>
#include <fcntl.h>
#include <stdint.h>

static size_t archive_clause(Topform c, unsigned *body_bytes,
                             unsigned *justification_bytes,
                             unsigned *logical_body_bytes)
{
  size_t position = (size_t) c->id;
  *body_bytes = 0;
  *justification_bytes = 0;
  *logical_body_bytes = 0;
  zap_topform(c);
  return position;
}

static Topform activate_clause(size_t position, unsigned long long id,
                               unsigned long long hint_id)
{
  Topform c;
  (void) position;
  (void) hint_id;
  c = get_topform();
  c->id = id;
  return c;
}

static size_t parse_count(const char *text)
{
  unsigned long long value;
  char *end = NULL;
  errno = 0;
  value = strtoull(text, &end, 10);
  if (errno != 0 || end == text || *end != '\0' || value < 1000 ||
      value > UINT32_MAX - 1)
    fatal_error("dense_selector_file_scale_test: invalid count");
  return (size_t) value;
}

static size_t parse_buffer(const char *text, size_t count)
{
  size_t value = parse_count(text);
  if (value > count / 2)
    fatal_error("dense_selector_file_scale_test: buffer exceeds half count");
  return value;
}

int main(int argc, char **argv)
{
  size_t count = argc > 1 ? parse_count(argv[1]) : 2200000;
  size_t buffer_entries = argc > 2 ? parse_buffer(argv[2], count) : 65536;
  Plist rules = NULL;
  Clist sos;
  struct dense_passive_directory_stats directory;
  struct dense_passive_selector_stats selectors;
  struct dense_passive_selector_stats after_select;
  struct memory_process_stats process;
  unsigned long long record_bytes, heap_bytes, records;
  unsigned expected_reference_bits = (unsigned)
    ((sizeof(size_t) < sizeof(uint64_t) ? sizeof(size_t) : sizeof(uint64_t)) *
     CHAR_BIT);
  size_t i;
  {
    unsigned long long wide = (unsigned long long) UINT32_MAX + 123ULL;
    size_t decoded = 0;
    BOOL fits = dense_passive_file_record_index(wide, &decoded);
    if (sizeof(size_t) >= 8) {
      if (!fits || (unsigned long long) decoded != wide ||
          dense_passive_file_record_reference(decoded) != wide)
        fatal_error("dense_selector_file_scale_test: 64-bit reference loss");
    }
    else if (fits)
      fatal_error("dense_selector_file_scale_test: oversized reference fit");
  }
  if (argc > 3) {
    fprintf(stderr, "usage: %s [records [buffer_entries]]\n", argv[0]);
    return 2;
  }
  init_standard_ladr();
  configure_dense_passive_selectors(DENSE_SELECTOR_FILE, buffer_entries);
  configure_dense_passive_directory(DENSE_DIRECTORY_FILE);
  configure_dense_passive(TRUE, archive_clause, activate_clause);
  rules = plist_append(
    rules, selector_rule_term("Age", "low", "age", "all", 1));
  init_giv_select(rules);
  zap_plist_of_terms(rules);
  sos = clist_init("file directory scale probe");
  for (i = 0; i < count; i++) {
    Topform c = get_topform();
    c->id = (unsigned long long) i + 1;
    insert_into_sos2(c, sos);
  }
  directory = dense_passive_directory_stats();
  selectors = dense_passive_selector_stats();
  dense_passive_memory(&record_bytes, &heap_bytes, &records);
  memory_get_process_stats(&process);
  if (records != count || record_bytes != 0 ||
      directory.entry_bytes == 0 ||
      directory.logical_bytes != count * directory.entry_bytes ||
      (directory.logical_bytes > 128ULL * 1024ULL * 1024ULL &&
       directory.file_eviction_passes == 0) ||
      selectors.mode != DENSE_SELECTOR_FILE || selectors.flushes == 0 ||
      selectors.merges == 0 ||
      selectors.record_reference_bits != expected_reference_bits ||
      selectors.entry_bytes != 24 ||
      selectors.run_entries + selectors.buffered_entries != count ||
      heap_bytes >
        (unsigned long long) buffer_entries * selectors.entry_bytes +
          1024ULL * 1024ULL)
    fatal_error("dense_selector_file_scale_test: accounting failure");
  {
    char *type = NULL;
    Topform first = get_given_clause2(sos, 0, NULL, &type);
    if (first == NULL || first->id != 1 || strcmp(type, "Age") != 0)
      fatal_error("dense_selector_file_scale_test: age order changed");
    zap_topform(first);
  }
  after_select = dense_passive_selector_stats();
#if !defined(__EMSCRIPTEN__) && defined(POSIX_FADV_DONTNEED)
  if ((selectors.runs != 0 && after_select.file_reads == 0) ||
      after_select.file_read_evictions != after_select.file_reads ||
      after_select.file_read_eviction_bytes !=
        after_select.file_read_bytes ||
      after_select.file_read_eviction_failures != 0 ||
      after_select.file_min_calls != 1 ||
      after_select.file_run_checks != selectors.runs)
    fatal_error("dense_selector_file_scale_test: read cache eviction failure");
#endif
  printf("{\"records\":%llu,\"directory_entry_bytes\":%u,"
         "\"directory_logical\":%llu,"
         "\"directory_allocated\":%llu,\"heap_bytes\":%llu,"
         "\"selector_buffer_limit\":%llu,"
         "\"selector_runs\":%llu,\"selector_run_bytes\":%llu,"
         "\"selector_flushes\":%llu,\"selector_merges\":%llu,"
         "\"selector_record_bits\":%u,\"selector_entry_bytes\":%u,"
         "\"selector_reads\":%llu,\"selector_read_bytes\":%llu,"
         "\"selector_read_evictions\":%llu,"
         "\"selector_read_eviction_bytes\":%llu,"
         "\"selector_read_eviction_failures\":%llu,"
         "\"selector_min_calls\":%llu,\"selector_run_checks\":%llu,"
         "\"selector_writes\":%llu,\"selector_write_bytes\":%llu,"
         "\"eviction_passes\":%llu,\"eviction_bytes\":%llu,"
         "\"pss_kib\":%llu,\"anonymous_kib\":%llu}\n",
         (unsigned long long) count, directory.entry_bytes,
         directory.logical_bytes,
         directory.allocated_bytes, heap_bytes,
         selectors.buffer_limit,
         selectors.runs, selectors.run_logical_bytes,
         selectors.flushes, selectors.merges,
         selectors.record_reference_bits, selectors.entry_bytes,
         after_select.file_reads, after_select.file_read_bytes,
         after_select.file_read_evictions,
         after_select.file_read_eviction_bytes,
         after_select.file_read_eviction_failures,
         after_select.file_min_calls, after_select.file_run_checks,
         selectors.file_writes, selectors.file_write_bytes,
         directory.file_eviction_passes,
         directory.file_eviction_bytes, process.pss_kbytes,
         process.anonymous_kbytes);
  clist_free(sos);
  zap_given_selectors();
  configure_dense_passive(FALSE, NULL, NULL);
  configure_dense_passive_directory(DENSE_DIRECTORY_MEMORY);
  configure_dense_passive_selectors(DENSE_SELECTOR_HEAP, 65536);
  return 0;
}
