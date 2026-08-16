/* Compact append-order storage for retained disabled clauses. */

#ifndef TP_CLAUSE_STORE_H
#define TP_CLAUSE_STORE_H

#include "topform.h"

typedef struct clause_store * Clause_store;

typedef enum {
  CLAUSE_STORE_ARCHIVE_OFF,
  CLAUSE_STORE_ARCHIVE_MEMORY,
  CLAUSE_STORE_ARCHIVE_MMAP,
  /* Anonymous temporary-file storage accessed through one bounded scratch
     record.  The file remains in the kernel page cache, but never becomes a
     process mapping and therefore is not charged wholesale to process RSS. */
  CLAUSE_STORE_ARCHIVE_FILE
} Clause_store_archive_mode;

struct clause_store_stats {
  unsigned long long records;
  unsigned long long record_bytes;
  unsigned long long backing_bytes;
  unsigned long long physical_bytes;
  unsigned long long handle_bytes;
  unsigned long long body_bytes;
  unsigned long long logical_body_bytes;
  unsigned long long materializations;
  unsigned long long validation_failures;
  unsigned long long mmap_eviction_passes;
  unsigned long long mmap_eviction_bytes;
  unsigned long long mmap_scan_eviction_passes;
  unsigned long long mmap_scan_eviction_bytes;
  unsigned long long io_buffer_bytes;
  unsigned long long write_buffer_bytes;
  unsigned long long file_reads;
  unsigned long long file_read_bytes;
  unsigned long long file_writes;
  unsigned long long file_write_bytes;
  unsigned long long file_write_calls;
  unsigned long long file_write_call_bytes;
  unsigned long long file_cache_eviction_passes;
  unsigned long long file_cache_eviction_bytes;
  unsigned long long file_syncs;
  unsigned long long file_cache_eviction_failures;
  unsigned long long offset_lookups;
  unsigned long long detached_records;
  unsigned long long detached_current;
  unsigned long long handle_bytes_avoided;
};

Clause_store clause_store_init(const char *name);

BOOL clause_store_enable_archive(Clause_store store,
                                 Clause_store_archive_mode mode);

void clause_store_free(Clause_store store);

void clause_store_delete_clauses(Clause_store store);

void clause_store_append(Clause_store store, Topform c);

/* Replace a resident appended clause by an immutable record and offset.
   Failure is non-destructive: the resident (possibly body-compressed)
   Topform remains registered and stored. */
BOOL clause_store_archive_clause(Clause_store store, Topform c);

/* Archive the appended clause through a temporary serialization copy, then
   leave the original materialized body alive but detached from the store and
   ID table.  The caller assumes ownership of that original Topform. */
BOOL clause_store_archive_clause_preserve(Clause_store store, Topform c);

/* Archive a clause whose immutable record is owned by an external compact
   directory.  OFFSET is the stable record address; no entry is added to the
   store's cumulative handle array.  The caller must eventually activate or
   discard every detached record before destroying the store. */
BOOL clause_store_archive_detached(Clause_store store, Topform c,
                                   size_t *offset);

BOOL clause_store_member(Clause_store store, Topform c);

size_t clause_store_length(Clause_store store);

size_t clause_store_current_length(Clause_store store);

BOOL clause_store_position_is_archived(Clause_store store, size_t position);

BOOL clause_store_position_is_current(Clause_store store, size_t position);

/* Return compact body, encoded justification, and estimated materialized-body
   bytes for one immutable record without decoding it. */
BOOL clause_store_payload_sizes(Clause_store store, size_t position,
                                unsigned long long *body_bytes,
                                unsigned long long *justification_bytes,
                                unsigned long long *logical_body_bytes);

unsigned long long clause_store_id(Clause_store store, size_t position);

BOOL clause_store_negative(Clause_store store, size_t position);

Ilist clause_store_parents(Clause_store store, size_t position);

Topform clause_store_get(Clause_store store, size_t position);

Topform clause_store_materialize(Clause_store store, size_t position);

Topform clause_store_activate(Clause_store store, size_t position);

Topform clause_store_materialize_offset(Clause_store store, size_t offset);

Topform clause_store_activate_offset(Clause_store store, size_t offset,
                                     unsigned long long expected_id);

BOOL clause_store_discard_detached(Clause_store store, size_t offset,
                                   unsigned long long expected_id);

Topform clause_store_materialize_by_id(unsigned long long id);

Ilist clause_parents_by_id(unsigned long long id);

BOOL clause_negative_by_id(unsigned long long id, BOOL *known);

unsigned long long clause_store_matching_hint_id(unsigned long long id);

void clause_store_release_materialized(Topform c);

void clause_store_release_materialized_plist(Plist clauses);

void clause_store_sort_by_id(Clause_store store);

BOOL clause_store_sync(Clause_store store);

/* Discard mmap pages covering an already-consumed inclusive range of
   immutable archive records.  Other archive modes are harmless no-ops.
   This bounds resident file pages during ordered reconstruction scans. */
void clause_store_advise_mmap_range_cold(Clause_store store,
                                         size_t first_position,
                                         size_t last_position);

void clause_store_advise_mmap_offsets_cold(Clause_store store,
                                           size_t first_offset,
                                           size_t last_offset);

struct clause_store_stats clause_store_get_stats(Clause_store store);

unsigned long long clause_store_allocated_bytes(Clause_store store);

unsigned long long clause_store_legacy_clist_bytes(Clause_store store);

/* Test hook: XOR one byte in the append-only backing. */
BOOL clause_store_test_corrupt(Clause_store store,
                               unsigned long long absolute_offset,
                               unsigned char mask);

#endif  /* TP_CLAUSE_STORE_H */
