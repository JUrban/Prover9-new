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

#include "memory.h"
#include "string.h"
#include "fatal.h"
#include <stdlib.h>
#include <unistd.h>
#include <string.h>
#include <stdint.h>
#if defined(__GLIBC__) && !defined(__EMSCRIPTEN__)
#include <malloc.h>
#endif
#if defined(__APPLE__) && !defined(__EMSCRIPTEN__)
#include <mach/mach.h>
#endif
#ifndef __EMSCRIPTEN__
#include <sys/mman.h>
#ifndef MAP_ANONYMOUS
#define MAP_ANONYMOUS MAP_ANON
#endif
#endif

#define DEFAULT_MAX_MEGS  49152  /* 48 GB; change with set_max_megs(n) */
#define MAX_SLAB_LISTS      128  /* larger requests use direct allocation */
#define SLAB_BYTES      (256 * 1024)
#define SLAB_MAGIC ((uintptr_t) 0x51ab51abU)

struct memory_slab {
  uintptr_t magic;
  unsigned class_index;
  size_t slot_size;
  unsigned capacity;
  unsigned next_unused;
  unsigned live_count;
  unsigned free_count;
  void *free_list;
  struct memory_slab *all_prev;
  struct memory_slab *all_next;
  struct memory_slab *avail_prev;
  struct memory_slab *avail_next;
};

struct memory_class {
  struct memory_slab *all;
  struct memory_slab *available;
};

static struct memory_class Classes[MAX_SLAB_LISTS];
static struct memory_stats Memory_stats;

static BOOL Max_megs_check = TRUE;
static int Max_megs = DEFAULT_MAX_MEGS;  /* change with set_max_megs(n) */
static void (*Exit_proc) (void);         /* set with set_max_megs_proc() */

static unsigned long long Bytes_palloced = 0;  /* 64-bit to handle >4GB */

static unsigned Mem_calls = 0;
static unsigned Mem_calls_overflows = 0;
static BOOL Compact_system_heap_enabled = FALSE;

#define BUMP_MEM_CALLS {Mem_calls++; if (Mem_calls==0) Mem_calls_overflows++;}

/* Forward declarations for safe allocation functions */
void *safe_malloc(size_t n);
void *safe_calloc(size_t nmemb, size_t size);
static void release_empty_slabs(void);

/*************
 *
 *    Reclaimable segregated slabs.
 *
 *************/

static
void max_megs_check(size_t additional)
{
  unsigned long long limit = (unsigned long long) Max_megs * 1024 * 1024;
  if (Max_megs_check && Memory_stats.reserved_bytes + additional > limit)
    release_empty_slabs();
  if (Max_megs_check && Memory_stats.reserved_bytes + additional > limit) {
    if (Exit_proc)
      (*Exit_proc)();
    else
      fatal_error("memory allocator, Max_megs parameter exceeded");
  }
}  /* max_megs_check */

static
void record_reservation(size_t bytes)
{
  Memory_stats.reserved_bytes += bytes;
  if (Memory_stats.reserved_bytes > Memory_stats.peak_reserved_bytes)
    Memory_stats.peak_reserved_bytes = Memory_stats.reserved_bytes;
}  /* record_reservation */

static
void record_allocation(size_t bytes)
{
  Memory_stats.cumulative_bytes += bytes;
  Memory_stats.logical_live_bytes += bytes;
  if (Memory_stats.logical_live_bytes > Memory_stats.logical_peak_bytes)
    Memory_stats.logical_peak_bytes = Memory_stats.logical_live_bytes;
}  /* record_allocation */

static
void available_add(struct memory_class *c, struct memory_slab *s)
{
  s->avail_prev = NULL;
  s->avail_next = c->available;
  if (c->available)
    c->available->avail_prev = s;
  c->available = s;
}  /* available_add */

static
void available_remove(struct memory_class *c, struct memory_slab *s)
{
  if (s->avail_prev)
    s->avail_prev->avail_next = s->avail_next;
  else
    c->available = s->avail_next;
  if (s->avail_next)
    s->avail_next->avail_prev = s->avail_prev;
  s->avail_prev = s->avail_next = NULL;
}  /* available_remove */

static
void all_add(struct memory_class *c, struct memory_slab *s)
{
  s->all_prev = NULL;
  s->all_next = c->all;
  if (c->all)
    c->all->all_prev = s;
  c->all = s;
}  /* all_add */

static
void all_remove(struct memory_class *c, struct memory_slab *s)
{
  if (s->all_prev)
    s->all_prev->all_next = s->all_next;
  else
    c->all = s->all_next;
  if (s->all_next)
    s->all_next->all_prev = s->all_prev;
}  /* all_remove */

static
void *slab_mapping_alloc(void)
{
#ifdef __EMSCRIPTEN__
  return safe_malloc(SLAB_BYTES);
#else
  size_t mapping_bytes = 2 * (size_t) SLAB_BYTES;
  void *mapping = mmap(NULL, mapping_bytes, PROT_READ | PROT_WRITE,
                       MAP_PRIVATE | MAP_ANONYMOUS, -1, 0);
  uintptr_t raw, aligned;
  size_t prefix, suffix;

  if (mapping == MAP_FAILED) {
    set_fatal_szs_status("MemoryOut");
    fatal_error("memory allocator, mmap failed");
  }
  raw = (uintptr_t) mapping;
  aligned = (raw + SLAB_BYTES - 1) & ~((uintptr_t) SLAB_BYTES - 1);
  prefix = (size_t) (aligned - raw);
  suffix = mapping_bytes - prefix - SLAB_BYTES;
  if (prefix != 0 && munmap(mapping, prefix) != 0)
    fatal_error("memory allocator, prefix munmap failed");
  if (suffix != 0 &&
      munmap((void *) (aligned + SLAB_BYTES), suffix) != 0)
    fatal_error("memory allocator, suffix munmap failed");
  return (void *) aligned;
#endif
}  /* slab_mapping_alloc */

static
void slab_mapping_free(struct memory_slab *s)
{
#ifdef __EMSCRIPTEN__
  safe_free(s);
#else
  if (munmap(s, SLAB_BYTES) != 0)
    fatal_error("memory allocator, slab munmap failed");
#endif
}  /* slab_mapping_free */

static
void release_slab(struct memory_class *c, struct memory_slab *s)
{
  available_remove(c, s);
  all_remove(c, s);
  s->magic = 0;
  Memory_stats.slab_count--;
  Memory_stats.reserved_bytes -= SLAB_BYTES;
  Memory_stats.reclaimed_slabs++;
  Memory_stats.reclaimed_bytes += SLAB_BYTES;
  slab_mapping_free(s);
}  /* release_slab */

static
void release_empty_slabs(void)
{
  unsigned i;
  for (i = 1; i < MAX_SLAB_LISTS; i++) {
    struct memory_class *c = &Classes[i];
    struct memory_slab *s = c->all;
    while (s) {
      struct memory_slab *next = s->all_next;
      if (s->live_count == 0)
        release_slab(c, s);
      s = next;
    }
  }
}  /* release_empty_slabs */

static
struct memory_slab *new_slab(unsigned n)
{
  struct memory_class *c = &Classes[n];
  struct memory_slab *s;
  size_t header = CEILING(sizeof(struct memory_slab), BYTES_POINTER) *
                  BYTES_POINTER;

  max_megs_check(SLAB_BYTES);
  s = slab_mapping_alloc();
  memset(s, 0, sizeof(*s));
  s->magic = SLAB_MAGIC;
  s->class_index = n;
  s->slot_size = n * BYTES_POINTER;
  s->capacity = (unsigned) ((SLAB_BYTES - header) / s->slot_size);
  if (s->capacity == 0)
    fatal_error("memory allocator, empty slab class");
  all_add(c, s);
  available_add(c, s);
  Memory_stats.slab_count++;
  if (Memory_stats.slab_count > Memory_stats.peak_slab_count)
    Memory_stats.peak_slab_count = Memory_stats.slab_count;
  record_reservation(SLAB_BYTES);
  return s;
}  /* new_slab */

static
void *slab_get(unsigned n)
{
  struct memory_class *c = &Classes[n];
  struct memory_slab *s = c->available;
  void *p;
  size_t header = CEILING(sizeof(struct memory_slab), BYTES_POINTER) *
                  BYTES_POINTER;

  if (s == NULL)
    s = new_slab(n);
  if (s->free_list) {
    p = s->free_list;
    s->free_list = *((void **) p);
    s->free_count--;
  }
  else {
    p = (char *) s + header + ((size_t) s->next_unused * s->slot_size);
    s->next_unused++;
    Bytes_palloced += s->slot_size;
  }
  s->live_count++;
  if (s->live_count == s->capacity)
    available_remove(c, s);
  record_allocation(s->slot_size);
  return p;
}  /* slab_get */

static
struct memory_slab *slab_for_pointer(void *p, unsigned n)
{
#ifdef __EMSCRIPTEN__
  struct memory_slab *s;
  uintptr_t address = (uintptr_t) p;
  for (s = Classes[n].all; s; s = s->all_next) {
    uintptr_t start = (uintptr_t) s;
    if (address >= start && address < start + SLAB_BYTES)
      return s;
  }
  return NULL;
#else
  uintptr_t base = (uintptr_t) p & ~((uintptr_t) SLAB_BYTES - 1);
  return (struct memory_slab *) base;
#endif
}  /* slab_for_pointer */

static
void slab_free(void *p, unsigned n)
{
  struct memory_class *c = &Classes[n];
  struct memory_slab *s = slab_for_pointer(p, n);

  if (s == NULL || s->magic != SLAB_MAGIC || s->class_index != n ||
      s->live_count == 0)
    fatal_error("free_mem, invalid slab pointer or size class");
  if (s->live_count == s->capacity)
    available_add(c, s);
  *((void **) p) = s->free_list;
  s->free_list = p;
  s->free_count++;
  s->live_count--;
  Memory_stats.logical_live_bytes -= s->slot_size;

  if (s->live_count == 0) {
    if (s->all_prev != NULL || s->all_next != NULL)
      release_slab(c, s);
    /* Otherwise keep one empty slab warm for this size class.  Its freelist
       is slab-local, and memory_release_unused() can purge the mapping. */
  }
}  /* slab_free */

/*************
 *
 *   get_cmem()
 *
 *************/

/* DOCUMENTATION
Get a chunk of memory that will hold n pointers (NOT n BYTES).
The memory is initialized to all 0.
*/

/* PUBLIC */
void *get_cmem(unsigned n)
{
  if (n == 0)
    return NULL;
  else {
    void **p;
    size_t bytes = (size_t) n * BYTES_POINTER;
    BUMP_MEM_CALLS;
    if (n >= MAX_SLAB_LISTS) {
      max_megs_check(bytes);
      record_reservation(bytes);
      Memory_stats.direct_live_bytes += bytes;
      record_allocation(bytes);
      return safe_calloc(n, BYTES_POINTER);
    }
    p = slab_get(n);
    memset(p, 0, bytes);
    return p;
  }
}  /* get_cmem */

/*************
 *
 *   get_mem()
 *
 *************/

/* DOCUMENTATION
Get a chunk of memory that will hold n pointers (NOT n BYTES).
The memory is NOT initialized.
*/


/* PUBLIC */
void *get_mem(unsigned n)
{
  if (n == 0)
    return NULL;
  else {
    size_t bytes = (size_t) n * BYTES_POINTER;
    BUMP_MEM_CALLS;
    if (n >= MAX_SLAB_LISTS) {
      void *p;
      max_megs_check(bytes);
      p = safe_malloc(bytes);
      record_reservation(bytes);
      Memory_stats.direct_live_bytes += bytes;
      record_allocation(bytes);
      return p;
    }
    return slab_get(n);
  }
}  /* get_mem */

/*************
 *
 *   free_mem()
 *
 *************/

/* DOCUMENTATION
Free a chunk of memory that holds n pointers (not n bytes)
that was returned from a previous get_mem() or get_cmem() call.
*/

/* PUBLIC */
void free_mem(void *q, unsigned n)
{
  if (n == 0)
    ;  /* do nothing */
  else {
    size_t bytes = (size_t) n * BYTES_POINTER;
    if (n >= MAX_SLAB_LISTS) {
      safe_free(q);
      Memory_stats.direct_live_bytes -= bytes;
      Memory_stats.logical_live_bytes -= bytes;
      Memory_stats.reserved_bytes -= bytes;
    }
    else {
      slab_free(q, n);
    }
  }
}  /* free_mem */

/*************
 *
 *   memory_report()
 *
 *************/

/* DOCUMENTATION
*/

/* PUBLIC */
void memory_report(FILE *fp)
{
  int i;
  struct memory_stats stats;
  memory_get_stats(&stats);
  fprintf(fp,
          "\nMemory report: live=%s, reserved=%s, peak_reserved=%s, "
          "reclaimed=%s bytes in %s slabs, cumulative=%s.\n",
          comma_num(stats.logical_live_bytes),
          comma_num(stats.reserved_bytes),
          comma_num(stats.peak_reserved_bytes),
          comma_num(stats.reclaimed_bytes),
          comma_num(stats.reclaimed_slabs),
          comma_num(stats.cumulative_bytes));
  for (i = 1; i < MAX_SLAB_LISTS; i++) {
    struct memory_slab *s;
    unsigned slabs = 0, live = 0, reusable = 0;
    for (s = Classes[i].all; s; s = s->all_next) {
      slabs++;
      live += s->live_count;
      reusable += s->free_count + (s->capacity - s->next_unused);
    }
    if (slabs != 0)
      fprintf(fp, "Class %3d: slabs=%u, live=%u, reusable=%u, %8.1f K live\n",
              i, slabs, live, reusable,
              (double) i * live * BYTES_POINTER / 1024.);
  }
}  /* memory_report */

/*************
 *
 *   memory_get_stats()
 *
 *************/

/* PUBLIC */
void memory_get_stats(struct memory_stats *stats)
{
  unsigned i;
  *stats = Memory_stats;
  stats->reusable_bytes = 0;
  stats->unallocated_bytes = 0;
  stats->metadata_bytes = 0;
  for (i = 1; i < MAX_SLAB_LISTS; i++) {
    struct memory_slab *s;
    for (s = Classes[i].all; s; s = s->all_next) {
      size_t payload = (size_t) s->capacity * s->slot_size;
      stats->reusable_bytes += (unsigned long long) s->free_count *
                               s->slot_size;
      stats->unallocated_bytes +=
        (unsigned long long) (s->capacity - s->next_unused) * s->slot_size;
      stats->metadata_bytes += SLAB_BYTES - payload;
    }
  }
  stats->fragmentation_bytes = stats->reserved_bytes >=
                               stats->logical_live_bytes ?
    stats->reserved_bytes - stats->logical_live_bytes : 0;
}  /* memory_get_stats */

/* PUBLIC */
void memory_get_process_stats(struct memory_process_stats *stats)
{
  memset(stats, 0, sizeof(*stats));
#if defined(__linux__) && !defined(__EMSCRIPTEN__)
  {
    FILE *fp = fopen("/proc/self/smaps_rollup", "r");
    char line[256];
    if (fp != NULL) {
      stats->smaps_supported = TRUE;
      while (fgets(line, sizeof(line), fp) != NULL) {
        unsigned long long value;
        if (sscanf(line, "Rss: %llu kB", &value) == 1)
          stats->rss_kbytes = value;
        else if (sscanf(line, "Pss: %llu kB", &value) == 1)
          stats->pss_kbytes = value;
        else if (sscanf(line, "Anonymous: %llu kB", &value) == 1)
          stats->anonymous_kbytes = value;
        else if (sscanf(line, "Shared_Clean: %llu kB", &value) == 1)
          stats->shared_clean_kbytes = value;
        else if (sscanf(line, "Shared_Dirty: %llu kB", &value) == 1)
          stats->shared_dirty_kbytes = value;
        else if (sscanf(line, "Private_Clean: %llu kB", &value) == 1)
          stats->private_clean_kbytes = value;
        else if (sscanf(line, "Private_Dirty: %llu kB", &value) == 1)
          stats->private_dirty_kbytes = value;
        else if (sscanf(line, "Swap: %llu kB", &value) == 1)
          stats->swap_kbytes = value;
      }
      fclose(fp);
    }
  }
#endif
#if defined(__GLIBC__) && !defined(__EMSCRIPTEN__)
#if __GLIBC_PREREQ(2, 33)
  {
    struct mallinfo2 info = mallinfo2();
    stats->libc_heap_supported = TRUE;
    stats->libc_arena_bytes = info.arena;
    stats->libc_mmap_bytes = info.hblkhd;
    stats->libc_in_use_bytes = info.uordblks;
    stats->libc_free_bytes = info.fordblks;
    stats->libc_releasable_bytes = info.keepcost;
  }
#endif
#endif
}  /* memory_get_process_stats */

/* PUBLIC */
BOOL memory_configure_compact_system_heap(void)
{
#if defined(__GLIBC__) && !defined(__EMSCRIPTEN__)
  /* Keep medium and large compact-index arrays out of the main arena.  A
     zero trim threshold prevents glibc from raising its dynamic mmap
     threshold, but the default threshold can still leave 64--128-KiB growth
     and scratch arrays stranded between long-lived objects.  These arrays
     are large enough to amortize a private mapping and important enough that
     munmap must return their pages after an index rebuild. */
  if (mallopt(M_MMAP_THRESHOLD, 64 * 1024) == 0)
    return FALSE;
  if (mallopt(M_TRIM_THRESHOLD, 0) == 0)
    return FALSE;
  Compact_system_heap_enabled = TRUE;
  return TRUE;
#else
  return FALSE;
#endif
}  /* memory_configure_compact_system_heap */

/* PUBLIC */
BOOL memory_compact_system_heap_enabled(void)
{
  return Compact_system_heap_enabled;
}  /* memory_compact_system_heap_enabled */

/* PUBLIC */
void memory_release_unused(void)
{
  release_empty_slabs();
}  /* memory_release_unused */

/* PUBLIC */
unsigned long long memory_slab_bytes(void)
{
  return SLAB_BYTES;
}  /* memory_slab_bytes */

/*************
 *
 *   memory_current_rss_kbytes() / memory_peak_rss_kbytes()
 *
 *************/

/* PUBLIC */
unsigned long long memory_current_rss_kbytes(void)
{
#if defined(__linux__) && !defined(__EMSCRIPTEN__)
  FILE *fp = fopen("/proc/self/statm", "r");
  unsigned long long total_pages, resident_pages;
  long page_bytes;
  if (fp == NULL)
    return 0;
  if (fscanf(fp, "%llu %llu", &total_pages, &resident_pages) != 2) {
    fclose(fp);
    return 0;
  }
  fclose(fp);
  (void) total_pages;
  page_bytes = sysconf(_SC_PAGESIZE);
  return page_bytes > 0 ?
    resident_pages * (unsigned long long) page_bytes / 1024 : 0;
#elif defined(__APPLE__) && !defined(__EMSCRIPTEN__)
#ifdef MACH_TASK_BASIC_INFO
  mach_task_basic_info_data_t info;
  mach_msg_type_number_t count = MACH_TASK_BASIC_INFO_COUNT;
  kern_return_t result = task_info(mach_task_self(), MACH_TASK_BASIC_INFO,
                                   (task_info_t) &info, &count);
#else
  task_basic_info_data_t info;
  mach_msg_type_number_t count = TASK_BASIC_INFO_COUNT;
  kern_return_t result = task_info(mach_task_self(), TASK_BASIC_INFO,
                                   (task_info_t) &info, &count);
#endif
  return result == KERN_SUCCESS ?
    (unsigned long long) info.resident_size / 1024 : 0;
#else
  return 0;
#endif
}  /* memory_current_rss_kbytes */

/* PUBLIC */
BOOL memory_current_rss_supported(void)
{
#if (defined(__linux__) || defined(__APPLE__)) && !defined(__EMSCRIPTEN__)
  return TRUE;
#else
  return FALSE;
#endif
}  /* memory_current_rss_supported */

/* PUBLIC */
BOOL memory_can_return_pages_to_os(void)
{
#ifdef __EMSCRIPTEN__
  /* free() makes the storage reusable inside the WebAssembly heap, but the
     linear memory cannot contract and return pages to the host. */
  return FALSE;
#else
  return TRUE;
#endif
}  /* memory_can_return_pages_to_os */

/* PUBLIC */
unsigned long long memory_peak_rss_kbytes(void)
{
#ifdef __EMSCRIPTEN__
  return 0;
#else
  struct rusage usage;
  unsigned long long peak, current;
  if (getrusage(RUSAGE_SELF, &usage) != 0)
    return 0;
#ifdef __APPLE__
  peak = (unsigned long long) usage.ru_maxrss / 1024;
#else
  peak = (unsigned long long) usage.ru_maxrss;
#endif
  current = memory_current_rss_kbytes();
  return peak > current ? peak : current;
#endif
}  /* memory_peak_rss_kbytes */

/*************
 *
 *    int megs_malloced() -- How many MB have been dynamically allocated?
 *
 *************/

/* DOCUMENTATION
This compatibility routine returns the high-water reservation rounded up to
megabytes.  It remains monotonic for callers that subtract a startup mark;
use memory_get_stats() for the current reservation.
*/

/* PUBLIC */
long long megs_malloced(void)
{
  return (long long) ((Memory_stats.peak_reserved_bytes + 1024*1024 - 1) /
                      (1024*1024));
}  /* megs_malloced */

/*************
 *
 *   set_max_megs()
 *
 *************/

/* DOCUMENTATION
This routine changes the limit on the amount of memory obtained
from malloc() by palloc().  The argument is in megabytes.
The default value is DEFAULT_MAX_MEGS.
*/

/* PUBLIC */
void set_max_megs(int megs)
{
  Max_megs = (megs == -1 ? INT_MAX : megs);
}  /* set_max_megs */

/*************
 *
 *   set_max_megs_proc()
 *
 *************/

/* DOCUMENTATION
This routine is used to specify the routine that will be called
if max_megs is exceeded.
*/

/* PUBLIC */
void set_max_megs_proc(void (*proc)(void))
{
  Exit_proc = proc;
}  /* set_max_megs_proc */

/*************
 *
 *   bytes_palloced()
 *
 *************/

/* DOCUMENTATION
How many bytes have been allocated by the palloc() routine?
This includes all of the get_mem() calls.
*/

/* PUBLIC */
unsigned long long bytes_palloced(void)
{
  return Bytes_palloced;
}  /* bytes_palloced */

/*************
 *
 *   tp_alloc()
 *
 *************/

/* DOCUMENTATION
Allocate n bytes of memory, aligned on a pointer boundary.
The memory is not initialized, and it cannot be freed.
*/

/* PUBLIC */
void *tp_alloc(size_t n)
{
  void *p;
  /* If n is not a multiple of BYTES_POINTER, round up so that it is. */
  if (n % BYTES_POINTER != 0) {
    n += (BYTES_POINTER - (n % BYTES_POINTER));
  }
  if (n == 0)
    return NULL;
  max_megs_check(n);
  p = safe_malloc(n);
  record_reservation(n);
  Memory_stats.permanent_live_bytes += n;
  record_allocation(n);
  Bytes_palloced += n;
  return p;
}  /* tp_alloc */

/*************
 *
 *   mega_mem_calls()
 *
 *************/

/* DOCUMENTATION
 */

/* PUBLIC */
unsigned mega_mem_calls(void)
{
  return
    (Mem_calls / 1000000) +
    ((UINT_MAX / 1000000) * Mem_calls_overflows);
}  /* mega_mem_calls */

/* PUBLIC */
unsigned long long memory_allocation_calls(void)
{
  return (unsigned long long) Mem_calls_overflows *
         ((unsigned long long) UINT_MAX + 1) + Mem_calls;
}  /* memory_allocation_calls */

/*************
 *
 *   disable_max_megs()
 *
 *************/

/* DOCUMENTATION
*/

/* PUBLIC */
void disable_max_megs(void)
{
  Max_megs_check = FALSE;
}  /* disable_max_megs */

/*************
 *
 *   enable_max_megs()
 *
 *************/

/* DOCUMENTATION
*/

/* PUBLIC */
void enable_max_megs(void)
{
  Max_megs_check = TRUE;
}  /* enable_max_megs */

/*************
 *
 *   alloc_retry_loop() - Handle allocation failure
 *
 *   DEBUG:   exponential backoff retries, then prompt user
 *   Release: immediate fatal exit
 *
 *************/

#ifdef DEBUG

static
void *alloc_retry_loop(void *(*alloc_func)(size_t), size_t n, const char *func_name)
{
  void *p;
  int wait_minutes;
  int response;

  for (;;) {  /* outer loop for user retry */
    /* exponential backoff: 1, 2, 4, 8, 16, 32, 64 minutes */
    for (wait_minutes = 1; wait_minutes <= 64; wait_minutes *= 2) {
      fprintf(stderr, "%s: allocation of %lu bytes failed, "
              "waiting %d minute(s) before retry...\n",
              func_name, (unsigned long)n, wait_minutes);
      fflush(stderr);
      sleep(wait_minutes * 60);
      p = alloc_func(n);
      if (p != NULL)
        return p;
    }

    /* All retries exhausted, prompt user */
    fprintf(stderr, "%s: allocation of %lu bytes failed after all retries.\n",
            func_name, (unsigned long)n);
    fprintf(stderr, "Try retry sequence again? (y/n): ");
    fflush(stderr);

    response = getchar();
    /* consume rest of line */
    while (getchar() != '\n' && !feof(stdin))
      ;

    if (response != 'y' && response != 'Y') {
      fprintf(stderr, "%s: exiting due to memory allocation failure.\n",
              func_name);
      exit(3);
    }
  }
}  /* alloc_retry_loop */

#else /* !DEBUG */

static
void *alloc_retry_loop(void *(*alloc_func)(size_t), size_t n, const char *func_name)
{
  static char msg[128];
  (void) alloc_func;
  fprintf(stderr, "%s: out of memory (allocation of %lu bytes failed)\n",
          func_name, (unsigned long)n);
  /* Report a status before dying: in TPTP mode fatal_error() prints the
     SZS line, so a memory-limited competition run is never silent.
     MemoryOut is the SZS ResourceOut value for memory exhaustion.
     Nothing on this path allocates. */
  set_fatal_szs_status("MemoryOut");
  snprintf(msg, sizeof(msg), "%s: out of memory (%lu bytes)",
           func_name, (unsigned long) n);
  fatal_error(msg);
  return NULL;  /* unreachable, silence compiler warning */
}  /* alloc_retry_loop */

#endif /* DEBUG */

#ifdef DEBUG

/*************
 *
 *   DEBUG mode: Memory tracking and logging variables
 *
 *************/

static BOOL Memory_logging_enabled = FALSE;
static unsigned long long Total_allocated = 0;
static unsigned long long Total_freed = 0;
static unsigned long Alloc_count = 0;
static unsigned long Free_count = 0;

/* Size header stored before each allocation */
#define SIZE_HEADER_SIZE sizeof(size_t)

/*************
 *
 *   enable_memory_logging()
 *
 *************/

/* PUBLIC */
void enable_memory_logging(void)
{
  Memory_logging_enabled = TRUE;
  fprintf(stderr, "MEMORY_LOG: Memory logging enabled\n");
  fflush(stderr);
}  /* enable_memory_logging */

/*************
 *
 *   disable_memory_logging()
 *
 *************/

/* PUBLIC */
void disable_memory_logging(void)
{
  Memory_logging_enabled = FALSE;
}  /* disable_memory_logging */

/*************
 *
 *   memory_logging_summary()
 *
 *************/

/* PUBLIC */
void memory_logging_summary(FILE *fp)
{
  fprintf(fp, "\n");
  fprintf(fp, "============ Memory Logging Summary ============\n");
  fprintf(fp, "Total allocations:   %s calls, %s bytes (%.2f MB)\n",
          comma_num(Alloc_count), comma_num(Total_allocated),
          Total_allocated / (1024.0 * 1024.0));
  fprintf(fp, "Total frees:         %s calls, %s bytes (%.2f MB)\n",
          comma_num(Free_count), comma_num(Total_freed),
          Total_freed / (1024.0 * 1024.0));
  fprintf(fp, "Net allocated:       %s bytes (%.2f MB)\n",
          comma_num(Total_allocated - Total_freed),
          (Total_allocated - Total_freed) / (1024.0 * 1024.0));
  fprintf(fp, "================================================\n");
  fflush(fp);
}  /* memory_logging_summary */

/*************
 *
 *   get_total_allocated()
 *
 *************/

/* PUBLIC */
unsigned long long get_total_allocated(void)
{
  return Total_allocated;
}  /* get_total_allocated */

/*************
 *
 *   get_total_freed()
 *
 *************/

/* PUBLIC */
unsigned long long get_total_freed(void)
{
  return Total_freed;
}  /* get_total_freed */

/*************
 *
 *   safe_malloc() - malloc with NULL check, retry mechanism, and tracking
 *
 *   Allocates extra space for size header to enable tracked frees.
 *
 *************/

/* PUBLIC */
void *safe_malloc(size_t n)
{
  void *p;
  size_t total_size;
  size_t *header;

  if (n == 0)
    return NULL;

  /* Catch negative-int-cast-to-size_t and other overflow bugs. */
#ifdef __LP64__
  if (n > (unsigned long long)1 << 40) {  /* 1 TB on 64-bit */
#else
  if (n > (unsigned long)1 << 30) {       /* 1 GB on 32-bit */
#endif
    fprintf(stderr,
            "safe_malloc: absurd allocation of %lu bytes "
            "(probable integer overflow)\n", (unsigned long)n);
    exit(3);
  }

  total_size = n + SIZE_HEADER_SIZE;
  p = malloc(total_size);
  if (p == NULL)
    p = alloc_retry_loop(malloc, total_size, "safe_malloc");

  /* Store size in header */
  header = (size_t *)p;
  *header = n;

  /* Update tracking */
  Total_allocated += n;
  Alloc_count++;

  if (Memory_logging_enabled) {
    fprintf(stderr, "MEMORY_LOG: safe_malloc(%lu) = %p [total_alloc=%llu, net=%lld]\n",
            (unsigned long)n, (void *)((char *)p + SIZE_HEADER_SIZE),
            Total_allocated, (long long)(Total_allocated - Total_freed));
    fflush(stderr);
  }

  /* Return pointer past the header */
  return (char *)p + SIZE_HEADER_SIZE;
}  /* safe_malloc */

/*************
 *
 *   calloc_wrapper() - wrapper for calloc to match malloc signature
 *
 *************/

static size_t Calloc_size;  /* used by calloc_wrapper */

static
void *calloc_wrapper(size_t n)
{
  return calloc(n, Calloc_size);
}  /* calloc_wrapper */

/*************
 *
 *   safe_calloc() - calloc with NULL check, retry mechanism, and tracking
 *
 *   Allocates extra space for size header to enable tracked frees.
 *
 *************/

/* PUBLIC */
void *safe_calloc(size_t nmemb, size_t size)
{
  void *p;
  size_t user_bytes;
  size_t total_bytes;
  size_t *header;

  if (nmemb == 0 || size == 0)
    return NULL;

  if (size != 0 && nmemb > (SIZE_MAX - SIZE_HEADER_SIZE) / size)
    fatal_error("safe_calloc: nmemb * size overflow");

  user_bytes = nmemb * size;
  total_bytes = user_bytes + SIZE_HEADER_SIZE;

  /* malloc + memset (calloc can't accommodate the size header) */
  p = malloc(total_bytes);
  if (p == NULL) {
    Calloc_size = 1;
    p = alloc_retry_loop(calloc_wrapper, total_bytes, "safe_calloc");
  }

  /* Store size in header */
  header = (size_t *)p;
  *header = user_bytes;

  /* Zero the user portion (not the header) */
  memset((char *)p + SIZE_HEADER_SIZE, 0, user_bytes);

  /* Update tracking */
  Total_allocated += user_bytes;
  Alloc_count++;

  if (Memory_logging_enabled) {
    fprintf(stderr, "MEMORY_LOG: safe_calloc(%lu, %lu) = %p [total_alloc=%llu, net=%lld]\n",
            (unsigned long)nmemb, (unsigned long)size,
            (void *)((char *)p + SIZE_HEADER_SIZE),
            Total_allocated, (long long)(Total_allocated - Total_freed));
    fflush(stderr);
  }

  /* Return pointer past the header */
  return (char *)p + SIZE_HEADER_SIZE;
}  /* safe_calloc */

/*************
 *
 *   safe_free() - free with tracking
 *
 *   Reads size from header and updates tracking.
 *
 *************/

/* PUBLIC */
void safe_free(void *p)
{
  size_t *header;
  size_t user_size;

  if (p == NULL)
    return;

  /* Get pointer to header (before user data) */
  header = (size_t *)((char *)p - SIZE_HEADER_SIZE);
  user_size = *header;

  /* Update tracking */
  Total_freed += user_size;
  Free_count++;

  if (Memory_logging_enabled) {
    fprintf(stderr, "MEMORY_LOG: safe_free(%p) size=%lu [total_freed=%llu, net=%lld]\n",
            p, (unsigned long)user_size,
            Total_freed, (long long)(Total_allocated - Total_freed));
    fflush(stderr);
  }

  /* Free the actual allocated block (including header) */
  free(header);
}  /* safe_free */

/*************
 *
 *   safe_realloc() - realloc with NULL check, retry, and tracking
 *
 *   Handles the size header so old pointer is not lost on failure.
 *
 *************/

/* PUBLIC */
void *safe_realloc(void *p, size_t n)
{
  void *new_p;
  size_t *old_header;
  size_t *new_header;
  size_t old_size;
  size_t total_size;

  if (p == NULL)
    return safe_malloc(n);

  if (n == 0) {
    safe_free(p);
    return NULL;
  }

  /* Get the real allocation pointer (before header) */
  old_header = (size_t *)((char *)p - SIZE_HEADER_SIZE);
  old_size = *old_header;

  total_size = n + SIZE_HEADER_SIZE;
  new_p = realloc(old_header, total_size);
  if (new_p == NULL) {
    /* realloc failed -- old block is still valid */
    fatal_error("safe_realloc: memory allocation failed");
  }

  /* Store new size in header */
  new_header = (size_t *)new_p;
  *new_header = n;

  /* Update tracking */
  Total_freed += old_size;
  Total_allocated += n;

  if (Memory_logging_enabled) {
    fprintf(stderr, "MEMORY_LOG: safe_realloc(%p, %lu) = %p [net=%lld]\n",
            p, (unsigned long)n,
            (void *)((char *)new_p + SIZE_HEADER_SIZE),
            (long long)(Total_allocated - Total_freed));
    fflush(stderr);
  }

  /* Return pointer past the header */
  return (char *)new_p + SIZE_HEADER_SIZE;
}  /* safe_realloc */

#else /* !DEBUG -- Release mode: no size headers, no tracking */

/*************
 *
 *   Release mode stubs for logging functions
 *
 *************/

/* PUBLIC */
void enable_memory_logging(void)
{
}  /* enable_memory_logging */

/* PUBLIC */
void disable_memory_logging(void)
{
}  /* disable_memory_logging */

/* PUBLIC */
void memory_logging_summary(FILE *fp)
{
}  /* memory_logging_summary */

/* PUBLIC */
unsigned long long get_total_allocated(void)
{
  return 0;
}  /* get_total_allocated */

/* PUBLIC */
unsigned long long get_total_freed(void)
{
  return 0;
}  /* get_total_freed */

/*************
 *
 *   safe_malloc() - malloc with NULL check and retry mechanism
 *
 *************/

/* PUBLIC */
void *safe_malloc(size_t n)
{
  void *p;

  if (n == 0)
    return NULL;

  /* Catch negative-int-cast-to-size_t and other overflow bugs. */
#ifdef __LP64__
  if (n > (unsigned long long)1 << 40) {  /* 1 TB on 64-bit */
#else
  if (n > (unsigned long)1 << 30) {       /* 1 GB on 32-bit */
#endif
    fprintf(stderr,
            "safe_malloc: absurd allocation of %lu bytes "
            "(probable integer overflow)\n", (unsigned long)n);
    exit(3);
  }

  p = malloc(n);
  if (p == NULL)
    p = alloc_retry_loop(malloc, n, "safe_malloc");

  return p;
}  /* safe_malloc */

/*************
 *
 *   safe_calloc() - calloc with NULL check and retry mechanism
 *
 *************/

static size_t Calloc_size;  /* used by calloc_wrapper */

static
void *calloc_wrapper(size_t n)
{
  return calloc(n, Calloc_size);
}  /* calloc_wrapper */

/* PUBLIC */
void *safe_calloc(size_t nmemb, size_t size)
{
  void *p;

  if (nmemb == 0 || size == 0)
    return NULL;

  if (size != 0 && nmemb > (size_t)(-1) / size)
    fatal_error("safe_calloc: nmemb * size overflow");

  p = calloc(nmemb, size);
  if (p == NULL) {
    Calloc_size = size;
    p = alloc_retry_loop(calloc_wrapper, nmemb, "safe_calloc");
  }

  return p;
}  /* safe_calloc */

/*************
 *
 *   safe_free() - free
 *
 *************/

/* PUBLIC */
void safe_free(void *p)
{
  if (p == NULL)
    return;
  free(p);
}  /* safe_free */

/*************
 *
 *   safe_realloc() - realloc with NULL check and retry mechanism
 *
 *************/

/* PUBLIC */
void *safe_realloc(void *p, size_t n)
{
  void *new_p;

  if (p == NULL)
    return safe_malloc(n);

  if (n == 0) {
    free(p);
    return NULL;
  }

  new_p = realloc(p, n);
  if (new_p == NULL)
    fatal_error("safe_realloc: memory allocation failed");

  return new_p;
}  /* safe_realloc */

#endif /* DEBUG */

/* PUBLIC */
int open_private_temp_file(const char *name_template)
{
#ifdef __EMSCRIPTEN__
  (void) name_template;
  return -1;
#else
  const char *directory = getenv("TMPDIR");
  size_t directory_length, template_length, separator, length, position;
  char *path;
  int fd;

  if (directory == NULL || directory[0] == '\0')
    directory = "/tmp";
  if (name_template == NULL || name_template[0] == '\0' ||
      strchr(name_template, '/') != NULL)
    return -1;
  directory_length = strlen(directory);
  template_length = strlen(name_template);
  separator = directory[directory_length - 1] == '/' ? 0 : 1;
  if (directory_length > SIZE_MAX - template_length - separator - 1)
    return -1;
  length = directory_length + separator + template_length + 1;
  path = safe_malloc(length);
  memcpy(path, directory, directory_length);
  position = directory_length;
  if (separator != 0)
    path[position++] = '/';
  memcpy(path + position, name_template, template_length + 1);

  fd = mkstemp(path);
  if (fd >= 0 && unlink(path) != 0) {
    close(fd);
    fd = -1;
  }
  safe_free(path);
  return fd;
#endif
}
