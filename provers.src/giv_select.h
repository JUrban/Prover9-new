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

#ifndef TP_GIV_SELECT_H
#define TP_GIV_SELECT_H

#include "search-structures.h"

/* INTRODUCTION
*/

/* Public definitions */

/* End of public definitions */

/* Public function prototypes from giv_select.c */

typedef size_t (*Dense_passive_archive_fn)(Topform c,
                                           unsigned *body_bytes,
                                           unsigned *justification_bytes,
                                           unsigned *logical_body_bytes);
typedef Topform (*Dense_passive_activate_fn)(size_t store_position,
                                             unsigned long long id,
                                             unsigned long long hint_id);

typedef enum {
  DENSE_DIRECTORY_MEMORY,
  DENSE_DIRECTORY_FILE
} Dense_passive_directory_mode;

typedef enum {
  DENSE_SELECTOR_HEAP,
  DENSE_SELECTOR_FILE
} Dense_passive_selector_mode;

struct dense_passive_directory_stats {
  Dense_passive_directory_mode mode;
  unsigned entry_bytes;
  unsigned long long logical_bytes;
  unsigned long long allocated_bytes;
  unsigned long long file_eviction_passes;
  unsigned long long file_eviction_bytes;
  unsigned long long file_eviction_failures;
};

struct dense_passive_selector_stats {
  Dense_passive_selector_mode mode;
  unsigned long long buffer_limit;
  unsigned record_reference_bits;
  unsigned entry_bytes;
  unsigned long long buffered_entries;
  unsigned long long buffer_bytes;
  unsigned long long run_entries;
  unsigned long long run_logical_bytes;
  unsigned long long run_physical_bytes;
  unsigned long long runs;
  unsigned long long peak_runs;
  unsigned long long flushes;
  unsigned long long merges;
  unsigned long long file_reads;
  unsigned long long file_read_bytes;
  unsigned long long file_writes;
  unsigned long long file_write_bytes;
  unsigned long long file_evictions;
  unsigned long long file_eviction_bytes;
  unsigned long long file_eviction_failures;
  unsigned long long file_read_evictions;
  unsigned long long file_read_eviction_bytes;
  unsigned long long file_read_eviction_failures;
  unsigned long long file_min_calls;
  unsigned long long file_buffer_checks;
  unsigned long long file_run_checks;
  unsigned long long stale_entries_discarded;
};

/* Read-only metadata for one active dense passive.  Dense passives are
   visited in insertion (and therefore clause-ID) order. */
struct dense_passive_view {
  unsigned long long id;
  unsigned long long hint_id;
  size_t store_position;
  double weight;
  unsigned simplifier_epoch;
  unsigned rewrite_epoch;
  unsigned first_fpa_id;
  unsigned body_bytes;
  unsigned justification_bytes;
  unsigned logical_body_bytes;
  int semantics;
  BOOL used;
  BOOL delayed_demodulator;
  BOOL rewrite_rule_dirty;
  BOOL archive_metadata_dirty;
};

typedef void (*Dense_passive_visit_fn)(
  const struct dense_passive_view *view, void *context);

typedef size_t (*Dense_passive_relocate_fn)(size_t old_position,
                                            void *context);

void configure_dense_passive(BOOL enabled,
                             Dense_passive_archive_fn archive_fn,
                             Dense_passive_activate_fn activate_fn);

void configure_dense_passive_directory(
  Dense_passive_directory_mode mode);

struct dense_passive_directory_stats dense_passive_directory_stats(void);

void configure_dense_passive_selectors(Dense_passive_selector_mode mode,
                                       size_t buffer_entries);

struct dense_passive_selector_stats dense_passive_selector_stats(void);

/* Checked conversion used by immutable file-selector entries.  Exposed so
   numerical-limit tests can cross 32-bit boundaries without allocating
   billions of passive records. */
unsigned long long dense_passive_file_record_reference(size_t record);
BOOL dense_passive_file_record_index(unsigned long long reference,
                                     size_t *record);

BOOL dense_passive_enabled(void);

unsigned long long dense_passive_size(void);

BOOL dense_passive_contains_id(unsigned long long id);

BOOL dense_passive_view_id(unsigned long long id,
                           struct dense_passive_view *view);

BOOL dense_passive_mark_used(unsigned long long id);

void dense_passive_foreach(Dense_passive_visit_fn visit, void *context);

#define DENSE_STALE_GENERAL 0
#define DENSE_STALE_HINTED  1
#define DENSE_STALE_REWRITE 2

unsigned dense_passive_scan_stale(size_t *cursor, unsigned rewrite_epoch,
                                  int filter, unsigned scan_limit,
                                  struct dense_passive_view *view);

/* Convert a physical scan cursor to/from the ID of its next active record.
   Checkpoints use this stable form because inactive records are omitted when
   the dense store is rebuilt.  ID 0 denotes an empty store. */
unsigned long long dense_passive_cursor_id(size_t cursor);

size_t dense_passive_cursor_from_id(unsigned long long id);

BOOL dense_passive_deactivate_id(unsigned long long id,
                                 struct dense_passive_view *view);

BOOL dense_passive_reactivate_id(unsigned long long id,
                                 unsigned simplifier_epoch,
                                 unsigned rewrite_epoch,
                                 BOOL delayed_demodulator,
                                 BOOL rewrite_rule_dirty);

/* Mark a live cold rewrite rule for exact interreduction.  Returns TRUE only
   on the clean-to-dirty transition, so repeated overlap postings coalesce. */
BOOL dense_passive_mark_rule_dirty(unsigned long long id);

unsigned long long dense_passive_stale_count(unsigned rewrite_epoch,
                                             unsigned long long *max_lag);

void dense_passive_set_rewrite_epoch(unsigned rewrite_epoch);

unsigned long long dense_passive_rewrite_debt(void);

void dense_passive_memory(unsigned long long *record_bytes,
                          unsigned long long *heap_bytes,
                          unsigned long long *records);

void dense_passive_payload_memory(unsigned long long *body_bytes,
                                  unsigned long long *justification_bytes,
                                  unsigned long long *logical_body_bytes);

unsigned long long dense_passive_delayed_demodulators(void);

BOOL dense_passive_compaction_needed(void);

void dense_passive_compact(Dense_passive_relocate_fn relocate,
                           void *context);

void dense_passive_compaction_stats(unsigned long long *compactions,
                                    unsigned long long *records_reclaimed);

void init_giv_select(Plist rules);

void insert_into_sos2(Topform c, Clist sos);

void remove_from_sos2(Topform c, Clist sos);

BOOL givens_available(void);

Topform get_given_clause2(Clist sos, int num_given,
			 Prover_options opt, char **type);

BOOL sos_keep2(Topform c, Clist sos, Prover_options opt);

void sos_displace2(void (*disable_proc) (Topform), BOOL quiet);

void reset_selector_indexes(void);

void zap_given_selectors(void);

void selector_report(void);

void fprint_selector_report(FILE *fp);

void get_low_selector_state(const char **name, int *count);

void set_low_selector_state(const char *name, int count);

void get_high_selector_state(const char **name, int *count);

void set_high_selector_state(const char *name, int count);

/* Read-only selector classification for a scratch, normalized candidate.
   The bit layout is identical to dense passive selector masks.  Priority is
   0 for a high-priority match, 1 for a low-priority match, and 2 when no
   selector matches. */
void given_selection_preview(Topform c,
			     unsigned long long *selector_mask,
			     unsigned *priority);

Term selector_rule_term(char *name, char *priority,
			char *order, char *rule, int part);

Plist selector_rules_from_options(Prover_options opt);

void bulk_insert_into_sos2(Clist sos);

#endif  /* conditional compilation of whole file */
