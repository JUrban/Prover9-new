/* Stable-ID posting lists used by compressed hint indexes.

   The table owns no Term or Topform pointers.  A reference is one 32-bit
   stable hint ID.  Callers conservatively tolerate entries left behind by
   rewrite, retirement, or expiry and periodically rebuild the index. */

#include "hint_postings.h"

struct hint_posting {
  unsigned long long key;
  unsigned *references;
  unsigned long long *profile_mask_planes;
  unsigned *profile_literal_counts;
  union {
    unsigned long long profile_mask_union;
    unsigned long long *dense_bits;
  } profile_or_dense;
  union {
    /* Profile postings use two words per 64 IDs; ordinary postings use the
       same pointer slot for the optional dense hierarchy. */
    unsigned long long *profile_block_summaries;
    unsigned long long *dense_summary;
  } summary;
  unsigned count;
  unsigned capacity;
  union {
    struct {
      unsigned words;
      unsigned summary_words;
    } dense;
    struct {
      unsigned short maximum_positive;
      unsigned short maximum_negative;
    } profile;
  } counts_or_dense;
  unsigned profile_mask_blocks;
  unsigned long long generation;
  unsigned char occupied;
  unsigned char profile;
};

struct hint_postings {
  struct hint_posting *table;
  unsigned capacity;
  unsigned keys;
  unsigned long long references;
  unsigned long long reference_capacity;
  unsigned long long profile_reference_capacity;
  unsigned long long profile_mask_bytes;
  unsigned long long profile_summary_bytes;
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
    if (index->table[i].profile_mask_planes != NULL)
      safe_free(index->table[i].profile_mask_planes);
    if (index->table[i].profile_literal_counts != NULL)
      safe_free(index->table[i].profile_literal_counts);
    if (!index->table[i].profile) {
      if (index->table[i].profile_or_dense.dense_bits != NULL)
        safe_free(index->table[i].profile_or_dense.dense_bits);
    }
    if (index->table[i].summary.dense_summary != NULL)
      safe_free(index->table[i].summary.dense_summary);
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
    (unsigned long long) (words - posting->counts_or_dense.dense.words) *
      sizeof(unsigned long long) +
    (unsigned long long) (summary_words -
                          posting->counts_or_dense.dense.summary_words) *
      sizeof(unsigned long long);
  if (index->dense_bytes > index->dense_budget_bytes ||
      added > index->dense_budget_bytes - index->dense_bytes) {
    index->dense_budget_denials++;
    return FALSE;
  }
  if (words > posting->counts_or_dense.dense.words) {
    unsigned old_words = posting->counts_or_dense.dense.words;
    posting->profile_or_dense.dense_bits = safe_realloc(
      posting->profile_or_dense.dense_bits,
      (size_t) words * sizeof(unsigned long long));
    memset(posting->profile_or_dense.dense_bits + old_words, 0,
           (size_t) (words - old_words) * sizeof(unsigned long long));
    posting->counts_or_dense.dense.words = words;
  }
  if (summary_words > posting->counts_or_dense.dense.summary_words) {
    unsigned old_words = posting->counts_or_dense.dense.summary_words;
    posting->summary.dense_summary = safe_realloc(
      posting->summary.dense_summary,
      (size_t) summary_words * sizeof(unsigned long long));
    memset(posting->summary.dense_summary + old_words, 0,
           (size_t) (summary_words - old_words) *
             sizeof(unsigned long long));
    posting->counts_or_dense.dense.summary_words = summary_words;
  }
  index->dense_bytes += added;
  return TRUE;
}

static void posting_dense_discard(struct hint_postings *index,
                                  struct hint_posting *posting)
{
  unsigned long long bytes =
    (unsigned long long) posting->counts_or_dense.dense.words *
      sizeof(unsigned long long) +
    (unsigned long long) posting->counts_or_dense.dense.summary_words *
      sizeof(unsigned long long);
  if (bytes > index->dense_bytes)
    fatal_error("hint_postings: dense byte underflow");
  if (posting->profile_or_dense.dense_bits != NULL)
    safe_free(posting->profile_or_dense.dense_bits);
  if (posting->summary.dense_summary != NULL)
    safe_free(posting->summary.dense_summary);
  posting->profile_or_dense.dense_bits = NULL;
  posting->summary.dense_summary = NULL;
  posting->counts_or_dense.dense.words = 0;
  posting->counts_or_dense.dense.summary_words = 0;
  index->dense_bytes -= bytes;
}

static BOOL posting_dense_add(struct hint_postings *index,
                              struct hint_posting *posting, unsigned id)
{
  unsigned word = id / 64;
  if (word >= posting->counts_or_dense.dense.words &&
      !posting_dense_reserve(index, posting, id + 1))
    return FALSE;
  posting->profile_or_dense.dense_bits[word] |= 1ULL << (id % 64);
  posting->summary.dense_summary[word / 64] |= 1ULL << (word % 64);
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
  else if (posting->profile)
    fatal_error("hint_postings_add: profile key reused as ordinary key");
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
  if (posting->profile_or_dense.dense_bits != NULL &&
      !posting_dense_add(index, posting, id))
    posting_dense_discard(index, posting);
  index->references++;
  if (posting->count > index->maximum_posting)
    index->maximum_posting = posting->count;
}

void hint_postings_add_profile(Hint_postings index, unsigned long long key,
                               unsigned id, unsigned long long mask,
                               unsigned positive, unsigned negative)
{
  struct hint_posting *posting;
  if (index == NULL || id == 0 ||
      positive > USHRT_MAX || negative > USHRT_MAX)
    fatal_error("hint_postings_add_profile: invalid argument");
  if ((unsigned long long) index->keys * 10 >=
      (unsigned long long) index->capacity * 7)
    posting_rehash(index, index->capacity * 2);
  posting = posting_slot(index, key);
  if (!posting->occupied) {
    posting->occupied = 1;
    posting->profile = 1;
    posting->key = key;
    index->keys++;
  }
  else if (!posting->profile)
    fatal_error("hint_postings_add_profile: ordinary key reused as profile");
  if (posting->count == posting->capacity) {
    unsigned old = posting->capacity;
    unsigned capacity = old == 0 ? 4 : old + (old + 1) / 2;
    if (capacity <= old)
      fatal_error("hint_postings_add_profile: posting capacity overflow");
    posting->references = safe_realloc(
      posting->references, (size_t) capacity * sizeof(unsigned));
    posting->profile_literal_counts = safe_realloc(
      posting->profile_literal_counts,
      (size_t) capacity * sizeof(unsigned));
    posting->capacity = capacity;
    index->reference_capacity += capacity - old;
    index->profile_reference_capacity += capacity - old;
  }
  if (posting->count / 64 >= posting->profile_mask_blocks) {
    unsigned old_blocks = posting->profile_mask_blocks;
    unsigned blocks = old_blocks + 1;
    posting->profile_mask_planes = safe_realloc(
      posting->profile_mask_planes,
      (size_t) blocks * 64 * sizeof(unsigned long long));
    memset(posting->profile_mask_planes + (size_t) old_blocks * 64, 0,
           64 * sizeof(unsigned long long));
    posting->summary.profile_block_summaries = safe_realloc(
      posting->summary.profile_block_summaries,
      (size_t) blocks * 2 * sizeof(unsigned long long));
    memset(posting->summary.profile_block_summaries +
             (size_t) old_blocks * 2,
           0, 2 * sizeof(unsigned long long));
    posting->profile_mask_blocks = blocks;
    index->profile_mask_bytes +=
      64 * sizeof(unsigned long long);
    index->profile_summary_bytes +=
      2 * sizeof(unsigned long long);
  }
  posting->references[posting->count] = id;
  posting->profile_literal_counts[posting->count] =
    (positive << 16) | negative;
  posting->profile_or_dense.profile_mask_union |= mask;
  if (positive > posting->counts_or_dense.profile.maximum_positive)
    posting->counts_or_dense.profile.maximum_positive =
      (unsigned short) positive;
  if (negative > posting->counts_or_dense.profile.maximum_negative)
    posting->counts_or_dense.profile.maximum_negative =
      (unsigned short) negative;
  {
    unsigned block = posting->count / 64;
    unsigned long long *summary =
      posting->summary.profile_block_summaries + (size_t) block * 2;
    unsigned maxima = (unsigned) summary[1];
    unsigned long long flag = 1ULL << (posting->count % 64);
    summary[0] |= mask;
    if (positive > (maxima >> 16))
      maxima = (positive << 16) | (maxima & 0xffffU);
    if (negative > (maxima & 0xffffU))
      maxima = (maxima & 0xffff0000U) | negative;
    summary[1] = maxima;
    while (mask != 0) {
      unsigned bit = (unsigned) __builtin_ctzll(mask);
      posting->profile_mask_planes[(size_t) block * 64 + bit] |= flag;
      mask &= mask - 1;
    }
  }
  posting->count++;
  posting->generation++;
  if (posting->generation == 0)
    posting->generation = 1;
  index->references++;
  if (posting->count > index->maximum_posting)
    index->maximum_posting = posting->count;
}

BOOL hint_postings_get_profile(Hint_postings index,
                               unsigned long long key,
                               struct hint_profile_view *view)
{
  struct hint_posting *posting;
  if (view == NULL)
    fatal_error("hint_postings_get_profile: NULL view");
  memset(view, 0, sizeof(*view));
  if (index == NULL)
    return FALSE;
  posting = posting_slot(index, key);
  if (!posting->occupied)
    return FALSE;
  if (!posting->profile)
    fatal_error("hint_postings_get_profile: ordinary posting");
  view->ids = posting->references;
  view->mask_planes = posting->profile_mask_planes;
  view->literal_counts = posting->profile_literal_counts;
  view->block_summaries = posting->summary.profile_block_summaries;
  view->mask_union = posting->profile_or_dense.profile_mask_union;
  view->count = posting->count;
  view->mask_blocks = posting->profile_mask_blocks;
  view->maximum_positive =
    posting->counts_or_dense.profile.maximum_positive;
  view->maximum_negative =
    posting->counts_or_dense.profile.maximum_negative;
  return TRUE;
}

unsigned long long hint_postings_profile_allocated_bytes(
  Hint_postings index)
{
  if (index == NULL)
    return 0;
  return sizeof(*index) +
    (unsigned long long) index->capacity * sizeof(struct hint_posting) +
    index->reference_capacity * sizeof(unsigned) +
    index->profile_reference_capacity * sizeof(unsigned) +
    index->profile_mask_bytes + index->profile_summary_bytes;
}

unsigned long long hint_postings_profile_layout_bytes(
  unsigned table_capacity,
  unsigned long long reference_capacity,
  unsigned long long mask_blocks)
{
  return sizeof(struct hint_postings) +
    (unsigned long long) table_capacity * sizeof(struct hint_posting) +
    reference_capacity * 2 * sizeof(unsigned) +
    mask_blocks * 66 * sizeof(unsigned long long);
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

unsigned long long hint_postings_reference_count(Hint_postings index)
{
  return index == NULL ? 0 : index->references;
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
  if (posting->profile)
    fatal_error("hint_postings_dense_view: profile posting");
  if (posting->profile_or_dense.dense_bits == NULL) {
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
  else if (posting->counts_or_dense.dense.words <
           bit_capacity / 64 + (bit_capacity % 64 != 0)) {
    if (!create)
      return FALSE;
    if (!posting_dense_reserve(index, posting, bit_capacity))
      return FALSE;
  }
  view->bits = posting->profile_or_dense.dense_bits;
  view->summary = posting->summary.dense_summary;
  view->words = posting->counts_or_dense.dense.words;
  view->summary_words = posting->counts_or_dense.dense.summary_words;
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
  stats->profile_bytes =
    index->profile_reference_capacity * sizeof(unsigned) +
    index->profile_mask_bytes + index->profile_summary_bytes;
  stats->profile_summary_bytes = index->profile_summary_bytes;
  for (i = 0; i < index->capacity; i++) {
    struct hint_posting *posting = index->table + i;
    if (posting->profile) {
      unsigned bucket = posting->count <= 1 ? 0 :
        posting->count <= 3 ? 1 : posting->count <= 7 ? 2 :
        posting->count <= 15 ? 3 : posting->count <= 31 ? 4 :
        posting->count <= 63 ? 5 : 6;
      stats->profile_key_histogram[bucket]++;
      stats->profile_reference_histogram[bucket] += posting->count;
      stats->profile_mask_words +=
        (unsigned long long) posting->profile_mask_blocks * 64;
    }
    if (!posting->profile &&
        posting->profile_or_dense.dense_bits != NULL) {
      stats->dense_keys++;
      stats->dense_bit_bytes +=
        (unsigned long long) posting->counts_or_dense.dense.words *
          sizeof(unsigned long long);
      stats->dense_summary_bytes +=
        (unsigned long long) posting->counts_or_dense.dense.summary_words *
          sizeof(unsigned long long);
    }
  }
}
