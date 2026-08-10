#include "compact_unit_index.h"

#include <stdint.h>
#include <string.h>

#define CUI_NONE 0U
#define CUI_TOMBSTONE UINT64_MAX

static unsigned Compaction_stale_pct = 25;

struct cui_node {
  uint32_t token_offset;
  uint32_t token_length;
  uint32_t first_child;
  uint32_t next_sibling;
  uint32_t first_posting;
  uint32_t last_posting;
};

struct cui_posting {
  uint32_t record;
  uint32_t next;
};

struct cui_record {
  unsigned long long proof_id;
  uint64_t symbol_mask;
  uint32_t token_offset;
  uint32_t token_length;
  uint32_t next_root;
  unsigned char sign;
  unsigned char active;
};

struct cui_query_term {
  Term term;
  uint32_t end;
};

struct compact_unit_index {
  struct cui_node *nodes;
  size_t node_count;
  size_t node_capacity;
  uint32_t roots[2];
  struct cui_posting *postings;
  size_t posting_count;
  size_t posting_capacity;
  struct cui_record *records;
  size_t record_count;
  size_t record_capacity;
  uint32_t *unifier_heads[2];
  size_t unifier_symbol_capacity;
  Compact_term_pool term_pool;
  const int32_t *tokens;
  BOOL owns_term_pool;
  unsigned long long *hash_keys;
  uint32_t *hash_values;
  size_t hash_capacity;
  size_t hash_count;
  size_t hash_tombstones;
  struct cui_query_term *query;
  size_t query_capacity;
  unsigned long long *result_ids;
  size_t result_capacity;
  unsigned long long active;
  unsigned long long peak;
  unsigned long long retired;
  unsigned long long compactions;
  unsigned long long bytes_reclaimed;
  unsigned long long generalization_queries;
  unsigned long long instance_queries;
  unsigned long long instance_exact_tests;
  unsigned long long unifier_queries;
  unsigned long long unifier_exact_tests;
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

static size_t grow_node_capacity(size_t current, size_t item_size,
                                 const char *message)
{
  size_t increment;
  size_t next;
  if (current == 0)
    return 64;
  increment = current / 4;
  if (increment < 16)
    increment = 16;
  if (increment > SIZE_MAX - current)
    fatal_error((char *) message);
  next = current + increment;
  if (next > SIZE_MAX / item_size)
    fatal_error((char *) message);
  return next;
}

static size_t grow_dense_capacity(size_t current, size_t item_size,
                                  const char *message)
{
  size_t increment, next;
  if (current == 0)
    return 64;
  increment = current / 4;
  if (increment < 64)
    increment = 64;
  if (increment > SIZE_MAX - current)
    fatal_error((char *) message);
  next = current + increment;
  if (next > SIZE_MAX / item_size)
    fatal_error((char *) message);
  return next;
}

static uint64_t hash_id(uint64_t x)
{
  x ^= x >> 30;
  x *= UINT64_C(0xbf58476d1ce4e5b9);
  x ^= x >> 27;
  x *= UINT64_C(0x94d049bb133111eb);
  x ^= x >> 31;
  return x;
}

static uint64_t symbol_bit(unsigned symbol)
{
  return UINT64_C(1) << ((symbol * UINT32_C(2654435761)) >> 26);
}

static unsigned long long index_bytes(Compact_unit_index index)
{
  struct compact_term_pool_stats terms;
  if (index == NULL)
    return 0;
  compact_term_pool_get_stats(index->term_pool, &terms);
  return sizeof(*index) +
    index->node_capacity * sizeof(*index->nodes) +
    index->posting_capacity * sizeof(*index->postings) +
    index->record_capacity * sizeof(*index->records) +
    index->unifier_symbol_capacity *
      (sizeof(*index->unifier_heads[0]) +
       sizeof(*index->unifier_heads[1])) +
    (index->owns_term_pool ? terms.total_bytes : 0) +
    index->hash_capacity *
      (sizeof(*index->hash_keys) + sizeof(*index->hash_values)) +
    index->query_capacity * sizeof(*index->query) +
    index->result_capacity * sizeof(*index->result_ids);
}

static void update_peak(Compact_unit_index index)
{
  unsigned long long bytes = index_bytes(index);
  if (bytes > index->peak_bytes)
    index->peak_bytes = bytes;
  if (index->active > index->peak)
    index->peak = index->active;
}

#define ENSURE_ARRAY(index, field, count, capacity, message) do {       \
  if ((index)->count == (index)->capacity) {                            \
    (index)->capacity = grow_dense_capacity((index)->capacity,          \
      sizeof(*(index)->field), (message));                              \
    (index)->field = safe_realloc((index)->field,                       \
      (index)->capacity * sizeof(*(index)->field));                     \
  }                                                                    \
} while (0)

static size_t hash_slot(Compact_unit_index index, uint64_t id,
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
    if (inserting && key == CUI_TOMBSTONE && tombstone == SIZE_MAX)
      tombstone = at;
    at = (at + 1) & mask;
  }
}

static void rehash(Compact_unit_index index, size_t capacity)
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
    if (old_keys[i] != 0 && old_keys[i] != CUI_TOMBSTONE) {
      size_t at = hash_slot(index, old_keys[i], TRUE);
      index->hash_keys[at] = old_keys[i];
      index->hash_values[at] = old_values[i];
    }
  safe_free(old_keys);
  safe_free(old_values);
}

static void ensure_hash(Compact_unit_index index)
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
        fatal_error("compact_unit_index: hash overflow");
      rehash(index, index->hash_capacity * 2);
    }
  }
}

static uint32_t lookup_record(Compact_unit_index index,
                              unsigned long long proof_id)
{
  size_t at;
  if (index == NULL || proof_id == 0 || index->hash_capacity == 0)
    return CUI_NONE;
  at = hash_slot(index, proof_id, FALSE);
  return index->hash_keys[at] == proof_id ?
    index->hash_values[at] : CUI_NONE;
}

static int code_compare(int32_t a, int32_t b)
{
  BOOL av = a < 0, bv = b < 0;
  if (av != bv)
    return av ? -1 : 1;
  if (av) {
    int32_t va = -a - 1, vb = -b - 1;
    return va < vb ? -1 : va > vb ? 1 : 0;
  }
  return a < b ? -1 : a > b ? 1 : 0;
}

static uint32_t new_node(Compact_unit_index index,
                         uint32_t token_offset, uint32_t token_length)
{
  uint32_t node;
  if (index->node_count == index->node_capacity) {
    index->node_capacity = grow_node_capacity(
      index->node_capacity, sizeof(*index->nodes),
      "compact_unit_index: node overflow");
    index->nodes = safe_realloc(
      index->nodes, index->node_capacity * sizeof(*index->nodes));
  }
  if (index->node_count > UINT32_MAX)
    fatal_error("compact_unit_index: node offsets exceed 32 bits");
  node = (uint32_t) index->node_count++;
  memset(&index->nodes[node], 0, sizeof(index->nodes[node]));
  index->nodes[node].token_offset = token_offset;
  index->nodes[node].token_length = token_length;
  return node;
}

static int32_t first_code(Compact_unit_index index, uint32_t node)
{
  struct cui_node *n = &index->nodes[node];
  if (n->token_length == 0)
    fatal_error("compact_unit_index: empty nonroot radix edge");
  return index->tokens[n->token_offset];
}

static uint32_t insert_token_path(Compact_unit_index index, uint32_t root,
                                  uint32_t offset, uint32_t length)
{
  uint32_t parent = root;
  uint32_t position = 0;
  while (position < length) {
    uint32_t current = index->nodes[parent].first_child;
    uint32_t previous = CUI_NONE;
    int32_t wanted = index->tokens[offset + position];
    while (current != CUI_NONE &&
           code_compare(first_code(index, current), wanted) < 0) {
      previous = current;
      current = index->nodes[current].next_sibling;
    }
    if (current == CUI_NONE || first_code(index, current) != wanted) {
      uint32_t added = new_node(index, offset + position,
                                length - position);
      if (previous == CUI_NONE) {
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
      uint32_t old_offset = index->nodes[current].token_offset;
      uint32_t old_length = index->nodes[current].token_length;
      uint32_t common = 0;
      while (common < old_length && position + common < length &&
             index->tokens[old_offset + common] ==
             index->tokens[offset + position + common])
        common++;
      if (common == old_length) {
        position += common;
        parent = current;
      }
      else {
        uint32_t old_next = index->nodes[current].next_sibling;
        uint32_t split = new_node(index, old_offset, common);
        uint32_t added;
        if (common == 0)
          fatal_error("compact_unit_index: invalid zero-length radix split");
        index->nodes[split].next_sibling = old_next;
        if (previous == CUI_NONE)
          index->nodes[parent].first_child = split;
        else
          index->nodes[previous].next_sibling = split;
        index->nodes[current].token_offset += common;
        index->nodes[current].token_length -= common;
        index->nodes[current].next_sibling = CUI_NONE;
        index->nodes[split].first_child = current;
        position += common;
        if (position == length)
          return split;
        added = new_node(index, offset + position, length - position);
        if (code_compare(first_code(index, added),
                         first_code(index, current)) < 0) {
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

static uint32_t append_tokens(Compact_unit_index index, Topform unit,
                              Term atom,
                              uint32_t *length, uint64_t *symbol_mask)
{
  uint32_t offset = compact_term_pool_intern(
    index->term_pool, unit->id, unit->literals, atom, length);
  uint32_t i;
  index->tokens = compact_term_pool_tokens(index->term_pool);
  *symbol_mask = 0;
  for (i = 0; i < *length; i++) {
    int32_t code = index->tokens[offset + i];
    if (code >= 0)
      *symbol_mask |= symbol_bit((unsigned) code);
  }
  return offset;
}

static void ensure_unifier_symbol(Compact_unit_index index, unsigned symbol)
{
  size_t old_capacity;
  if ((size_t) symbol < index->unifier_symbol_capacity)
    return;
  old_capacity = index->unifier_symbol_capacity;
  while ((size_t) symbol >= index->unifier_symbol_capacity)
    index->unifier_symbol_capacity = grow_capacity(
      index->unifier_symbol_capacity, sizeof(*index->unifier_heads[0]),
      "compact_unit_index: unifier root overflow");
  index->unifier_heads[0] = safe_realloc(
    index->unifier_heads[0], index->unifier_symbol_capacity *
      sizeof(*index->unifier_heads[0]));
  index->unifier_heads[1] = safe_realloc(
    index->unifier_heads[1], index->unifier_symbol_capacity *
      sizeof(*index->unifier_heads[1]));
  memset(index->unifier_heads[0] + old_capacity, 0,
         (index->unifier_symbol_capacity - old_capacity) *
           sizeof(*index->unifier_heads[0]));
  memset(index->unifier_heads[1] + old_capacity, 0,
         (index->unifier_symbol_capacity - old_capacity) *
           sizeof(*index->unifier_heads[1]));
}

static void index_record(Compact_unit_index index, uint32_t record)
{
  struct cui_record *r = &index->records[record];
  uint32_t node = insert_token_path(
    index, index->roots[r->sign ? 1 : 0], r->token_offset, r->token_length);
  uint32_t posting;
  ENSURE_ARRAY(index, postings, posting_count, posting_capacity,
               "compact_unit_index: posting overflow");
  if (index->posting_count > UINT32_MAX)
    fatal_error("compact_unit_index: posting offsets exceed 32 bits");
  posting = (uint32_t) index->posting_count++;
  memset(&index->postings[posting], 0, sizeof(index->postings[posting]));
  index->postings[posting].record = record;
  if (index->nodes[node].first_posting == CUI_NONE)
    index->nodes[node].first_posting = posting;
  else
    index->postings[index->nodes[node].last_posting].next = posting;
  index->nodes[node].last_posting = posting;
}

Compact_unit_index compact_unit_index_init_with_pool(Compact_term_pool pool)
{
  Compact_unit_index index = safe_calloc(1, sizeof(*index));
  if (pool == NULL)
    fatal_error("compact_unit_index_init_with_pool: null term pool");
  index->term_pool = pool;
  index->tokens = compact_term_pool_tokens(pool);
  (void) new_node(index, 0, 0);  /* reserved null node */
  index->roots[0] = new_node(index, 0, 0);
  index->roots[1] = new_node(index, 0, 0);
  ENSURE_ARRAY(index, postings, posting_count, posting_capacity,
               "compact_unit_index: posting overflow");
  memset(&index->postings[0], 0, sizeof(index->postings[0]));
  index->posting_count = 1;
  ENSURE_ARRAY(index, records, record_count, record_capacity,
               "compact_unit_index: record overflow");
  memset(&index->records[0], 0, sizeof(index->records[0]));
  index->record_count = 1;
  update_peak(index);
  return index;
}

Compact_unit_index compact_unit_index_init(void)
{
  Compact_term_pool pool = compact_term_pool_init();
  Compact_unit_index index = compact_unit_index_init_with_pool(pool);
  index->owns_term_pool = TRUE;
  update_peak(index);
  return index;
}

BOOL compact_unit_index_add(Compact_unit_index index, Topform unit)
{
  struct cui_record *record;
  uint32_t at_record;
  size_t at_hash;
  if (index == NULL || unit == NULL || unit->id == 0 ||
      unit->literals == NULL || unit->literals->next != NULL ||
      lookup_record(index, unit->id) != CUI_NONE)
    return FALSE;
  ENSURE_ARRAY(index, records, record_count, record_capacity,
               "compact_unit_index: record overflow");
  if (index->record_count > UINT32_MAX)
    fatal_error("compact_unit_index: record offsets exceed 32 bits");
  at_record = (uint32_t) index->record_count++;
  record = &index->records[at_record];
  memset(record, 0, sizeof(*record));
  record->proof_id = unit->id;
  record->sign = unit->literals->sign;
  record->active = TRUE;
  record->token_offset = append_tokens(index, unit, unit->literals->atom,
                                        &record->token_length,
                                        &record->symbol_mask);
  if (record->token_length == 0 || index->tokens[record->token_offset] < 0)
    fatal_error("compact_unit_index: unit atom has no fixed root");
  {
    unsigned root = (unsigned) index->tokens[record->token_offset];
    ensure_unifier_symbol(index, root);
    record->next_root = index->unifier_heads[record->sign ? 1 : 0][root];
    index->unifier_heads[record->sign ? 1 : 0][root] = at_record;
  }
  index_record(index, at_record);
  ensure_hash(index);
  at_hash = hash_slot(index, unit->id, TRUE);
  if (index->hash_keys[at_hash] == CUI_TOMBSTONE)
    index->hash_tombstones--;
  index->hash_keys[at_hash] = unit->id;
  index->hash_values[at_hash] = at_record;
  index->hash_count++;
  index->active++;
  update_peak(index);
  return TRUE;
}

BOOL compact_unit_index_remove(Compact_unit_index index,
                               unsigned long long proof_id)
{
  uint32_t record = lookup_record(index, proof_id);
  size_t at;
  if (record == CUI_NONE || !index->records[record].active)
    return FALSE;
  index->records[record].active = FALSE;
  at = hash_slot(index, proof_id, FALSE);
  index->hash_keys[at] = CUI_TOMBSTONE;
  index->hash_values[at] = 0;
  index->hash_count--;
  index->hash_tombstones++;
  index->active--;
  index->retired++;
  return TRUE;
}

BOOL compact_unit_index_contains(Compact_unit_index index,
                                 unsigned long long proof_id)
{
  return lookup_record(index, proof_id) != CUI_NONE;
}

static void copy_live_record(Compact_unit_index destination,
                             const struct cui_record *old)
{
  struct cui_record *record;
  uint32_t at_record;
  size_t at_hash;
  ENSURE_ARRAY(destination, records, record_count, record_capacity,
               "compact_unit_index: compacted record overflow");
  if (destination->record_count > UINT32_MAX)
    fatal_error("compact_unit_index: compacted record offsets exceed 32 bits");
  at_record = (uint32_t) destination->record_count++;
  record = &destination->records[at_record];
  *record = *old;
  record->next_root = CUI_NONE;
  {
    unsigned root = (unsigned)
      destination->tokens[record->token_offset];
    ensure_unifier_symbol(destination, root);
    record->next_root =
      destination->unifier_heads[record->sign ? 1 : 0][root];
    destination->unifier_heads[record->sign ? 1 : 0][root] = at_record;
  }
  index_record(destination, at_record);
  ensure_hash(destination);
  at_hash = hash_slot(destination, record->proof_id, TRUE);
  destination->hash_keys[at_hash] = record->proof_id;
  destination->hash_values[at_hash] = at_record;
  destination->hash_count++;
  destination->active++;
  update_peak(destination);
}

BOOL compact_unit_index_compaction_needed(Compact_unit_index index)
{
  unsigned long long physical, stale, threshold;
  if (index == NULL || index->record_count <= 1)
    return FALSE;
  physical = index->record_count - 1;
  stale = physical - index->active;
  threshold = (index->active / 100) * Compaction_stale_pct +
    ((index->active % 100) * Compaction_stale_pct + 99) / 100;
  if (threshold < 1024)
    threshold = 1024;
  return stale >= threshold;
}

void compact_unit_index_set_compaction_stale_pct(unsigned percentage)
{
  if (percentage == 0 || percentage > 1000)
    fatal_error("compact_unit_index: invalid stale percentage");
  Compaction_stale_pct = percentage;
}

static void compact_unit_index_compact_internal(Compact_unit_index index,
                                                BOOL force)
{
  Compact_unit_index replacement;
  struct compact_unit_index old;
  unsigned long long old_bytes, old_peak, old_peak_active;
  unsigned long long retired, compactions, reclaimed;
  unsigned long long generalization_queries, instance_queries;
  unsigned long long instance_exact_tests, unifier_queries;
  unsigned long long unifier_exact_tests;
  size_t i;
  if (index == NULL ||
      (!force && !compact_unit_index_compaction_needed(index)) ||
      (force && index->record_count - 1 == index->active))
    return;
  old_bytes = index_bytes(index);
  old_peak = index->peak_bytes;
  old_peak_active = index->peak;
  retired = index->retired;
  compactions = index->compactions;
  reclaimed = index->bytes_reclaimed;
  generalization_queries = index->generalization_queries;
  instance_queries = index->instance_queries;
  instance_exact_tests = index->instance_exact_tests;
  unifier_queries = index->unifier_queries;
  unifier_exact_tests = index->unifier_exact_tests;
  replacement = compact_unit_index_init_with_pool(index->term_pool);
  replacement->tokens = compact_term_pool_tokens(index->term_pool);
  for (i = 1; i < index->record_count; i++)
    if (index->records[i].active)
      copy_live_record(replacement, &index->records[i]);
  old = *index;
  *index = *replacement;
  safe_free(replacement);
  safe_free(old.nodes);
  safe_free(old.postings);
  safe_free(old.records);
  safe_free(old.unifier_heads[0]);
  safe_free(old.unifier_heads[1]);
  safe_free(old.hash_keys);
  safe_free(old.hash_values);
  safe_free(old.query);
  safe_free(old.result_ids);
  index->retired = retired;
  index->compactions = compactions + 1;
  index->bytes_reclaimed = reclaimed +
    (old_bytes > index_bytes(index) ? old_bytes - index_bytes(index) : 0);
  index->generalization_queries = generalization_queries;
  index->instance_queries = instance_queries;
  index->instance_exact_tests = instance_exact_tests;
  index->unifier_queries = unifier_queries;
  index->unifier_exact_tests = unifier_exact_tests;
  if (old_peak > index->peak_bytes)
    index->peak_bytes = old_peak;
  if (old_peak_active > index->peak)
    index->peak = old_peak_active;
}

void compact_unit_index_compact(Compact_unit_index index)
{
  compact_unit_index_compact_internal(index, FALSE);
}

void compact_unit_index_compact_all_stale(Compact_unit_index index)
{
  compact_unit_index_compact_internal(index, TRUE);
}

void compact_unit_index_copy_live_clauses(Compact_unit_index index,
                                          Compact_term_pool destination,
                                          Compact_term_rebase_map map)
{
  size_t i;
  if (index == NULL)
    return;
  for (i = 1; i < index->record_count; i++)
    if (!compact_term_pool_copy_clause(
          destination, index->term_pool, map,
          index->records[i].proof_id))
      fatal_error("compact_unit_index: cannot copy compacted pool clause");
}

void compact_unit_index_retain_live_clauses(Compact_unit_index index,
                                            Compact_term_rebase_map map)
{
  size_t i;
  if (index == NULL)
    return;
  for (i = 1; i < index->record_count; i++)
    if (!compact_term_rebase_map_retain_clause(
          map, index->term_pool, index->records[i].proof_id))
      fatal_error("compact_unit_index: cannot retain term-pool clause");
}

void compact_unit_index_rebase_term_pool(Compact_unit_index index,
                                         Compact_term_pool pool,
                                         Compact_term_rebase_map map)
{
  size_t i;
  if (index == NULL)
    return;
  for (i = 1; i < index->record_count; i++)
    index->records[i].token_offset = compact_term_rebase_offset(
      map, index->records[i].token_offset);
  for (i = 1; i < index->node_count; i++)
    if (index->nodes[i].token_length != 0)
      index->nodes[i].token_offset = compact_term_rebase_offset(
        map, index->nodes[i].token_offset);
  index->term_pool = pool;
  index->tokens = compact_term_pool_tokens(pool);
  update_peak(index);
}

static void flatten_query(Compact_unit_index index, Term term,
                          size_t *count)
{
  size_t at;
  int i;
  if (*count == index->query_capacity) {
    index->query_capacity = grow_capacity(
      index->query_capacity, sizeof(*index->query),
      "compact_unit_index: query overflow");
    index->query = safe_realloc(
      index->query, index->query_capacity * sizeof(*index->query));
  }
  at = (*count)++;
  index->query[at].term = term;
  for (i = 0; i < ARITY(term); i++)
    flatten_query(index, ARG(term, i), count);
  if (*count > UINT32_MAX)
    fatal_error("compact_unit_index: query offsets exceed 32 bits");
  index->query[at].end = (uint32_t) *count;
}

static BOOL match_generalization_edge(
  Compact_unit_index index, uint32_t node, uint32_t position, uint32_t end,
  Term *bindings, unsigned *new_bindings, unsigned *new_count,
  uint32_t *next_position)
{
  struct cui_node *edge = &index->nodes[node];
  uint32_t i;
  *new_count = 0;
  for (i = 0; i < edge->token_length; i++) {
    int32_t code = index->tokens[edge->token_offset + i];
    Term query_term;
    if (position >= end)
      return FALSE;
    query_term = index->query[position].term;
    if (code < 0) {
      unsigned variable = (unsigned) (-code - 1);
      if (variable >= MAX_VARS)
        fatal_error("compact_unit_index: variable exceeds MAX_VARS");
      if (bindings[variable] == NULL) {
        bindings[variable] = query_term;
        new_bindings[(*new_count)++] = variable;
      }
      else if (!term_ident(bindings[variable], query_term))
        return FALSE;
      position = index->query[position].end;
    }
    else {
      if (VARIABLE(query_term) || SYMNUM(query_term) != code)
        return FALSE;
      position++;
    }
  }
  *next_position = position;
  return TRUE;
}

static void undo_generalization_bindings(Term *bindings,
                                         const unsigned *new_bindings,
                                         unsigned new_count)
{
  while (new_count != 0)
    bindings[new_bindings[--new_count]] = NULL;
}

static unsigned long long generalization_rec(
  Compact_unit_index index, uint32_t node, uint32_t position,
  uint32_t end, Term *bindings, unsigned long long exclude_id)
{
  uint32_t child;
  if (position == end) {
    uint32_t posting;
    for (posting = index->nodes[node].first_posting;
         posting != CUI_NONE; posting = index->postings[posting].next) {
      struct cui_record *record =
        &index->records[index->postings[posting].record];
      if (record->active && record->proof_id != exclude_id)
        return record->proof_id;
    }
    return 0;
  }
  for (child = index->nodes[node].first_child; child != CUI_NONE;
       child = index->nodes[child].next_sibling) {
    unsigned new_bindings[MAX_VARS];
    unsigned new_count = 0;
    uint32_t next_position = position;
    unsigned long long found = 0;
    if (match_generalization_edge(index, child, position, end, bindings,
                                  new_bindings, &new_count,
                                  &next_position))
      found = generalization_rec(index, child, next_position, end,
                                 bindings, exclude_id);
    undo_generalization_bindings(bindings, new_bindings, new_count);
    if (found != 0)
      return found;
  }
  return 0;
}

unsigned long long compact_unit_generalization_first(
  Compact_unit_index index, Term target, BOOL sign,
  unsigned long long exclude_id)
{
  size_t count = 0;
  Term bindings[MAX_VARS];
  if (index == NULL || target == NULL)
    return 0;
  index->tokens = compact_term_pool_tokens(index->term_pool);
  index->generalization_queries++;
  memset(bindings, 0, sizeof(bindings));
  flatten_query(index, target, &count);
  {
    unsigned long long result = count == 0 ? 0 : generalization_rec(
      index, index->roots[sign ? 1 : 0], 0, (uint32_t) count,
      bindings, exclude_id);
    update_peak(index);
    return result;
  }
}

static uint32_t token_term_end(Compact_unit_index index, uint32_t position,
                               uint32_t end)
{
  int32_t code;
  int i;
  if (position >= end)
    return UINT32_MAX;
  code = index->tokens[position++];
  if (code < 0)
    return position;
  for (i = 0; i < sn_to_arity(code); i++) {
    position = token_term_end(index, position, end);
    if (position == UINT32_MAX)
      return UINT32_MAX;
  }
  return position;
}

static BOOL pattern_matches_tokens(Compact_unit_index index, Term pattern,
                                   uint32_t *position, uint32_t end,
                                   uint32_t *starts, uint32_t *ends)
{
  int32_t code;
  int i;
  if (*position >= end)
    return FALSE;
  if (VARIABLE(pattern)) {
    unsigned variable = (unsigned) VARNUM(pattern);
    uint32_t start = *position;
    uint32_t finish = token_term_end(index, start, end);
    if (variable >= MAX_VARS || finish == UINT32_MAX)
      return FALSE;
    if (starts[variable] == UINT32_MAX) {
      starts[variable] = start;
      ends[variable] = finish;
    }
    else {
      size_t old_length = ends[variable] - starts[variable];
      size_t new_length = finish - start;
      if (old_length != new_length ||
          memcmp(index->tokens + starts[variable], index->tokens + start,
                 new_length * sizeof(*index->tokens)) != 0)
        return FALSE;
    }
    *position = finish;
    return TRUE;
  }
  code = index->tokens[(*position)++];
  if (code < 0 || code != SYMNUM(pattern))
    return FALSE;
  for (i = 0; i < ARITY(pattern); i++)
    if (!pattern_matches_tokens(index, ARG(pattern, i), position, end,
                                starts, ends))
      return FALSE;
  return TRUE;
}

static uint64_t resident_symbol_mask(Term term)
{
  uint64_t mask = 0;
  int i;
  if (!VARIABLE(term)) {
    mask |= symbol_bit((unsigned) SYMNUM(term));
    for (i = 0; i < ARITY(term); i++)
      mask |= resident_symbol_mask(ARG(term, i));
  }
  return mask;
}

static int descending_id_compare(const void *a, const void *b)
{
  unsigned long long x = *(const unsigned long long *) a;
  unsigned long long y = *(const unsigned long long *) b;
  return x < y ? 1 : x > y ? -1 : 0;
}

unsigned long long *compact_unit_instance_ids(
  Compact_unit_index index, Term pattern, BOOL sign,
  unsigned long long exclude_id, size_t *count)
{
  uint64_t wanted;
  size_t found = 0;
  size_t i;
  if (count == NULL)
    return NULL;
  *count = 0;
  if (index == NULL || pattern == NULL)
    return NULL;
  index->tokens = compact_term_pool_tokens(index->term_pool);
  index->instance_queries++;
  wanted = resident_symbol_mask(pattern);
  for (i = 1; i < index->record_count; i++) {
    struct cui_record *record = &index->records[i];
    uint32_t position;
    uint32_t starts[MAX_VARS], ends[MAX_VARS];
    unsigned j;
    if (!record->active || record->sign != (unsigned char) sign ||
        record->proof_id == exclude_id ||
        (record->symbol_mask & wanted) != wanted)
      continue;
    index->instance_exact_tests++;
    for (j = 0; j < MAX_VARS; j++)
      starts[j] = UINT32_MAX;
    position = record->token_offset;
    if (pattern_matches_tokens(index, pattern, &position,
                               record->token_offset + record->token_length,
                               starts, ends) &&
        position == record->token_offset + record->token_length) {
      if (found == index->result_capacity) {
        index->result_capacity = grow_capacity(
          index->result_capacity, sizeof(*index->result_ids),
          "compact_unit_index: result overflow");
        index->result_ids = safe_realloc(
          index->result_ids,
          index->result_capacity * sizeof(*index->result_ids));
      }
      index->result_ids[found++] = record->proof_id;
    }
  }
  if (found == 0)
    return NULL;
  qsort(index->result_ids, found, sizeof(*index->result_ids),
        descending_id_compare);
  {
    unsigned long long *result = safe_malloc(found * sizeof(*result));
    memcpy(result, index->result_ids, found * sizeof(*result));
    *count = found;
    update_peak(index);
    return result;
  }
}

struct cui_expr {
  BOOL token;
  union {
    Term resident;
    uint32_t position;
  } value;
  uint32_t token_end;
};

struct cui_unify_state {
  Compact_unit_index index;
  struct cui_expr resident_bindings[MAX_VARS];
  struct cui_expr token_bindings[MAX_VARS];
  BOOL resident_bound[MAX_VARS];
  BOOL token_bound[MAX_VARS];
};

static BOOL expr_variable(struct cui_unify_state *state,
                          struct cui_expr expr, unsigned *variable)
{
  if (expr.token) {
    int32_t code;
    if (expr.value.position >= expr.token_end)
      return FALSE;
    code = state->index->tokens[expr.value.position];
    if (code >= 0)
      return FALSE;
    *variable = (unsigned) (-code - 1);
    return TRUE;
  }
  if (!VARIABLE(expr.value.resident))
    return FALSE;
  *variable = (unsigned) VARNUM(expr.value.resident);
  return TRUE;
}

static struct cui_expr dereference_expr(struct cui_unify_state *state,
                                        struct cui_expr expr)
{
  unsigned variable;
  unsigned guard = 0;
  while (expr_variable(state, expr, &variable)) {
    if (variable >= MAX_VARS)
      return expr;
    if (expr.token) {
      if (!state->token_bound[variable])
        return expr;
      expr = state->token_bindings[variable];
    }
    else {
      if (!state->resident_bound[variable])
        return expr;
      expr = state->resident_bindings[variable];
    }
    if (++guard > MAX_VARS * 2)
      fatal_error("compact_unit_index: cyclic unification binding");
  }
  return expr;
}

static int expr_symbol(struct cui_unify_state *state, struct cui_expr expr)
{
  return expr.token ? state->index->tokens[expr.value.position] :
                      SYMNUM(expr.value.resident);
}

static int expr_arity(struct cui_unify_state *state, struct cui_expr expr)
{
  return expr.token ? sn_to_arity(expr_symbol(state, expr)) :
                      ARITY(expr.value.resident);
}

static struct cui_expr expr_child(struct cui_unify_state *state,
                                  struct cui_expr expr, int child)
{
  if (!expr.token) {
    expr.value.resident = ARG(expr.value.resident, child);
    return expr;
  }
  else {
    int i;
    uint32_t position = expr.value.position + 1;
    for (i = 0; i < child; i++)
      position = token_term_end(state->index, position, expr.token_end);
    expr.value.position = position;
    return expr;
  }
}

static BOOL expr_same_variable(struct cui_unify_state *state,
                               struct cui_expr a, struct cui_expr b)
{
  unsigned av, bv;
  return a.token == b.token && expr_variable(state, a, &av) &&
    expr_variable(state, b, &bv) && av == bv;
}

static BOOL expr_occurs(struct cui_unify_state *state, BOOL token_variable,
                        unsigned variable, struct cui_expr expr)
{
  unsigned other;
  int i, arity;
  expr = dereference_expr(state, expr);
  if (expr_variable(state, expr, &other))
    return expr.token == token_variable && other == variable;
  arity = expr_arity(state, expr);
  for (i = 0; i < arity; i++)
    if (expr_occurs(state, token_variable, variable,
                    expr_child(state, expr, i)))
      return TRUE;
  return FALSE;
}

static BOOL bind_expr_variable(struct cui_unify_state *state,
                               struct cui_expr variable_expr,
                               struct cui_expr value)
{
  unsigned variable;
  if (!expr_variable(state, variable_expr, &variable) ||
      variable >= MAX_VARS)
    return FALSE;
  if (expr_occurs(state, variable_expr.token, variable, value))
    return FALSE;
  if (variable_expr.token) {
    state->token_bound[variable] = TRUE;
    state->token_bindings[variable] = value;
  }
  else {
    state->resident_bound[variable] = TRUE;
    state->resident_bindings[variable] = value;
  }
  return TRUE;
}

static BOOL unify_exprs(struct cui_unify_state *state, struct cui_expr a,
                        struct cui_expr b)
{
  unsigned variable;
  int i, arity;
  a = dereference_expr(state, a);
  b = dereference_expr(state, b);
  if (expr_same_variable(state, a, b))
    return TRUE;
  if (expr_variable(state, a, &variable))
    return bind_expr_variable(state, a, b);
  if (expr_variable(state, b, &variable))
    return bind_expr_variable(state, b, a);
  if (expr_symbol(state, a) != expr_symbol(state, b) ||
      expr_arity(state, a) != expr_arity(state, b))
    return FALSE;
  arity = expr_arity(state, a);
  for (i = 0; i < arity; i++)
    if (!unify_exprs(state, expr_child(state, a, i),
                     expr_child(state, b, i)))
      return FALSE;
  return TRUE;
}

static BOOL resident_unifies_record(Compact_unit_index index, Term query,
                                    struct cui_record *record)
{
  struct cui_unify_state state;
  struct cui_expr resident, token;
  memset(&state, 0, sizeof(state));
  state.index = index;
  resident.token = FALSE;
  resident.value.resident = query;
  resident.token_end = 0;
  token.token = TRUE;
  token.value.position = record->token_offset;
  token.token_end = record->token_offset + record->token_length;
  return unify_exprs(&state, resident, token);
}

unsigned long long *compact_unit_unifier_ids(
  Compact_unit_index index, Term query, BOOL sign,
  unsigned long long exclude_id, size_t *count)
{
  size_t found = 0;
  uint32_t i;
  int query_root;
  if (count == NULL)
    return NULL;
  *count = 0;
  if (index == NULL || query == NULL || VARIABLE(query))
    return NULL;
  index->tokens = compact_term_pool_tokens(index->term_pool);
  index->unifier_queries++;
  query_root = SYMNUM(query);
  if ((size_t) query_root >= index->unifier_symbol_capacity)
    return NULL;
  for (i = index->unifier_heads[sign ? 1 : 0][query_root];
       i != CUI_NONE; i = index->records[i].next_root) {
    struct cui_record *record = &index->records[i];
    if (!record->active || record->sign != (unsigned char) sign ||
        record->proof_id == exclude_id || record->token_length == 0 ||
        index->tokens[record->token_offset] != query_root)
      continue;
    index->unifier_exact_tests++;
    if (resident_unifies_record(index, query, record)) {
      if (found == index->result_capacity) {
        index->result_capacity = grow_capacity(
          index->result_capacity, sizeof(*index->result_ids),
          "compact_unit_index: result overflow");
        index->result_ids = safe_realloc(
          index->result_ids,
          index->result_capacity * sizeof(*index->result_ids));
      }
      index->result_ids[found++] = record->proof_id;
    }
  }
  if (found == 0) {
    update_peak(index);
    return NULL;
  }
  qsort(index->result_ids, found, sizeof(*index->result_ids),
        descending_id_compare);
  {
    unsigned long long *result = safe_malloc(found * sizeof(*result));
    memcpy(result, index->result_ids, found * sizeof(*result));
    *count = found;
    update_peak(index);
    return result;
  }
}

void compact_unit_index_get_stats(Compact_unit_index index,
                                  struct compact_unit_index_stats *stats)
{
  struct compact_term_pool_stats terms;
  if (stats == NULL)
    return;
  memset(stats, 0, sizeof(*stats));
  if (index == NULL)
    return;
  compact_term_pool_get_stats(index->term_pool, &terms);
  stats->active = index->active;
  stats->peak = index->peak;
  stats->retired = index->retired;
  stats->physical = index->record_count - 1;
  stats->compactions = index->compactions;
  stats->bytes_reclaimed = index->bytes_reclaimed;
  stats->generalization_queries = index->generalization_queries;
  stats->instance_queries = index->instance_queries;
  stats->instance_exact_tests = index->instance_exact_tests;
  stats->unifier_queries = index->unifier_queries;
  stats->unifier_exact_tests = index->unifier_exact_tests;
  stats->node_items = index->node_count;
  stats->posting_items = index->posting_count;
  stats->node_bytes = index->node_capacity * sizeof(*index->nodes);
  stats->posting_bytes = index->posting_capacity * sizeof(*index->postings);
  stats->record_bytes = index->record_capacity * sizeof(*index->records);
  stats->root_bytes = index->unifier_symbol_capacity *
    (sizeof(*index->unifier_heads[0]) +
     sizeof(*index->unifier_heads[1]));
  stats->token_bytes = index->owns_term_pool ? terms.token_bytes : 0;
  stats->hash_bytes = index->hash_capacity *
    (sizeof(*index->hash_keys) + sizeof(*index->hash_values));
  stats->scratch_bytes =
    index->query_capacity * sizeof(*index->query) +
    index->result_capacity * sizeof(*index->result_ids);
  stats->total_bytes = index_bytes(index);
  stats->peak_bytes = index->peak_bytes;
}

void compact_unit_index_free(Compact_unit_index index)
{
  if (index == NULL)
    return;
  safe_free(index->nodes);
  safe_free(index->postings);
  safe_free(index->records);
  safe_free(index->unifier_heads[0]);
  safe_free(index->unifier_heads[1]);
  if (index->owns_term_pool)
    compact_term_pool_free(index->term_pool);
  safe_free(index->hash_keys);
  safe_free(index->hash_values);
  safe_free(index->query);
  safe_free(index->result_ids);
  safe_free(index);
}
