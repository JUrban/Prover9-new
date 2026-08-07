/* Exercise reclaimable pointer-count slabs with enough live storage to span
   many mappings.  Sequentially freeing early slabs while a late pointer is
   still live detects stale global freelists and pointer invalidation. */

#include "../ladr/memory.h"

#include <stdio.h>
#include <stdint.h>
#include <stdlib.h>

#define DEFAULT_OBJECTS 300000
#define OBJECT_PTRS 32

static int Failures;

#define CHECK(condition, message) do { \
  if (!(condition)) { \
    fprintf(stderr, "FAIL: %s\n", message); \
    Failures++; \
  } \
} while (0)

struct churn_entry {
  void **p;
  unsigned n;
  uintptr_t marker;
};

static void mixed_class_churn(unsigned long long baseline_live,
                              unsigned long long baseline_reserved,
                              unsigned long long baseline_slabs)
{
  static const unsigned sizes[] = {1, 2, 3, 7, 15, 31, 63, 127};
  enum { ENTRIES = 20000, ROUNDS = 4 };
  struct churn_entry *entries = safe_malloc(sizeof(*entries) * ENTRIES);
  unsigned state = 0x9e3779b9U;
  struct memory_stats stats;
  int round, i;

  for (round = 0; round < ROUNDS; round++) {
    for (i = 0; i < ENTRIES; i++) {
      unsigned n = sizes[(i + round) %
                         (sizeof(sizes) / sizeof(sizes[0]))];
      uintptr_t marker = (uintptr_t) (i + 1) ^
                         ((uintptr_t) (round + 1) << 24);
      void **p = get_mem(n);
      p[0] = (void *) marker;
      if (n > 1)
        p[n-1] = (void *) ~marker;
      entries[i].p = p;
      entries[i].n = n;
      entries[i].marker = marker;
    }

    for (i = ENTRIES - 1; i > 0; i--) {
      int j;
      struct churn_entry tmp;
      state = state * 1664525U + 1013904223U;
      j = (int) (state % (unsigned) (i + 1));
      tmp = entries[i];
      entries[i] = entries[j];
      entries[j] = tmp;
    }
    for (i = 0; i < ENTRIES; i++) {
      struct churn_entry *e = &entries[i];
      CHECK(e->p[0] == (void *) e->marker,
            "mixed-class payload survives shuffled churn");
      if (e->n > 1)
        CHECK(e->p[e->n-1] == (void *) ~e->marker,
              "mixed-class tail survives shuffled churn");
      free_mem(e->p, e->n);
    }
    memory_get_stats(&stats);
    CHECK(stats.logical_live_bytes == baseline_live,
          "mixed-class round returns logical live bytes to baseline");
  }
  safe_free(entries);
  memory_release_unused();
  memory_get_stats(&stats);
  CHECK(stats.reserved_bytes == baseline_reserved &&
        stats.slab_count == baseline_slabs,
        "mixed-class purge returns mappings to baseline");
}

int main(int argc, char **argv)
{
  unsigned count = DEFAULT_OBJECTS;
  void **objects;
  unsigned i;
  struct memory_stats before, allocated, almost_freed, freed, direct;
  unsigned long long rss_before, rss_allocated, rss_freed, rss_peak;
  BOOL rss_supported = memory_current_rss_supported();
  BOOL returns_pages = memory_can_return_pages_to_os();

  if (argc == 2) {
    unsigned long parsed = strtoul(argv[1], NULL, 10);
    if (parsed == 0 || parsed > 2000000UL) {
      fprintf(stderr, "allocator_churn_test: invalid object count\n");
      return 2;
    }
    count = (unsigned) parsed;
  }

  objects = safe_malloc((size_t) count * sizeof(*objects));
  memory_get_stats(&before);
  rss_before = memory_current_rss_kbytes();

  for (i = 0; i < count; i++) {
    unsigned char *p = get_mem(OBJECT_PTRS);
    p[0] = (unsigned char) i;
    p[OBJECT_PTRS * BYTES_POINTER - 1] = (unsigned char) (i >> 8);
    objects[i] = p;
  }
  memory_get_stats(&allocated);
  rss_allocated = memory_current_rss_kbytes();
  CHECK(allocated.logical_live_bytes == before.logical_live_bytes +
        (unsigned long long) count * OBJECT_PTRS * BYTES_POINTER,
        "logical live bytes match outstanding slab objects");
  CHECK(allocated.reserved_bytes > before.reserved_bytes,
        "backing reservation grows under load");
  CHECK(allocated.slab_count > before.slab_count + 1,
        "load spans multiple independently reclaimable slabs");
  CHECK(allocated.fragmentation_bytes == allocated.reusable_bytes +
        allocated.unallocated_bytes + allocated.metadata_bytes,
        "fragmentation decomposes into exact slab components");

  for (i = 0; i + 1 < count; i++) {
    unsigned char *p = objects[i];
    if (p[0] != (unsigned char) i ||
        p[OBJECT_PTRS * BYTES_POINTER - 1] != (unsigned char) (i >> 8)) {
      fprintf(stderr, "allocator_churn_test: payload corruption at %u\n", i);
      return 1;
    }
    free_mem(p, OBJECT_PTRS);
  }

  memory_get_stats(&almost_freed);
  CHECK(((unsigned char *) objects[count-1])[0] == (unsigned char) (count-1) &&
        ((unsigned char *) objects[count-1])[OBJECT_PTRS * BYTES_POINTER - 1] ==
        (unsigned char) ((count-1) >> 8),
        "late live pointer survives reclamation of earlier slabs");
  CHECK(almost_freed.logical_live_bytes == before.logical_live_bytes +
        OBJECT_PTRS * BYTES_POINTER,
        "one retained object has exact logical accounting");
  CHECK(almost_freed.reserved_bytes < allocated.reserved_bytes,
        "wholly free slabs return their mappings before class teardown");

  free_mem(objects[count-1], OBJECT_PTRS);
  safe_free(objects);
  memory_get_stats(&freed);
  CHECK(freed.logical_live_bytes == before.logical_live_bytes,
        "logical live bytes return to baseline with a warm slab");
  CHECK(freed.slab_count == before.slab_count + 1,
        "one empty size-class slab remains warm");
  CHECK(freed.reserved_bytes == before.reserved_bytes + 1024 * 1024,
        "warm slab reservation is explicit in accounting");
  memory_release_unused();
  memory_get_stats(&freed);
  rss_freed = memory_current_rss_kbytes();
  rss_peak = memory_peak_rss_kbytes();
  CHECK(freed.logical_live_bytes == before.logical_live_bytes,
        "logical live bytes return to baseline");
  CHECK(freed.reserved_bytes == before.reserved_bytes,
        "all slab reservations return to baseline");
  CHECK(freed.slab_count == before.slab_count,
        "all test slabs are reclaimed");
  CHECK(freed.reclaimed_slabs >=
        allocated.slab_count - before.slab_count,
        "reclamation counter observes every emptied test slab");
  if (rss_supported) {
    CHECK(rss_before != 0 && rss_allocated != 0 && rss_freed != 0,
          "supported current-RSS probe returns measurements");
    CHECK(rss_allocated > rss_before + 32 * 1024,
          "resident set observes touched slab payload");
    if (returns_pages)
      CHECK(rss_freed + 32 * 1024 < rss_allocated,
            "resident set falls after slab mappings are returned");
  }
  else {
    CHECK(rss_before == 0 && rss_allocated == 0 && rss_freed == 0,
          "unsupported current-RSS probe is explicit");
  }

  mixed_class_churn(freed.logical_live_bytes, freed.reserved_bytes,
                    freed.slab_count);

  {
    void **zeroed = get_cmem(17);
    for (i = 0; i < 17; i++)
      CHECK(zeroed[i] == NULL, "get_cmem zeroes a fresh slab allocation");
    free_mem(zeroed, 17);
    memory_release_unused();
  }

  {
    unsigned direct_ptrs = 512;
    size_t direct_bytes = (size_t) direct_ptrs * BYTES_POINTER;
    void *large = get_mem(direct_ptrs);
    memory_get_stats(&direct);
    CHECK(direct.direct_live_bytes == freed.direct_live_bytes + direct_bytes,
          "direct allocation bytes are tracked separately");
    CHECK(direct.logical_live_bytes == freed.logical_live_bytes + direct_bytes,
          "direct allocation contributes to logical live bytes");
    free_mem(large, direct_ptrs);
    memory_get_stats(&direct);
    CHECK(direct.direct_live_bytes == freed.direct_live_bytes &&
          direct.logical_live_bytes == freed.logical_live_bytes &&
          direct.reserved_bytes == freed.reserved_bytes,
          "direct allocation accounting returns to baseline");
  }

  memory_get_stats(&freed);

  if (Failures != 0) {
    fprintf(stderr, "allocator_churn_test: %d failure(s)\n", Failures);
    return 1;
  }
  printf("allocator_churn_test: PASS objects=%u logical_peak=%llu "
         "reserved_peak=%llu reclaimed_slabs=%llu reclaimed_bytes=%llu "
         "rss_before_kb=%llu rss_live_kb=%llu rss_after_kb=%llu "
         "rss_peak_kb=%llu rss_supported=%s returns_pages=%s "
         "cumulative=%llu\n",
         count, freed.logical_peak_bytes, freed.peak_reserved_bytes,
         freed.reclaimed_slabs, freed.reclaimed_bytes,
         rss_before, rss_allocated, rss_freed, rss_peak,
         rss_supported ? "yes" : "no", returns_pages ? "yes" : "no",
         freed.cumulative_bytes);
  return 0;
}
