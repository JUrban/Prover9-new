#include "compact_feature_index.h"

#include <stdint.h>
#include <stdlib.h>
#include <string.h>

#define CFI_NONE 0U
#define CFI_TOMBSTONE UINT64_MAX

struct cfi_node {
  uint32_t label_offset;
  uint32_t label_length;
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
  int32_t *labels;
  size_t label_count;
  size_t label_capacity;
  struct cfi_posting *postings;
  size_t posting_count;
  size_t posting_capacity;
  struct cfi_record *records;
  size_t record_count;
  size_t record_capacity;
  struct compact_feature_structural_summary *structural_summaries;
  unsigned long long *hash_keys;
  uint32_t *hash_values;
  size_t hash_capacity;
  size_t hash_count;
  size_t hash_tombstones;
  unsigned long long *results;
  size_t result_capacity;
  int feature_length;
  BOOL structural_filter;
  uint32_t root;
  unsigned long long active;
  unsigned long long peak;
  unsigned long long retired;
  unsigned long long forward_queries;
  unsigned long long forward_candidates;
  unsigned long long forward_structural_rejects;
  unsigned long long forward_variable_rejects;
  unsigned long long back_queries;
  unsigned long long back_candidates;
  unsigned long long back_structural_rejects;
  unsigned long long back_variable_rejects;
  struct compact_query_profile forward_profile;
  struct compact_query_profile back_profile;
  unsigned long long query_nodes;
  unsigned long long query_postings;
  unsigned long long query_live;
  unsigned long long query_dead;
  unsigned long long query_structural_rejects;
  unsigned long long query_variable_rejects;
  Clock forward_lookup_clock;
  Clock back_lookup_clock;
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

struct cfi_term_occurrence {
  Term term;
  uint64_t location;
  unsigned term_hash;
};

static uint64_t rigid_fact_mask(uint64_t literal_context, uint64_t path,
                                unsigned symbol)
{
  uint64_t fact = hash_id(literal_context ^ hash_id(path) ^
                 hash_id((uint64_t) symbol +
                         UINT64_C(0x9e3779b97f4a7c15)));
  return (UINT64_C(1) << (fact & 63)) |
         (UINT64_C(1) << ((fact >> 17) & 63));
}

static uint32_t equality_fact_mask(uint64_t first, uint64_t second)
{
  uint64_t low = first < second ? first : second;
  uint64_t high = first < second ? second : first;
  uint64_t fact = hash_id(low ^ hash_id(
    high + UINT64_C(0x455155414c504154)));
  return (UINT32_C(1) << (fact & 31)) |
         (UINT32_C(1) << ((fact >> 13) & 31));
}

static void append_occurrence(struct cfi_term_occurrence **occurrences,
                              size_t *count, size_t *capacity,
                              Term term, uint64_t context, uint64_t path)
{
  if (*count == *capacity) {
    *capacity = grow_capacity(*capacity, sizeof(**occurrences),
                              "compact_feature_index: occurrence overflow");
    *occurrences = safe_realloc(
      *occurrences, *capacity * sizeof(**occurrences));
  }
  (*occurrences)[*count].term = term;
  (*occurrences)[*count].location = hash_id(context ^ hash_id(path));
  (*occurrences)[*count].term_hash = hash_term(term);
  (*count)++;
}

static void summarize_term(Term term, uint64_t literal_context,
                           uint64_t path, BOOL atom_root,
                           struct compact_feature_structural_summary *summary,
                           struct cfi_term_occurrence **occurrences,
                           size_t *count, size_t *capacity)
{
  int i;
  if (!atom_root)
    append_occurrence(occurrences, count, capacity, term,
                      literal_context, path);
  if (VARIABLE(term))
    return;
  summary->rigid |= rigid_fact_mask(
    literal_context, path, (unsigned) SYMNUM(term));
  for (i = 0; i < ARITY(term); i++) {
    uint64_t child_path = hash_id(
      path ^ ((uint64_t) (unsigned) (i + 1) *
              UINT64_C(0xd6e8feb86659fd93)));
    summarize_term(ARG(term, i), literal_context, child_path, FALSE,
                   summary, occurrences, count, capacity);
  }
}

static int occurrence_hash_order(const void *a, const void *b)
{
  const struct cfi_term_occurrence *x = a;
  const struct cfi_term_occurrence *y = b;
  return x->term_hash < y->term_hash ? -1 :
         x->term_hash > y->term_hash ? 1 : 0;
}

struct compact_feature_structural_summary compact_feature_clause_summary(
  Topform clause)
{
  struct compact_feature_structural_summary summary;
  struct cfi_term_occurrence *occurrences = NULL;
  size_t count = 0, capacity = 0, i, j;
  Literals literal;
  memset(&summary, 0, sizeof(summary));
  if (clause == NULL)
    return summary;
  for (literal = clause->literals; literal != NULL; literal = literal->next) {
    Term atom = literal->atom;
    uint64_t context;
    if (atom == NULL || VARIABLE(atom))
      continue;
    context = hash_id(((uint64_t) (unsigned) SYMNUM(atom) << 1) |
                      (literal->sign ? UINT64_C(1) : UINT64_C(0)));
    summarize_term(atom, context, UINT64_C(0xa0761d6478bd642f), TRUE,
                   &summary, &occurrences, &count, &capacity);
  }
  for (i = 0; i < count; i++)
    if (VARIABLE(occurrences[i].term))
      for (j = i + 1; j < count; j++)
        if (VARIABLE(occurrences[j].term) &&
            VARNUM(occurrences[i].term) == VARNUM(occurrences[j].term) &&
            occurrences[i].location != occurrences[j].location)
          summary.variable_constraints |= equality_fact_mask(
            occurrences[i].location, occurrences[j].location);
  if (count > 1)
    qsort(occurrences, count, sizeof(*occurrences), occurrence_hash_order);
  for (i = 0; i < count; i++) {
    for (j = i + 1;
         j < count && occurrences[j].term_hash == occurrences[i].term_hash;
         j++)
      if (occurrences[i].location != occurrences[j].location &&
          term_ident(occurrences[i].term, occurrences[j].term))
        summary.equal_positions |= equality_fact_mask(
          occurrences[i].location, occurrences[j].location);
  }
  safe_free(occurrences);
  return summary;
}

static unsigned long long index_bytes(Compact_feature_index index)
{
  if (index == NULL)
    return 0;
  return sizeof(*index) +
    index->node_capacity * sizeof(*index->nodes) +
    index->label_capacity * sizeof(*index->labels) +
    index->posting_capacity * sizeof(*index->postings) +
    index->record_capacity * sizeof(*index->records) +
    (index->structural_summaries == NULL ? 0 :
     index->record_capacity * sizeof(*index->structural_summaries)) +
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
  else if ((index->hash_count + index->hash_tombstones + 1) * 20 >=
           index->hash_capacity * 17) {
    if (index->hash_tombstones != 0 &&
        (index->hash_count + 1) * 20 < index->hash_capacity * 17)
      rehash(index, index->hash_capacity);
    else {
      if (index->hash_capacity > SIZE_MAX / 2)
        fatal_error("compact_feature_index: hash overflow");
      rehash(index, index->hash_capacity * 2);
    }
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

static void ensure_labels(Compact_feature_index index, size_t extra)
{
  size_t needed;
  if (extra > SIZE_MAX - index->label_count)
    fatal_error("compact_feature_index: label overflow");
  needed = index->label_count + extra;
  if (needed > UINT32_MAX)
    fatal_error("compact_feature_index: label offsets exceed 32 bits");
  while (needed > index->label_capacity) {
    index->label_capacity = grow_capacity(
      index->label_capacity, sizeof(*index->labels),
      "compact_feature_index: label capacity overflow");
    index->labels = safe_realloc(
      index->labels, index->label_capacity * sizeof(*index->labels));
  }
}

static uint32_t append_labels(Compact_feature_index index,
                              const int *features, int start, int length)
{
  uint32_t offset = (uint32_t) index->label_count;
  ensure_labels(index, (size_t) length);
  memcpy(index->labels + index->label_count, features + start,
         (size_t) length * sizeof(*index->labels));
  index->label_count += (size_t) length;
  return offset;
}

static uint32_t new_node(Compact_feature_index index,
                         uint32_t label_offset, uint32_t label_length)
{
  uint32_t node;
  ENSURE_ARRAY(index, nodes, node_count, node_capacity,
               "compact_feature_index: node overflow");
  if (index->node_count > UINT32_MAX)
    fatal_error("compact_feature_index: node offsets exceed 32 bits");
  node = (uint32_t) index->node_count++;
  memset(&index->nodes[node], 0, sizeof(index->nodes[node]));
  index->nodes[node].label_offset = label_offset;
  index->nodes[node].label_length = label_length;
  return node;
}

static int32_t first_label(Compact_feature_index index, uint32_t node)
{
  struct cfi_node *n = &index->nodes[node];
  if (n->label_length == 0)
    fatal_error("compact_feature_index: empty nonroot radix edge");
  return index->labels[n->label_offset];
}

static uint32_t insert_vector(Compact_feature_index index,
                              const int *features)
{
  uint32_t parent = index->root;
  int level = 0;
  while (level < index->feature_length) {
    uint32_t current = index->nodes[parent].first_child;
    uint32_t previous = CFI_NONE;
    int32_t wanted = features[level];
    while (current != CFI_NONE && first_label(index, current) < wanted) {
      previous = current;
      current = index->nodes[current].next_sibling;
    }
    if (current == CFI_NONE || first_label(index, current) != wanted) {
      uint32_t offset = append_labels(
        index, features, level, index->feature_length - level);
      uint32_t added = new_node(
        index, offset, (uint32_t) (index->feature_length - level));
      if (previous == CFI_NONE) {
        index->nodes[added].next_sibling =
          index->nodes[parent].first_child;
        index->nodes[parent].first_child = added;
      }
      else {
        index->nodes[added].next_sibling =
          index->nodes[previous].next_sibling;
        index->nodes[previous].next_sibling = added;
      }
      return added;
    }
    else {
      uint32_t offset = index->nodes[current].label_offset;
      uint32_t length = index->nodes[current].label_length;
      uint32_t common = 0;
      while (common < length &&
             index->labels[offset + common] == features[level + common])
        common++;
      if (common == length) {
        level += (int) length;
        parent = current;
      }
      else {
        uint32_t old_next = index->nodes[current].next_sibling;
        uint32_t split = new_node(index, offset, common);
        uint32_t added_offset, added;
        index->nodes[split].next_sibling = old_next;
        if (previous == CFI_NONE)
          index->nodes[parent].first_child = split;
        else
          index->nodes[previous].next_sibling = split;
        index->nodes[current].label_offset += common;
        index->nodes[current].label_length -= common;
        index->nodes[current].next_sibling = CFI_NONE;
        index->nodes[split].first_child = current;
        level += (int) common;
        added_offset = append_labels(
          index, features, level, index->feature_length - level);
        added = new_node(
          index, added_offset,
          (uint32_t) (index->feature_length - level));
        if (first_label(index, added) < first_label(index, current)) {
          index->nodes[added].next_sibling = current;
          index->nodes[split].first_child = added;
        }
        else
          index->nodes[current].next_sibling = added;
        return added;
      }
    }
  }
  return parent;
}

static void ensure_records(Compact_feature_index index)
{
  if (index->record_count == index->record_capacity) {
    size_t old_capacity = index->record_capacity;
    index->record_capacity = grow_capacity(
      old_capacity, sizeof(*index->records),
      "compact_feature_index: record overflow");
    index->records = safe_realloc(
      index->records, index->record_capacity * sizeof(*index->records));
    if (index->structural_filter) {
      index->structural_summaries = safe_realloc(
        index->structural_summaries,
        index->record_capacity * sizeof(*index->structural_summaries));
      memset(index->structural_summaries + old_capacity, 0,
             (index->record_capacity - old_capacity) *
             sizeof(*index->structural_summaries));
    }
  }
}

Compact_feature_index compact_feature_index_init(int feature_length,
                                                 BOOL structural_filter)
{
  Compact_feature_index index;
  if (feature_length <= 0)
    fatal_error("compact_feature_index_init: feature length must be positive");
  index = safe_calloc(1, sizeof(*index));
  index->feature_length = feature_length;
  index->structural_filter = structural_filter;
  index->forward_lookup_clock = clock_init("compact_nonunit_forward_lookup");
  index->back_lookup_clock = clock_init("compact_nonunit_back_lookup");
  (void) new_node(index, 0, 0);  /* reserved null node */
  index->root = new_node(index, 0, 0);
  ENSURE_ARRAY(index, postings, posting_count, posting_capacity,
               "compact_feature_index: posting overflow");
  memset(&index->postings[0], 0, sizeof(index->postings[0]));
  index->posting_count = 1;
  ensure_records(index);
  memset(&index->records[0], 0, sizeof(index->records[0]));
  index->record_count = 1;
  update_peak(index);
  return index;
}

BOOL compact_feature_index_add(Compact_feature_index index,
                               unsigned long long proof_id,
                               const int *features,
                               struct compact_feature_structural_summary
                                 structural)
{
  uint32_t record_index, node, posting;
  struct cfi_record *record;
  size_t at_hash;
  if (index == NULL || proof_id == 0 || features == NULL ||
      lookup_record(index, proof_id) != CFI_NONE)
    return FALSE;
  ensure_records(index);
  if (index->record_count > UINT32_MAX)
    fatal_error("compact_feature_index: record offsets exceed 32 bits");
  record_index = (uint32_t) index->record_count++;
  record = &index->records[record_index];
  memset(record, 0, sizeof(*record));
  record->proof_id = proof_id;
  record->active = TRUE;
  if (index->structural_filter)
    index->structural_summaries[record_index] = structural;
  node = insert_vector(index, features);
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
                         BOOL forward,
                         struct compact_feature_structural_summary query,
                         size_t *count)
{
  uint32_t posting;
  for (posting = index->nodes[node].first_posting; posting != CFI_NONE;
       posting = index->postings[posting].next) {
    struct cfi_record *record =
      &index->records[index->postings[posting].record];
    index->query_postings++;
    if (record->active) {
      struct compact_feature_structural_summary stored;
      memset(&stored, 0, sizeof(stored));
      if (index->structural_filter)
        stored = index->structural_summaries[
          index->postings[posting].record];
      index->query_live++;
      if (index->structural_filter &&
          (forward ? (stored.rigid & ~query.rigid) != 0 :
                     (query.rigid & ~stored.rigid) != 0))
        index->query_structural_rejects++;
      else if (index->structural_filter &&
               (forward ?
                 (stored.variable_constraints & ~query.equal_positions) != 0 :
                 (query.variable_constraints & ~stored.equal_positions) != 0))
        index->query_variable_rejects++;
      else {
        ensure_results(index, *count + 1);
        index->results[(*count)++] = record->proof_id;
      }
    }
    else
      index->query_dead++;
  }
}

static void collect_candidates(Compact_feature_index index, uint32_t node,
                               int level, const int *query, BOOL forward,
                               struct compact_feature_structural_summary
                                 structural,
                               size_t *count)
{
  uint32_t child;
  if (level == index->feature_length) {
    collect_leaf(index, node, forward, structural, count);
    return;
  }
  child = index->nodes[node].first_child;
  if (!forward)
    while (child != CFI_NONE && first_label(index, child) < query[level])
      child = index->nodes[child].next_sibling;
  while (child != CFI_NONE &&
         (!forward || first_label(index, child) <= query[level])) {
    struct cfi_node *edge = &index->nodes[child];
    uint32_t i;
    BOOL eligible = level + (int) edge->label_length <=
                    index->feature_length;
    index->query_nodes++;
    for (i = 0; eligible && i < edge->label_length; i++) {
      int32_t label = index->labels[edge->label_offset + i];
      int32_t bound = query[level + (int) i];
      eligible = forward ? label <= bound : label >= bound;
    }
    if (eligible)
      collect_candidates(index, child, level + (int) edge->label_length,
                         query, forward, structural, count);
    child = index->nodes[child].next_sibling;
  }
}

static unsigned long long *candidates(Compact_feature_index index,
                                      const int *query, BOOL forward,
                                      struct compact_feature_structural_summary
                                        structural,
                                      size_t *count)
{
  unsigned long long *answer;
  *count = 0;
  if (index == NULL || query == NULL)
    return NULL;
  clock_start(forward ? index->forward_lookup_clock :
              index->back_lookup_clock);
  index->query_nodes = 0;
  index->query_postings = 0;
  index->query_live = 0;
  index->query_dead = 0;
  index->query_structural_rejects = 0;
  index->query_variable_rejects = 0;
  collect_candidates(index, index->root, 0, query, forward, structural,
                     count);
  answer = *count == 0 ? NULL : safe_malloc(*count * sizeof(*answer));
  if (*count != 0)
    memcpy(answer, index->results, *count * sizeof(*answer));
  if (forward) {
    index->forward_queries++;
    index->forward_candidates += *count;
    index->forward_structural_rejects += index->query_structural_rejects;
    index->forward_variable_rejects += index->query_variable_rejects;
    compact_profile_note(&index->forward_profile, *count,
                         index->query_nodes + index->query_postings,
                         index->query_live, index->query_dead,
                         0, 0, 0);
  }
  else {
    index->back_queries++;
    index->back_candidates += *count;
    index->back_structural_rejects += index->query_structural_rejects;
    index->back_variable_rejects += index->query_variable_rejects;
    compact_profile_note(&index->back_profile, *count,
                         index->query_nodes + index->query_postings,
                         index->query_live, index->query_dead,
                         0, 0, 0);
  }
  update_peak(index);
  clock_stop(forward ? index->forward_lookup_clock :
             index->back_lookup_clock);
  return answer;
}

unsigned long long *compact_feature_forward_candidates(
  Compact_feature_index index, const int *query,
  struct compact_feature_structural_summary structural,
  size_t *count)
{
  return candidates(index, query, TRUE, structural, count);
}

unsigned long long *compact_feature_back_candidates(
  Compact_feature_index index, const int *query,
  struct compact_feature_structural_summary structural,
  size_t *count)
{
  return candidates(index, query, FALSE, structural, count);
}

void compact_feature_note_exact_query(
  Compact_feature_index index, BOOL forward,
  size_t exact_tests, size_t successes, size_t materializations)
{
  struct compact_query_profile *profile;
  if (index == NULL)
    return;
  profile = forward ? &index->forward_profile : &index->back_profile;
  compact_profile_note_exact(profile, exact_tests, successes,
                             materializations);
  profile->successes += successes;
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
  stats->forward_structural_rejects = index->forward_structural_rejects;
  stats->forward_variable_rejects = index->forward_variable_rejects;
  stats->back_queries = index->back_queries;
  stats->back_candidates = index->back_candidates;
  stats->back_structural_rejects = index->back_structural_rejects;
  stats->back_variable_rejects = index->back_variable_rejects;
  stats->forward_profile = index->forward_profile;
  stats->back_profile = index->back_profile;
  stats->forward_lookup_seconds = clock_seconds(index->forward_lookup_clock);
  stats->back_lookup_seconds = clock_seconds(index->back_lookup_clock);
  stats->node_bytes = index->node_capacity * sizeof(*index->nodes);
  stats->label_bytes = index->label_capacity * sizeof(*index->labels);
  stats->posting_bytes = index->posting_capacity * sizeof(*index->postings);
  stats->record_bytes = index->record_capacity * sizeof(*index->records);
  stats->structural_bytes = index->structural_summaries == NULL ? 0 :
    index->record_capacity * sizeof(*index->structural_summaries);
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
  safe_free(index->labels);
  safe_free(index->postings);
  safe_free(index->records);
  safe_free(index->structural_summaries);
  safe_free(index->hash_keys);
  safe_free(index->hash_values);
  safe_free(index->results);
  free_clock(index->forward_lookup_clock);
  free_clock(index->back_lookup_clock);
  safe_free(index);
}
