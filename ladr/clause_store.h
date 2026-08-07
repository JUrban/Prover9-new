/* Compact append-order storage for retained disabled clauses. */

#ifndef TP_CLAUSE_STORE_H
#define TP_CLAUSE_STORE_H

#include "topform.h"

typedef struct clause_store * Clause_store;

Clause_store clause_store_init(const char *name);

void clause_store_free(Clause_store store);

void clause_store_delete_clauses(Clause_store store);

void clause_store_append(Clause_store store, Topform c);

BOOL clause_store_member(Clause_store store, Topform c);

size_t clause_store_length(Clause_store store);

Topform clause_store_get(Clause_store store, size_t position);

void clause_store_sort_by_id(Clause_store store);

unsigned long long clause_store_allocated_bytes(Clause_store store);

unsigned long long clause_store_legacy_clist_bytes(Clause_store store);

#endif  /* TP_CLAUSE_STORE_H */
