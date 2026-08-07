/* Compact append-order storage for retained disabled clauses. */

#ifndef TP_CLAUSE_STORE_H
#define TP_CLAUSE_STORE_H

#include "topform.h"

typedef struct clause_store * Clause_store;

typedef enum {
  CLAUSE_STORE_ARCHIVE_OFF,
  CLAUSE_STORE_ARCHIVE_MEMORY,
  CLAUSE_STORE_ARCHIVE_MMAP
} Clause_store_archive_mode;

struct clause_store_stats {
  unsigned long long records;
  unsigned long long record_bytes;
  unsigned long long backing_bytes;
  unsigned long long handle_bytes;
  unsigned long long body_bytes;
  unsigned long long logical_body_bytes;
  unsigned long long materializations;
  unsigned long long validation_failures;
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

Topform clause_store_materialize_by_id(unsigned long long id);

Ilist clause_parents_by_id(unsigned long long id);

BOOL clause_negative_by_id(unsigned long long id, BOOL *known);

unsigned long long clause_store_matching_hint_id(unsigned long long id);

void clause_store_release_materialized(Topform c);

void clause_store_release_materialized_plist(Plist clauses);

void clause_store_sort_by_id(Clause_store store);

BOOL clause_store_sync(Clause_store store);

struct clause_store_stats clause_store_get_stats(Clause_store store);

unsigned long long clause_store_allocated_bytes(Clause_store store);

unsigned long long clause_store_legacy_clist_bytes(Clause_store store);

/* Test hook: XOR one byte in the append-only backing. */
BOOL clause_store_test_corrupt(Clause_store store,
                               unsigned long long absolute_offset,
                               unsigned char mask);

#endif  /* TP_CLAUSE_STORE_H */
