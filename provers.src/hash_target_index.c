#include "hash_target_index.h"

#include "../ladr/clause_misc.h"
#include "../ladr/clock.h"
#include "../ladr/memory.h"
#include "../ladr/symbols.h"

#include <stdint.h>

struct hash_target_index {
  uint32_t *postings;
  uint64_t *offsets;
  unsigned key_count;
  struct hash_target_index_stats stats;
};

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
                               struct hash_target_index_stats *stats)
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
  for (i = 0; i < ARITY(t); i++)
    census_target_term(ARG(t, i), seen, serial, key_count, counts, stats);
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
    census_target_term(left, seen, i + 1, key_count, counts, &index->stats);
    census_target_term(right, seen, i + 1, key_count, counts, &index->stats);
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
  resident_bytes = index->stats.recipe_bytes +
    ((unsigned long long) key_count + 1) * sizeof(*index->offsets) +
    index->stats.root_records * sizeof(*index->postings) + sizeof(*index);
  construction_bytes = resident_bytes +
    (unsigned long long) key_count *
      (sizeof(*counts) + sizeof(*cursor) + sizeof(*seen));
  if (budget_bytes != 0 && resident_bytes > budget_bytes)
    fatal_error("hash target root directory exceeds hash_target_index_kb");
  index->stats.index_bytes = resident_bytes - index->stats.recipe_bytes;
  index->stats.construction_peak_bytes = construction_bytes;
  index->postings = safe_malloc(
    (size_t) index->stats.root_records * sizeof(*index->postings));
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
    index->stats.reconstructed_targets++;
    delete_clause(target);
  }
  end_generalized_hash_target_scan();
  for (i = 0; i < key_count; i++)
    if (cursor[i] != index->offsets[i + 1])
      fatal_error("hash target root posting fill is inconsistent");
  safe_free(cursor);
  safe_free(seen);
  safe_free(counts);
  index->stats.build_seconds = user_seconds() - started;
  return index;
}

void hash_target_index_destroy(Hash_target_index index)
{
  if (index == NULL)
    return;
  safe_free(index->postings);
  safe_free(index->offsets);
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
}

BOOL hash_target_query_next(Hash_target_query *query, unsigned *recipe_id)
{
  Hash_target_index index = query->index;
  if (query->all_targets) {
    if (query->all_next >= index->stats.targets)
      return FALSE;
    *recipe_id = query->all_next++;
  }
  else if (query->first < query->first_end &&
           query->second < query->second_end) {
    uint32_t a = index->postings[query->first];
    uint32_t b = index->postings[query->second];
    if (a < b) {
      *recipe_id = a;
      query->first++;
    }
    else if (b < a) {
      *recipe_id = b;
      query->second++;
    }
    else {
      *recipe_id = a;
      query->first++;
      query->second++;
    }
  }
  else if (query->first < query->first_end)
    *recipe_id = index->postings[query->first++];
  else if (query->second < query->second_end)
    *recipe_id = index->postings[query->second++];
  else
    return FALSE;
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
          "rigid_records=%llu, variable_records=%llu, max_posting=%llu, "
          "recipe_bytes=%llu, index_bytes=%llu, construction_peak_bytes=%llu, "
          "budget_bytes=%llu, reconstructed_targets=%llu, positions=%llu, "
          "build_seconds=%.3f, queries=%llu, variable_queries=%llu, "
          "candidates=%llu.\n",
          s.targets, s.root_keys, s.root_records, s.rigid_root_records,
          s.variable_root_records, s.maximum_posting, s.recipe_bytes,
          s.index_bytes, s.construction_peak_bytes, s.budget_bytes,
          s.reconstructed_targets, s.reconstructed_positions,
          s.build_seconds, s.queries, s.variable_queries, s.candidates);
}
