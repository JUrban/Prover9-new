/*  Copyright (C) 2006, 2007 William McCune

    This file is part of the LADR Deduction Library.

    The LADR Deduction Library is free software; you can redistribute it
    and/or modify it under the terms of the GNU General Public License,
    version 2.

    The LADR Deduction Library is distributed in the hope that it will be
    useful, but WITHOUT ANY WARRANTY; without even the implied warranty of
    MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
    GNU General Public License for more details.

    You should have received a copy of the GNU General Public License
    along with the LADR Deduction Library; if not, write to the Free Software
    Foundation, Inc., 51 Franklin St, Fifth Floor, Boston, MA 02110-1301 USA.
*/

#include "giv_select.h"
#include "semantics.h"
#include "../ladr/avltree.h"
#include "../ladr/clause_eval.h"
#include "../ladr/fpa.h"
#include <errno.h>
#include <limits.h>
#include <stdint.h>
#include <unistd.h>
#ifndef __EMSCRIPTEN__
#include <sys/mman.h>
#include <sys/stat.h>
#include <fcntl.h>
#endif

/* Private definitions and types */

enum { GS_ORDER_WEIGHT,
       GS_ORDER_AGE,
       GS_ORDER_HINT_AGE,
       GS_ORDER_RANDOM
};  /* order */

typedef struct giv_select *Giv_select;

#define DENSE_SELECTOR_RUN_LEVELS 64
/* 8,192 24-byte entries make a 192 KiB block, a multiple of both common 4 KiB
   and 64 KiB page sizes.  Besides amortizing pread/fadvise calls, this lets
   consumed sequential ranges be discarded without retaining a partial page
   needed by the next block.  One buffer exists only for each logarithmically
   bounded live run. */
#define DENSE_SELECTOR_READ_ENTRIES 8192

struct dense_selector_entry {
  union {
    unsigned long long hint_id;
    double weight;
  } key;
  uint64_t record;
};

/* Dense records are appended in strictly increasing clause-ID order, and a
   directory compaction rebuilds every selector run.  The physical record
   reference is consequently also the exact final age tie-breaker; storing
   the ID beside it would add eight redundant bytes to every external entry. */
typedef char dense_selector_entry_must_remain_16_bytes[
  sizeof(struct dense_selector_entry) == 16 ? 1 : -1];
typedef char dense_file_record_reference_must_hold_size_t[
  sizeof(size_t) <= sizeof(uint64_t) ? 1 : -1];

struct dense_selector_run {
  int fd;
  unsigned long long remaining;
  off_t next_offset;
  struct dense_selector_entry head;
  BOOL head_valid;
  struct dense_selector_entry *read_buffer;
  size_t read_size;
  size_t read_at;
};

struct giv_select {
  char         *name;
  int          order;
  Clause_eval  property;
  int          part;
  unsigned long long selected;
  Ordertype (*compare) (void *, void *);  /* function for ordering idx */
  Avl_node idx;          /* index of clauses (binary search (AVL) tree) */
  uint32_t *dense_heap;  /* record indexes; lazy deletion */
  size_t dense_size;
  size_t dense_capacity;
  size_t dense_active;
  unsigned dense_bit;
  struct dense_selector_entry *dense_buffer;
  size_t dense_buffer_size;
  size_t dense_buffer_capacity;
  struct dense_selector_run dense_runs[DENSE_SELECTOR_RUN_LEVELS];
  unsigned long long dense_peak_runs;
  unsigned long long dense_flushes;
  unsigned long long dense_merges;
  unsigned long long dense_file_reads;
  unsigned long long dense_file_read_bytes;
  unsigned long long dense_file_writes;
  unsigned long long dense_file_write_bytes;
  unsigned long long dense_file_evictions;
  unsigned long long dense_file_eviction_bytes;
  unsigned long long dense_file_eviction_failures;
  unsigned long long dense_file_read_evictions;
  unsigned long long dense_file_read_eviction_bytes;
  unsigned long long dense_file_read_eviction_failures;
  unsigned long long dense_file_min_calls;
  unsigned long long dense_file_buffer_checks;
  unsigned long long dense_file_run_checks;
  unsigned long long dense_stale_entries_discarded;
};  /* struct giv_select */

#define DENSE_PASSIVE_ACTIVE  0x01U
#define DENSE_PASSIVE_DELAYED 0x02U
#define DENSE_PASSIVE_RULE_DIRTY 0x04U
#define DENSE_PASSIVE_SEMANTICS_SHIFT 3U
#define DENSE_PASSIVE_SEMANTICS_MASK  0x18U
#define DENSE_PASSIVE_USED 0x20U
#define DENSE_PASSIVE_ARCHIVE_DIRTY 0x40U
#define DENSE_DIRECTORY_EVICT_STEP (64U * 1024U * 1024U)
#define DENSE_DIRECTORY_HOT_WINDOW (64U * 1024U * 1024U)

struct dense_passive_record {
  unsigned long long id;
  unsigned long long hint_id;
  unsigned long long selector_mask;
  size_t store_position;
  double weight;
  unsigned simplifier_epoch;
  unsigned rewrite_epoch;
  unsigned flags;
  unsigned first_fpa_id;
  unsigned body_bytes;
  unsigned justification_bytes;
  unsigned logical_body_bytes;
};

typedef struct select_state *Select_state;

/* Static variables */

static struct select_state {
  Plist selectors;    /* list of Giv_select */
  unsigned long long occurrences;  /* memberships across selectors */
  Plist current;      /* for ratio state */
  int  count;         /* for ratio state */
  int  cycle_size;
} High, Low; /* The two lists of selectors and their positions */

static BOOL Rule_needs_semantics = FALSE;
static unsigned long long Sos_size = 0;
static double Low_water_keep = INT_MAX;
static double Low_water_displace = INT_MAX;
static unsigned long long Sos_deleted = 0;
static unsigned long long Sos_displaced = 0;

static BOOL Debug = FALSE;

static BOOL Dense_passive = FALSE;
static Dense_passive_archive_fn Dense_archive = NULL;
static Dense_passive_activate_fn Dense_activate = NULL;
static struct dense_passive_record *Dense_records = NULL;
static Dense_passive_directory_mode Dense_directory_mode =
  DENSE_DIRECTORY_MEMORY;
static Dense_passive_selector_mode Dense_selector_mode = DENSE_SELECTOR_HEAP;
static size_t Dense_selector_buffer_limit = 65536;
static int Dense_record_fd = -1;
static size_t Dense_directory_advised_bytes = 0;
static size_t Dense_directory_last_evict_bytes = 0;
static unsigned long long Dense_directory_eviction_passes = 0;
static unsigned long long Dense_directory_eviction_bytes = 0;
static unsigned long long Dense_directory_eviction_failures = 0;
static size_t Dense_record_count = 0;
static size_t Dense_record_capacity = 0;
static size_t Dense_active_count = 0;
static unsigned Dense_selector_count = 0;
static unsigned long long Dense_compactions = 0;
static unsigned long long Dense_records_reclaimed = 0;
static unsigned Dense_rewrite_epoch = 1;
static unsigned long long Dense_rewrite_fresh = 0;
static unsigned long long Dense_rewrite_stale = 0;
static unsigned long long Dense_rule_stale = 0;
static unsigned long long Dense_body_bytes = 0;
static unsigned long long Dense_justification_bytes = 0;
static unsigned long long Dense_logical_body_bytes = 0;

static size_t dense_find_record(unsigned long long id);

/* Legacy literal FPA indexes assign one monotonically increasing ID to each
   root atom, in literal order, when a retained clause is first indexed.
   Dense storage destroys those Term objects and later decodes fresh ones.
   Preserve the first ID; the remaining literal-root IDs are consecutive and
   can therefore be reconstructed without a per-literal side allocation. */
static unsigned dense_clause_first_fpa_id(Topform c)
{
  Literals literal;
  unsigned first = 0;
  unsigned offset = 0;
  for (literal = c->literals; literal != NULL; literal = literal->next) {
    unsigned id = (unsigned) FPA_ID(literal->atom);
    if (offset == 0)
      first = id;
    else if ((first == 0 && id != 0) ||
             (first != 0 &&
              (first > UINT_MAX - offset || id != first + offset)))
      fatal_error("dense passive: nonconsecutive literal FPA IDs");
    offset++;
  }
  return first;
}

static void dense_restore_clause_fpa_ids(Topform c, unsigned first)
{
  Literals literal;
  unsigned offset = 0;
  if (first == 0)
    return;
  for (literal = c->literals; literal != NULL; literal = literal->next) {
    if (first > UINT_MAX - offset)
      fatal_error("dense passive: literal FPA ID overflow");
    if (FPA_ID(literal->atom) != 0 &&
        FPA_ID(literal->atom) != first + offset)
      fatal_error("dense passive: materialized literal has wrong FPA ID");
    FPA_ID(literal->atom) = first + offset;
    offset++;
  }
}

static void dense_release_directory(
  struct dense_passive_record *records, size_t capacity, int fd,
  Dense_passive_directory_mode mode)
{
  if (mode == DENSE_DIRECTORY_FILE) {
#ifndef __EMSCRIPTEN__
    if (records != NULL &&
        munmap(records, capacity * sizeof(*records)) != 0)
      fatal_error("dense_release_directory: munmap failed");
    if (fd >= 0 && close(fd) != 0)
      fatal_error("dense_release_directory: close failed");
#else
    (void) records;
    (void) capacity;
    (void) fd;
#endif
  }
  else
    safe_free(records);
}

static void dense_reset_directory_storage(void)
{
  dense_release_directory(Dense_records, Dense_record_capacity,
                          Dense_record_fd, Dense_directory_mode);
  Dense_records = NULL;
  Dense_record_fd = -1;
  Dense_record_capacity = 0;
  Dense_directory_advised_bytes = 0;
  Dense_directory_last_evict_bytes = 0;
}

static void dense_resize_directory(size_t capacity)
{
  if (capacity == 0 ||
      capacity > SIZE_MAX / sizeof(*Dense_records))
    fatal_error("dense_resize_directory: capacity overflow");
  if (Dense_directory_mode == DENSE_DIRECTORY_MEMORY) {
    Dense_records = safe_realloc(
      Dense_records, capacity * sizeof(*Dense_records));
    Dense_record_capacity = capacity;
    return;
  }
#ifndef __EMSCRIPTEN__
  {
    size_t bytes = capacity * sizeof(*Dense_records);
    void *mapping;
    if (Dense_record_fd < 0) {
      Dense_record_fd = open_private_temp_file(
        "prover9-passive-directory-XXXXXX");
      if (Dense_record_fd < 0)
        fatal_error("dense_resize_directory: cannot create backing file");
    }
    if (ftruncate(Dense_record_fd, (off_t) bytes) != 0)
      fatal_error("dense_resize_directory: cannot resize backing file");
    mapping = mmap(NULL, bytes, PROT_READ | PROT_WRITE, MAP_SHARED,
                   Dense_record_fd, 0);
    if (mapping == MAP_FAILED)
      fatal_error("dense_resize_directory: mmap failed");
    if (Dense_records != NULL &&
        munmap(Dense_records,
               Dense_record_capacity * sizeof(*Dense_records)) != 0)
      fatal_error("dense_resize_directory: old munmap failed");
    Dense_records = mapping;
    Dense_record_capacity = capacity;
  }
#else
  fatal_error("dense_resize_directory: file mode is unavailable");
#endif
}

static void dense_evict_directory_pages(void)
{
#if !defined(__EMSCRIPTEN__) && defined(POSIX_FADV_DONTNEED)
  long page_size_long;
  size_t logical, page_size, cold_end, begin;
  if (Dense_directory_mode != DENSE_DIRECTORY_FILE ||
      Dense_records == NULL)
    return;
  logical = Dense_record_count * sizeof(*Dense_records);
  if (logical <= DENSE_DIRECTORY_HOT_WINDOW)
    return;
  if ((Dense_directory_last_evict_bytes == 0 &&
       logical < DENSE_DIRECTORY_HOT_WINDOW +
                   DENSE_DIRECTORY_EVICT_STEP) ||
      (Dense_directory_last_evict_bytes != 0 &&
       logical - Dense_directory_last_evict_bytes <
         DENSE_DIRECTORY_EVICT_STEP))
    return;
  Dense_directory_last_evict_bytes = logical;
  page_size_long = sysconf(_SC_PAGESIZE);
  if (page_size_long <= 0)
    return;
  page_size = (size_t) page_size_long;
  cold_end = (logical - DENSE_DIRECTORY_HOT_WINDOW) /
    page_size * page_size;
  begin = Dense_directory_advised_bytes;
  if (cold_end <= begin)
    return;
  if (msync((unsigned char *) Dense_records + begin,
            cold_end - begin, MS_SYNC) != 0 ||
      madvise((unsigned char *) Dense_records + begin,
              cold_end - begin, MADV_DONTNEED) != 0 ||
      posix_fadvise(Dense_record_fd, (off_t) begin,
                    (off_t) (cold_end - begin),
                    POSIX_FADV_DONTNEED) != 0) {
    Dense_directory_eviction_failures++;
    return;
  }
  Dense_directory_advised_bytes = cold_end;
  Dense_directory_eviction_passes++;
  Dense_directory_eviction_bytes += cold_end - begin;
#endif
}

static unsigned dense_semantics_flags(int semantics)
{
  if (semantics < SEMANTICS_NOT_EVALUATED || semantics > SEMANTICS_FALSE)
    fatal_error("dense_semantics_flags: invalid semantics value");
  return (unsigned) semantics << DENSE_PASSIVE_SEMANTICS_SHIFT;
}

static int dense_record_semantics(const struct dense_passive_record *r)
{
  return (int) ((r->flags & DENSE_PASSIVE_SEMANTICS_MASK) >>
                DENSE_PASSIVE_SEMANTICS_SHIFT);
}

static void dense_record_view(const struct dense_passive_record *r,
                              struct dense_passive_view *view)
{
  view->id = r->id;
  view->hint_id = r->hint_id;
  view->store_position = r->store_position;
  view->weight = r->weight;
  view->simplifier_epoch = r->simplifier_epoch;
  view->rewrite_epoch = r->rewrite_epoch;
  view->first_fpa_id = r->first_fpa_id;
  view->body_bytes = r->body_bytes;
  view->justification_bytes = r->justification_bytes;
  view->logical_body_bytes = r->logical_body_bytes;
  view->semantics = dense_record_semantics(r);
  view->used = (r->flags & DENSE_PASSIVE_USED) != 0;
  view->delayed_demodulator =
    (r->flags & DENSE_PASSIVE_DELAYED) != 0;
  view->rewrite_rule_dirty =
    (r->flags & DENSE_PASSIVE_RULE_DIRTY) != 0;
  view->archive_metadata_dirty =
    (r->flags & DENSE_PASSIVE_ARCHIVE_DIRTY) != 0;
}

static void dense_add_payload(const struct dense_passive_record *r)
{
  if (ULLONG_MAX - Dense_body_bytes < r->body_bytes ||
      ULLONG_MAX - Dense_justification_bytes < r->justification_bytes ||
      ULLONG_MAX - Dense_logical_body_bytes < r->logical_body_bytes)
    fatal_error("dense_add_payload: accounting overflow");
  Dense_body_bytes += r->body_bytes;
  Dense_justification_bytes += r->justification_bytes;
  Dense_logical_body_bytes += r->logical_body_bytes;
}

static void dense_subtract_payload(const struct dense_passive_record *r)
{
  if (Dense_body_bytes < r->body_bytes ||
      Dense_justification_bytes < r->justification_bytes ||
      Dense_logical_body_bytes < r->logical_body_bytes)
    fatal_error("dense_subtract_payload: accounting underflow");
  Dense_body_bytes -= r->body_bytes;
  Dense_justification_bytes -= r->justification_bytes;
  Dense_logical_body_bytes -= r->logical_body_bytes;
}

static size_t dense_grow_capacity(size_t current, size_t element_size,
                                  char *where)
{
  size_t capacity = current == 0 ? 64 : current + current / 2;
  if (capacity <= current || capacity > SIZE_MAX / element_size)
    fatal_error(where);
  return capacity;
}

/*
 * memory management
 */

#define PTRS_GIV_SELECT CEILING(sizeof(struct giv_select), BYTES_POINTER)
static unsigned Giv_select_gets, Giv_select_frees;

/*************
 *
 *   Giv_select get_giv_select()
 *
 *************/

static
Giv_select get_giv_select(void)
{
  Giv_select p = get_cmem(PTRS_GIV_SELECT);
  Giv_select_gets++;
  return(p);
}  /* get_giv_select */

/*************
 *
 *    free_giv_select()
 *
 *************/

static
void free_giv_select(Giv_select p)
{
  free_mem(p, PTRS_GIV_SELECT);
  Giv_select_frees++;
}  /* free_giv_select */

/* PUBLIC */
void configure_dense_passive(BOOL enabled,
                             Dense_passive_archive_fn archive_fn,
                             Dense_passive_activate_fn activate_fn)
{
  Dense_passive = enabled;
  Dense_archive = archive_fn;
  Dense_activate = activate_fn;
  if (enabled && (archive_fn == NULL || activate_fn == NULL))
    fatal_error("configure_dense_passive: callbacks are required");
}  /* configure_dense_passive */

/* PUBLIC */
void configure_dense_passive_directory(
  Dense_passive_directory_mode mode)
{
  if (mode != DENSE_DIRECTORY_MEMORY && mode != DENSE_DIRECTORY_FILE)
    fatal_error("configure_dense_passive_directory: invalid mode");
#ifdef __EMSCRIPTEN__
  if (mode == DENSE_DIRECTORY_FILE)
    fatal_error("configure_dense_passive_directory: file mode unavailable");
#endif
  if (Dense_record_count != 0 || Dense_record_capacity != 0)
    fatal_error("configure_dense_passive_directory: live directory");
  Dense_directory_mode = mode;
  Dense_directory_eviction_passes = 0;
  Dense_directory_eviction_bytes = 0;
  Dense_directory_eviction_failures = 0;
}

/* PUBLIC */
void configure_dense_passive_selectors(Dense_passive_selector_mode mode,
                                       size_t buffer_entries)
{
  if (mode != DENSE_SELECTOR_HEAP && mode != DENSE_SELECTOR_FILE)
    fatal_error("configure_dense_passive_selectors: invalid mode");
#ifdef __EMSCRIPTEN__
  if (mode == DENSE_SELECTOR_FILE)
    fatal_error("configure_dense_passive_selectors: file mode unavailable");
#endif
  if (Dense_record_count != 0 || High.selectors != NULL ||
      Low.selectors != NULL)
    fatal_error("configure_dense_passive_selectors: live selectors");
  if (buffer_entries < 2 ||
      buffer_entries > SIZE_MAX / sizeof(struct dense_selector_entry))
    fatal_error("configure_dense_passive_selectors: invalid buffer size");
  Dense_selector_mode = mode;
  Dense_selector_buffer_limit = buffer_entries;
}

/* PUBLIC */
struct dense_passive_directory_stats dense_passive_directory_stats(void)
{
  struct dense_passive_directory_stats stats;
  memset(&stats, 0, sizeof(stats));
  stats.mode = Dense_directory_mode;
  stats.entry_bytes = (unsigned) sizeof(*Dense_records);
  stats.logical_bytes =
    (unsigned long long) Dense_record_count * sizeof(*Dense_records);
  stats.allocated_bytes =
    (unsigned long long) Dense_record_capacity * sizeof(*Dense_records);
  stats.file_eviction_passes = Dense_directory_eviction_passes;
  stats.file_eviction_bytes = Dense_directory_eviction_bytes;
  stats.file_eviction_failures = Dense_directory_eviction_failures;
  return stats;
}

/* PUBLIC */
BOOL dense_passive_enabled(void)
{
  return Dense_passive;
}

/* PUBLIC */
unsigned long long dense_passive_size(void)
{
  return (unsigned long long) Dense_active_count;
}

/* PUBLIC */
BOOL dense_passive_contains_id(unsigned long long id)
{
  size_t lo = 0, hi = Dense_record_count;
  while (lo < hi) {
    size_t mid = lo + (hi - lo) / 2;
    if (Dense_records[mid].id < id)
      lo = mid + 1;
    else
      hi = mid;
  }
  return lo < Dense_record_count && Dense_records[lo].id == id &&
         (Dense_records[lo].flags & DENSE_PASSIVE_ACTIVE) != 0;
}  /* dense_passive_contains_id */

/* PUBLIC */
BOOL dense_passive_view_id(unsigned long long id,
                           struct dense_passive_view *view)
{
  size_t at = dense_find_record(id);
  if (view == NULL || at == SIZE_MAX ||
      (Dense_records[at].flags & DENSE_PASSIVE_ACTIVE) == 0)
    return FALSE;
  dense_record_view(&Dense_records[at], view);
  return TRUE;
}

/* PUBLIC */
BOOL dense_passive_mark_used(unsigned long long id)
{
  size_t at = dense_find_record(id);
  if (at == SIZE_MAX ||
      (Dense_records[at].flags & DENSE_PASSIVE_ACTIVE) == 0)
    return FALSE;
  if ((Dense_records[at].flags & DENSE_PASSIVE_USED) == 0)
    Dense_records[at].flags |=
      DENSE_PASSIVE_USED | DENSE_PASSIVE_ARCHIVE_DIRTY;
  return TRUE;
}

static size_t dense_find_record(unsigned long long id)
{
  size_t lo = 0, hi = Dense_record_count;
  while (lo < hi) {
    size_t mid = lo + (hi - lo) / 2;
    if (Dense_records[mid].id < id)
      lo = mid + 1;
    else
      hi = mid;
  }
  return lo < Dense_record_count && Dense_records[lo].id == id ? lo :
         SIZE_MAX;
}

/* PUBLIC */
void dense_passive_foreach(Dense_passive_visit_fn visit, void *context)
{
  size_t i;
  if (visit == NULL)
    fatal_error("dense_passive_foreach: null visitor");
  for (i = 0; i < Dense_record_count; i++) {
    struct dense_passive_record *r = &Dense_records[i];
    if ((r->flags & DENSE_PASSIVE_ACTIVE) != 0) {
      struct dense_passive_view view;
      dense_record_view(r, &view);
      visit(&view, context);
    }
  }
}  /* dense_passive_foreach */

/* PUBLIC */
unsigned dense_passive_scan_stale(size_t *cursor, unsigned rewrite_epoch,
                                  int filter, unsigned scan_limit,
                                  struct dense_passive_view *view)
{
  unsigned scanned = 0;
  size_t at;
  if (!Dense_passive || cursor == NULL || view == NULL ||
      Dense_record_count == 0 || scan_limit == 0)
    return 0;
  at = *cursor < Dense_record_count ? *cursor : 0;
  while (scanned < scan_limit && scanned < Dense_record_count) {
    struct dense_passive_record *r = &Dense_records[at];
    scanned++;
    at++;
    if (at == Dense_record_count)
      at = 0;
    if ((r->flags & DENSE_PASSIVE_ACTIVE) != 0 &&
        r->rewrite_epoch < rewrite_epoch &&
        (filter == DENSE_STALE_GENERAL ||
         (filter == DENSE_STALE_HINTED && r->hint_id != 0) ||
         (filter == DENSE_STALE_REWRITE &&
          (r->flags & (DENSE_PASSIVE_DELAYED |
                       DENSE_PASSIVE_RULE_DIRTY)) ==
            (DENSE_PASSIVE_DELAYED | DENSE_PASSIVE_RULE_DIRTY)))) {
      dense_record_view(r, view);
      *cursor = at;
      return scanned;
    }
  }
  *cursor = at;
  return scanned;
}

/* PUBLIC */
unsigned long long dense_passive_cursor_id(size_t cursor)
{
  size_t scanned = 0;
  size_t at;
  if (!Dense_passive || Dense_record_count == 0)
    return 0;
  at = cursor < Dense_record_count ? cursor : 0;
  while (scanned++ < Dense_record_count) {
    struct dense_passive_record *r = &Dense_records[at];
    if ((r->flags & DENSE_PASSIVE_ACTIVE) != 0)
      return r->id;
    if (++at == Dense_record_count)
      at = 0;
  }
  return 0;
}  /* dense_passive_cursor_id */

/* PUBLIC */
size_t dense_passive_cursor_from_id(unsigned long long id)
{
  size_t lo = 0, hi = Dense_record_count, i;
  if (!Dense_passive || Dense_record_count == 0 || id == 0)
    return 0;
  while (lo < hi) {
    size_t mid = lo + (hi - lo) / 2;
    if (Dense_records[mid].id < id)
      lo = mid + 1;
    else
      hi = mid;
  }
  for (i = lo; i < Dense_record_count; i++)
    if ((Dense_records[i].flags & DENSE_PASSIVE_ACTIVE) != 0)
      return i;
  for (i = 0; i < lo; i++)
    if ((Dense_records[i].flags & DENSE_PASSIVE_ACTIVE) != 0)
      return i;
  return 0;
}  /* dense_passive_cursor_from_id */

/* PUBLIC */
unsigned long long dense_passive_stale_count(unsigned rewrite_epoch,
                                             unsigned long long *max_lag)
{
  unsigned long long count = 0, lag = 0;
  size_t i;
  for (i = 0; i < Dense_record_count; i++) {
    struct dense_passive_record *r = &Dense_records[i];
    if ((r->flags & DENSE_PASSIVE_ACTIVE) != 0 &&
        r->rewrite_epoch < rewrite_epoch) {
      unsigned long long current_lag = rewrite_epoch - r->rewrite_epoch;
      count++;
      if (current_lag > lag)
        lag = current_lag;
    }
  }
  if (max_lag != NULL)
    *max_lag = lag;
  return count;
}

/* PUBLIC */
void dense_passive_set_rewrite_epoch(unsigned rewrite_epoch)
{
  if (rewrite_epoch == 0)
    rewrite_epoch = 1;
  if (rewrite_epoch > Dense_rewrite_epoch) {
    Dense_rewrite_stale += Dense_rewrite_fresh;
    Dense_rewrite_fresh = 0;
  }
  else if (rewrite_epoch < Dense_rewrite_epoch) {
    if (Dense_active_count != 0)
      fatal_error("dense_passive_set_rewrite_epoch: cannot rewind live store");
    Dense_rewrite_fresh = 0;
    Dense_rewrite_stale = 0;
    Dense_rule_stale = 0;
  }
  Dense_rewrite_epoch = rewrite_epoch;
}

/* PUBLIC */
unsigned long long dense_passive_rewrite_debt(void)
{
  return Dense_rule_stale;
}

/* PUBLIC */
void dense_passive_memory(unsigned long long *record_bytes,
                          unsigned long long *heap_bytes,
                          unsigned long long *records)
{
  unsigned long long heaps = 0;
  Plist p;
  if (Dense_selector_mode == DENSE_SELECTOR_FILE)
    heaps = dense_passive_selector_stats().buffer_bytes;
  else {
    for (p = High.selectors; p != NULL; p = p->next) {
      Giv_select gs = p->v;
      heaps += (unsigned long long) gs->dense_capacity * sizeof(uint32_t);
    }
    for (p = Low.selectors; p != NULL; p = p->next) {
      Giv_select gs = p->v;
      heaps += (unsigned long long) gs->dense_capacity * sizeof(uint32_t);
    }
  }
  if (record_bytes != NULL)
    *record_bytes = Dense_directory_mode == DENSE_DIRECTORY_MEMORY ?
      (unsigned long long) Dense_record_capacity *
        sizeof(struct dense_passive_record) : 0;
  if (heap_bytes != NULL)
    *heap_bytes = heaps;
  if (records != NULL)
    *records = Dense_active_count;
}  /* dense_passive_memory */

/* PUBLIC */
void dense_passive_payload_memory(unsigned long long *body_bytes,
                                  unsigned long long *justification_bytes,
                                  unsigned long long *logical_body_bytes)
{
  if (body_bytes != NULL)
    *body_bytes = Dense_body_bytes;
  if (justification_bytes != NULL)
    *justification_bytes = Dense_justification_bytes;
  if (logical_body_bytes != NULL)
    *logical_body_bytes = Dense_logical_body_bytes;
}  /* dense_passive_payload_memory */

/* PUBLIC */
unsigned long long dense_passive_delayed_demodulators(void)
{
  unsigned long long count = 0;
  size_t i;
  for (i = 0; i < Dense_record_count; i++)
    if ((Dense_records[i].flags &
         (DENSE_PASSIVE_ACTIVE | DENSE_PASSIVE_DELAYED)) ==
        (DENSE_PASSIVE_ACTIVE | DENSE_PASSIVE_DELAYED))
      count++;
  return count;
}  /* dense_passive_delayed_demodulators */

/* PUBLIC */
BOOL dense_passive_compaction_needed(void)
{
  size_t inactive = Dense_record_count - Dense_active_count;
  return Dense_passive && Dense_record_count >= 1024 && inactive >= 256 &&
         inactive >= (Dense_active_count + 1) / 2;
}

/* PUBLIC */
void dense_passive_compaction_stats(unsigned long long *compactions,
                                    unsigned long long *records_reclaimed)
{
  if (compactions != NULL)
    *compactions = Dense_compactions;
  if (records_reclaimed != NULL)
    *records_reclaimed = Dense_records_reclaimed;
}

static int dense_compare(Giv_select gs, uint32_t ai, uint32_t bi)
{
  /* Dense records are appended in strictly increasing proof-ID order and
     compaction preserves that order.  Age comparison therefore needs no
     access to the (possibly file-backed) directory. */
  if (gs->order == GS_ORDER_AGE)
    return ai < bi ? -1 : ai > bi ? 1 : 0;
  struct dense_passive_record *a = &Dense_records[ai];
  struct dense_passive_record *b = &Dense_records[bi];
  if (gs->order == GS_ORDER_WEIGHT) {
    if (a->weight < b->weight) return -1;
    if (a->weight > b->weight) return 1;
  }
  else if (gs->order == GS_ORDER_HINT_AGE) {
    if (a->hint_id != 0 && b->hint_id == 0) return -1;
    if (a->hint_id == 0 && b->hint_id != 0) return 1;
    if (a->hint_id < b->hint_id) return -1;
    if (a->hint_id > b->hint_id) return 1;
  }
  if (a->id < b->id) return -1;
  if (a->id > b->id) return 1;
  return 0;
}

static int dense_entry_compare(int order,
                               const struct dense_selector_entry *a,
                               const struct dense_selector_entry *b)
{
  if (order == GS_ORDER_WEIGHT) {
    if (a->key.weight < b->key.weight) return -1;
    if (a->key.weight > b->key.weight) return 1;
  }
  else if (order == GS_ORDER_HINT_AGE) {
    if (a->key.hint_id != 0 && b->key.hint_id == 0) return -1;
    if (a->key.hint_id == 0 && b->key.hint_id != 0) return 1;
    if (a->key.hint_id < b->key.hint_id) return -1;
    if (a->key.hint_id > b->key.hint_id) return 1;
  }
  /* Record order is clause-ID order; see dense_insert_passive() and
     dense_passive_compact(). */
  if (a->record < b->record) return -1;
  if (a->record > b->record) return 1;
  return 0;
}

static int Dense_entry_sort_order = GS_ORDER_AGE;

/* PUBLIC */
unsigned long long dense_passive_file_record_reference(size_t record)
{
  return (uint64_t) record;
}

/* PUBLIC */
BOOL dense_passive_file_record_index(unsigned long long reference,
                                     size_t *record)
{
  size_t decoded = (size_t) reference;
  if (record == NULL || (unsigned long long) decoded != reference)
    return FALSE;
  *record = decoded;
  return TRUE;
}

static int dense_entry_qsort_compare(const void *va, const void *vb)
{
  return dense_entry_compare(
    Dense_entry_sort_order,
    (const struct dense_selector_entry *) va,
    (const struct dense_selector_entry *) vb);
}

static void dense_selector_write_all(Giv_select gs, int fd,
                                     const void *buffer, size_t bytes)
{
#ifndef __EMSCRIPTEN__
  const unsigned char *p = buffer;
  size_t done = 0;
  while (done < bytes) {
    ssize_t n = write(fd, p + done, bytes - done);
    if (n < 0 && errno == EINTR)
      continue;
    if (n <= 0)
      fatal_error("dense_selector_write_all: write failed");
    done += (size_t) n;
  }
  gs->dense_file_writes++;
  gs->dense_file_write_bytes += bytes;
#else
  (void) gs;
  (void) fd;
  (void) buffer;
  (void) bytes;
  fatal_error("dense_selector_write_all: file mode unavailable");
#endif
}

static void dense_selector_run_init(struct dense_selector_run *run)
{
  memset(run, 0, sizeof(*run));
  run->fd = -1;
}

static void dense_selector_run_close(struct dense_selector_run *run)
{
  if (run->fd >= 0 && close(run->fd) != 0)
    fatal_error("dense_selector_run_close: close failed");
  safe_free(run->read_buffer);
  dense_selector_run_init(run);
}

static unsigned long long dense_selector_run_entries(
  const struct dense_selector_run *run)
{
  return run->remaining + (run->head_valid ? 1 : 0);
}

static BOOL dense_selector_run_next(Giv_select gs,
                                    struct dense_selector_run *run,
                                    struct dense_selector_entry *entry)
{
#ifndef __EMSCRIPTEN__
  if (run->remaining == 0)
    return FALSE;
  if (run->read_at == run->read_size) {
    size_t request = run->remaining < DENSE_SELECTOR_READ_ENTRIES ?
      (size_t) run->remaining : DENSE_SELECTOR_READ_ENTRIES;
    size_t bytes = request * sizeof(*run->read_buffer);
    size_t done = 0;
#if defined(POSIX_FADV_DONTNEED)
    off_t block_offset = run->next_offset;
#endif
    if (run->read_buffer == NULL)
      run->read_buffer = safe_malloc(
        DENSE_SELECTOR_READ_ENTRIES * sizeof(*run->read_buffer));
    while (done < bytes) {
      ssize_t n = pread(run->fd, (unsigned char *) run->read_buffer + done,
                        bytes - done, run->next_offset + (off_t) done);
      if (n < 0 && errno == EINTR)
        continue;
      if (n <= 0)
        fatal_error("dense_selector_run_next: read failed");
      done += (size_t) n;
    }
    run->next_offset += (off_t) bytes;
    run->read_size = request;
    run->read_at = 0;
    gs->dense_file_reads++;
    gs->dense_file_read_bytes += bytes;
#if defined(POSIX_FADV_DONTNEED)
    /* pread has copied the immutable block into the bounded userspace buffer;
       neither selection nor a merge will revisit this file range.  Discard it
       now so a long-lived run cannot turn consumed history into cgroup cache. */
    if (posix_fadvise(run->fd, block_offset, (off_t) bytes,
                      POSIX_FADV_DONTNEED) != 0)
      gs->dense_file_read_eviction_failures++;
    else {
      gs->dense_file_read_evictions++;
      gs->dense_file_read_eviction_bytes += bytes;
    }
#endif
  }
  *entry = run->read_buffer[run->read_at++];
  run->remaining--;
  return TRUE;
#else
  (void) gs;
  (void) run;
  (void) entry;
  fatal_error("dense_selector_run_next: file mode unavailable");
  return FALSE;
#endif
}

static BOOL dense_selector_run_stream_next(
  Giv_select gs, struct dense_selector_run *run,
  struct dense_selector_entry *entry)
{
  if (run->head_valid) {
    *entry = run->head;
    run->head_valid = FALSE;
    return TRUE;
  }
  return dense_selector_run_next(gs, run, entry);
}

static void dense_selector_run_evict(Giv_select gs,
                                     struct dense_selector_run *run)
{
#if !defined(__EMSCRIPTEN__) && defined(POSIX_FADV_DONTNEED)
  unsigned long long bytes = dense_selector_run_entries(run) *
    sizeof(struct dense_selector_entry);
  if (fsync(run->fd) != 0 ||
      posix_fadvise(run->fd, 0, 0, POSIX_FADV_DONTNEED) != 0) {
    gs->dense_file_eviction_failures++;
    return;
  }
  gs->dense_file_evictions++;
  gs->dense_file_eviction_bytes += bytes;
#else
  (void) gs;
  (void) run;
#endif
}

static struct dense_selector_run dense_selector_run_from_buffer(
  Giv_select gs, const struct dense_selector_entry *entries, size_t count)
{
  struct dense_selector_run run;
  dense_selector_run_init(&run);
#ifndef __EMSCRIPTEN__
  run.fd = open_private_temp_file("prover9-selector-XXXXXX");
  if (run.fd < 0)
    fatal_error("dense_selector_run_from_buffer: cannot create backing file");
  dense_selector_write_all(gs, run.fd, entries,
                           count * sizeof(*entries));
  run.remaining = count;
  run.next_offset = 0;
#else
  (void) gs;
  (void) entries;
  (void) count;
  fatal_error("dense_selector_run_from_buffer: file mode unavailable");
#endif
  return run;
}

static struct dense_selector_run dense_selector_merge_runs(
  Giv_select gs, struct dense_selector_run *a,
  struct dense_selector_run *b)
{
  struct dense_selector_run output;
  struct dense_selector_entry ae, be;
  struct dense_selector_entry out[DENSE_SELECTOR_READ_ENTRIES];
  size_t out_size = 0;
  unsigned long long count = 0;
  BOOL have_a, have_b;
  dense_selector_run_init(&output);
#ifndef __EMSCRIPTEN__
  output.fd = open_private_temp_file("prover9-selector-XXXXXX");
  if (output.fd < 0)
    fatal_error("dense_selector_merge_runs: cannot create backing file");
  have_a = dense_selector_run_stream_next(gs, a, &ae);
  have_b = dense_selector_run_stream_next(gs, b, &be);
  while (have_a || have_b) {
    if (!have_b || (have_a && dense_entry_compare(gs->order, &ae, &be) <= 0)) {
      out[out_size++] = ae;
      have_a = dense_selector_run_stream_next(gs, a, &ae);
    }
    else {
      out[out_size++] = be;
      have_b = dense_selector_run_stream_next(gs, b, &be);
    }
    count++;
    if (out_size == DENSE_SELECTOR_READ_ENTRIES) {
      dense_selector_write_all(gs, output.fd, out, sizeof(out));
      out_size = 0;
    }
  }
  if (out_size != 0)
    dense_selector_write_all(gs, output.fd, out,
                             out_size * sizeof(*out));
  output.remaining = count;
  output.next_offset = 0;
  dense_selector_run_close(a);
  dense_selector_run_close(b);
#else
  (void) gs;
  (void) a;
  (void) b;
  fatal_error("dense_selector_merge_runs: file mode unavailable");
#endif
  return output;
}

static unsigned dense_selector_run_count(Giv_select gs)
{
  unsigned level, count = 0;
  for (level = 0; level < DENSE_SELECTOR_RUN_LEVELS; level++)
    if (gs->dense_runs[level].fd >= 0)
      count++;
  return count;
}

static void dense_selector_flush(Giv_select gs)
{
  struct dense_selector_run incoming;
  unsigned level;
  if (gs->dense_buffer_size == 0)
    return;
  Dense_entry_sort_order = gs->order;
  qsort(gs->dense_buffer, gs->dense_buffer_size,
        sizeof(*gs->dense_buffer), dense_entry_qsort_compare);
  incoming = dense_selector_run_from_buffer(
    gs, gs->dense_buffer, gs->dense_buffer_size);
  gs->dense_buffer_size = 0;
  gs->dense_flushes++;
  for (level = 0; level < DENSE_SELECTOR_RUN_LEVELS; level++) {
    if (gs->dense_runs[level].fd < 0) {
      gs->dense_runs[level] = incoming;
      dense_selector_run_init(&incoming);
      dense_selector_run_evict(gs, &gs->dense_runs[level]);
      break;
    }
    incoming = dense_selector_merge_runs(
      gs, &gs->dense_runs[level], &incoming);
    gs->dense_merges++;
  }
  if (level == DENSE_SELECTOR_RUN_LEVELS)
    fatal_error("dense_selector_flush: run-level overflow");
  {
    unsigned runs = dense_selector_run_count(gs);
    if (runs > gs->dense_peak_runs)
      gs->dense_peak_runs = runs;
  }
}

static void dense_selector_buffer_remove_root(Giv_select gs)
{
  struct dense_selector_entry last;
  size_t i = 0;
  if (gs->dense_buffer_size == 0)
    return;
  last = gs->dense_buffer[--gs->dense_buffer_size];
  while (i * 2 + 1 < gs->dense_buffer_size) {
    size_t child = i * 2 + 1;
    if (child + 1 < gs->dense_buffer_size &&
        dense_entry_compare(gs->order, &gs->dense_buffer[child+1],
                            &gs->dense_buffer[child]) < 0)
      child++;
    if (dense_entry_compare(gs->order, &last,
                            &gs->dense_buffer[child]) <= 0)
      break;
    gs->dense_buffer[i] = gs->dense_buffer[child];
    i = child;
  }
  if (gs->dense_buffer_size != 0)
    gs->dense_buffer[i] = last;
}

static void dense_selector_file_push(Giv_select gs, size_t record)
{
  struct dense_passive_record *r = &Dense_records[record];
  struct dense_selector_entry entry;
  size_t i;
  if (gs->dense_buffer_size == gs->dense_buffer_capacity) {
    size_t capacity = dense_grow_capacity(
      gs->dense_buffer_capacity, sizeof(*gs->dense_buffer),
      "dense_selector_file_push: capacity overflow");
    if (capacity > Dense_selector_buffer_limit)
      capacity = Dense_selector_buffer_limit;
    if (capacity <= gs->dense_buffer_capacity)
      fatal_error("dense_selector_file_push: buffer limit overflow");
    gs->dense_buffer = safe_realloc(
      gs->dense_buffer, capacity * sizeof(*gs->dense_buffer));
    gs->dense_buffer_capacity = capacity;
  }
  if (gs->order == GS_ORDER_WEIGHT)
    entry.key.weight = r->weight;
  else if (gs->order == GS_ORDER_HINT_AGE)
    entry.key.hint_id = r->hint_id;
  else
    entry.key.hint_id = 0;
  entry.record = dense_passive_file_record_reference(record);
  i = gs->dense_buffer_size++;
  while (i > 0) {
    size_t parent = (i - 1) / 2;
    if (dense_entry_compare(gs->order, &gs->dense_buffer[parent],
                            &entry) <= 0)
      break;
    gs->dense_buffer[i] = gs->dense_buffer[parent];
    i = parent;
  }
  gs->dense_buffer[i] = entry;
  if (gs->dense_buffer_size == Dense_selector_buffer_limit)
    dense_selector_flush(gs);
}

static BOOL dense_selector_file_min(Giv_select gs,
                                    struct dense_selector_entry *entry,
                                    int *source)
{
  unsigned level;
  BOOL found = FALSE;
  gs->dense_file_min_calls++;
  if (gs->dense_buffer_size != 0) {
    gs->dense_file_buffer_checks++;
    *entry = gs->dense_buffer[0];
    *source = -1;
    found = TRUE;
  }
  for (level = 0; level < DENSE_SELECTOR_RUN_LEVELS; level++) {
    struct dense_selector_run *run = &gs->dense_runs[level];
    if (run->fd < 0)
      continue;
    gs->dense_file_run_checks++;
    if (!run->head_valid) {
      if (!dense_selector_run_next(gs, run, &run->head)) {
        dense_selector_run_close(run);
        continue;
      }
      run->head_valid = TRUE;
    }
    if (!found || dense_entry_compare(gs->order, &run->head, entry) < 0) {
      *entry = run->head;
      *source = (int) level;
      found = TRUE;
    }
  }
  return found;
}

static void dense_selector_file_remove_min(Giv_select gs, int source)
{
  if (source < 0)
    dense_selector_buffer_remove_root(gs);
  else
    gs->dense_runs[source].head_valid = FALSE;
}

static void dense_selector_file_reset(Giv_select gs)
{
  unsigned level;
  safe_free(gs->dense_buffer);
  gs->dense_buffer = NULL;
  gs->dense_buffer_size = 0;
  gs->dense_buffer_capacity = 0;
  for (level = 0; level < DENSE_SELECTOR_RUN_LEVELS; level++)
    dense_selector_run_close(&gs->dense_runs[level]);
}

static void dense_selector_initialize(Giv_select gs)
{
  unsigned level;
  gs->dense_heap = NULL;
  gs->dense_size = 0;
  gs->dense_capacity = 0;
  gs->dense_active = 0;
  gs->dense_buffer = NULL;
  gs->dense_buffer_size = 0;
  gs->dense_buffer_capacity = 0;
  for (level = 0; level < DENSE_SELECTOR_RUN_LEVELS; level++)
    dense_selector_run_init(&gs->dense_runs[level]);
  gs->dense_peak_runs = 0;
  gs->dense_flushes = 0;
  gs->dense_merges = 0;
  gs->dense_file_reads = 0;
  gs->dense_file_read_bytes = 0;
  gs->dense_file_writes = 0;
  gs->dense_file_write_bytes = 0;
  gs->dense_file_evictions = 0;
  gs->dense_file_eviction_bytes = 0;
  gs->dense_file_eviction_failures = 0;
  gs->dense_file_read_evictions = 0;
  gs->dense_file_read_eviction_bytes = 0;
  gs->dense_file_read_eviction_failures = 0;
  gs->dense_file_min_calls = 0;
  gs->dense_file_buffer_checks = 0;
  gs->dense_file_run_checks = 0;
  gs->dense_stale_entries_discarded = 0;
}

static void dense_selector_add_stats(
  Giv_select gs, struct dense_passive_selector_stats *stats)
{
  unsigned level;
  stats->buffered_entries += gs->dense_buffer_size;
  if (gs->dense_buffer != NULL)
    stats->buffer_bytes +=
      (unsigned long long) gs->dense_buffer_capacity *
        sizeof(*gs->dense_buffer);
  for (level = 0; level < DENSE_SELECTOR_RUN_LEVELS; level++) {
    struct dense_selector_run *run = &gs->dense_runs[level];
    if (run->fd >= 0) {
      unsigned long long entries = dense_selector_run_entries(run);
      struct stat sb;
      stats->runs++;
      stats->run_entries += entries;
      stats->run_logical_bytes +=
        entries * sizeof(struct dense_selector_entry);
#ifndef __EMSCRIPTEN__
      if (fstat(run->fd, &sb) == 0)
        stats->run_physical_bytes +=
          (unsigned long long) sb.st_blocks * 512ULL;
#endif
    }
    if (run->read_buffer != NULL)
      stats->buffer_bytes +=
        DENSE_SELECTOR_READ_ENTRIES * sizeof(*run->read_buffer);
  }
  stats->peak_runs += gs->dense_peak_runs;
  stats->flushes += gs->dense_flushes;
  stats->merges += gs->dense_merges;
  stats->file_reads += gs->dense_file_reads;
  stats->file_read_bytes += gs->dense_file_read_bytes;
  stats->file_writes += gs->dense_file_writes;
  stats->file_write_bytes += gs->dense_file_write_bytes;
  stats->file_evictions += gs->dense_file_evictions;
  stats->file_eviction_bytes += gs->dense_file_eviction_bytes;
  stats->file_eviction_failures += gs->dense_file_eviction_failures;
  stats->file_read_evictions += gs->dense_file_read_evictions;
  stats->file_read_eviction_bytes += gs->dense_file_read_eviction_bytes;
  stats->file_read_eviction_failures +=
    gs->dense_file_read_eviction_failures;
  stats->file_min_calls += gs->dense_file_min_calls;
  stats->file_buffer_checks += gs->dense_file_buffer_checks;
  stats->file_run_checks += gs->dense_file_run_checks;
  stats->stale_entries_discarded += gs->dense_stale_entries_discarded;
}

/* PUBLIC */
struct dense_passive_selector_stats dense_passive_selector_stats(void)
{
  struct dense_passive_selector_stats stats;
  Plist p;
  memset(&stats, 0, sizeof(stats));
  stats.mode = Dense_selector_mode;
  stats.buffer_limit = Dense_selector_buffer_limit;
  stats.record_reference_bits = (unsigned)
    ((sizeof(size_t) <
      sizeof(((struct dense_selector_entry *) 0)->record) ?
      sizeof(size_t) :
      sizeof(((struct dense_selector_entry *) 0)->record)) * CHAR_BIT);
  stats.entry_bytes = (unsigned) sizeof(struct dense_selector_entry);
  for (p = High.selectors; p != NULL; p = p->next)
    dense_selector_add_stats(p->v, &stats);
  for (p = Low.selectors; p != NULL; p = p->next)
    dense_selector_add_stats(p->v, &stats);
  return stats;
}

static void dense_heap_push(Giv_select gs, uint32_t record)
{
  size_t i;
  if (gs->dense_size == gs->dense_capacity) {
    size_t capacity = dense_grow_capacity(gs->dense_capacity,
                                          sizeof(uint32_t),
                                          "dense_heap_push: capacity overflow");
    gs->dense_heap = safe_realloc(gs->dense_heap,
                                  capacity * sizeof(uint32_t));
    gs->dense_capacity = capacity;
  }
  i = gs->dense_size++;
  while (i > 0) {
    size_t parent = (i - 1) / 2;
    if (dense_compare(gs, gs->dense_heap[parent], record) <= 0)
      break;
    gs->dense_heap[i] = gs->dense_heap[parent];
    i = parent;
  }
  gs->dense_heap[i] = record;
}

static void dense_selector_push(Giv_select gs, size_t record)
{
  if (Dense_selector_mode == DENSE_SELECTOR_FILE)
    dense_selector_file_push(gs, record);
  else {
    if (record > UINT32_MAX)
      fatal_error("dense_selector_push: heap record index overflow");
    dense_heap_push(gs, (uint32_t) record);
  }
  gs->dense_active++;
}

static void dense_heap_remove_root(Giv_select gs)
{
  uint32_t last;
  size_t i = 0;
  if (gs->dense_size == 0)
    return;
  last = gs->dense_heap[--gs->dense_size];
  while (i * 2 + 1 < gs->dense_size) {
    size_t child = i * 2 + 1;
    if (child + 1 < gs->dense_size &&
        dense_compare(gs, gs->dense_heap[child+1],
                      gs->dense_heap[child]) < 0)
      child++;
    if (dense_compare(gs, last, gs->dense_heap[child]) <= 0)
      break;
    gs->dense_heap[i] = gs->dense_heap[child];
    i = child;
  }
  if (gs->dense_size != 0)
    gs->dense_heap[i] = last;
}

static BOOL dense_selector_peek(Giv_select gs, size_t *record)
{
  unsigned long long bit = 1ULL << gs->dense_bit;
  if (Dense_selector_mode == DENSE_SELECTOR_FILE) {
    struct dense_selector_entry entry;
    int source = -1;
    while (dense_selector_file_min(gs, &entry, &source)) {
      size_t at;
      if (dense_passive_file_record_index(entry.record, &at) &&
          at < Dense_record_count) {
        struct dense_passive_record *r = &Dense_records[at];
        if ((r->flags & DENSE_PASSIVE_ACTIVE) != 0 &&
            (r->selector_mask & bit) != 0) {
          *record = at;
          return TRUE;
        }
      }
      dense_selector_file_remove_min(gs, source);
      gs->dense_stale_entries_discarded++;
    }
    return FALSE;
  }
  while (gs->dense_size != 0) {
    struct dense_passive_record *r =
      &Dense_records[gs->dense_heap[0]];
    if ((r->flags & DENSE_PASSIVE_ACTIVE) != 0 &&
        (r->selector_mask & bit) != 0) {
      *record = gs->dense_heap[0];
      return TRUE;
    }
    dense_heap_remove_root(gs);
  }
  return FALSE;
}

static void dense_selector_drop_empty_file_runs(Giv_select gs)
{
  if (Dense_selector_mode == DENSE_SELECTOR_FILE &&
      gs->dense_active == 0 &&
      (gs->dense_buffer_size != 0 || dense_selector_run_count(gs) != 0))
    dense_selector_file_reset(gs);
}

static void dense_selector_clear_contents(Giv_select gs)
{
  safe_free(gs->dense_heap);
  gs->dense_heap = NULL;
  gs->dense_size = 0;
  gs->dense_capacity = 0;
  dense_selector_file_reset(gs);
  gs->dense_active = 0;
}

/* PUBLIC */
void dense_passive_compact(Dense_passive_relocate_fn relocate,
                           void *context)
{
  struct dense_passive_record *old_records = Dense_records;
  size_t old_count = Dense_record_count;
  size_t old_capacity = Dense_record_capacity;
  int old_fd = Dense_record_fd;
  size_t active = Dense_active_count;
  size_t capacity = 0;
  size_t i, n = 0;
  Plist p;

  if (!Dense_passive || relocate == NULL)
    fatal_error("dense_passive_compact: invalid state or callback");
  Dense_records = NULL;
  Dense_record_fd = -1;
  Dense_record_capacity = 0;
  Dense_directory_advised_bytes = 0;
  Dense_directory_last_evict_bytes = 0;
  if (active != 0) {
    capacity = active + active / 8 + 16;
    if (capacity < active ||
        capacity > SIZE_MAX / sizeof(struct dense_passive_record))
      fatal_error("dense_passive_compact: capacity overflow");
    dense_resize_directory(capacity);
  }

  for (i = 0; i < old_count; i++) {
    if ((old_records[i].flags & DENSE_PASSIVE_ACTIVE) != 0) {
      struct dense_passive_record r = old_records[i];
      r.store_position = relocate(r.store_position, context);
      if (r.store_position == SIZE_MAX)
        fatal_error("dense_passive_compact: passive relocation failed");
      Dense_records[n++] = r;
    }
  }
  if (n != active)
    fatal_error("dense_passive_compact: active-record count mismatch");
  dense_release_directory(old_records, old_capacity, old_fd,
                          Dense_directory_mode);
  Dense_record_count = active;
  if (active == 0)
    Dense_record_capacity = 0;

  High.occurrences = 0;
  for (p = High.selectors; p != NULL; p = p->next) {
    Giv_select gs = p->v;
    dense_selector_clear_contents(gs);
  }
  Low.occurrences = 0;
  for (p = Low.selectors; p != NULL; p = p->next) {
    Giv_select gs = p->v;
    dense_selector_clear_contents(gs);
  }

  for (i = 0; i < active; i++) {
    struct dense_passive_record *r = &Dense_records[i];
    for (p = High.selectors; p != NULL; p = p->next) {
      Giv_select gs = p->v;
      if ((r->selector_mask & (1ULL << gs->dense_bit)) != 0) {
        dense_selector_push(gs, i);
        High.occurrences++;
      }
    }
    for (p = Low.selectors; p != NULL; p = p->next) {
      Giv_select gs = p->v;
      if ((r->selector_mask & (1ULL << gs->dense_bit)) != 0) {
        dense_selector_push(gs, i);
        Low.occurrences++;
      }
    }
  }
  Dense_compactions++;
  Dense_records_reclaimed += old_count - active;
}  /* dense_passive_compact */

static size_t selector_size(Giv_select gs)
{
  return Dense_passive ? gs->dense_active : (size_t) avl_size(gs->idx);
}

/*************
 *
 *   current_cycle_size()
 *
 *************/

static
int current_cycle_size(Select_state s)
{
  int sum = 0;
  Plist p;
  for (p = s->selectors; p; p = p->next) {
    Giv_select gs = p->v;
    if (selector_size(gs) > 0)
      sum += gs->part;
  }
  return sum;
}  /* current_cycle_size */

/*************
 *
 *   reset_selector_indexes()
 *
 *   Clear all selector AVL trees and counters.  Used by checkpoint
 *   restore to discard stale entries before reinserting from checkpoint.
 *
 *************/

/* PUBLIC */
void reset_selector_indexes(void)
{
  Plist p;
  for (p = High.selectors; p; p = p->next) {
    Giv_select gs = p->v;
    gs->idx = NULL;  /* leak old AVL nodes (small, one-time) */
    dense_selector_clear_contents(gs);
    gs->selected = 0;
  }
  High.occurrences = 0;
  for (p = Low.selectors; p; p = p->next) {
    Giv_select gs = p->v;
    gs->idx = NULL;
    dense_selector_clear_contents(gs);
    gs->selected = 0;
  }
  Low.occurrences = 0;
  dense_reset_directory_storage();
  Dense_record_count = 0;
  Dense_active_count = 0;
  Dense_rewrite_epoch = 1;
  Dense_rewrite_fresh = 0;
  Dense_rewrite_stale = 0;
  Dense_rule_stale = 0;
  Dense_body_bytes = 0;
  Dense_justification_bytes = 0;
  Dense_logical_body_bytes = 0;
  Dense_compactions = 0;
  Dense_records_reclaimed = 0;
  Sos_size = 0;
}  /* reset_selector_indexes */

/*************
 *
 *   init_giv_select()
 *
 *************/

/* DOCUMENTATION
*/

/* PUBLIC */
void init_giv_select(Plist rules)
{
  Plist p;

  for (p = rules; p; p = p->next) {
    Term t = p->v;
    int n = 0;
    Term order_term;
    Term property_term;
    Giv_select gs;
    if (!is_term(t, "=", 2) ||
	!is_term(ARG(t,0), "part", 4) ||
	!CONSTANT(ARG(ARG(t,0),0)) ||
	!(is_constant(ARG(ARG(t,0),1), "high") ||
	  is_constant(ARG(ARG(t,0),1), "low")) ||
	!((n = natural_constant_term(ARG(t,1))) > 0))
      fatal_error("Given selection rule must be: "
		  "part(<name>,high|low,age|wt|random,<property>)=<n>");

    order_term = ARG(ARG(t,0),2);
    property_term = ARG(ARG(t,0),3);
    gs = get_giv_select();
    dense_selector_initialize(gs);
    if (Dense_selector_count >= 64)
      fatal_error("dense passive supports at most 64 given selectors");
    gs->dense_bit = Dense_selector_count++;
    
    if (is_constant(ARG(ARG(t,0),1), "high")) {
      High.selectors = plist_append(High.selectors, gs);
      if (n > INT_MAX - High.cycle_size)
	High.cycle_size = INT_MAX;  /* saturate to avoid overflow */
      else
	High.cycle_size += n;
    }
    else {
      Low.selectors  = plist_append(Low.selectors,  gs);
      if (n > INT_MAX - Low.cycle_size)
	Low.cycle_size = INT_MAX;  /* saturate to avoid overflow */
      else
	Low.cycle_size += n;
    }

    gs->name = term_symbol(ARG(ARG(t,0),0));
    gs->part = n;
    if (is_constant(order_term,"weight")) {
      gs->order = GS_ORDER_WEIGHT;
      gs->compare = (Ordertype (*) (void *, void *)) cl_wt_id_compare;
    }
    else if (is_constant(order_term,"age")) {
      gs->order = GS_ORDER_AGE;
      gs->compare = (Ordertype (*) (void *, void *)) cl_id_compare;
    }
    else if (is_constant(order_term,"hint_age")) {
      gs->order = GS_ORDER_HINT_AGE;
      gs->compare = (Ordertype (*) (void *, void *)) cl_hint_id_compare;
    }
    else if (is_constant(order_term,"random")) {
      if (Dense_passive)
        fatal_error("passive_store=dense does not yet support random selection");
      gs->order = GS_ORDER_RANDOM;
      gs->compare = (Ordertype (*) (void *, void *)) cl_id_compare;
    }
    else
      fatal_error("Given selection order must be weight, age, hint_age, or random.");
    gs->property = compile_clause_eval_rule(property_term);
    if (gs->property == NULL)
      fatal_error("Error in clause-property expression of given selection rule");
    else if (rule_contains_semantics(gs->property))
      Rule_needs_semantics = TRUE;
  }
  High.current = High.selectors;
  Low.current = Low.selectors;
}  /* init_giv_select */

/*************
 *
 *   update_selectors()
 *
 *************/

static
void update_selectors(Topform c, BOOL insert)
{
  BOOL matched = FALSE;
  Plist p;
  for (p = High.selectors; p; p = p->next) {
    Giv_select gs = p->v;
    if (eval_clause_in_rule(c, gs->property)) {
      matched = TRUE;
      if (insert) {
	gs->idx = avl_insert(gs->idx, c, gs->compare);
	High.occurrences++;
      }
      else {
	gs->idx = avl_delete(gs->idx, c, gs->compare);
	High.occurrences--;
      }
    }
  }
  /* If it is high-priority, don't let it also be low priority. */
  if (!matched) {
    for (p = Low.selectors; p; p = p->next) {
      Giv_select gs = p->v;
      if (eval_clause_in_rule(c, gs->property)) {
	matched = TRUE;
	if (insert) {
	  gs->idx = avl_insert(gs->idx, c, gs->compare);
	  Low.occurrences++;
	}
	else {
	  gs->idx = avl_delete(gs->idx, c, gs->compare);
	  Low.occurrences--;
	}
      }
    }
  }
  if (!matched) {
    static BOOL Already_warned = FALSE;

    if (!Already_warned) {
      fprintf(stderr, "\n\nWARNING: one or more kept clauses do not match "
	     "any given_selection rules (see output).\n\n");
      printf("\nWARNING: the following clause does not match "
	     "any given_selection rules.\n"
	     "This message will not be repeated.\n");
      f_clause(c);
      Already_warned = TRUE;
    }
  }
}  /* update_selectors */

static unsigned long long dense_selector_mask(Topform c)
{
  unsigned long long mask = 0;
  BOOL matched = FALSE;
  Plist p;
  for (p = High.selectors; p != NULL; p = p->next) {
    Giv_select gs = p->v;
    if (eval_clause_in_rule(c, gs->property)) {
      matched = TRUE;
      mask |= 1ULL << gs->dense_bit;
    }
  }
  if (!matched) {
    for (p = Low.selectors; p != NULL; p = p->next) {
      Giv_select gs = p->v;
      if (eval_clause_in_rule(c, gs->property)) {
        matched = TRUE;
        mask |= 1ULL << gs->dense_bit;
      }
    }
  }
  if (!matched) {
    static BOOL Already_warned_dense = FALSE;
    if (!Already_warned_dense) {
      fprintf(stderr, "\n\nWARNING: one or more kept clauses do not match "
              "any given_selection rules (see output).\n\n");
      printf("\nWARNING: the following clause does not match "
             "any given_selection rules.\n"
             "This message will not be repeated.\n");
      f_clause(c);
      Already_warned_dense = TRUE;
    }
  }
  return mask;
}

/* PUBLIC */
void given_selection_preview(Topform c,
			     unsigned long long *selector_mask,
			     unsigned *priority)
{
  unsigned long long mask = 0;
  BOOL matched = FALSE;
  Plist p;

  if (Rule_needs_semantics)
    set_semantics(c);
  for (p = High.selectors; p != NULL; p = p->next) {
    Giv_select gs = p->v;
    if (eval_clause_in_rule(c, gs->property)) {
      matched = TRUE;
      mask |= 1ULL << gs->dense_bit;
    }
  }
  if (matched) {
    if (priority != NULL)
      *priority = 0;
  }
  else {
    for (p = Low.selectors; p != NULL; p = p->next) {
      Giv_select gs = p->v;
      if (eval_clause_in_rule(c, gs->property)) {
        matched = TRUE;
        mask |= 1ULL << gs->dense_bit;
      }
    }
    if (priority != NULL)
      *priority = matched ? 1 : 2;
  }
  if (selector_mask != NULL)
    *selector_mask = mask;
}  /* given_selection_preview */

static void dense_insert_passive(Topform c)
{
  struct dense_passive_record r;
  size_t record;
  Plist p;
  if (c->id == 0)
    fatal_error("dense_insert_passive: clause has no ID");
  if (Dense_selector_mode == DENSE_SELECTOR_HEAP &&
      Dense_record_count > UINT32_MAX)
    fatal_error("dense_insert_passive: heap record index overflow");
  if (Dense_record_count != 0 &&
      Dense_records[Dense_record_count-1].id >= c->id)
    fatal_error("dense_insert_passive: clause IDs are not increasing");
  if (Rule_needs_semantics)
    set_semantics(c);
  memset(&r, 0, sizeof(r));
  r.id = c->id;
  r.hint_id = c->matching_hint == NULL ? 0 : c->matching_hint->id;
  r.selector_mask = dense_selector_mask(c);
  r.weight = c->weight;
  r.simplifier_epoch = c->simplifier_epoch;
  r.rewrite_epoch = c->rewrite_epoch;
  r.first_fpa_id = dense_clause_first_fpa_id(c);
  r.flags = DENSE_PASSIVE_ACTIVE |
            (c->used ? DENSE_PASSIVE_USED : 0) |
            (c->delayed_demodulator ? DENSE_PASSIVE_DELAYED : 0) |
            (c->rewrite_rule_dirty ? DENSE_PASSIVE_RULE_DIRTY : 0) |
            dense_semantics_flags(c->semantics);
  r.store_position = Dense_archive(c, &r.body_bytes,
                                   &r.justification_bytes,
                                   &r.logical_body_bytes);
  if (r.store_position == SIZE_MAX)
    fatal_error("dense_insert_passive: archive failed");
  if (Dense_record_count == Dense_record_capacity) {
    size_t capacity = dense_grow_capacity(
      Dense_record_capacity, sizeof(struct dense_passive_record),
      "dense_insert_passive: capacity overflow");
    dense_resize_directory(capacity);
  }
  record = Dense_record_count;
  Dense_records[Dense_record_count++] = r;
  dense_evict_directory_pages();
  Dense_active_count++;
  dense_add_payload(&r);
  if (r.rewrite_epoch == Dense_rewrite_epoch) {
    Dense_rewrite_fresh++;
  }
  else {
    Dense_rewrite_stale++;
  }
  if ((r.flags & (DENSE_PASSIVE_DELAYED | DENSE_PASSIVE_RULE_DIRTY)) ==
      (DENSE_PASSIVE_DELAYED | DENSE_PASSIVE_RULE_DIRTY))
    Dense_rule_stale++;
  for (p = High.selectors; p != NULL; p = p->next) {
    Giv_select gs = p->v;
    if ((r.selector_mask & (1ULL << gs->dense_bit)) != 0) {
      dense_selector_push(gs, record);
      High.occurrences++;
    }
  }
  for (p = Low.selectors; p != NULL; p = p->next) {
    Giv_select gs = p->v;
    if ((r.selector_mask & (1ULL << gs->dense_bit)) != 0) {
      dense_selector_push(gs, record);
      Low.occurrences++;
    }
  }
  Sos_size++;
}

static void dense_deactivate_record(size_t record)
{
  struct dense_passive_record *r = &Dense_records[record];
  Plist p;
  if ((r->flags & DENSE_PASSIVE_ACTIVE) == 0)
    fatal_error("dense_deactivate_record: inactive record");
  if (r->rewrite_epoch == Dense_rewrite_epoch) {
    Dense_rewrite_fresh--;
  }
  else {
    Dense_rewrite_stale--;
  }
  if ((r->flags & (DENSE_PASSIVE_DELAYED | DENSE_PASSIVE_RULE_DIRTY)) ==
      (DENSE_PASSIVE_DELAYED | DENSE_PASSIVE_RULE_DIRTY))
    Dense_rule_stale--;
  r->flags &= ~DENSE_PASSIVE_ACTIVE;
  Dense_active_count--;
  dense_subtract_payload(r);
  for (p = High.selectors; p != NULL; p = p->next) {
    Giv_select gs = p->v;
    if ((r->selector_mask & (1ULL << gs->dense_bit)) != 0) {
      gs->dense_active--;
      High.occurrences--;
    }
  }
  for (p = Low.selectors; p != NULL; p = p->next) {
    Giv_select gs = p->v;
    if ((r->selector_mask & (1ULL << gs->dense_bit)) != 0) {
      gs->dense_active--;
      Low.occurrences--;
    }
  }
  Sos_size--;
}

static void dense_reactivate_record(size_t record)
{
  struct dense_passive_record *r = &Dense_records[record];
  Plist p;
  if ((r->flags & DENSE_PASSIVE_ACTIVE) != 0)
    fatal_error("dense_reactivate_record: active record");
  r->flags |= DENSE_PASSIVE_ACTIVE;
  Dense_active_count++;
  dense_add_payload(r);
  if (r->rewrite_epoch == Dense_rewrite_epoch) {
    Dense_rewrite_fresh++;
  }
  else {
    Dense_rewrite_stale++;
  }
  if ((r->flags & (DENSE_PASSIVE_DELAYED | DENSE_PASSIVE_RULE_DIRTY)) ==
      (DENSE_PASSIVE_DELAYED | DENSE_PASSIVE_RULE_DIRTY))
    Dense_rule_stale++;
  for (p = High.selectors; p != NULL; p = p->next) {
    Giv_select gs = p->v;
    if ((r->selector_mask & (1ULL << gs->dense_bit)) != 0) {
      /* A heap retains its lazy entry.  File runs may already have discarded
         that inactive entry, so reinsertion is required; an unconsumed
         duplicate is harmless and is pruned after the next deactivation. */
      if (Dense_selector_mode == DENSE_SELECTOR_FILE)
        dense_selector_file_push(gs, record);
      gs->dense_active++;
      High.occurrences++;
    }
  }
  for (p = Low.selectors; p != NULL; p = p->next) {
    Giv_select gs = p->v;
    if ((r->selector_mask & (1ULL << gs->dense_bit)) != 0) {
      if (Dense_selector_mode == DENSE_SELECTOR_FILE)
        dense_selector_file_push(gs, record);
      gs->dense_active++;
      Low.occurrences++;
    }
  }
  Sos_size++;
}

/* PUBLIC */
BOOL dense_passive_deactivate_id(unsigned long long id,
                                 struct dense_passive_view *view)
{
  size_t at = dense_find_record(id);
  struct dense_passive_record *r;
  if (at == SIZE_MAX ||
      (Dense_records[at].flags & DENSE_PASSIVE_ACTIVE) == 0)
    return FALSE;
  r = &Dense_records[at];
  if (view != NULL) {
    dense_record_view(r, view);
  }
  dense_deactivate_record(at);
  return TRUE;
}

/* PUBLIC */
BOOL dense_passive_reactivate_id(unsigned long long id,
                                 unsigned simplifier_epoch,
                                 unsigned rewrite_epoch,
                                 BOOL delayed_demodulator,
                                 BOOL rewrite_rule_dirty)
{
  size_t at = dense_find_record(id);
  struct dense_passive_record *r;
  if (at == SIZE_MAX ||
      (Dense_records[at].flags & DENSE_PASSIVE_ACTIVE) != 0)
    return FALSE;
  r = &Dense_records[at];
  if (r->simplifier_epoch != simplifier_epoch ||
      r->rewrite_epoch != rewrite_epoch ||
      ((r->flags & DENSE_PASSIVE_DELAYED) != 0) != delayed_demodulator ||
      ((r->flags & DENSE_PASSIVE_RULE_DIRTY) != 0) != rewrite_rule_dirty)
    r->flags |= DENSE_PASSIVE_ARCHIVE_DIRTY;
  r->simplifier_epoch = simplifier_epoch;
  r->rewrite_epoch = rewrite_epoch;
  if (delayed_demodulator)
    r->flags |= DENSE_PASSIVE_DELAYED;
  else
    r->flags &= ~DENSE_PASSIVE_DELAYED;
  if (rewrite_rule_dirty)
    r->flags |= DENSE_PASSIVE_RULE_DIRTY;
  else
    r->flags &= ~DENSE_PASSIVE_RULE_DIRTY;
  dense_reactivate_record(at);
  return TRUE;
}

/* PUBLIC */
BOOL dense_passive_mark_rule_dirty(unsigned long long id)
{
  size_t at = dense_find_record(id);
  struct dense_passive_record *r;
  if (at == SIZE_MAX)
    return FALSE;
  r = &Dense_records[at];
  if ((r->flags & (DENSE_PASSIVE_ACTIVE | DENSE_PASSIVE_DELAYED)) !=
        (DENSE_PASSIVE_ACTIVE | DENSE_PASSIVE_DELAYED) ||
      (r->flags & DENSE_PASSIVE_RULE_DIRTY) != 0)
    return FALSE;
  r->flags |= DENSE_PASSIVE_RULE_DIRTY | DENSE_PASSIVE_ARCHIVE_DIRTY;
  Dense_rule_stale++;
  return TRUE;
}

/*************
 *
 *   insert_into_sos2()
 *
 *************/

/* DOCUMENTATION
This routine appends a clause to the sos list and updates
the (private) index for extracting sos clauses.
*/

/* PUBLIC */
void insert_into_sos2(Topform c, Clist sos)
{
  if (Dense_passive) {
    dense_insert_passive(c);
    return;
  }
  if (Rule_needs_semantics)
    set_semantics(c);  /* in case not yet evaluated */

  update_selectors(c, TRUE);
  clist_append(c, sos);
  Sos_size++;
}  /* insert_into_sos2 */

/*************
 *
 *   remove_from_sos2()
 *
 *************/

/* DOCUMENTATION
This routine removes a clause from the sos list and updates
the index for extracting the lightest and heaviest clauses.
*/

/* PUBLIC */
void remove_from_sos2(Topform c, Clist sos)
{
  if (Dense_passive)
    fatal_error("remove_from_sos2: dense passives are removed by record ID");
  /* A cold DISCOUNT passive keeps selector metadata in Topform but may not
     have a materialized literal body.  Selector membership rules are
     re-evaluated during removal, so restore the body before touching the AVL
     trees.  The caller is selecting or disabling the clause and needs the
     body in either case. */
  if (c->compressed != NULL && !materialize_clause(c))
    fatal_error("remove_from_sos2: invalid compressed passive clause");
  update_selectors(c, FALSE);
  clist_remove(c, sos);
  Sos_size--;
}  /* remove_from_sos2 */

/*************
 *
 *   bulk_insert_into_sos2()
 *
 *   Bulk-insert all clauses from a Clist into the SOS selector AVL trees.
 *   Uses sorted-array AVL construction: O(n log n) for qsort + O(n) for
 *   tree build, vs O(n log n) for n individual AVL inserts but with much
 *   better constant factor (no rotations, no per-insert rule evaluation
 *   overhead via batching).
 *
 *************/

/* qsort comparator wrapper - uses a static function pointer */
static Ordertype (*Bulk_compare)(void *, void *);

static int bulk_qsort_compare(const void *a, const void *b)
{
  Ordertype r = Bulk_compare(*(void **)a, *(void **)b);
  return (r == LESS_THAN) ? -1 : (r == GREATER_THAN) ? 1 : 0;
}

/* PUBLIC */
void bulk_insert_into_sos2(Clist sos)
{
  int n, i;
  void **all;
  BOOL *high_matched;
  Clist_pos cp;
  Plist p;

  n = sos->length;
  if (n == 0) return;

  if (Dense_passive) {
    while (sos->first != NULL) {
      Topform c = sos->first->c;
      clist_remove(c, sos);
      dense_insert_passive(c);
    }
    return;
  }

  /* Build array of all clauses, evaluating semantics if needed */
  all = (void **) safe_malloc(n * sizeof(void *));
  high_matched = (BOOL *) safe_malloc(n * sizeof(BOOL));
  i = 0;
  for (cp = sos->first; cp != NULL; cp = cp->next) {
    Topform c = cp->c;
    if (Rule_needs_semantics)
      set_semantics(c);
    all[i++] = c;
  }
  for (i = 0; i < n; i++)
    high_matched[i] = FALSE;

  /* For each selector, filter matching clauses, sort, build AVL */
  {
    void **matched = (void **) safe_malloc(n * sizeof(void *));
    Select_state states[2];
    int si;

    states[0] = &High;
    states[1] = &Low;

    for (si = 0; si < 2; si++) {
      for (p = states[si]->selectors; p; p = p->next) {
        Giv_select gs = p->v;
        int nm = 0;

        /* Normal insertion puts a clause in matching high selectors, or in
           matching low selectors if and only if no high selector matched. */
        for (i = 0; i < n; i++) {
          if (si == 0) {
            if (eval_clause_in_rule((Topform) all[i], gs->property)) {
              matched[nm++] = all[i];
              high_matched[i] = TRUE;
            }
          }
          else if (!high_matched[i] &&
                   eval_clause_in_rule((Topform) all[i], gs->property))
              matched[nm++] = all[i];
        }

        if (nm > 0) {
          /* Sort by selector's compare function */
          Bulk_compare = gs->compare;
          qsort(matched, nm, sizeof(void *), bulk_qsort_compare);

          /* Build balanced AVL tree from sorted array */
          gs->idx = avl_build_sorted(matched, nm);

          states[si]->occurrences += nm;
        }
      }
    }
    safe_free(matched);
  }

  Sos_size = n;
  safe_free(high_matched);
  safe_free(all);
}  /* bulk_insert_into_sos2 */

/*************
 *
 *   next_selector()
 *
 *************/

static
Giv_select next_selector(Select_state s)
{
  if (s->selectors == NULL)
    return NULL;
  else {
    Plist start = s->current;
    Giv_select gs = s->current->v;
    if (Dense_passive)
      dense_selector_drop_empty_file_runs(gs);
    while (selector_size(gs) == 0 || s->count >= gs->part) {
      s->current = s->current->next;
      if (!s->current)
	s->current = s->selectors;
      gs = s->current->v;
      if (Dense_passive)
        dense_selector_drop_empty_file_runs(gs);
      s->count = 0;
      if (s->current == start)
	break;  /* we're back to the start */
    }
    if (selector_size(gs) == 0)
      return NULL;
    else {
      s->count++;  /* for next call */
      return gs;
    }
  }
}  /* next_selector */

/*************
 *
 *   givens_available()
 *
 *************/

/* DOCUMENTATION
*/

/* PUBLIC */
BOOL givens_available(void)
{
  return (High.occurrences > 0 || Low.occurrences > 0);
}  /* givens_available */

/*************
 *
 *   get_given_clause2()
 *
 *************/

/* DOCUMENTATION
*/

/* PUBLIC */
Topform get_given_clause2(Clist sos, int num_given,
			 Prover_options opt, char **type)
{
  Topform giv;
  Giv_select gs = next_selector(&High);
  if (gs == NULL)
    gs = next_selector(&Low);
  if (gs == NULL)
    return NULL;  /* no clauses are available */

  if (Dense_passive) {
    size_t record;
    if (!dense_selector_peek(gs, &record))
      fatal_error("get_given_clause2: selected dense queue is empty");
    struct dense_passive_record r = Dense_records[record];
    dense_deactivate_record(record);
    giv = Dense_activate(r.store_position, r.id, r.hint_id);
    if (giv == NULL || giv->id != r.id)
      fatal_error("get_given_clause2: dense archive identity mismatch");
    dense_restore_clause_fpa_ids(giv, r.first_fpa_id);
    giv->weight = r.weight;
    giv->semantics = dense_record_semantics(&r);
    giv->simplifier_epoch = r.simplifier_epoch;
    giv->rewrite_epoch = r.rewrite_epoch;
    giv->used = (r.flags & DENSE_PASSIVE_USED) != 0;
    giv->delayed_demodulator =
      (r.flags & DENSE_PASSIVE_DELAYED) != 0;
    giv->rewrite_rule_dirty =
      (r.flags & DENSE_PASSIVE_RULE_DIRTY) != 0;
    *type = gs->name;
    gs->selected += 1;
    return giv;
  }
    
  if (gs->order == GS_ORDER_RANDOM) {
    int n = avl_size(gs->idx);
    int i = (rand() % n) + 1;
    giv = avl_nth_item(gs->idx, i);
  }
  else
    giv = avl_smallest(gs->idx);

  *type = gs->name;
  gs->selected += 1;

  remove_from_sos2(giv, sos);
  return giv;
}  /* get_given_clause2 */

/*************
 *
 *   iterations_to_selection()
 *
 *************/

static
double iterations_to_selection(int part, int n,
			       int cycle_size, unsigned long long occurrences,
                               unsigned long long sos_size)
{
  /* This approximates the number of iterations (of given selection) until
     the n-th clause in the selector is selected.  Simplyfying assumptions:
       1. High-priority selectors are empty.
       2. Other selectors don't become empty.
       3. No clauses are inserted before the n-th clause.  (unrealistic)
   */
  double x = n * ((double) cycle_size / part);
  return x / ((double) occurrences / sos_size);
}  /* iterations_to_selection */

/*************
 *
 *   least_iters_to_selection()
 *
 *************/

static
double least_iters_to_selection(Topform c, Select_state s, Plist ignore)
{
  Plist p;
  double least = INT_MAX;  /* where is DOUBLE_MAX?? */
  for (p = s->selectors; p; p = p->next) {
    if (p != ignore) {
      Giv_select gs = p->v;
      if (Rule_needs_semantics)
	set_semantics(c);  /* in case not yet evaluated */

      if (eval_clause_in_rule(c, gs->property)) {
	int n, cycle;
	double x;
	if (gs->order == GS_ORDER_AGE && c->id == INT_MAX)
	  n = avl_size(gs->idx) + 1;
	else
	  n = avl_place(gs->idx, c, gs->compare);
	cycle = current_cycle_size(s);
	x = iterations_to_selection(gs->part, n, cycle,
				    s->occurrences, Sos_size);
	if (Debug)
	  printf("%s(%.3f),cycle=%d,part=%d,place=%d,size=%d,iters=%.2f\n",
		 gs->name, c->weight, cycle, gs->part, n,avl_size(gs->idx),x);
	least = (x < least ? x : least);
      }
    }
  }
  return least;
}  /* least_iters_to_selection */

/***************
 *
 *   sos_keep2()
 *
 **************/

/* DOCUMENTATION
*/

/* PUBLIC */
BOOL sos_keep2(Topform c, Clist sos, Prover_options opt)
{
  int keep_factor = parm(opt->sos_keep_factor);
  int sos_size = clist_length(sos);
  int sos_limit = (parm(opt->sos_limit)== -1 ? INT_MAX : parm(opt->sos_limit));
  BOOL keep;
  if (sos_size < sos_limit / keep_factor)
    keep = TRUE;
  else {
    int iters;
    c->id = INT_MAX;
    iters = least_iters_to_selection(c, &Low, NULL);
    if (Debug)
      printf("iters=%d, wt=%.3f\n", iters, c->weight);
    if (iters < sos_limit / keep_factor)
      keep = TRUE;
    else {
      if (c->weight < Low_water_keep) {
	Low_water_keep = c->weight;
	if (!flag(opt->quiet)) {
	  printf("\nLow Water (keep): wt=%.3f, iters=%d\n", c->weight, iters);
	  if (stringparm(opt->stats, "all"))
	    selector_report();
	  fflush(stdout);
	}
      }
      Sos_deleted++;
      keep = FALSE;  /* delete clause */
    }
    c->id = 0;
  }
  return keep;
}  /* sos_keep2 */

/*************
 *
 *   worst_clause_of_priority_group()
 *
 *************/

static
Topform worst_clause_of_priority_group(Select_state ss)
{
  Topform worst = NULL; /* worst clause (with most iterations_to_selection)  */
  double max = 0.0;     /* iterations_to_selection for current worst clause  */
  Plist p;
  for (p = ss->selectors; p; p = p->next) {
    Giv_select gs = p->v;
    if (gs->idx) {
      Topform c = avl_largest(gs->idx);
      double x = iterations_to_selection(gs->part, avl_size(gs->idx),
					 current_cycle_size(ss),
					 ss->occurrences,
					 Sos_size);

      /* If that clause occurs in other selectors,
         find the lowest iterations_to_selection. */

      double y = least_iters_to_selection(c, ss, p);  /* ignore p */

      double least = (x < y ? x : y);

      if (least > max) {
	max = least;
	worst = c;
      }
    }
  }
  return worst;
}  /* worst_clause_of_priority_group */

/*************
 *
 *   worst_clause()
 *
 *************/

static
Topform worst_clause(void)
{
  Topform worst = worst_clause_of_priority_group(&Low);
  if (worst == NULL) {
    worst = worst_clause_of_priority_group(&High);
    if (worst)
      printf("\nWARNING: worst clause (id=%llu, wt=%.3f) has high priority.\n",
	     worst->id, worst->weight);
  }
  return worst;
}  /* worst_clause */

/*************
 *
 *   sos_displace2() - delete the worst sos clause
 *
 *************/

/* DOCUMENTATION
Disable the "worst" clause.
*/

/* PUBLIC */
void sos_displace2(void (*disable_proc) (Topform), BOOL quiet)
{
  Topform worst = worst_clause();
  if (worst == NULL) {
    selector_report();
    fatal_error("sos_displace2, cannot find worst clause");
  }
  else {
    if (worst->weight < Low_water_displace) {
      Low_water_displace = worst->weight;
      if (!quiet) {
	printf("\nLow Water (displace): id=%llu, wt=%.3f\n",
	       worst->id, worst->weight);
	fflush(stdout);
      }
    }
    Sos_displaced++;
    disable_proc(worst);
  }
}  /* sos_displace2 */

/*************
 *
 *   zap_given_selectors()
 *
 *************/

/* DOCUMENTATION
*/

/* PUBLIC */
void zap_given_selectors(void)
{
  Plist p;
  for (p = High.selectors; p; p = p->next) {
    Giv_select gs = p->v;
    zap_clause_eval_rule(gs->property);
    avl_zap(gs->idx);
    dense_selector_clear_contents(gs);
    free_giv_select(gs);
  }
  zap_plist(High.selectors);  /* shallow */
  for (p = Low.selectors; p; p = p->next) {
    Giv_select gs = p->v;
    zap_clause_eval_rule(gs->property);
    avl_zap(gs->idx);
    dense_selector_clear_contents(gs);
    free_giv_select(gs);
  }
  zap_plist(Low.selectors);  /* shallow */
  High.selectors = NULL;
  High.current = NULL;
  High.occurrences = 0;
  High.count = 0;
  High.cycle_size = 0;
  Low.selectors = NULL;
  Low.current = NULL;
  Low.occurrences = 0;
  Low.count = 0;
  Low.cycle_size = 0;
  Dense_selector_count = 0;
  Rule_needs_semantics = FALSE;
  dense_reset_directory_storage();
  Dense_record_count = 0;
  Dense_active_count = 0;
  Dense_rewrite_epoch = 1;
  Dense_rewrite_fresh = 0;
  Dense_rewrite_stale = 0;
  Dense_rule_stale = 0;
  Dense_body_bytes = 0;
  Dense_justification_bytes = 0;
  Dense_logical_body_bytes = 0;
}  /* zap_given_selectors */

/*************
 *
 *   get_low_selector_state()
 *
 *************/

/* PUBLIC */
void get_low_selector_state(const char **name, int *count)
{
  if (Low.current) {
    Giv_select gs = Low.current->v;
    *name = gs->name;
    *count = Low.count;
  }
  else {
    *name = "";
    *count = 0;
  }
}  /* get_low_selector_state */

/*************
 *
 *   set_low_selector_state()
 *
 *************/

/* PUBLIC */
void set_low_selector_state(const char *name, int count)
{
  Plist p;
  for (p = Low.selectors; p; p = p->next) {
    Giv_select gs = p->v;
    if (strcmp(gs->name, name) == 0) {
      Low.current = p;
      Low.count = count;
      return;
    }
  }
  /* Name not found - leave at default (first selector, count=0). */
}  /* set_low_selector_state */

/*************
 *
 *   get_high_selector_state()
 *
 *************/

/* PUBLIC */
void get_high_selector_state(const char **name, int *count)
{
  if (High.current) {
    Giv_select gs = High.current->v;
    *name = gs->name;
    *count = High.count;
  }
  else {
    *name = "";
    *count = 0;
  }
}  /* get_high_selector_state */

/*************
 *
 *   set_high_selector_state()
 *
 *************/

/* PUBLIC */
void set_high_selector_state(const char *name, int count)
{
  Plist p;
  for (p = High.selectors; p; p = p->next) {
    Giv_select gs = p->v;
    if (strcmp(gs->name, name) == 0) {
      High.current = p;
      High.count = count;
      return;
    }
  }
}  /* set_high_selector_state */

/*************
 *
 *   selector_report()
 *
 *************/

/* DOCUMENTATION
*/

/* PUBLIC */
void fprint_selector_report(FILE *fp)
{
  Plist p;
  print_separator(fp, "SELECTOR REPORT", TRUE);
  fprintf(fp, "Sos_deleted=%llu, Sos_displaced=%llu, Sos_size=%llu\n",
	  Sos_deleted, Sos_displaced, Sos_size);
  fprintf(fp, "%10s %10s %10s %10s %10s %10s\n",
	  "SELECTOR", "PART", "PRIORITY", "ORDER", "SIZE", "SELECTED");
  for (p = High.selectors; p; p = p->next) {
    Giv_select gs = p->v;
    char *s1, *s2;
    s1 = "high";
    switch (gs->order) {
    case GS_ORDER_WEIGHT: s2 = "weight"; break;
    case GS_ORDER_AGE: s2 = "age"; break;
    case GS_ORDER_HINT_AGE: s2 = "hint_age"; break;
    case GS_ORDER_RANDOM: s2 = "random"; break;
    default: s2 = "???"; break;
    }
    fprintf(fp, "%10s %10d %10s %10s %10llu %10llu\n",
	    gs->name, gs->part, s1, s2,
            (unsigned long long) selector_size(gs), gs->selected);
  }
  for (p = Low.selectors; p; p = p->next) {
    Giv_select gs = p->v;
    char *s1, *s2;
    s1 = "low";
    switch (gs->order) {
    case GS_ORDER_WEIGHT: s2 = "weight"; break;
    case GS_ORDER_AGE: s2 = "age"; break;
    case GS_ORDER_HINT_AGE: s2 = "hint_age"; break;
    case GS_ORDER_RANDOM: s2 = "random"; break;
    default: s2 = "???"; break;
    }
    fprintf(fp, "%10s %10d %10s %10s %10llu %10llu\n",
	    gs->name, gs->part, s1, s2,
            (unsigned long long) selector_size(gs), gs->selected);
  }
  print_separator(fp, "end of selector report", FALSE);
  fflush(fp);
}  /* fprint_selector_report */

void selector_report(void)
{
  fprint_selector_report(stdout);
}  /* selector_report */

/*************
 *
 *   selector_rule_term()
 *
 *************/

/* DOCUMENTATION
*/

/* PUBLIC */
Term selector_rule_term(char *name, char *priority,
			char *order, char *rule, int part)
{
  Term left =  get_rigid_term("part", 4);
  Term right = nat_to_term(part);
  ARG(left,0) = get_rigid_term(name, 0);
  ARG(left,1) = get_rigid_term(priority, 0);
  ARG(left,2) = get_rigid_term(order, 0);
  ARG(left,3) = get_rigid_term(rule, 0);
  return build_binary_term_safe("=", left, right);
}  /* selector_rule_term */

/*************
 *
 *   selector_rules_from_options()
 *
 *************/

/* DOCUMENTATION
*/

/* PUBLIC */
Plist selector_rules_from_options(Prover_options opt)
{
  Plist p = NULL;

  if (flag(opt->input_sos_first)) {
    p = plist_append(p, selector_rule_term("I", "high", "age",
					   "initial", INT_MAX));
  }

  if (parm(opt->hints_part) == INT_MAX) {
    p = plist_append(p, selector_rule_term("H", "high", "weight",
					   "hint", 1));
  }
  else if (parm(opt->hints_part) > 0) {
    p = plist_append(p, selector_rule_term("H", "low", "weight",
					   "hint", parm(opt->hints_part)));
  }

  if (parm(opt->age_part) > 0) {
    p = plist_append(p, selector_rule_term("A", "low", "age",
					   "all", parm(opt->age_part)));
  }
  if (parm(opt->false_part) > 0) {
    p = plist_append(p, selector_rule_term("F", "low", "weight",
					   "false", parm(opt->false_part)));
  }
  if (parm(opt->true_part) > 0) {
    p = plist_append(p, selector_rule_term("T", "low", "weight",
					   "true", parm(opt->true_part)));
  }
  if (parm(opt->weight_part) > 0) {
    p = plist_append(p, selector_rule_term("W", "low", "weight",
					   "all", parm(opt->weight_part)));
  }
  if (parm(opt->random_part) > 0) {
    p = plist_append(p, selector_rule_term("R", "low", "random",
					   "all", parm(opt->random_part)));
  }

  return p;
}  /* selector_rules_from_options */
