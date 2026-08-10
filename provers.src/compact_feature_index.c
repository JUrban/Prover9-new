#include "compact_feature_index.h"

#include <stdint.h>
#include <stdlib.h>
#include <string.h>

#define CFI_NONE 0U
#define CFI_TOMBSTONE UINT64_MAX

struct cfi_node {
  int32_t label;
  uint32_t first_child;
  uint32_t next_sibling;
  uint32_t first_posting;
};

struct cfi_posting {
  uint32_t record;
  uint32_t next;
};

struct cfi_record {
  unsigned long long proof_id;
  unsigned char active;
};

struct compact_feature_index {
  struct cfi_node *nodes;
  size_t node_count;
  size_t node_capacity;
  struct cfi_posting *postings;
  size_t posting_count;
  size_t posting_capacity;
  struct cfi_record *records;
  size_t record_count;
  size_t record_capacity;
  unsigned long long *hash_keys;
  uint32_t *hash_values;
  size_t hash_capacity;
  size_t hash_count;
  size_t hash_tombstones;
  unsigned long long *results;
  size_t result_capacity;
  int feature_length;
  uint32_t root;
  unsigned long long active;
  unsigned long long peak;
  unsigned long long retired;
  unsigned long long forward_queries;
  unsigned long long forward_candidates;
  unsigned long long back_queries;
  unsigned long long back_candidates;
  unsigned long long peak_bytes;
};

static size_t grow_capacity(size_t current, size_t item_size,
                            const char *message)
{
  size_t next = current == 0 ? 64 : current * 2;
  if (next < current || next > SIZE_MAX / item_size)
    fatal_error((char *) message);
  return next;
}

#define ENSURE_ARRAY(index, field, count, capacity, message) do {       \
  if ((index)->count == (index)->capacity) {                            \
    (index)->capacity = grow_capacity((index)->capacity,                \
      sizeof(*(index)->field), (message));                              \
    (index)->field = safe_realloc((index)->field,                       \
      (index)->capacity * sizeof(*(index)->field));                     \
  }                                                                    \
} while (0)

static uint64_t hash_id(uint64_t x)
{
  x ^= x >> 30;
  x *= UINT64_C(0xbf58476d1ce4e5b9);
  x ^= x >> 27;
  x *= UINT64_C(0x94d049bb133111eb);
  x ^= x >> 31;
  return x;
}

static unsigned long long index_bytes(Compact_feature_index index)
{
  if (index == NULL)
    return 0;
  return sizeof(*index) +
    index->node_capacity * sizeof(*index->nodes) +
    index->posting_capacity * sizeof(*index->postings) +
    index->record_capacity * sizeof(*index->records) +
    index->hash_capacity *
      (sizeof(*index->hash_keys) + sizeof(*index->hash_values)) +
    index->result_capacity * sizeof(*index->results);
}

static void update_peak(Compact_feature_index index)
{
  unsigned long long bytes = index_bytes(index);
  if (bytes > index->peak_bytes)
    index->peak_bytes = bytes;
  if (index->active > index->peak)
    index->peak = index->active;
}

static size_t hash_slot(Compact_feature_index index, uint64_t id,
                        BOOL inserting)
{
  size_t mask = index->hash_capacity - 1;
  size_t at = (size_t) hash_id(id) & mask;
  size_t tombstone = SIZE_MAX;
  for (;;) {
    uint64_t key = index->hash_keys[at];
    if (key == 0)
      return inserting && tombstone != SIZE_MAX ? tombstone : at;
    if (key == id)
      return at;
    if (inserting && key == CFI_TOMBSTONE && tombstone == SIZE_MAX)
      tombstone = at;
    at = (at + 1) & mask;
  }
}

static void rehash(Compact_feature_index index, size_t capacity)
{
  unsigned long long *old_keys = index->hash_keys;
  uint32_t *old_values = index->hash_values;
  size_t old_capacity = index->hash_capacity;
  size_t i;
  index->hash_keys = safe_calloc(capacity, sizeof(*index->hash_keys));
  index->hash_values = safe_calloc(capacity, sizeof(*index->hash_values));
  index->hash_capacity = capacity;
  index->hash_tombstones = 0;
  for (i = 0; i < old_capacity; i++)
    if (old_keys[i] != 0 && old_keys[i] != CFI_TOMBSTONE) {
      size_t at = hash_slot(index, old_keys[i], TRUE);
      index->hash_keys[at] = old_keys[i];
      index->hash_values[at] = old_values[i];
    }
  safe_free(old_keys);
  safe_free(old_values);
}

static void ensure_hash(Compact_feature_index index)
{
  if (index->hash_capacity == 0)
    rehash(index, 128);
  else if ((index->hash_count + index->hash_tombstones + 1) * 10 >=
           index->hash_capacity * 7) {
    if (index->hash_capacity > SIZE_MAX / 2)
      fatal_error("compact_feature_index: hash overflow");
    rehash(index, index->hash_capacity * 2);
  }
}

static uint32_t lookup_record(Compact_feature_index index,
                              unsigned long long proof_id)
{
  size_t at;
  if (index == NULL || proof_id == 0 || index->hash_capacity == 0)
    return CFI_NONE;
  at = hash_slot(index, proof_id, FALSE);
  return index->hash_keys[at] == proof_id ?
    index->hash_values[at] : CFI_NONE;
}

static uint32_t new_node(Compact_feature_index index, int32_t label)
{
  uint32_t node;
  ENSURE_ARRAY(index, nodes, node_count, node_capacity,
               "compact_feature_index: node overflow");
  if (index->node_count > UINT32_MAX)
    fatal_error("compact_feature_index: node offsets exceed 32 bits");
  node = (uint32_t) index->node_count++;
  memset(&index->nodes[node], 0, sizeof(index->nodes[node]));
  index->nodes[node].label = label;
  return node;
}

static uint32_t child_for_label(Compact_feature_index index,
                                uint32_t parent, int32_t label)
{
  uint32_t current = index->nodes[parent].first_child;
  uint32_t previous = CFI_NONE;
  while (current != CFI_NONE && index->nodes[current].label < label) {
    previous = current;
    current = index->nodes[current].next_sibling;
  }
  if (current != CFI_NONE && index->nodes[current].label == label)
    return current;
  current = new_node(index, label);
  if (previous == CFI_NONE) {
    index->nodes[current].next_sibling = index->nodes[parent].first_child;
    index->nodes[parent].first_child = current;
  }
  else {
    index->nodes[current].next_sibling =
      index->nodes[previous].next_sibling;
    index->nodes[previous].next_sibling = current;
  }
  return current;
}

Compact_feature_index compact_feature_index_init(int feature_length)
{
  Compact_feature_index index;
  if (feature_length <= 0)
    fatal_error("compact_feature_index_init: feature length must be positive");
  index = safe_calloc(1, sizeof(*index));
  index->feature_length = feature_length;
  (void) new_node(index, 0);  /* reserved null node */
  index->root = new_node(index, 0);
  ENSURE_ARRAY(index, postings, posting_count, posting_capacity,
               "compact_feature_index: posting overflow");
  memset(&index->postings[0], 0, sizeof(index->postings[0]));
  index->posting_count = 1;
  ENSURE_ARRAY(index, records, record_count, record_capacity,
               "compact_feature_index: record overflow");
  memset(&index->records[0], 0, sizeof(index->records[0]));
  index->record_count = 1;
  update_peak(index);
  return index;
}

BOOL compact_feature_index_add(Compact_feature_index index,
                               unsigned long long proof_id,
                               const int *features)
{
  uint32_t record_index, node, posting;
  struct cfi_record *record;
  size_t at_hash;
  int i;
  if (index == NULL || proof_id == 0 || features == NULL ||
      lookup_record(index, proof_id) != CFI_NONE)
    return FALSE;
  ENSURE_ARRAY(index, records, record_count, record_capacity,
               "compact_feature_index: record overflow");
  if (index->record_count > UINT32_MAX)
    fatal_error("compact_feature_index: record offsets exceed 32 bits");
  record_index = (uint32_t) index->record_count++;
  record = &index->records[record_index];
  memset(record, 0, sizeof(*record));
  record->proof_id = proof_id;
  record->active = TRUE;
  node = index->root;
  for (i = 0; i < index->feature_length; i++)
    node = child_for_label(index, node, features[i]);
  ENSURE_ARRAY(index, postings, posting_count, posting_capacity,
               "compact_feature_index: posting overflow");
  if (index->posting_count > UINT32_MAX)
    fatal_error("compact_feature_index: posting offsets exceed 32 bits");
  posting = (uint32_t) index->posting_count++;
  index->postings[posting].record = record_index;
  index->postings[posting].next = index->nodes[node].first_posting;
  index->nodes[node].first_posting = posting;
  ensure_hash(index);
  at_hash = hash_slot(index, proof_id, TRUE);
  if (index->hash_keys[at_hash] == CFI_TOMBSTONE)
    index->hash_tombstones--;
  index->hash_keys[at_hash] = proof_id;
  index->hash_values[at_hash] = record_index;
  index->hash_count++;
  index->active++;
  update_peak(index);
  return TRUE;
}

BOOL compact_feature_index_remove(Compact_feature_index index,
                                  unsigned long long proof_id)
{
  uint32_t record = lookup_record(index, proof_id);
  size_t at;
  if (record == CFI_NONE || !index->records[record].active)
    return FALSE;
  index->records[record].active = FALSE;
  at = hash_slot(index, proof_id, FALSE);
  index->hash_keys[at] = CFI_TOMBSTONE;
  index->hash_values[at] = CFI_NONE;
  index->hash_count--;
  index->hash_tombstones++;
  index->active--;
  index->retired++;
  return TRUE;
}

static void ensure_results(Compact_feature_index index, size_t needed)
{
  while (needed > index->result_capacity) {
    index->result_capacity = grow_capacity(
      index->result_capacity, sizeof(*index->results),
      "compact_feature_index: result overflow");
    index->results = safe_realloc(
      index->results, index->result_capacity * sizeof(*index->results));
  }
}

static void collect_leaf(Compact_feature_index index, uint32_t node,
                         size_t *count)
{
  uint32_t posting;
  for (posting = index->nodes[node].first_posting; posting != CFI_NONE;
       posting = index->postings[posting].next) {
    struct cfi_record *record =
      &index->records[index->postings[posting].record];
    if (record->active) {
      ensure_results(index, *count + 1);
      index->results[(*count)++] = record->proof_id;
    }
  }
}

static void collect_candidates(Compact_feature_index index, uint32_t node,
                               int level, const int *query, BOOL forward,
                               size_t *count)
{
  uint32_t child;
  if (level == index->feature_length) {
    collect_leaf(index, node, count);
    return;
  }
  child = index->nodes[node].first_child;
  if (!forward)
    while (child != CFI_NONE && index->nodes[child].label < query[level])
      child = index->nodes[child].next_sibling;
  while (child != CFI_NONE &&
         (!forward || index->nodes[child].label <= query[level])) {
    collect_candidates(index, child, level + 1, query, forward, count);
    child = index->nodes[child].next_sibling;
  }
}

static unsigned long long *candidates(Compact_feature_index index,
                                      const int *query, BOOL forward,
                                      size_t *count)
{
  unsigned long long *answer;
  *count = 0;
  if (index == NULL || query == NULL)
    return NULL;
  collect_candidates(index, index->root, 0, query, forward, count);
  answer = *count == 0 ? NULL : safe_malloc(*count * sizeof(*answer));
  if (*count != 0)
    memcpy(answer, index->results, *count * sizeof(*answer));
  if (forward) {
    index->forward_queries++;
    index->forward_candidates += *count;
  }
  else {
    index->back_queries++;
    index->back_candidates += *count;
  }
  update_peak(index);
  return answer;
}

unsigned long long *compact_feature_forward_candidates(
  Compact_feature_index index, const int *query, size_t *count)
{
  return candidates(index, query, TRUE, count);
}

unsigned long long *compact_feature_back_candidates(
  Compact_feature_index index, const int *query, size_t *count)
{
  return candidates(index, query, FALSE, count);
}

void compact_feature_index_get_stats(Compact_feature_index index,
                                     struct compact_feature_index_stats *stats)
{
  memset(stats, 0, sizeof(*stats));
  if (index == NULL)
    return;
  stats->active = index->active;
  stats->peak = index->peak;
  stats->retired = index->retired;
  stats->physical = index->record_count - 1;
  stats->forward_queries = index->forward_queries;
  stats->forward_candidates = index->forward_candidates;
  stats->back_queries = index->back_queries;
  stats->back_candidates = index->back_candidates;
  stats->node_bytes = index->node_capacity * sizeof(*index->nodes);
  stats->posting_bytes = index->posting_capacity * sizeof(*index->postings);
  stats->record_bytes = index->record_capacity * sizeof(*index->records);
  stats->hash_bytes = index->hash_capacity *
    (sizeof(*index->hash_keys) + sizeof(*index->hash_values));
  stats->scratch_bytes = index->result_capacity * sizeof(*index->results);
  stats->total_bytes = index_bytes(index);
  stats->peak_bytes = index->peak_bytes;
}

void compact_feature_index_free(Compact_feature_index index)
{
  if (index == NULL)
    return;
  safe_free(index->nodes);
  safe_free(index->postings);
  safe_free(index->records);
  safe_free(index->hash_keys);
  safe_free(index->hash_values);
  safe_free(index->results);
  safe_free(index);
}
