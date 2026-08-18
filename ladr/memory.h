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

#ifndef TP_MEMORY_H
#define TP_MEMORY_H

#include "fatal.h"
#include <sys/time.h>
#include <sys/resource.h>
#include <unistd.h>

/* INTRODUCTION
*/

/* Public definitions */

/* The following definitions exist because the memory get/free
   routines measure memory by pointers instead of bytes. */

#define CEILING(n,d)   ((n)%(d) == 0 ? (n)/(d) : (n)/(d) + 1)
#define BYTES_POINTER  sizeof(void *)  /* bytes per pointer */
#define PTRS(n)        CEILING(n, BYTES_POINTER) /* ptrs needed for n bytes */

/* End of public definitions */

/* Snapshot of the pointer-count allocator.  "logical" counts bytes requested
   from get_mem/get_cmem (rounded only to their public pointer units), while
   "reserved" counts the backing mappings plus directly allocated storage. */

struct memory_stats {
  unsigned long long logical_live_bytes;
  unsigned long long logical_peak_bytes;
  unsigned long long reserved_bytes;
  unsigned long long peak_reserved_bytes;
  unsigned long long reusable_bytes;
  unsigned long long unallocated_bytes;
  unsigned long long metadata_bytes;
  unsigned long long fragmentation_bytes;
  unsigned long long direct_live_bytes;
  unsigned long long permanent_live_bytes;
  unsigned long long slab_count;
  unsigned long long peak_slab_count;
  unsigned long long cached_slabs;
  unsigned long long peak_cached_slabs;
  unsigned long long reused_slabs;
  unsigned long long slab_cache_evictions;
  unsigned long long reclaimed_slabs;
  unsigned long long reclaimed_bytes;
  unsigned long long cumulative_bytes;
};

/* Whole-process diagnostics.  smaps values are KiB as reported by the
   kernel; libc values are bytes.  Unsupported platform fields remain zero. */
struct memory_process_stats {
  BOOL smaps_supported;
  BOOL libc_heap_supported;
  unsigned long long rss_kbytes;
  unsigned long long pss_kbytes;
  unsigned long long anonymous_kbytes;
  unsigned long long shared_clean_kbytes;
  unsigned long long shared_dirty_kbytes;
  unsigned long long private_clean_kbytes;
  unsigned long long private_dirty_kbytes;
  unsigned long long swap_kbytes;
  unsigned long long libc_arena_bytes;
  unsigned long long libc_mmap_bytes;
  unsigned long long libc_in_use_bytes;
  unsigned long long libc_free_bytes;
  unsigned long long libc_releasable_bytes;
};

/* Public function prototypes from memory.c */

void *get_cmem(unsigned n);

void *get_mem(unsigned n);

void free_mem(void *q, unsigned n);

void memory_report(FILE *fp);

void memory_get_stats(struct memory_stats *stats);

void memory_get_process_stats(struct memory_process_stats *stats);

/* Request a low-retention libc heap policy before workload allocation.
   Returns FALSE on unsupported platforms or if libc rejects the request. */
BOOL memory_configure_compact_system_heap(void);

BOOL memory_compact_system_heap_enabled(void);

void memory_release_unused(void);

unsigned long long memory_slab_bytes(void);

unsigned long long memory_slab_cache_limit(void);

unsigned long long memory_current_rss_kbytes(void);

unsigned long long memory_peak_rss_kbytes(void);

BOOL memory_current_rss_supported(void);

BOOL memory_can_return_pages_to_os(void);

long long megs_malloced(void);

void set_max_megs(int megs);

void set_max_megs_proc(void (*proc)(void));

unsigned long long bytes_palloced(void);

void *tp_alloc(size_t n);

unsigned mega_mem_calls(void);

unsigned long long memory_allocation_calls(void);

void disable_max_megs(void);

void enable_max_megs(void);

void *safe_malloc(size_t n);

void *safe_calloc(size_t nmemb, size_t size);

void safe_free(void *p);

void *safe_realloc(void *p, size_t n);

/* Create and immediately unlink a private temporary file in TMPDIR.  An
   unset or empty TMPDIR uses /tmp.  The caller owns the returned descriptor;
   -1 reports creation or unlink failure. */
int open_private_temp_file(const char *name_template);

void enable_memory_logging(void);

void disable_memory_logging(void);

void memory_logging_summary(FILE *fp);

unsigned long long get_total_allocated(void);

unsigned long long get_total_freed(void);

#endif  /* conditional compilation of whole file */
