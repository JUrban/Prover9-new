/* Stable-ID posting lists used by compressed hint indexes.

   The table owns no Term or Topform pointers.  A reference is one 32-bit
   stable hint ID.  Callers conservatively tolerate entries left behind by
   rewrite, retirement, or expiry and periodically rebuild the index. */

#include "hint_postings.h"

struct hint_posting {
  unsigned long long key;
  unsigned *references;
  unsigned count;
  unsigned capacity;
  unsigned char occupied;
};

struct hint_postings {
  struct hint_posting *table;
  unsigned capacity;
  unsigned keys;
  unsigned long long references;
  unsigned long long reference_capacity;
  unsigned long long maximum_posting;
};

static unsigned long long posting_hash(unsigned long long x)
{
  x ^= x >> 30;
  x *= 0xbf58476d1ce4e5b9ULL;
  x ^= x >> 27;
  x *= 0x94d049bb133111ebULL;
  x ^= x >> 31;
  return x;
}

static struct hint_posting *posting_slot(struct hint_postings *index,
                                         unsigned long long key)
{
  unsigned mask = index->capacity - 1;
  unsigned slot = (unsigned) posting_hash(key) & mask;
  while (index->table[slot].occupied && index->table[slot].key != key)
    slot = (slot + 1) & mask;
  return index->table + slot;
}

static void posting_rehash(struct hint_postings *index, unsigned capacity)
{
  struct hint_posting *old_table = index->table;
  unsigned old_capacity = index->capacity;
  unsigned i;
  index->table = safe_calloc(capacity, sizeof(struct hint_posting));
  index->capacity = capacity;
  index->keys = 0;
  for (i = 0; i < old_capacity; i++) {
    if (old_table[i].occupied) {
      struct hint_posting *dest = posting_slot(index, old_table[i].key);
      *dest = old_table[i];
      index->keys++;
    }
  }
  safe_free(old_table);
}

Hint_postings hint_postings_init(void)
{
  struct hint_postings *index = safe_calloc(1, sizeof(struct hint_postings));
  index->capacity = 256;
  index->table = safe_calloc(index->capacity, sizeof(struct hint_posting));
  return index;
}

void hint_postings_destroy(Hint_postings index)
{
  unsigned i;
  if (index == NULL)
    return;
  for (i = 0; i < index->capacity; i++) {
    if (index->table[i].references != NULL)
      safe_free(index->table[i].references);
  }
  safe_free(index->table);
  safe_free(index);
}

void hint_postings_add(Hint_postings index, unsigned long long key,
                       unsigned id)
{
  struct hint_posting *posting;
  if (index == NULL || id == 0)
    fatal_error("hint_postings_add: invalid index or ID");
  if ((unsigned long long) index->keys * 10 >=
      (unsigned long long) index->capacity * 7)
    posting_rehash(index, index->capacity * 2);
  posting = posting_slot(index, key);
  if (!posting->occupied) {
    posting->occupied = 1;
    posting->key = key;
    index->keys++;
  }
  if (posting->count == posting->capacity) {
    unsigned old = posting->capacity;
    unsigned capacity = old == 0 ? 4 : old + (old + 1) / 2;
    if (capacity <= old)
      fatal_error("hint_postings_add: posting capacity overflow");
    posting->references = safe_realloc(
      posting->references,
      (size_t) capacity * sizeof(unsigned));
    posting->capacity = capacity;
    index->reference_capacity += capacity - old;
  }
  posting->references[posting->count++] = id;
  index->references++;
  if (posting->count > index->maximum_posting)
    index->maximum_posting = posting->count;
}

const unsigned *hint_postings_get(Hint_postings index,
                                  unsigned long long key,
                                  unsigned *count)
{
  struct hint_posting *posting;
  if (count == NULL)
    fatal_error("hint_postings_get: NULL count");
  if (index == NULL) {
    *count = 0;
    return NULL;
  }
  posting = posting_slot(index, key);
  if (!posting->occupied) {
    *count = 0;
    return NULL;
  }
  *count = posting->count;
  return posting->references;
}

void hint_postings_get_stats(Hint_postings index,
                             struct hint_postings_stats *stats)
{
  if (stats == NULL)
    fatal_error("hint_postings_get_stats: NULL stats");
  memset(stats, 0, sizeof(struct hint_postings_stats));
  if (index == NULL)
    return;
  stats->keys = index->keys;
  stats->references = index->references;
  stats->table_bytes =
    (unsigned long long) index->capacity * sizeof(struct hint_posting) +
    sizeof(struct hint_postings);
  stats->reference_bytes = index->reference_capacity *
                           sizeof(unsigned);
  stats->maximum_posting = index->maximum_posting;
}
