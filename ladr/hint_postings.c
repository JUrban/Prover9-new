/* Stable-ID posting lists used by compressed hint indexes.

   The table owns no Term or Topform pointers.  A reference is one 32-bit
   stable hint ID.  Callers conservatively tolerate entries left behind by
   rewrite, retirement, or expiry and periodically rebuild the index. */

#include "hint_postings.h"

struct hint_posting {
  unsigned long long key;
  unsigned *references;
  unsigned long long *dense_bits;
  unsigned long long *dense_summary;
  unsigned count;
  unsigned capacity;
  unsigned dense_words;
  unsigned dense_summary_words;
  unsigned long long generation;
  unsigned char occupied;
};

struct hint_postings {
  struct hint_posting *table;
  unsigned capacity;
  unsigned keys;
  unsigned long long references;
  unsigned long long reference_capacity;
  unsigned long long maximum_posting;
  unsigned long long dense_bytes;
  unsigned long long dense_budget_bytes;
  unsigned long long dense_budget_denials;
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
  index->dense_budget_bytes = ~0ULL;
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
    if (index->table[i].dense_bits != NULL)
      safe_free(index->table[i].dense_bits);
    if (index->table[i].dense_summary != NULL)
      safe_free(index->table[i].dense_summary);
  }
  safe_free(index->table);
  safe_free(index);
}

static BOOL posting_dense_reserve(struct hint_postings *index,
                                  struct hint_posting *posting,
                                  unsigned bit_capacity)
{
  unsigned words = bit_capacity / 64 + (bit_capacity % 64 != 0);
  unsigned summary_words = words / 64 + (words % 64 != 0);
  unsigned long long added =
    (unsigned long long) (words - posting->dense_words) *
      sizeof(unsigned long long) +
    (unsigned long long) (summary_words - posting->dense_summary_words) *
      sizeof(unsigned long long);
  if (index->dense_bytes > index->dense_budget_bytes ||
      added > index->dense_budget_bytes - index->dense_bytes) {
    index->dense_budget_denials++;
    return FALSE;
  }
  if (words > posting->dense_words) {
    unsigned old_words = posting->dense_words;
    posting->dense_bits = safe_realloc(
      posting->dense_bits, (size_t) words * sizeof(unsigned long long));
    memset(posting->dense_bits + old_words, 0,
           (size_t) (words - old_words) * sizeof(unsigned long long));
    posting->dense_words = words;
  }
  if (summary_words > posting->dense_summary_words) {
    unsigned old_words = posting->dense_summary_words;
    posting->dense_summary = safe_realloc(
      posting->dense_summary,
      (size_t) summary_words * sizeof(unsigned long long));
    memset(posting->dense_summary + old_words, 0,
           (size_t) (summary_words - old_words) *
             sizeof(unsigned long long));
    posting->dense_summary_words = summary_words;
  }
  index->dense_bytes += added;
  return TRUE;
}

static void posting_dense_discard(struct hint_postings *index,
                                  struct hint_posting *posting)
{
  unsigned long long bytes =
    (unsigned long long) posting->dense_words *
      sizeof(unsigned long long) +
    (unsigned long long) posting->dense_summary_words *
      sizeof(unsigned long long);
  if (bytes > index->dense_bytes)
    fatal_error("hint_postings: dense byte underflow");
  if (posting->dense_bits != NULL)
    safe_free(posting->dense_bits);
  if (posting->dense_summary != NULL)
    safe_free(posting->dense_summary);
  posting->dense_bits = NULL;
  posting->dense_summary = NULL;
  posting->dense_words = 0;
  posting->dense_summary_words = 0;
  index->dense_bytes -= bytes;
}

static BOOL posting_dense_add(struct hint_postings *index,
                              struct hint_posting *posting, unsigned id)
{
  unsigned word = id / 64;
  if (word >= posting->dense_words &&
      !posting_dense_reserve(index, posting, id + 1))
    return FALSE;
  posting->dense_bits[word] |= 1ULL << (id % 64);
  posting->dense_summary[word / 64] |= 1ULL << (word % 64);
  return TRUE;
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
  posting->generation++;
  if (posting->generation == 0)
    posting->generation = 1;
  if (posting->dense_bits != NULL &&
      !posting_dense_add(index, posting, id))
    posting_dense_discard(index, posting);
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

unsigned long long hint_postings_generation(Hint_postings index,
                                             unsigned long long key)
{
  struct hint_posting *posting;
  if (index == NULL)
    return 0;
  posting = posting_slot(index, key);
  return posting->occupied ? posting->generation : 0;
}

void hint_postings_set_dense_budget(Hint_postings index,
                                    unsigned long long bytes)
{
  if (index == NULL || index->dense_bytes != 0)
    fatal_error("hint_postings_set_dense_budget: invalid state");
  index->dense_budget_bytes = bytes;
}

BOOL hint_postings_dense_view(Hint_postings index,
                              unsigned long long key,
                              unsigned bit_capacity,
                              BOOL create,
                              struct hint_dense_view *view)
{
  struct hint_posting *posting;
  unsigned i;
  if (view == NULL)
    fatal_error("hint_postings_dense_view: NULL view");
  memset(view, 0, sizeof(*view));
  if (index == NULL)
    return FALSE;
  posting = posting_slot(index, key);
  if (!posting->occupied)
    return FALSE;
  if (posting->dense_bits == NULL) {
    if (!create)
      return FALSE;
    if (!posting_dense_reserve(index, posting, bit_capacity))
      return FALSE;
    for (i = 0; i < posting->count; i++)
      if (!posting_dense_add(index, posting, posting->references[i])) {
        posting_dense_discard(index, posting);
        return FALSE;
      }
  }
  else if (posting->dense_words <
           bit_capacity / 64 + (bit_capacity % 64 != 0)) {
    if (!create)
      return FALSE;
    if (!posting_dense_reserve(index, posting, bit_capacity))
      return FALSE;
  }
  view->bits = posting->dense_bits;
  view->summary = posting->dense_summary;
  view->words = posting->dense_words;
  view->summary_words = posting->dense_summary_words;
  return TRUE;
}

void hint_postings_get_stats(Hint_postings index,
                             struct hint_postings_stats *stats)
{
  unsigned i;
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
  stats->dense_budget_bytes = index->dense_budget_bytes;
  stats->dense_budget_denials = index->dense_budget_denials;
  for (i = 0; i < index->capacity; i++) {
    struct hint_posting *posting = index->table + i;
    if (posting->dense_bits != NULL) {
      stats->dense_keys++;
      stats->dense_bit_bytes +=
        (unsigned long long) posting->dense_words *
          sizeof(unsigned long long);
      stats->dense_summary_bytes +=
        (unsigned long long) posting->dense_summary_words *
          sizeof(unsigned long long);
    }
  }
}
