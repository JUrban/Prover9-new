#include "compact_feature_index.h"

#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>

#define CFI_NONE 0U
#define CFI_TOMBSTONE UINT64_MAX
#define CFI_BACK_STRUCTURAL_BITS 96
#define CFI_GROWTH_STALE_FLOOR 16384ULL

static unsigned Compaction_stale_pct = 25;

struct cfi_node {
  uint32_t label_offset;
  uint32_t label_length;
  uint32_t first_child;
  uint32_t next_sibling;
  uint32_t first_posting;
  uint32_t parent;
  uint32_t live_count;
};

struct cfi_posting {
  uint32_t record;
  uint32_t next;
};

struct cfi_record {
  unsigned long long proof_id;
  uint32_t leaf;
  unsigned char active;
};

struct cfi_snapshot_record {
  unsigned long long proof_id;
  struct compact_feature_structural_summary structural;
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
  uint64_t *back_structural_bitmaps[CFI_BACK_STRUCTURAL_BITS];
  size_t structural_bitmap_words;
  unsigned long long structural_bit_counts[CFI_BACK_STRUCTURAL_BITS];
  unsigned long long *hash_keys;
  uint32_t *hash_values;
  size_t hash_capacity;
  size_t hash_count;
  size_t hash_tombstones;
  unsigned long long *results;
  size_t result_capacity;
  uint32_t *structural_results;
  size_t structural_result_capacity;
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
  unsigned long long back_structural_bitmap_queries;
  unsigned long long back_structural_bitmap_words;
  unsigned long long back_structural_bitmap_records;
  unsigned long long compactions;
  unsigned long long bytes_reclaimed;
  unsigned long long snapshot_records;
  unsigned long long snapshot_bytes;
  unsigned long long maintenance_scratch_peak;
  struct compact_query_profile forward_profile;
  struct compact_query_profile back_profile;
  unsigned long long query_nodes;
  unsigned long long query_postings;
  unsigned long long query_live;
  unsigned long long query_dead;
  unsigned long long query_structural_rejects;
  unsigned long long query_variable_rejects;
  struct compact_query_timer forward_lookup_timer;
  struct compact_query_timer back_lookup_timer;
  Clock maintenance_clock;
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
    (unsigned long long) CFI_BACK_STRUCTURAL_BITS *
      index->structural_bitmap_words * sizeof(uint64_t) +
    index->hash_capacity *
      (sizeof(*index->hash_keys) + sizeof(*index->hash_values)) +
    index->result_capacity * sizeof(*index->results) +
    index->structural_result_capacity * sizeof(*index->structural_results);
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
      index->nodes[added].parent = parent;
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
        index->nodes[split].parent = parent;
        index->nodes[split].live_count = index->nodes[current].live_count;
        if (previous == CFI_NONE)
          index->nodes[parent].first_child = split;
        else
          index->nodes[previous].next_sibling = split;
        index->nodes[current].label_offset += common;
        index->nodes[current].label_length -= common;
        index->nodes[current].next_sibling = CFI_NONE;
        index->nodes[current].parent = split;
        index->nodes[split].first_child = current;
        level += (int) common;
        added_offset = append_labels(
          index, features, level, index->feature_length - level);
        added = new_node(
          index, added_offset,
          (uint32_t) (index->feature_length - level));
        index->nodes[added].parent = split;
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

static void change_live_path(Compact_feature_index index, uint32_t node,
                             BOOL adding)
{
  for (;;) {
    if (node == CFI_NONE || node >= index->node_count)
      fatal_error("compact_feature_index: corrupt live-count path");
    if (adding) {
      if (index->nodes[node].live_count == UINT32_MAX)
        fatal_error("compact_feature_index: node live count overflow");
      index->nodes[node].live_count++;
    }
    else {
      if (index->nodes[node].live_count == 0)
        fatal_error("compact_feature_index: node live count underflow");
      index->nodes[node].live_count--;
    }
    if (node == index->root)
      break;
    node = index->nodes[node].parent;
  }
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

static void ensure_structural_bitmaps(Compact_feature_index index,
                                      uint32_t record)
{
  size_t needed, words, i;
  if (!index->structural_filter)
    return;
  needed = (size_t) record / 64 + 1;
  if (needed <= index->structural_bitmap_words)
    return;
  words = index->structural_bitmap_words == 0 ? 1 :
    index->structural_bitmap_words;
  while (words < needed) {
    if (words > SIZE_MAX / 2 || words * 2 > SIZE_MAX / sizeof(uint64_t))
      fatal_error("compact_feature_index: structural bitmap overflow");
    words *= 2;
  }
  for (i = 0; i < CFI_BACK_STRUCTURAL_BITS; i++) {
    index->back_structural_bitmaps[i] = safe_realloc(
      index->back_structural_bitmaps[i],
      words * sizeof(*index->back_structural_bitmaps[i]));
    memset(index->back_structural_bitmaps[i] +
             index->structural_bitmap_words, 0,
           (words - index->structural_bitmap_words) *
             sizeof(*index->back_structural_bitmaps[i]));
  }
  index->structural_bitmap_words = words;
}

static void add_structural_bits(Compact_feature_index index,
                                uint32_t record,
                                struct compact_feature_structural_summary s)
{
  unsigned bit;
  ensure_structural_bitmaps(index, record);
  for (bit = 0; bit < 64; bit++)
    if ((s.rigid & (UINT64_C(1) << bit)) != 0) {
      index->back_structural_bitmaps[bit][record / 64] |=
        UINT64_C(1) << (record % 64);
      index->structural_bit_counts[bit]++;
    }
  for (bit = 0; bit < 32; bit++)
    if ((s.equal_positions & (UINT32_C(1) << bit)) != 0) {
      unsigned at = 64 + bit;
      index->back_structural_bitmaps[at][record / 64] |=
        UINT64_C(1) << (record % 64);
      index->structural_bit_counts[at]++;
    }
}

static void subtract_structural_bits(
  Compact_feature_index index,
  struct compact_feature_structural_summary s)
{
  unsigned bit;
  if (!index->structural_filter)
    return;
  for (bit = 0; bit < 64; bit++)
    if ((s.rigid & (UINT64_C(1) << bit)) != 0) {
      if (index->structural_bit_counts[bit] == 0)
        fatal_error("compact_feature_index: rigid count underflow");
      index->structural_bit_counts[bit]--;
    }
  for (bit = 0; bit < 32; bit++)
    if ((s.equal_positions & (UINT32_C(1) << bit)) != 0) {
      unsigned at = 64 + bit;
      if (index->structural_bit_counts[at] == 0)
        fatal_error("compact_feature_index: equality count underflow");
      index->structural_bit_counts[at]--;
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
  index->maintenance_clock = clock_init("compact_nonunit_maintenance");
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
  if (index->structural_filter)
    add_structural_bits(index, record_index, structural);
  node = insert_vector(index, features);
  record->leaf = node;
  change_live_path(index, node, TRUE);
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
  if (index->structural_filter)
    subtract_structural_bits(index, index->structural_summaries[record]);
  change_live_path(index, index->records[record].leaf, FALSE);
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

BOOL compact_feature_index_compaction_needed(Compact_feature_index index)
{
  unsigned long long physical, stale, threshold;
  if (index == NULL || index->record_count <= 1)
    return FALSE;
  physical = index->record_count - 1;
  stale = physical - index->active;
  threshold = (index->active / 100) * Compaction_stale_pct +
    ((index->active % 100) * Compaction_stale_pct + 99) / 100;
  if (threshold < index->active &&
      threshold < CFI_GROWTH_STALE_FLOOR)
    threshold = index->active < CFI_GROWTH_STALE_FLOOR ?
      index->active : CFI_GROWTH_STALE_FLOOR;
  if (threshold < 1024)
    threshold = 1024;
  return stale >= threshold;
}

void compact_feature_index_set_compaction_stale_pct(unsigned percentage)
{
  if (percentage == 0 || percentage > 1000)
    fatal_error("compact_feature_index: invalid stale percentage");
  Compaction_stale_pct = percentage;
}

static void set_parent_links(Compact_feature_index index, uint32_t parent,
                             uint32_t *parents)
{
  uint32_t child;
  for (child = index->nodes[parent].first_child; child != CFI_NONE;
       child = index->nodes[child].next_sibling) {
    if (child >= index->node_count || parents[child] != CFI_NONE)
      fatal_error("compact_feature_index: corrupt radix parent link");
    parents[child] = parent;
    set_parent_links(index, child, parents);
  }
}

static void reconstruct_record_features(Compact_feature_index index,
                                        const struct cfi_record *record,
                                        const uint32_t *parents,
                                        int *features)
{
  uint32_t node = record->leaf;
  size_t end = (size_t) index->feature_length;
  if (node == CFI_NONE || node >= index->node_count)
    fatal_error("compact_feature_index: corrupt record leaf");
  while (node != index->root) {
    const struct cfi_node *edge = &index->nodes[node];
    size_t i;
    if (edge->label_length == 0 || edge->label_length > end ||
        edge->label_offset > index->label_count ||
        edge->label_length > index->label_count - edge->label_offset)
      fatal_error("compact_feature_index: corrupt radix record path");
    end -= edge->label_length;
    for (i = 0; i < edge->label_length; i++)
      features[end + i] = index->labels[edge->label_offset + i];
    node = parents[node];
    if (node == CFI_NONE)
      fatal_error("compact_feature_index: disconnected record leaf");
  }
  if (end != 0)
    fatal_error("compact_feature_index: incomplete record feature path");
}

static void free_index_arrays(struct compact_feature_index *index)
{
  size_t i;
  safe_free(index->nodes);
  safe_free(index->labels);
  safe_free(index->postings);
  safe_free(index->records);
  safe_free(index->structural_summaries);
  for (i = 0; i < CFI_BACK_STRUCTURAL_BITS; i++)
    safe_free(index->back_structural_bitmaps[i]);
  safe_free(index->hash_keys);
  safe_free(index->hash_values);
  safe_free(index->results);
  safe_free(index->structural_results);
}

static void compact_feature_index_compact_internal(
  Compact_feature_index index, BOOL force)
{
  Compact_feature_index replacement;
  struct compact_feature_index old;
  struct cfi_snapshot_record snapshot_record;
  uint32_t *parents;
  int *features;
  FILE *snapshot;
  int snapshot_fd;
  unsigned long long old_bytes, old_peak, old_peak_active;
  unsigned long long retired, compactions, reclaimed;
  unsigned long long snapshot_records, snapshot_bytes;
  unsigned long long maintenance_scratch_peak;
  unsigned long long forward_queries, forward_candidates;
  unsigned long long forward_structural_rejects, forward_variable_rejects;
  unsigned long long back_queries, back_candidates;
  unsigned long long back_structural_rejects, back_variable_rejects;
  unsigned long long back_structural_bitmap_queries;
  unsigned long long back_structural_bitmap_words;
  unsigned long long back_structural_bitmap_records;
  struct compact_query_profile forward_profile, back_profile;
  size_t i, written = 0;
  size_t feature_bytes;

  if (index == NULL ||
      (!force && !compact_feature_index_compaction_needed(index)) ||
      (force && index->record_count - 1 == index->active))
    return;
  if ((size_t) index->feature_length > SIZE_MAX / sizeof(*features))
    fatal_error("compact_feature_index: feature snapshot overflow");
  feature_bytes = (size_t) index->feature_length * sizeof(*features);
  if (index->node_count > SIZE_MAX / sizeof(*parents))
    fatal_error("compact_feature_index: parent snapshot overflow");

  clock_start(index->maintenance_clock);
  old_bytes = index_bytes(index);
  old_peak = index->peak_bytes;
  old_peak_active = index->peak;
  retired = index->retired;
  compactions = index->compactions;
  reclaimed = index->bytes_reclaimed;
  snapshot_records = index->snapshot_records;
  snapshot_bytes = index->snapshot_bytes;
  maintenance_scratch_peak = index->maintenance_scratch_peak;
  forward_queries = index->forward_queries;
  forward_candidates = index->forward_candidates;
  forward_structural_rejects = index->forward_structural_rejects;
  forward_variable_rejects = index->forward_variable_rejects;
  back_queries = index->back_queries;
  back_candidates = index->back_candidates;
  back_structural_rejects = index->back_structural_rejects;
  back_variable_rejects = index->back_variable_rejects;
  back_structural_bitmap_queries = index->back_structural_bitmap_queries;
  back_structural_bitmap_words = index->back_structural_bitmap_words;
  back_structural_bitmap_records = index->back_structural_bitmap_records;
  forward_profile = index->forward_profile;
  back_profile = index->back_profile;

  parents = safe_calloc(index->node_count, sizeof(*parents));
  features = safe_malloc(feature_bytes);
  set_parent_links(index, index->root, parents);
  {
    unsigned long long scratch =
      (unsigned long long) index->node_count * sizeof(*parents) +
      feature_bytes;
    if (scratch > maintenance_scratch_peak)
      maintenance_scratch_peak = scratch;
    if (old_bytes <= ULLONG_MAX - scratch && old_bytes + scratch > old_peak)
      old_peak = old_bytes + scratch;
  }

  snapshot_fd = open_private_temp_file("prover9-nonunit-XXXXXX");
  if (snapshot_fd < 0)
    fatal_error("compact_feature_index: cannot create rebuild snapshot");
  snapshot = fdopen(snapshot_fd, "w+b");
  if (snapshot == NULL) {
    close(snapshot_fd);
    fatal_error("compact_feature_index: cannot open rebuild snapshot");
  }
  for (i = 1; i < index->record_count; i++)
    if (index->records[i].active) {
      snapshot_record.proof_id = index->records[i].proof_id;
      memset(&snapshot_record.structural, 0,
             sizeof(snapshot_record.structural));
      if (index->structural_filter)
        snapshot_record.structural = index->structural_summaries[i];
      reconstruct_record_features(index, &index->records[i], parents,
                                  features);
      if (fwrite(&snapshot_record, sizeof(snapshot_record), 1, snapshot) != 1 ||
          fwrite(features, feature_bytes, 1, snapshot) != 1)
        fatal_error("compact_feature_index: cannot write rebuild snapshot");
      written++;
    }
  if (written != index->active || fflush(snapshot) != 0 ||
      fseek(snapshot, 0, SEEK_SET) != 0)
    fatal_error("compact_feature_index: incomplete rebuild snapshot");
  safe_free(parents);
  safe_free(features);

  /* The complete live recipe is now on disk.  Release predecessor arrays
     before allocating the replacement so compaction does not double a large
     nonunit index in process RAM. */
  old = *index;
  free_index_arrays(&old);
  replacement = compact_feature_index_init(old.feature_length,
                                           old.structural_filter);
  features = safe_malloc(feature_bytes);
  for (i = 0; i < written; i++) {
    if (fread(&snapshot_record, sizeof(snapshot_record), 1, snapshot) != 1 ||
        fread(features, feature_bytes, 1, snapshot) != 1)
      fatal_error("compact_feature_index: cannot read rebuild snapshot");
    if (!compact_feature_index_add(replacement, snapshot_record.proof_id,
                                   features, snapshot_record.structural))
      fatal_error("compact_feature_index: cannot rebuild live record");
  }
  safe_free(features);
  if (fclose(snapshot) != 0)
    fatal_error("compact_feature_index: cannot close rebuild snapshot");

  free_clock(replacement->maintenance_clock);
  replacement->forward_lookup_timer = old.forward_lookup_timer;
  replacement->back_lookup_timer = old.back_lookup_timer;
  replacement->maintenance_clock = old.maintenance_clock;
  *index = *replacement;
  safe_free(replacement);

  index->retired = retired;
  index->compactions = compactions + 1;
  index->bytes_reclaimed = reclaimed +
    (old_bytes > index_bytes(index) ? old_bytes - index_bytes(index) : 0);
  index->snapshot_records = snapshot_records + written;
  index->snapshot_bytes = snapshot_bytes + written *
    (sizeof(snapshot_record) + feature_bytes);
  index->maintenance_scratch_peak = maintenance_scratch_peak;
  index->forward_queries = forward_queries;
  index->forward_candidates = forward_candidates;
  index->forward_structural_rejects = forward_structural_rejects;
  index->forward_variable_rejects = forward_variable_rejects;
  index->back_queries = back_queries;
  index->back_candidates = back_candidates;
  index->back_structural_rejects = back_structural_rejects;
  index->back_variable_rejects = back_variable_rejects;
  index->back_structural_bitmap_queries = back_structural_bitmap_queries;
  index->back_structural_bitmap_words = back_structural_bitmap_words;
  index->back_structural_bitmap_records = back_structural_bitmap_records;
  index->forward_profile = forward_profile;
  index->back_profile = back_profile;
  if (old_peak > index->peak_bytes)
    index->peak_bytes = old_peak;
  if (old_peak_active > index->peak)
    index->peak = old_peak_active;
  clock_stop(index->maintenance_clock);
}

void compact_feature_index_compact(Compact_feature_index index)
{
  compact_feature_index_compact_internal(index, FALSE);
}

void compact_feature_index_compact_all_stale(Compact_feature_index index)
{
  compact_feature_index_compact_internal(index, TRUE);
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

static void ensure_structural_results(Compact_feature_index index,
                                      size_t needed)
{
  while (needed > index->structural_result_capacity) {
    index->structural_result_capacity = grow_capacity(
      index->structural_result_capacity,
      sizeof(*index->structural_results),
      "compact_feature_index: structural result overflow");
    index->structural_results = safe_realloc(
      index->structural_results,
      index->structural_result_capacity *
        sizeof(*index->structural_results));
  }
}

static BOOL feature_edge_eligible(Compact_feature_index index,
                                  uint32_t node, int level,
                                  const int *query, BOOL forward)
{
  struct cfi_node *edge = &index->nodes[node];
  uint32_t i;
  BOOL eligible = level + (int) edge->label_length <=
                  index->feature_length;
  for (i = 0; eligible && i < edge->label_length; i++) {
    int32_t label = index->labels[edge->label_offset + i];
    int32_t bound = query[level + (int) i];
    eligible = forward ? label <= bound : label >= bound;
  }
  return eligible;
}

static unsigned long long count_numeric_candidates(
  Compact_feature_index index, uint32_t node, int level,
  const int *query, BOOL forward)
{
  uint32_t child;
  unsigned long long count = 0;
  if (level == index->feature_length)
    return index->nodes[node].live_count;
  child = index->nodes[node].first_child;
  if (!forward)
    while (child != CFI_NONE && first_label(index, child) < query[level])
      child = index->nodes[child].next_sibling;
  while (child != CFI_NONE &&
         (!forward || first_label(index, child) <= query[level])) {
    struct cfi_node *edge = &index->nodes[child];
    index->query_nodes++;
    if (feature_edge_eligible(index, child, level, query, forward))
      count += count_numeric_candidates(
        index, child, level + (int) edge->label_length, query, forward);
    child = edge->next_sibling;
  }
  return count;
}

static BOOL scaled_less(unsigned long long small,
                        unsigned long long large, unsigned factor)
{
  return small <= ULLONG_MAX / factor && small * factor < large;
}

static BOOL choose_back_structural_bitmap(
  Compact_feature_index index, const int *query,
  struct compact_feature_structural_summary structural,
  unsigned *selected_bit)
{
  unsigned bit;
  unsigned long long rare = ULLONG_MAX, estimated, numerical;
  size_t words = index->record_count / 64 +
    (index->record_count % 64 != 0);
  BOOL required = FALSE;
  for (bit = 0; bit < 64; bit++)
    if ((structural.rigid & (UINT64_C(1) << bit)) != 0) {
      required = TRUE;
      if (index->structural_bit_counts[bit] < rare) {
        rare = index->structural_bit_counts[bit];
        *selected_bit = bit;
      }
    }
  for (bit = 0; bit < 32; bit++)
    if ((structural.variable_constraints &
         (UINT32_C(1) << bit)) != 0) {
      unsigned at = 64 + bit;
      required = TRUE;
      if (index->structural_bit_counts[at] < rare) {
        rare = index->structural_bit_counts[at];
        *selected_bit = at;
      }
    }
  if (!required)
    return FALSE;
  if (rare == 0)
    return TRUE;
  estimated = words > ULLONG_MAX - rare ? ULLONG_MAX : words + rare;
  if (!scaled_less(estimated, index->active, 4))
    return FALSE;
  numerical = count_numeric_candidates(
    index, index->root, 0, query, FALSE);
  return scaled_less(estimated, numerical, 4);
}

static BOOL record_features_eligible(Compact_feature_index index,
                                     uint32_t record_index,
                                     const int *query, BOOL forward)
{
  uint32_t node = index->records[record_index].leaf;
  size_t end = (size_t) index->feature_length;
  while (node != index->root) {
    struct cfi_node *edge;
    size_t start, i;
    if (node == CFI_NONE || node >= index->node_count)
      fatal_error("compact_feature_index: corrupt feature test leaf");
    edge = &index->nodes[node];
    if (edge->label_length == 0 || edge->label_length > end)
      fatal_error("compact_feature_index: corrupt feature test edge");
    start = end - edge->label_length;
    for (i = 0; i < edge->label_length; i++) {
      int label = index->labels[edge->label_offset + i];
      int bound = query[start + i];
      if (forward ? label > bound : label < bound)
        return FALSE;
    }
    end = start;
    node = edge->parent;
  }
  if (end != 0)
    fatal_error("compact_feature_index: incomplete feature test path");
  return TRUE;
}

static int compare_record_features(Compact_feature_index index,
                                   uint32_t left, uint32_t right)
{
  size_t position;
  if (index->records[left].leaf == index->records[right].leaf)
    return left < right ? 1 : left > right ? -1 : 0;
  for (position = 0; position < (size_t) index->feature_length; position++) {
    uint32_t node;
    size_t end;
    int values[2];
    unsigned side;
    uint32_t records[2] = {left, right};
    for (side = 0; side < 2; side++) {
      node = index->records[records[side]].leaf;
      end = (size_t) index->feature_length;
      for (;;) {
        struct cfi_node *edge;
        size_t start;
        if (node == index->root)
          fatal_error("compact_feature_index: missing sort feature");
        if (node == CFI_NONE || node >= index->node_count)
          fatal_error("compact_feature_index: corrupt sort leaf");
        edge = &index->nodes[node];
        start = end - edge->label_length;
        if (position >= start && position < end) {
          values[side] = index->labels[
            edge->label_offset + position - start];
          break;
        }
        end = start;
        node = edge->parent;
      }
    }
    if (values[0] < values[1])
      return -1;
    if (values[0] > values[1])
      return 1;
  }
  fatal_error("compact_feature_index: distinct leaves have equal features");
  return 0;
}

static Compact_feature_index Structural_sort_index;

static int structural_record_order(const void *left, const void *right)
{
  return compare_record_features(
    Structural_sort_index, *(const uint32_t *) left,
    *(const uint32_t *) right);
}

static void collect_back_structural_bitmap(
  Compact_feature_index index, const int *query,
  struct compact_feature_structural_summary structural,
  unsigned selected_bit, size_t *count)
{
  size_t words = index->record_count / 64 +
    (index->record_count % 64 != 0);
  size_t word, selected = 0, i;
  index->back_structural_bitmap_queries++;
  for (word = 0; word < words; word++) {
    uint64_t bits = index->back_structural_bitmaps[selected_bit][word];
    index->query_nodes++;
    index->back_structural_bitmap_words++;
    while (bits != 0) {
      unsigned bit = (unsigned) __builtin_ctzll(bits);
      size_t at = word * 64 + bit;
      struct cfi_record *record;
      struct compact_feature_structural_summary stored;
      bits &= bits - 1;
      if (at == 0 || at >= index->record_count)
        continue;
      record = &index->records[at];
      index->query_postings++;
      index->back_structural_bitmap_records++;
      if (!record->active) {
        index->query_dead++;
        continue;
      }
      index->query_live++;
      stored = index->structural_summaries[at];
      if ((structural.rigid & ~stored.rigid) != 0)
        index->query_structural_rejects++;
      else if ((structural.variable_constraints &
                ~stored.equal_positions) != 0)
        index->query_variable_rejects++;
      else if (record_features_eligible(
                 index, (uint32_t) at, query, FALSE)) {
        ensure_structural_results(index, selected + 1);
        index->structural_results[selected++] = (uint32_t) at;
      }
    }
  }
  if (selected > 1) {
    Structural_sort_index = index;
    qsort(index->structural_results, selected,
          sizeof(*index->structural_results), structural_record_order);
    Structural_sort_index = NULL;
  }
  ensure_results(index, selected);
  for (i = 0; i < selected; i++)
    index->results[i] =
      index->records[index->structural_results[i]].proof_id;
  *count = selected;
}

static void collect_leaf(Compact_feature_index index, uint32_t node,
                         BOOL forward,
                         const struct compact_feature_structural_summary *query,
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
          (forward ? (stored.rigid & ~query->rigid) != 0 :
                     (query->rigid & ~stored.rigid) != 0))
        index->query_structural_rejects++;
      else if (index->structural_filter &&
               (forward ?
                 (stored.variable_constraints & ~query->equal_positions) != 0 :
                 (query->variable_constraints & ~stored.equal_positions) != 0))
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

static void collect_forward_candidates(
  Compact_feature_index index, uint32_t node, int level, const int *query,
  const struct compact_feature_structural_summary *structural, size_t *count)
{
  uint32_t child;
  if (level == index->feature_length) {
    collect_leaf(index, node, TRUE, structural, count);
    return;
  }
  child = index->nodes[node].first_child;
  while (child != CFI_NONE && first_label(index, child) <= query[level]) {
    struct cfi_node *edge = &index->nodes[child];
    uint32_t i;
    BOOL eligible = level + (int) edge->label_length <=
                    index->feature_length;
    index->query_nodes++;
    for (i = 0; eligible && i < edge->label_length; i++) {
      int32_t label = index->labels[edge->label_offset + i];
      int32_t bound = query[level + (int) i];
      eligible = label <= bound;
    }
    if (eligible)
      collect_forward_candidates(
        index, child, level + (int) edge->label_length,
        query, structural, count);
    child = index->nodes[child].next_sibling;
  }
}

static void collect_back_candidates(
  Compact_feature_index index, uint32_t node, int level, const int *query,
  const struct compact_feature_structural_summary *structural, size_t *count)
{
  uint32_t child;
  if (level == index->feature_length) {
    collect_leaf(index, node, FALSE, structural, count);
    return;
  }
  child = index->nodes[node].first_child;
  while (child != CFI_NONE && first_label(index, child) < query[level])
    child = index->nodes[child].next_sibling;
  while (child != CFI_NONE) {
    struct cfi_node *edge = &index->nodes[child];
    uint32_t i;
    BOOL eligible = level + (int) edge->label_length <=
                    index->feature_length;
    index->query_nodes++;
    for (i = 0; eligible && i < edge->label_length; i++) {
      int32_t label = index->labels[edge->label_offset + i];
      int32_t bound = query[level + (int) i];
      eligible = label >= bound;
    }
    if (eligible)
      collect_back_candidates(
        index, child, level + (int) edge->label_length,
        query, structural, count);
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
  compact_query_timer_start(forward ? &index->forward_lookup_timer :
                            &index->back_lookup_timer);
  index->query_nodes = 0;
  index->query_postings = 0;
  index->query_live = 0;
  index->query_dead = 0;
  index->query_structural_rejects = 0;
  index->query_variable_rejects = 0;
  if (!forward && index->structural_filter) {
    unsigned selected_bit = 0;
    if (choose_back_structural_bitmap(
          index, query, structural, &selected_bit))
      collect_back_structural_bitmap(
        index, query, structural, selected_bit, count);
    else
      collect_back_candidates(
        index, index->root, 0, query, &structural, count);
  }
  else if (forward)
    collect_forward_candidates(
      index, index->root, 0, query, &structural, count);
  else
    collect_back_candidates(
      index, index->root, 0, query, &structural, count);
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
  compact_query_timer_stop(forward ? &index->forward_lookup_timer :
                           &index->back_lookup_timer);
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
  stats->back_structural_bitmap_queries =
    index->back_structural_bitmap_queries;
  stats->back_structural_bitmap_words =
    index->back_structural_bitmap_words;
  stats->back_structural_bitmap_records =
    index->back_structural_bitmap_records;
  stats->compactions = index->compactions;
  stats->bytes_reclaimed = index->bytes_reclaimed;
  stats->snapshot_records = index->snapshot_records;
  stats->snapshot_bytes = index->snapshot_bytes;
  stats->maintenance_scratch_peak = index->maintenance_scratch_peak;
  stats->forward_profile = index->forward_profile;
  stats->back_profile = index->back_profile;
  stats->forward_lookup_seconds =
    index->forward_lookup_timer.estimated_seconds;
  stats->back_lookup_seconds = index->back_lookup_timer.estimated_seconds;
  stats->forward_timing_eligible = index->forward_lookup_timer.eligible;
  stats->forward_timing_samples = index->forward_lookup_timer.samples;
  stats->back_timing_eligible = index->back_lookup_timer.eligible;
  stats->back_timing_samples = index->back_lookup_timer.samples;
  stats->timing_sample_rate = COMPACT_TIMING_SAMPLE_RATE;
  stats->maintenance_seconds = clock_seconds(index->maintenance_clock);
  stats->node_bytes = index->node_capacity * sizeof(*index->nodes);
  stats->label_bytes = index->label_capacity * sizeof(*index->labels);
  stats->posting_bytes = index->posting_capacity * sizeof(*index->postings);
  stats->record_bytes = index->record_capacity * sizeof(*index->records);
  stats->structural_bytes = index->structural_summaries == NULL ? 0 :
    index->record_capacity * sizeof(*index->structural_summaries);
  stats->structural_index_bytes =
    (unsigned long long) CFI_BACK_STRUCTURAL_BITS *
      index->structural_bitmap_words * sizeof(uint64_t);
  stats->hash_bytes = index->hash_capacity *
    (sizeof(*index->hash_keys) + sizeof(*index->hash_values));
  stats->scratch_bytes = index->result_capacity * sizeof(*index->results) +
    index->structural_result_capacity * sizeof(*index->structural_results);
  stats->total_bytes = index_bytes(index);
  stats->peak_bytes = index->peak_bytes;
}

void compact_feature_index_free(Compact_feature_index index)
{
  if (index == NULL)
    return;
  free_index_arrays(index);
  free_clock(index->maintenance_clock);
  safe_free(index);
}
