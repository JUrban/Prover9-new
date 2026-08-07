/* Compact append-order storage for retained disabled clauses. */

#include "clause_store.h"
#include "clist.h"
#include "clause_misc.h"
#include "memory.h"
#include <limits.h>

struct clause_store {
  Topform *clauses;
  size_t length;
  size_t capacity;
};

/* PUBLIC */
Clause_store clause_store_init(const char *name)
{
  Clause_store store = safe_malloc(sizeof(struct clause_store));
  (void) name;
  store->clauses = NULL;
  store->length = 0;
  store->capacity = 0;
  return store;
}

/* PUBLIC */
void clause_store_free(Clause_store store)
{
  size_t i;
  if (store == NULL)
    return;
  for (i = 0; i < store->length; i++)
    store->clauses[i]->disabled = 0;
  safe_free(store->clauses);
  safe_free(store);
}

/* PUBLIC */
void clause_store_delete_clauses(Clause_store store)
{
  size_t i;
  if (store == NULL)
    return;
  for (i = 0; i < store->length; i++) {
    Topform c = store->clauses[i];
    c->disabled = 0;
    if (c->containers == NULL)
      delete_clause(c);
  }
  store->length = 0;
  clause_store_free(store);
}

/* PUBLIC */
void clause_store_append(Clause_store store, Topform c)
{
  if (store == NULL || c == NULL)
    fatal_error("clause_store_append: null argument");
  if (c->disabled)
    fatal_error("clause_store_append: clause is already disabled");
  if (store->length == store->capacity) {
    size_t new_capacity = store->capacity == 0 ? 16 : store->capacity * 2;
    if (new_capacity < store->capacity ||
        new_capacity > ((size_t) -1) / sizeof(Topform))
      fatal_error("clause_store_append: capacity overflow");
    store->clauses = safe_realloc(store->clauses,
                                  new_capacity * sizeof(Topform));
    store->capacity = new_capacity;
  }
  store->clauses[store->length++] = c;
  c->disabled = 1;
}

/* PUBLIC */
BOOL clause_store_member(Clause_store store, Topform c)
{
  return store != NULL && c != NULL && c->disabled;
}

/* PUBLIC */
size_t clause_store_length(Clause_store store)
{
  return store == NULL ? 0 : store->length;
}

/* PUBLIC */
Topform clause_store_get(Clause_store store, size_t position)
{
  if (store == NULL || position >= store->length)
    fatal_error("clause_store_get: position out of range");
  return store->clauses[position];
}

/* PUBLIC */
void clause_store_sort_by_id(Clause_store store)
{
  if (store != NULL && store->length > 1) {
    if (store->length > (size_t) INT_MAX)
      fatal_error("clause_store_sort_by_id: too many clauses");
    merge_sort((void **) store->clauses, (int) store->length,
               (Ordertype (*)(void *, void *)) cl_id_compare);
  }
}

/* PUBLIC */
unsigned long long clause_store_allocated_bytes(Clause_store store)
{
  return store == NULL ? 0 :
    (unsigned long long) sizeof(struct clause_store) +
    (unsigned long long) store->capacity * sizeof(Topform);
}

/* PUBLIC */
unsigned long long clause_store_legacy_clist_bytes(Clause_store store)
{
  return store == NULL ? 0 :
    (unsigned long long) sizeof(struct clist) +
    (unsigned long long) store->length * sizeof(struct clist_pos);
}
