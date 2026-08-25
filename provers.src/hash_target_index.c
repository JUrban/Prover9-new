#include "hash_target_index.h"

#include "../ladr/clause_misc.h"
#include "../ladr/clock.h"
#include "../ladr/memory.h"
#include "../ladr/symbols.h"

#include <stdint.h>

struct target_feature_slot {
  uint64_t key;
  uint64_t count;
  uint64_t first;
  uint64_t cursor;
};

struct hash_target_index {
  uint32_t *postings;
  uint32_t *feature_postings;
  uint64_t *offsets;
  struct target_feature_slot *features;
  unsigned feature_capacity;
  unsigned feature_count;
  unsigned key_count;
  struct hash_target_index_stats stats;
};

static uint64_t mix_target_feature(uint64_t value)
{
  value ^= value >> 30;
  value *= UINT64_C(0xbf58476d1ce4e5b9);
  value ^= value >> 27;
  value *= UINT64_C(0x94d049bb133111eb);
  value ^= value >> 31;
  return value;
}

static uint64_t target_child_feature(Term parent, unsigned child)
{
  uint64_t a = (uint32_t) SYMNUM(parent);
  uint64_t b = (uint32_t) SYMNUM(ARG(parent, child));
  uint64_t key = mix_target_feature(a + UINT64_C(0x9e3779b97f4a7c15));
  key ^= mix_target_feature(b + UINT64_C(0x243f6a8885a308d3));
  key ^= mix_target_feature((uint64_t) child + UINT64_C(0x13198a2e03707344));
  return key == 0 ? 1 : key;
}

static unsigned feature_slot_index(uint64_t key, unsigned capacity)
{
  return (unsigned) mix_target_feature(key) & (capacity - 1);
}

static struct target_feature_slot *find_feature_slot(
  Hash_target_index index, uint64_t key)
{
  unsigned at;
  if (index->feature_capacity == 0)
    return NULL;
  at = feature_slot_index(key, index->feature_capacity);
  while (index->features[at].key != 0 && index->features[at].key != key)
    at = (at + 1) & (index->feature_capacity - 1);
  return index->features[at].key == key ? index->features + at : NULL;
}

static void rehash_target_features(Hash_target_index index,
                                   unsigned capacity)
{
  struct target_feature_slot *old = index->features;
  unsigned old_capacity = index->feature_capacity;
  unsigned i;
  index->features = safe_calloc(capacity, sizeof(*index->features));
  index->feature_capacity = capacity;
  for (i = 0; i < old_capacity; i++)
    if (old[i].key != 0) {
      unsigned at = feature_slot_index(old[i].key, capacity);
      while (index->features[at].key != 0)
        at = (at + 1) & (capacity - 1);
      index->features[at] = old[i];
    }
  safe_free(old);
}

static struct target_feature_slot *insert_target_feature(
  Hash_target_index index, uint64_t key)
{
  unsigned at;
  if (index->feature_capacity == 0)
    rehash_target_features(index, 1024);
  if ((unsigned long long) (index->feature_count + 1) * 10 >=
      (unsigned long long) index->feature_capacity * 7) {
    if (index->feature_capacity > UINT_MAX / 2)
      fatal_error("hash target feature directory overflow");
    rehash_target_features(index, index->feature_capacity * 2);
  }
  at = feature_slot_index(key, index->feature_capacity);
  while (index->features[at].key != 0 && index->features[at].key != key)
    at = (at + 1) & (index->feature_capacity - 1);
  if (index->features[at].key == 0) {
    index->features[at].key = key;
    index->feature_count++;
  }
  return index->features + at;
}

static int compare_u64(const void *a, const void *b)
{
  uint64_t x = *(const uint64_t *) a;
  uint64_t y = *(const uint64_t *) b;
  return x < y ? -1 : x > y ? 1 : 0;
}

static void append_target_feature(uint64_t key, uint64_t **items,
                                  unsigned *count, unsigned *capacity)
{
  if (*count == *capacity) {
    unsigned next = *capacity == 0 ? 128 : *capacity * 2;
    if (next <= *capacity)
      fatal_error("hash target feature scratch overflow");
    *items = safe_realloc(*items, (size_t) next * sizeof(**items));
    *capacity = next;
  }
  (*items)[(*count)++] = key;
}

static unsigned target_root_key(Term t, unsigned key_count)
{
  if (VARIABLE(t))
    return 0;
  else {
    unsigned long long key = (unsigned long long) SYMNUM(t) + 1;
    if (key >= key_count)
      fatal_error("hash target root symbol exceeds directory");
    return (unsigned) key;
  }
}

static void census_target_term(Term t, unsigned *seen, unsigned serial,
                               unsigned key_count, uint64_t *counts,
                               struct hash_target_index_stats *stats,
                               uint64_t **features, unsigned *feature_count,
                               unsigned *feature_capacity)
{
  unsigned key = target_root_key(t, key_count);
  int i;
  stats->reconstructed_positions++;
  /* A normalized target variable is a literal result variable, not a
     discrimination-tree wildcard.  A rigid replacement keeps its rigid
     root under substitution; a variable replacement uses the explicit
     all-target query path below.  No variable-root posting is useful. */
  if (key != 0 && seen[key] != serial) {
    seen[key] = serial;
    counts[key]++;
    stats->root_records++;
    stats->rigid_root_records++;
  }
  for (i = 0; i < ARITY(t); i++) {
    if (!VARIABLE(t) && !VARIABLE(ARG(t, i)))
      append_target_feature(target_child_feature(t, (unsigned) i),
                            features, feature_count, feature_capacity);
    census_target_term(ARG(t, i), seen, serial, key_count, counts, stats,
                       features, feature_count, feature_capacity);
  }
}

static void fill_target_term(Term t, unsigned *seen, unsigned serial,
                             unsigned key_count, uint64_t *cursor,
                             uint32_t *postings, unsigned recipe_id)
{
  unsigned key = target_root_key(t, key_count);
  int i;
  if (key != 0 && seen[key] != serial) {
    seen[key] = serial;
    postings[cursor[key]++] = (uint32_t) recipe_id;
  }
  for (i = 0; i < ARITY(t); i++)
    fill_target_term(ARG(t, i), seen, serial, key_count, cursor,
                     postings, recipe_id);
}

static void collect_target_features(Term t, uint64_t **features,
                                    unsigned *count, unsigned *capacity)
{
  int i;
  for (i = 0; i < ARITY(t); i++) {
    if (!VARIABLE(t) && !VARIABLE(ARG(t, i)))
      append_target_feature(target_child_feature(t, (unsigned) i),
                            features, count, capacity);
    collect_target_features(ARG(t, i), features, count, capacity);
  }
}

static unsigned unique_target_features(uint64_t *features, unsigned count)
{
  unsigned i, unique = 0;
  if (count > 1)
    qsort(features, count, sizeof(*features), compare_u64);
  for (i = 0; i < count; i++)
    if (unique == 0 || features[i] != features[unique - 1])
      features[unique++] = features[i];
  return unique;
}

static void target_atom_sides(Topform target, Term *left, Term *right)
{
  if (target == NULL || !unit_clause(target->literals) ||
      !pos_eq(target->literals) || ARITY(target->literals->atom) != 2)
    fatal_error("hash target recipe did not reconstruct a unit equation");
  *left = ARG(target->literals->atom, 0);
  *right = ARG(target->literals->atom, 1);
}

Hash_target_index hash_target_index_build(unsigned long long budget_bytes)
{
  Hash_target_index index;
  uint64_t *counts, *cursor;
  unsigned *seen;
  uint64_t *feature_scratch = NULL;
  unsigned feature_scratch_count = 0, feature_scratch_capacity = 0;
  unsigned targets = generalized_hash_target_count();
  unsigned key_count;
  unsigned i;
  unsigned long long resident_bytes, construction_bytes;
  double started = user_seconds();

  if (targets == 0)
    fatal_error("target-directed inference requires reconstructable targets");
  if (targets == UINT32_MAX)
    fatal_error("hash target recipe IDs exceed the compact directory");
  if (greatest_symnum() < 0 || greatest_symnum() > INT_MAX - 2)
    fatal_error("hash target symbol directory overflow");
  key_count = (unsigned) greatest_symnum() + 2;
  index = safe_calloc(1, sizeof(*index));
  index->key_count = key_count;
  index->stats.targets = targets;
  index->stats.root_keys = key_count;
  index->stats.recipe_bytes = generalized_hash_target_storage_bytes();
  index->stats.budget_bytes = budget_bytes;
  counts = safe_calloc(key_count, sizeof(*counts));
  seen = safe_calloc(key_count, sizeof(*seen));

  begin_generalized_hash_target_scan();
  for (i = 0; i < targets; i++) {
    Topform target = reconstruct_generalized_hash_target(i);
    Term left, right;
    target_atom_sides(target, &left, &right);
    feature_scratch_count = 0;
    census_target_term(left, seen, i + 1, key_count, counts, &index->stats,
                       &feature_scratch, &feature_scratch_count,
                       &feature_scratch_capacity);
    census_target_term(right, seen, i + 1, key_count, counts, &index->stats,
                       &feature_scratch, &feature_scratch_count,
                       &feature_scratch_capacity);
    feature_scratch_count = unique_target_features(
      feature_scratch, feature_scratch_count);
    {
      unsigned j;
      for (j = 0; j < feature_scratch_count; j++) {
        struct target_feature_slot *slot = insert_target_feature(
          index, feature_scratch[j]);
        slot->count++;
        index->stats.feature_records++;
      }
    }
    index->stats.reconstructed_targets++;
    delete_clause(target);
  }
  end_generalized_hash_target_scan();

  index->offsets = safe_malloc(((size_t) key_count + 1) *
                               sizeof(*index->offsets));
  index->offsets[0] = 0;
  for (i = 0; i < key_count; i++) {
    if (ULLONG_MAX - index->offsets[i] < counts[i])
      fatal_error("hash target root posting count overflow");
    index->offsets[i + 1] = index->offsets[i] + counts[i];
    if (counts[i] > index->stats.maximum_posting)
      index->stats.maximum_posting = counts[i];
  }
  if (index->offsets[key_count] != index->stats.root_records)
    fatal_error("hash target root census is inconsistent");
  if (index->stats.root_records > SIZE_MAX / sizeof(*index->postings))
    fatal_error("hash target root postings exceed address space");
  {
    unsigned long long feature_offset = 0;
    for (i = 0; i < index->feature_capacity; i++)
      if (index->features[i].key != 0) {
        index->features[i].first = feature_offset;
        index->features[i].cursor = feature_offset;
        if (ULLONG_MAX - feature_offset < index->features[i].count)
          fatal_error("hash target feature posting count overflow");
        feature_offset += index->features[i].count;
        if (index->features[i].count > index->stats.maximum_posting)
          index->stats.maximum_posting = index->features[i].count;
      }
    if (feature_offset != index->stats.feature_records)
      fatal_error("hash target feature census is inconsistent");
  }
  index->stats.feature_keys = index->feature_count;
  if (index->stats.feature_records >
      SIZE_MAX / sizeof(*index->feature_postings))
    fatal_error("hash target feature postings exceed address space");
  resident_bytes = index->stats.recipe_bytes +
    ((unsigned long long) key_count + 1) * sizeof(*index->offsets) +
    index->stats.root_records * sizeof(*index->postings) +
    index->stats.feature_records * sizeof(*index->feature_postings) +
    (unsigned long long) index->feature_capacity * sizeof(*index->features) +
    sizeof(*index);
  construction_bytes = resident_bytes +
    (unsigned long long) key_count *
      (sizeof(*counts) + sizeof(*cursor) + sizeof(*seen)) +
    (unsigned long long) feature_scratch_capacity * sizeof(*feature_scratch);
  if (budget_bytes != 0 && resident_bytes > budget_bytes)
    fatal_error("hash target root directory exceeds hash_target_index_kb");
  index->stats.index_bytes = resident_bytes - index->stats.recipe_bytes;
  index->stats.construction_peak_bytes = construction_bytes;
  index->postings = safe_malloc(
    (size_t) index->stats.root_records * sizeof(*index->postings));
  index->feature_postings = safe_malloc(
    (size_t) index->stats.feature_records *
      sizeof(*index->feature_postings));
  cursor = safe_malloc((size_t) key_count * sizeof(*cursor));
  memcpy(cursor, index->offsets, (size_t) key_count * sizeof(*cursor));
  memset(seen, 0, (size_t) key_count * sizeof(*seen));

  begin_generalized_hash_target_scan();
  for (i = 0; i < targets; i++) {
    Topform target = reconstruct_generalized_hash_target(i);
    Term left, right;
    target_atom_sides(target, &left, &right);
    fill_target_term(left, seen, i + 1, key_count, cursor,
                     index->postings, i);
    fill_target_term(right, seen, i + 1, key_count, cursor,
                     index->postings, i);
    feature_scratch_count = 0;
    collect_target_features(left, &feature_scratch, &feature_scratch_count,
                            &feature_scratch_capacity);
    collect_target_features(right, &feature_scratch, &feature_scratch_count,
                            &feature_scratch_capacity);
    feature_scratch_count = unique_target_features(
      feature_scratch, feature_scratch_count);
    {
      unsigned j;
      for (j = 0; j < feature_scratch_count; j++) {
        struct target_feature_slot *slot = find_feature_slot(
          index, feature_scratch[j]);
        if (slot == NULL || slot->cursor >= slot->first + slot->count)
          fatal_error("hash target feature posting fill is inconsistent");
        index->feature_postings[slot->cursor++] = (uint32_t) i;
      }
    }
    index->stats.reconstructed_targets++;
    delete_clause(target);
  }
  end_generalized_hash_target_scan();
  for (i = 0; i < key_count; i++)
    if (cursor[i] != index->offsets[i + 1])
      fatal_error("hash target root posting fill is inconsistent");
  for (i = 0; i < index->feature_capacity; i++)
    if (index->features[i].key != 0 &&
        index->features[i].cursor !=
          index->features[i].first + index->features[i].count)
      fatal_error("hash target feature posting fill is incomplete");
  safe_free(cursor);
  safe_free(seen);
  safe_free(counts);
  safe_free(feature_scratch);
  index->stats.build_seconds = user_seconds() - started;
  return index;
}

void hash_target_index_destroy(Hash_target_index index)
{
  if (index == NULL)
    return;
  safe_free(index->postings);
  safe_free(index->feature_postings);
  safe_free(index->offsets);
  safe_free(index->features);
  safe_free(index);
}

void hash_target_query_init(Hash_target_index index, Term replacement,
                            Hash_target_query *query)
{
  unsigned key;
  memset(query, 0, sizeof(*query));
  query->index = index;
  index->stats.queries++;
  if (VARIABLE(replacement)) {
    query->all_targets = TRUE;
    index->stats.variable_queries++;
    return;
  }
  key = target_root_key(replacement, index->key_count);
  query->first = index->offsets[key];
  query->first_end = index->offsets[key + 1];
  if (COMPLEX(replacement)) {
    int i;
    for (i = 0; i < ARITY(replacement) && query->filter_count < 32; i++)
      if (!VARIABLE(ARG(replacement, i))) {
        struct target_feature_slot *slot = find_feature_slot(
          index, target_child_feature(replacement, (unsigned) i));
        if (slot == NULL) {
          query->first = query->first_end;
          query->filter_count = 0;
          return;
        }
        query->filters[query->filter_count].first = slot->first;
        query->filters[query->filter_count].end = slot->first + slot->count;
        query->filter_count++;
      }
  }
}

BOOL hash_target_query_next(Hash_target_query *query, unsigned *recipe_id)
{
  Hash_target_index index = query->index;
  if (query->all_targets) {
    if (query->all_next >= index->stats.targets)
      return FALSE;
    *recipe_id = query->all_next++;
  }
  else {
    while (query->first < query->first_end) {
      uint32_t candidate = index->postings[query->first++];
      unsigned i;
      BOOL keep = TRUE;
      for (i = 0; i < query->filter_count; i++) {
        unsigned long long first = query->filters[i].first;
        unsigned long long end = query->filters[i].end;
        unsigned long long lo = first, hi = end;
        index->stats.feature_tests++;
        while (lo < hi) {
          unsigned long long middle = lo + (hi - lo) / 2;
          if (index->feature_postings[middle] < candidate)
            lo = middle + 1;
          else
            hi = middle;
        }
        if (lo == end || index->feature_postings[lo] != candidate) {
          keep = FALSE;
          index->stats.feature_rejects++;
          break;
        }
      }
      if (keep) {
        *recipe_id = candidate;
        index->stats.candidates++;
        return TRUE;
      }
    }
    return FALSE;
  }
  index->stats.candidates++;
  return TRUE;
}

void hash_target_index_get_stats(Hash_target_index index,
                                 struct hash_target_index_stats *stats)
{
  if (index == NULL)
    memset(stats, 0, sizeof(*stats));
  else
    *stats = index->stats;
}

void fprint_hash_target_index_stats(FILE *fp, Hash_target_index index)
{
  struct hash_target_index_stats s;
  hash_target_index_get_stats(index, &s);
  fprintf(fp,
          "Hash_target_index: targets=%u, root_keys=%u, root_records=%llu, "
          "rigid_records=%llu, variable_records=%llu, feature_keys=%u, "
          "feature_records=%llu, max_posting=%llu, "
          "recipe_bytes=%llu, index_bytes=%llu, construction_peak_bytes=%llu, "
          "budget_bytes=%llu, reconstructed_targets=%llu, positions=%llu, "
          "build_seconds=%.3f, queries=%llu, variable_queries=%llu, "
          "candidates=%llu, feature_tests=%llu, feature_rejects=%llu.\n",
          s.targets, s.root_keys, s.root_records, s.rigid_root_records,
          s.variable_root_records, s.feature_keys, s.feature_records,
          s.maximum_posting, s.recipe_bytes,
          s.index_bytes, s.construction_peak_bytes, s.budget_bytes,
          s.reconstructed_targets, s.reconstructed_positions,
          s.build_seconds, s.queries, s.variable_queries, s.candidates,
          s.feature_tests, s.feature_rejects);
}
