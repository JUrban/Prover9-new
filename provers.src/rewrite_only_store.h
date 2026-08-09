#ifndef TP_REWRITE_ONLY_STORE_H
#define TP_REWRITE_ONLY_STORE_H

#include "../ladr/ladr.h"

typedef struct rewrite_only_store * Rewrite_only_store;

Rewrite_only_store rewrite_only_store_init(void);

BOOL rewrite_only_store_insert(Rewrite_only_store store, Topform clause,
                               int type,
                               unsigned long long owned_clause_bytes);

Topform rewrite_only_store_find(Rewrite_only_store store,
                                unsigned long long id, int *type,
                                unsigned long long *owned_clause_bytes);

Topform rewrite_only_store_remove(Rewrite_only_store store,
                                  unsigned long long id, int *type,
                                  unsigned long long *owned_clause_bytes);

Topform rewrite_only_store_take_any(Rewrite_only_store store, int *type,
                                    unsigned long long *owned_clause_bytes);

unsigned long long rewrite_only_store_count(Rewrite_only_store store);

unsigned long long rewrite_only_store_allocated_bytes(
  Rewrite_only_store store);

unsigned long long rewrite_only_store_peak_allocated_bytes(
  Rewrite_only_store store);

unsigned long long rewrite_only_store_identity_hash(Rewrite_only_store store);

void rewrite_only_store_free(Rewrite_only_store store);

#endif
