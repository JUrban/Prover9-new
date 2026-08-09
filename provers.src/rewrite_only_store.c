#include "rewrite_only_store.h"

#include <stdint.h>
#include <string.h>

struct rewrite_only_slot {
  unsigned long long id;  /* zero is empty; nonzero + NULL is a tombstone */
  Topform clause;
  unsigned long long owned_clause_bytes;
  int type;
};

struct rewrite_only_store {
  struct rewrite_only_slot *slots;
  size_t capacity;
  size_t count;
  size_t tombstones;
  unsigned long long owned_clause_bytes;
  unsigned long long peak_allocated_bytes;
};

static size_t hash_id(unsigned long long id)
{
  uint64_t x = id;
  x ^= x >> 30;
  x *= UINT64_C(0xbf58476d1ce4e5b9);
  x ^= x >> 27;
  x *= UINT64_C(0x94d049bb133111eb);
  x ^= x >> 31;
  return (size_t) x;
}

static unsigned long long allocated_bytes(Rewrite_only_store store)
{
  if (store == NULL)
    return 0;
  return sizeof(*store) +
    (unsigned long long) store->capacity * sizeof(*store->slots) +
    store->owned_clause_bytes;
}

static void update_peak(Rewrite_only_store store)
{
  unsigned long long bytes = allocated_bytes(store);
  if (bytes > store->peak_allocated_bytes)
    store->peak_allocated_bytes = bytes;
}

static size_t find_slot(Rewrite_only_store store, unsigned long long id,
                        BOOL for_insert)
{
  size_t mask = store->capacity - 1;
  size_t at = hash_id(id) & mask;
  size_t first_tombstone = SIZE_MAX;

  for (;;) {
    struct rewrite_only_slot *slot = &store->slots[at];
    if (slot->id == 0)
      return for_insert && first_tombstone != SIZE_MAX ? first_tombstone : at;
    if (slot->clause != NULL && slot->id == id)
      return at;
    if (for_insert && slot->clause == NULL && first_tombstone == SIZE_MAX)
      first_tombstone = at;
    at = (at + 1) & mask;
  }
}

static void rehash(Rewrite_only_store store, size_t new_capacity)
{
  struct rewrite_only_slot *old_slots = store->slots;
  size_t old_capacity = store->capacity;
  size_t i;

  store->slots = safe_calloc(new_capacity, sizeof(*store->slots));
  store->capacity = new_capacity;
  store->tombstones = 0;
  for (i = 0; i < old_capacity; i++) {
    struct rewrite_only_slot *old = &old_slots[i];
    if (old->clause != NULL) {
      size_t at = find_slot(store, old->id, TRUE);
      store->slots[at] = *old;
    }
  }
  safe_free(old_slots);
  update_peak(store);
}

static void ensure_insert_capacity(Rewrite_only_store store)
{
  if (store->capacity == 0)
    rehash(store, 64);
  else if ((store->count + store->tombstones + 1) * 10 >=
           store->capacity * 7) {
    if (store->capacity > SIZE_MAX / 2)
      fatal_error("rewrite_only_store: capacity overflow");
    rehash(store, store->capacity * 2);
  }
}

Rewrite_only_store rewrite_only_store_init(void)
{
  Rewrite_only_store store = safe_calloc(1, sizeof(*store));
  update_peak(store);
  return store;
}

BOOL rewrite_only_store_insert(Rewrite_only_store store, Topform clause,
                               int type,
                               unsigned long long owned_clause_bytes)
{
  size_t at;
  struct rewrite_only_slot *slot;
  if (store == NULL || clause == NULL || clause->id == 0)
    return FALSE;
  ensure_insert_capacity(store);
  at = find_slot(store, clause->id, TRUE);
  slot = &store->slots[at];
  if (slot->clause != NULL)
    return FALSE;
  if (slot->id != 0)
    store->tombstones--;
  slot->id = clause->id;
  slot->clause = clause;
  slot->type = type;
  slot->owned_clause_bytes = owned_clause_bytes;
  store->count++;
  store->owned_clause_bytes += owned_clause_bytes;
  update_peak(store);
  return TRUE;
}

Topform rewrite_only_store_find(Rewrite_only_store store,
                                unsigned long long id, int *type,
                                unsigned long long *owned_clause_bytes)
{
  struct rewrite_only_slot *slot;
  size_t at;
  if (store == NULL || id == 0 || store->capacity == 0)
    return NULL;
  at = find_slot(store, id, FALSE);
  slot = &store->slots[at];
  if (slot->id != id || slot->clause == NULL)
    return NULL;
  if (type != NULL)
    *type = slot->type;
  if (owned_clause_bytes != NULL)
    *owned_clause_bytes = slot->owned_clause_bytes;
  return slot->clause;
}

Topform rewrite_only_store_remove(Rewrite_only_store store,
                                  unsigned long long id, int *type,
                                  unsigned long long *owned_clause_bytes)
{
  struct rewrite_only_slot *slot;
  Topform clause;
  size_t at;
  if (store == NULL || id == 0 || store->capacity == 0)
    return NULL;
  at = find_slot(store, id, FALSE);
  slot = &store->slots[at];
  if (slot->id != id || slot->clause == NULL)
    return NULL;
  clause = slot->clause;
  if (type != NULL)
    *type = slot->type;
  if (owned_clause_bytes != NULL)
    *owned_clause_bytes = slot->owned_clause_bytes;
  if (store->owned_clause_bytes < slot->owned_clause_bytes)
    fatal_error("rewrite_only_store: byte accounting underflow");
  store->owned_clause_bytes -= slot->owned_clause_bytes;
  slot->clause = NULL;
  slot->owned_clause_bytes = 0;
  slot->type = 0;
  store->count--;
  store->tombstones++;
  return clause;
}

Topform rewrite_only_store_take_any(Rewrite_only_store store, int *type,
                                    unsigned long long *owned_clause_bytes)
{
  size_t i;
  if (store == NULL)
    return NULL;
  for (i = 0; i < store->capacity; i++)
    if (store->slots[i].clause != NULL)
      return rewrite_only_store_remove(store, store->slots[i].id, type,
                                       owned_clause_bytes);
  return NULL;
}

unsigned long long rewrite_only_store_count(Rewrite_only_store store)
{
  return store == NULL ? 0 : store->count;
}

unsigned long long rewrite_only_store_allocated_bytes(
  Rewrite_only_store store)
{
  return allocated_bytes(store);
}

unsigned long long rewrite_only_store_peak_allocated_bytes(
  Rewrite_only_store store)
{
  return store == NULL ? 0 : store->peak_allocated_bytes;
}

unsigned long long rewrite_only_store_identity_hash(Rewrite_only_store store)
{
  uint64_t hash = 0;
  size_t i;
  if (store == NULL)
    return 0;
  /* Commutative across hash-table layouts; checkpoint identity is the set of
     stable proof IDs and legacy rule types, not transient slot placement. */
  for (i = 0; i < store->capacity; i++)
    if (store->slots[i].clause != NULL)
      hash ^= hash_id(store->slots[i].id ^
                      ((uint64_t) (unsigned) store->slots[i].type << 56));
  return hash;
}

void rewrite_only_store_free(Rewrite_only_store store)
{
  if (store == NULL)
    return;
  if (store->count != 0)
    fatal_error("rewrite_only_store_free: live rules remain");
  safe_free(store->slots);
  safe_free(store);
}
