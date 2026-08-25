#include "compact_unit_index.h"
#include "compact_id_map.h"

#include <stdint.h>
#include <string.h>

#define CUI_NONE 0U
#define CUI_GROWTH_STALE_FLOOR 16384ULL
#define CUI_ADAPTIVE_ROUTE_CAPACITY 65536U
#define CUI_ADAPTIVE_POSITION_FACTOR 12ULL
#define CUI_ADAPTIVE_EMPTY_TREE_FLOOR 8ULL
#define CUI_FEATURE_CHUNK_DATA 248U

static unsigned Compaction_stale_pct = 25;
static Compact_unit_strategy Unit_strategy = COMPACT_UNIT_ROOT_SCAN;
static unsigned Unit_feature_depth = 0;

struct cui_node {
  Compact_term_slice tokens;
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
  Compact_term_slice tokens;
  uint32_t next_root;
  uint32_t query_stamp;
  unsigned char sign;
  unsigned char active;
  uint32_t root_symbol;
};

typedef char compact_unit_node_must_remain_24_bytes[
  sizeof(struct cui_node) == 24 ? 1 : -1];
typedef char compact_unit_record_must_remain_40_bytes[
  sizeof(struct cui_record) == 40 ? 1 : -1];

struct cui_feature_bucket {
  uint64_t key;
  uint32_t first_chunk;
  uint32_t last_chunk;
  uint32_t count;
  uint32_t last_record;
};

struct cui_feature_chunk {
  uint32_t next;
  uint16_t used;
  uint16_t reserved;
  unsigned char data[CUI_FEATURE_CHUNK_DATA];
};

typedef char compact_unit_feature_chunk_must_remain_256_bytes[
  sizeof(struct cui_feature_chunk) == 256 ? 1 : -1];

struct cui_query_term {
  Term term;
  uint32_t end;
};

/* A generalization edge can bind each stored-pattern variable at most once
   before recursion.  Remember that exact set in two 64-bit words instead of
   keeping a 100-element variable-number array in every recursive frame. */
struct cui_new_bindings {
  uint64_t low;
  uint64_t high;
};

typedef char cui_new_bindings_cover_max_vars[
  MAX_VARS <= 128 ? 1 : -1];

/* Explicit depth-first-search state for generalization lookup.  Each frame
   owns exactly the bindings introduced by the edge from its parent, so
   popping a failed branch restores the same state as recursive unwinding. */
struct cui_generalization_frame {
  uint32_t node;
  uint32_t position;
  uint32_t next_child;
  int32_t wanted;
  struct cui_new_bindings new_bindings;
};

typedef char cui_generalization_frame_must_remain_32_bytes[
  sizeof(struct cui_generalization_frame) == 32 ? 1 : -1];

struct cui_adaptive_route {
  uint64_t key;
  unsigned long long tree_nodes;
};

struct compact_unit_index {
  Compact_unit_strategy strategy;
  unsigned feature_depth;
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
  struct cui_feature_bucket *feature_buckets;
  size_t feature_bucket_count;
  size_t feature_bucket_capacity;
  uint32_t *feature_hash;
  size_t feature_hash_capacity;
  struct cui_feature_chunk *feature_chunks;
  size_t feature_chunk_count;
  size_t feature_chunk_capacity;
  size_t feature_posting_count;
  Compact_term_pool term_pool;
  const int32_t *query_token_base;
  unsigned long long query_logical_base;
  BOOL owns_term_pool;
  Compact_id_map id_map;
  struct cui_query_term *query;
  size_t query_capacity;
  struct cui_generalization_frame *generalization_stack;
  size_t generalization_stack_capacity;
  unsigned long long *result_ids;
  size_t result_capacity;
  uint64_t *path_stack;
  size_t path_capacity;
  unsigned *child_stack;
  size_t child_capacity;
  unsigned *selected_path;
  size_t selected_path_capacity;
  unsigned *refinement_path;
  size_t refinement_path_capacity;
  uint64_t *selected_variable_keys;
  size_t selected_variable_capacity;
  uint64_t *first_variable_keys;
  size_t first_variable_capacity;
  struct cui_adaptive_route *adaptive_routes;
  uint32_t query_stamp;
  unsigned long long active;
  unsigned long long peak;
  unsigned long long retired;
  unsigned long long compactions;
  unsigned long long bytes_reclaimed;
  unsigned long long generalization_queries;
  unsigned long long instance_queries;
  unsigned long long instance_exact_tests;
  unsigned long long instance_tree_queries;
  unsigned long long instance_tree_nodes_examined;
  unsigned long long instance_tree_postings_examined;
  unsigned long long unifier_queries;
  unsigned long long unifier_exact_tests;
  unsigned long long position_queries;
  unsigned long long position_fallback_queries;
  unsigned long long position_postings_examined;
  unsigned long long position_duplicate_postings;
  unsigned long long code_tree_queries;
  unsigned long long code_tree_nodes_examined;
  unsigned long long code_tree_postings_examined;
  unsigned long long code_tree_variable_parents;
  unsigned long long code_tree_variable_children;
  unsigned long long code_tree_pending_parents;
  unsigned long long code_tree_pending_children;
  unsigned long long code_tree_rigid_parents;
  unsigned long long code_tree_rigid_children;
  unsigned long long code_tree_rigid_sibling_checks;
  unsigned long long adaptive_queries;
  unsigned long long adaptive_tree_choices;
  unsigned long long adaptive_position_choices;
  unsigned long long adaptive_position_empty_choices;
  unsigned long long position_refinement_queries;
  unsigned long long position_refinement_checks;
  unsigned long long position_refinement_rejects;
  unsigned long long position_tertiary_queries;
  unsigned long long position_tertiary_checks;
  unsigned long long position_tertiary_rejects;
  unsigned long long adaptive_route_hits;
  unsigned long long adaptive_route_misses;
  unsigned long long adaptive_route_replacements;
  struct compact_query_profile generalization_profile;
  struct compact_query_profile instance_profile;
  struct compact_query_profile unifier_profile;
  struct compact_query_timer generalization_timer;
  struct compact_query_timer instance_timer;
  struct compact_query_timer unifier_timer;
  Clock sort_clock;
  Clock maintenance_clock;
  unsigned long long peak_bytes;
};

struct cui_query_work {
  unsigned long long nodes;
  unsigned long long postings;
  unsigned long long live;
  unsigned long long dead;
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

static uint64_t symbol_bit(unsigned symbol)
{
  return UINT64_C(1) << ((symbol * UINT32_C(2654435761)) >> 26);
}

static uint64_t mix64(uint64_t value)
{
  value ^= value >> 30;
  value *= UINT64_C(0xbf58476d1ce4e5b9);
  value ^= value >> 27;
  value *= UINT64_C(0x94d049bb133111eb);
  value ^= value >> 31;
  return value;
}

static uint64_t child_path(uint64_t parent, unsigned child)
{
  return mix64(parent ^ (UINT64_C(0x9e3779b97f4a7c15) + child));
}

/* Hash collisions only merge posting lists and therefore add candidates; the
   direct compressed unifier remains authoritative and prevents false answers. */
static uint64_t exact_feature_key(uint64_t path, unsigned symbol, BOOL sign)
{
  uint64_t key = mix64(path ^ (UINT64_C(0x4558414354) << 16) ^
                       ((uint64_t) symbol * UINT64_C(0x9e3779b97f4a7c15)) ^
                       (sign ? UINT64_C(0x7369676e) : 0));
  return key == 0 ? 1 : key;
}

static uint64_t variable_feature_key(uint64_t path, BOOL sign)
{
  uint64_t key = mix64(path ^ (UINT64_C(0x5641524941424c45)) ^
                       (sign ? UINT64_C(0x7369676e) : 0));
  return key == 0 ? 1 : key;
}

static size_t feature_hash_slot(Compact_unit_index index, uint64_t key)
{
  size_t at = (size_t) mix64(key) & (index->feature_hash_capacity - 1);
  for (;;) {
    uint32_t bucket = index->feature_hash[at];
    if (bucket == CUI_NONE || index->feature_buckets[bucket].key == key)
      return at;
    at = (at + 1) & (index->feature_hash_capacity - 1);
  }
}

static void rehash_features(Compact_unit_index index, size_t capacity)
{
  uint32_t *old = index->feature_hash;
  size_t i;
  index->feature_hash = safe_calloc(capacity, sizeof(*index->feature_hash));
  index->feature_hash_capacity = capacity;
  for (i = 1; i < index->feature_bucket_count; i++) {
    size_t at = feature_hash_slot(index, index->feature_buckets[i].key);
    index->feature_hash[at] = (uint32_t) i;
  }
  safe_free(old);
}

static uint32_t find_feature_bucket(Compact_unit_index index, uint64_t key)
{
  size_t at;
  if (index->feature_hash_capacity == 0)
    return CUI_NONE;
  at = feature_hash_slot(index, key);
  return index->feature_hash[at];
}

static uint32_t find_or_add_feature_bucket(Compact_unit_index index,
                                           uint64_t key)
{
  size_t at;
  uint32_t bucket;
  if (index->feature_hash_capacity == 0)
    rehash_features(index, 128);
  else if ((index->feature_bucket_count + 1) * 20 >=
           index->feature_hash_capacity * 17) {
    if (index->feature_hash_capacity > SIZE_MAX / 2)
      fatal_error("compact_unit_index: feature hash overflow");
    rehash_features(index, index->feature_hash_capacity * 2);
  }
  at = feature_hash_slot(index, key);
  bucket = index->feature_hash[at];
  if (bucket != CUI_NONE)
    return bucket;
  if (index->feature_bucket_count == index->feature_bucket_capacity) {
    index->feature_bucket_capacity = grow_dense_capacity(
      index->feature_bucket_capacity, sizeof(*index->feature_buckets),
      "compact_unit_index: feature bucket overflow");
    index->feature_buckets = safe_realloc(
      index->feature_buckets,
      index->feature_bucket_capacity * sizeof(*index->feature_buckets));
  }
  if (index->feature_bucket_count > UINT32_MAX)
    fatal_error("compact_unit_index: feature bucket offsets exceed 32 bits");
  bucket = (uint32_t) index->feature_bucket_count++;
  memset(&index->feature_buckets[bucket], 0,
         sizeof(index->feature_buckets[bucket]));
  index->feature_buckets[bucket].key = key;
  index->feature_hash[at] = bucket;
  return bucket;
}

static void append_feature_posting(Compact_unit_index index, uint64_t key,
                                   uint32_t record)
{
  uint32_t bucket = find_or_add_feature_bucket(index, key);
  uint32_t delta;
  unsigned char encoded[5];
  size_t encoded_length = 0;
  uint32_t chunk;
  struct cui_feature_bucket *b = &index->feature_buckets[bucket];
  if (b->count != 0 && record <= b->last_record)
    fatal_error("compact_unit_index: feature postings are not monotone");
  delta = b->count == 0 ? record : record - b->last_record;
  do {
    unsigned char byte = (unsigned char) (delta & 0x7fU);
    delta >>= 7;
    if (delta != 0)
      byte |= 0x80U;
    encoded[encoded_length++] = byte;
  } while (delta != 0);
  chunk = b->last_chunk;
  if (chunk == CUI_NONE ||
      index->feature_chunks[chunk].used + encoded_length >
        CUI_FEATURE_CHUNK_DATA) {
    uint32_t added;
    if (index->feature_chunk_count == index->feature_chunk_capacity) {
      index->feature_chunk_capacity = grow_dense_capacity(
        index->feature_chunk_capacity, sizeof(*index->feature_chunks),
        "compact_unit_index: feature chunk overflow");
      index->feature_chunks = safe_realloc(
        index->feature_chunks,
        index->feature_chunk_capacity * sizeof(*index->feature_chunks));
    }
    if (index->feature_chunk_count > UINT32_MAX)
      fatal_error("compact_unit_index: feature chunk offsets exceed 32 bits");
    added = (uint32_t) index->feature_chunk_count++;
    memset(&index->feature_chunks[added], 0,
           sizeof(index->feature_chunks[added]));
    if (b->first_chunk == CUI_NONE)
      b->first_chunk = added;
    else
      index->feature_chunks[b->last_chunk].next = added;
    b->last_chunk = added;
    chunk = added;
  }
  memcpy(index->feature_chunks[chunk].data +
           index->feature_chunks[chunk].used,
         encoded, encoded_length);
  index->feature_chunks[chunk].used += (uint16_t) encoded_length;
  if (index->feature_posting_count == SIZE_MAX || b->count == UINT32_MAX)
    fatal_error("compact_unit_index: feature postings exceed 32 bits");
  index->feature_posting_count++;
  b->count++;
  b->last_record = record;
}

static uint32_t index_token_features(Compact_unit_index index,
                                     const int32_t *tokens,
                                     uint32_t record, uint32_t position,
                                     uint32_t end, uint64_t path,
                                     unsigned depth)
{
  int32_t code;
  int i, arity;
  if (position >= end)
    fatal_error("compact_unit_index: truncated feature term");
  code = tokens[position++];
  if (code < 0) {
    if (depth != 0 &&
        (index->feature_depth == 0 || depth <= index->feature_depth))
      append_feature_posting(index,
                             variable_feature_key(
                               path, index->records[record].sign),
                             record);
    return position;
  }
  if (depth != 0 &&
      (index->feature_depth == 0 || depth <= index->feature_depth))
    append_feature_posting(index,
                           exact_feature_key(path, (unsigned) code,
                                             index->records[record].sign),
                           record);
  arity = sn_to_arity(code);
  for (i = 0; i < arity; i++)
    position = index_token_features(index, tokens, record, position, end,
                                    child_path(path, (unsigned) i),
                                    depth + 1);
  return position;
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
    index->feature_bucket_capacity * sizeof(*index->feature_buckets) +
    index->feature_hash_capacity * sizeof(*index->feature_hash) +
    index->feature_chunk_capacity * sizeof(*index->feature_chunks) +
    (index->owns_term_pool ? terms.total_bytes : 0) +
    compact_id_map_bytes(index->id_map) +
    index->query_capacity * sizeof(*index->query) +
    index->generalization_stack_capacity *
      sizeof(*index->generalization_stack) +
    index->result_capacity * sizeof(*index->result_ids) +
    index->path_capacity * sizeof(*index->path_stack) +
    index->child_capacity * sizeof(*index->child_stack) +
    index->selected_path_capacity * sizeof(*index->selected_path) +
    index->refinement_path_capacity * sizeof(*index->refinement_path) +
    index->selected_variable_capacity *
      sizeof(*index->selected_variable_keys) +
    index->first_variable_capacity *
      sizeof(*index->first_variable_keys) +
    (index->adaptive_routes == NULL ? 0 :
     CUI_ADAPTIVE_ROUTE_CAPACITY * sizeof(*index->adaptive_routes));
}

static void update_peak(Compact_unit_index index)
{
  unsigned long long bytes = index_bytes(index);
  if (bytes > index->peak_bytes)
    index->peak_bytes = bytes;
  if (index->active > index->peak)
    index->peak = index->active;
}

/* A compact term pool cannot grow or rebase while an index query is active.
   Resolve its storage once per public query so the node hot paths only decode
   the packed offset and length instead of calling the validating public slice
   resolver for every visited radix edge. */
static void prepare_query_term_access(Compact_unit_index index)
{
  index->query_token_base = compact_term_pool_tokens(index->term_pool);
  index->query_logical_base =
    compact_term_pool_logical_base(index->term_pool);
}

static inline uint32_t query_slice_length(Compact_term_slice slice)
{
  return (uint32_t) (slice >> COMPACT_TERM_SLICE_OFFSET_BITS);
}

static inline const int32_t *query_slice_tokens(
  Compact_unit_index index, Compact_term_slice slice)
{
  unsigned long long offset = slice & COMPACT_TERM_SLICE_OFFSET_MAX;
  return index->query_token_base +
    (size_t) (offset - index->query_logical_base);
}

static inline int32_t query_first_code(Compact_unit_index index,
                                       uint32_t node)
{
  return query_slice_tokens(index, index->nodes[node].tokens)[0];
}

#define ENSURE_ARRAY(index, field, count, capacity, message) do {       \
  if ((index)->count == (index)->capacity) {                            \
    (index)->capacity = grow_dense_capacity((index)->capacity,          \
      sizeof(*(index)->field), (message));                              \
    (index)->field = safe_realloc((index)->field,                       \
      (index)->capacity * sizeof(*(index)->field));                     \
  }                                                                    \
} while (0)

static uint32_t lookup_record(Compact_unit_index index,
                              unsigned long long proof_id)
{
  uint32_t value = CUI_NONE;
  return index != NULL &&
    compact_id_map_get(index->id_map, proof_id, &value) ? value : CUI_NONE;
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
                         Compact_term_slice tokens)
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
  index->nodes[node].tokens = tokens;
  return node;
}

static int32_t first_code(Compact_unit_index index, uint32_t node)
{
  struct cui_node *n = &index->nodes[node];
  if (compact_term_slice_length(n->tokens) == 0)
    fatal_error("compact_unit_index: empty nonroot radix edge");
  return compact_term_pool_slice_tokens(index->term_pool, n->tokens)[0];
}

static uint32_t insert_token_path(Compact_unit_index index, uint32_t root,
                                  Compact_term_slice slice)
{
  uint32_t parent = root;
  uint32_t position = 0;
  uint32_t length = compact_term_slice_length(slice);
  const int32_t *tokens = compact_term_pool_slice_tokens(
    index->term_pool, slice);
  while (position < length) {
    uint32_t current = index->nodes[parent].first_child;
    uint32_t previous = CUI_NONE;
    int32_t wanted = tokens[position];
    while (current != CUI_NONE &&
           code_compare(first_code(index, current), wanted) < 0) {
      previous = current;
      current = index->nodes[current].next_sibling;
    }
    if (current == CUI_NONE || first_code(index, current) != wanted) {
      Compact_term_slice suffix;
      uint32_t added;
      if (!compact_term_slice_subslice(
            slice, position, length - position, &suffix))
        fatal_error("compact_unit_index: invalid radix suffix");
      added = new_node(index, suffix);
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
      Compact_term_slice old_slice = index->nodes[current].tokens;
      const int32_t *old_tokens = compact_term_pool_slice_tokens(
        index->term_pool, old_slice);
      uint32_t old_length = compact_term_slice_length(old_slice);
      uint32_t common = 0;
      while (common < old_length && position + common < length &&
             old_tokens[common] == tokens[position + common])
        common++;
      if (common == old_length) {
        position += common;
        parent = current;
      }
      else {
        uint32_t old_next = index->nodes[current].next_sibling;
        Compact_term_slice prefix, old_suffix;
        uint32_t split;
        uint32_t added;
        if (common == 0)
          fatal_error("compact_unit_index: invalid zero-length radix split");
        if (!compact_term_slice_subslice(old_slice, 0, common, &prefix) ||
            !compact_term_slice_subslice(
              old_slice, common, old_length - common, &old_suffix))
          fatal_error("compact_unit_index: invalid radix split slices");
        split = new_node(index, prefix);
        index->nodes[split].next_sibling = old_next;
        if (previous == CUI_NONE)
          index->nodes[parent].first_child = split;
        else
          index->nodes[previous].next_sibling = split;
        index->nodes[current].tokens = old_suffix;
        index->nodes[current].next_sibling = CUI_NONE;
        index->nodes[split].first_child = current;
        position += common;
        if (position == length)
          return split;
        {
          Compact_term_slice suffix;
          if (!compact_term_slice_subslice(
                slice, position, length - position, &suffix))
            fatal_error("compact_unit_index: invalid added radix suffix");
          added = new_node(index, suffix);
        }
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

static Compact_term_slice append_tokens(Compact_unit_index index,
                                        Topform unit, Term atom,
                                        uint64_t *symbol_mask)
{
  Compact_term_slice slice = compact_term_pool_intern_slice(
    index->term_pool, unit->id, unit->literals, atom);
  const int32_t *tokens = compact_term_pool_slice_tokens(
    index->term_pool, slice);
  uint32_t length = compact_term_slice_length(slice);
  uint32_t i;
  *symbol_mask = 0;
  for (i = 0; i < length; i++) {
    int32_t code = tokens[i];
    if (code >= 0)
      *symbol_mask |= symbol_bit((unsigned) code);
  }
  return slice;
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
  const int32_t *tokens = compact_term_pool_slice_tokens(
    index->term_pool, r->tokens);
  uint32_t length = compact_term_slice_length(r->tokens);
  uint32_t node = insert_token_path(
    index, index->roots[r->sign ? 1 : 0], r->tokens);
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
  if ((index->strategy == COMPACT_UNIT_POSITION ||
       index->strategy == COMPACT_UNIT_ADAPTIVE) &&
      index_token_features(index, tokens, record, 0, length,
                           UINT64_C(0x726f6f745f706174), 0) !=
        length)
    fatal_error("compact_unit_index: malformed feature term");
}

static Compact_unit_index compact_unit_index_init_with_pool_strategy(
  Compact_term_pool pool, Compact_unit_strategy strategy)
{
  Compact_unit_index index = safe_calloc(1, sizeof(*index));
  if (pool == NULL)
    fatal_error("compact_unit_index_init_with_pool: null term pool");
  index->term_pool = pool;
  index->strategy = strategy;
  index->feature_depth = Unit_feature_depth;
  index->id_map = compact_id_map_init(1);
  index->sort_clock = clock_init("compact_unit_sort");
  index->maintenance_clock = clock_init("compact_unit_maintenance");
  (void) new_node(index, 0);  /* reserved null node */
  index->roots[0] = new_node(index, 0);
  index->roots[1] = new_node(index, 0);
  ENSURE_ARRAY(index, postings, posting_count, posting_capacity,
               "compact_unit_index: posting overflow");
  memset(&index->postings[0], 0, sizeof(index->postings[0]));
  index->posting_count = 1;
  ENSURE_ARRAY(index, records, record_count, record_capacity,
               "compact_unit_index: record overflow");
  memset(&index->records[0], 0, sizeof(index->records[0]));
  index->record_count = 1;
  if (index->strategy == COMPACT_UNIT_POSITION ||
      index->strategy == COMPACT_UNIT_ADAPTIVE) {
    ENSURE_ARRAY(index, feature_buckets, feature_bucket_count,
                 feature_bucket_capacity,
                 "compact_unit_index: feature bucket overflow");
    memset(&index->feature_buckets[0], 0,
           sizeof(index->feature_buckets[0]));
    index->feature_bucket_count = 1;
    ENSURE_ARRAY(index, feature_chunks, feature_chunk_count,
                 feature_chunk_capacity,
                 "compact_unit_index: feature chunk overflow");
    memset(&index->feature_chunks[0], 0,
           sizeof(index->feature_chunks[0]));
    index->feature_chunk_count = 1;
  }
  if (index->strategy == COMPACT_UNIT_ADAPTIVE)
    index->adaptive_routes = safe_calloc(
      CUI_ADAPTIVE_ROUTE_CAPACITY, sizeof(*index->adaptive_routes));
  update_peak(index);
  return index;
}

Compact_unit_index compact_unit_index_init_with_pool(Compact_term_pool pool)
{
  return compact_unit_index_init_with_pool_strategy(pool, Unit_strategy);
}

Compact_unit_index compact_unit_index_init(void)
{
  Compact_term_pool pool = compact_term_pool_init();
  Compact_unit_index index = compact_unit_index_init_with_pool(pool);
  index->owns_term_pool = TRUE;
  update_peak(index);
  return index;
}

Compact_unit_index compact_unit_index_init_strategy(
  Compact_unit_strategy strategy)
{
  Compact_term_pool pool;
  Compact_unit_index index;
  if (strategy < COMPACT_UNIT_ROOT_SCAN ||
      strategy > COMPACT_UNIT_ADAPTIVE)
    fatal_error("compact_unit_index_init_strategy: invalid strategy");
  pool = compact_term_pool_init();
  index = compact_unit_index_init_with_pool_strategy(pool, strategy);
  index->owns_term_pool = TRUE;
  update_peak(index);
  return index;
}

BOOL compact_unit_index_add(Compact_unit_index index, Topform unit)
{
  struct cui_record *record;
  const int32_t *tokens;
  uint32_t at_record;
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
  record->tokens = append_tokens(index, unit, unit->literals->atom,
                                 &record->symbol_mask);
  tokens = compact_term_pool_slice_tokens(index->term_pool, record->tokens);
  if (compact_term_slice_length(record->tokens) == 0 || tokens[0] < 0)
    fatal_error("compact_unit_index: unit atom has no fixed root");
  record->root_symbol = (uint32_t) tokens[0];
  {
    unsigned root = record->root_symbol;
    ensure_unifier_symbol(index, root);
    record->next_root = index->unifier_heads[record->sign ? 1 : 0][root];
    index->unifier_heads[record->sign ? 1 : 0][root] = at_record;
  }
  index_record(index, at_record);
  if (!compact_id_map_put(index->id_map, unit->id, &at_record))
    fatal_error("compact_unit_index: duplicate proof ID");
  index->active++;
  update_peak(index);
  return TRUE;
}

BOOL compact_unit_index_remove(Compact_unit_index index,
                               unsigned long long proof_id)
{
  uint32_t record = lookup_record(index, proof_id);
  if (record == CUI_NONE || !index->records[record].active)
    return FALSE;
  index->records[record].active = FALSE;
  if (!compact_id_map_remove(index->id_map, proof_id))
    fatal_error("compact_unit_index: missing proof ID on removal");
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
  struct cui_record saved = *old;
  struct cui_record *record;
  uint32_t at_record;
  ENSURE_ARRAY(destination, records, record_count, record_capacity,
               "compact_unit_index: compacted record overflow");
  if (destination->record_count > UINT32_MAX)
    fatal_error("compact_unit_index: compacted record offsets exceed 32 bits");
  at_record = (uint32_t) destination->record_count++;
  record = &destination->records[at_record];
  *record = saved;
  record->next_root = CUI_NONE;
  record->query_stamp = 0;
  {
    unsigned root = record->root_symbol;
    ensure_unifier_symbol(destination, root);
    record->next_root =
      destination->unifier_heads[record->sign ? 1 : 0][root];
    destination->unifier_heads[record->sign ? 1 : 0][root] = at_record;
  }
  index_record(destination, at_record);
  if (!compact_id_map_put(destination->id_map, record->proof_id, &at_record))
    fatal_error("compact_unit_index: duplicate compacted proof ID");
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
  if (threshold < index->active &&
      threshold < CUI_GROWTH_STALE_FLOOR)
    threshold = index->active < CUI_GROWTH_STALE_FLOOR ?
      index->active : CUI_GROWTH_STALE_FLOOR;
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

void compact_unit_index_set_strategy(Compact_unit_strategy strategy)
{
  if (strategy != COMPACT_UNIT_ROOT_SCAN &&
      strategy != COMPACT_UNIT_POSITION &&
      strategy != COMPACT_UNIT_CODE_TREE &&
      strategy != COMPACT_UNIT_ADAPTIVE)
    fatal_error("compact_unit_index: invalid strategy");
  Unit_strategy = strategy;
}

void compact_unit_index_set_feature_depth(unsigned depth)
{
  Unit_feature_depth = depth;
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
  size_t i, packed;
  if (index == NULL ||
      (!force && !compact_unit_index_compaction_needed(index)) ||
      (force && index->record_count - 1 == index->active))
    return;
  clock_start(index->maintenance_clock);
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

  /* Records contain the complete immutable recipe for rebuilding nodes,
     postings, roots, and the ID map.  Pack live records downward first,
     release every other predecessor array, then shrink and reuse the packed
     array while rebuilding its secondary indexes.  This avoids overlapping
     predecessor and replacement records without changing insertion order. */
  packed = 1;
  for (i = 1; i < index->record_count; i++)
    if (index->records[i].active) {
      if (packed != i)
        index->records[packed] = index->records[i];
      packed++;
    }
  old = *index;
  safe_free(old.nodes);
  safe_free(old.postings);
  safe_free(old.unifier_heads[0]);
  safe_free(old.unifier_heads[1]);
  safe_free(old.feature_buckets);
  safe_free(old.feature_hash);
  safe_free(old.feature_chunks);
  compact_id_map_free(old.id_map);
  safe_free(old.query);
  safe_free(old.generalization_stack);
  safe_free(old.result_ids);
  safe_free(old.path_stack);
  safe_free(old.child_stack);
  safe_free(old.selected_path);
  safe_free(old.refinement_path);
  safe_free(old.selected_variable_keys);
  safe_free(old.first_variable_keys);
  safe_free(old.adaptive_routes);
  old.records = safe_realloc(
    old.records, packed * sizeof(*old.records));
  old.record_capacity = packed;
  old.record_count = packed;
  replacement = compact_unit_index_init_with_pool_strategy(
    old.term_pool, old.strategy);
  /* A live index owns its feature coverage contract.  Rebuilding must not
     observe a later process-global option change. */
  replacement->feature_depth = old.feature_depth;
  replacement->owns_term_pool = old.owns_term_pool;
  safe_free(replacement->records);
  replacement->records = old.records;
  replacement->record_capacity = packed;
  replacement->record_count = 1;
  for (i = 1; i < packed; i++)
    copy_live_record(replacement, &old.records[i]);
  free_clock(replacement->sort_clock);
  free_clock(replacement->maintenance_clock);
  replacement->generalization_timer = old.generalization_timer;
  replacement->instance_timer = old.instance_timer;
  replacement->unifier_timer = old.unifier_timer;
  replacement->sort_clock = old.sort_clock;
  replacement->maintenance_clock = old.maintenance_clock;
  *index = *replacement;
  safe_free(replacement);
  index->retired = retired;
  index->compactions = compactions + 1;
  index->bytes_reclaimed = reclaimed +
    (old_bytes > index_bytes(index) ? old_bytes - index_bytes(index) : 0);
  index->generalization_queries = generalization_queries;
  index->instance_queries = instance_queries;
  index->instance_exact_tests = instance_exact_tests;
  index->instance_tree_queries = old.instance_tree_queries;
  index->instance_tree_nodes_examined = old.instance_tree_nodes_examined;
  index->instance_tree_postings_examined =
    old.instance_tree_postings_examined;
  index->unifier_queries = unifier_queries;
  index->unifier_exact_tests = unifier_exact_tests;
  index->position_queries = old.position_queries;
  index->position_fallback_queries = old.position_fallback_queries;
  index->position_postings_examined = old.position_postings_examined;
  index->position_duplicate_postings = old.position_duplicate_postings;
  index->code_tree_queries = old.code_tree_queries;
  index->code_tree_nodes_examined = old.code_tree_nodes_examined;
  index->code_tree_postings_examined = old.code_tree_postings_examined;
  index->code_tree_variable_parents = old.code_tree_variable_parents;
  index->code_tree_variable_children = old.code_tree_variable_children;
  index->code_tree_pending_parents = old.code_tree_pending_parents;
  index->code_tree_pending_children = old.code_tree_pending_children;
  index->code_tree_rigid_parents = old.code_tree_rigid_parents;
  index->code_tree_rigid_children = old.code_tree_rigid_children;
  index->code_tree_rigid_sibling_checks =
    old.code_tree_rigid_sibling_checks;
  index->adaptive_queries = old.adaptive_queries;
  index->adaptive_tree_choices = old.adaptive_tree_choices;
  index->adaptive_position_choices = old.adaptive_position_choices;
  index->adaptive_position_empty_choices =
    old.adaptive_position_empty_choices;
  index->position_refinement_queries = old.position_refinement_queries;
  index->position_refinement_checks = old.position_refinement_checks;
  index->position_refinement_rejects = old.position_refinement_rejects;
  index->position_tertiary_queries = old.position_tertiary_queries;
  index->position_tertiary_checks = old.position_tertiary_checks;
  index->position_tertiary_rejects = old.position_tertiary_rejects;
  index->adaptive_route_hits = old.adaptive_route_hits;
  index->adaptive_route_misses = old.adaptive_route_misses;
  index->adaptive_route_replacements = old.adaptive_route_replacements;
  index->generalization_profile = old.generalization_profile;
  index->instance_profile = old.instance_profile;
  index->unifier_profile = old.unifier_profile;
  if (old_peak > index->peak_bytes)
    index->peak_bytes = old_peak;
  if (old_peak_active > index->peak)
    index->peak = old_peak_active;
  clock_stop(index->maintenance_clock);
}

void compact_unit_index_compact(Compact_unit_index index)
{
  compact_unit_index_compact_internal(index, FALSE);
}

void compact_unit_index_compact_all_stale(Compact_unit_index index)
{
  compact_unit_index_compact_internal(index, TRUE);
}

BOOL compact_unit_index_reclaim_owning_pool(Compact_unit_index index)
{
  Compact_term_rebase_map map;
  Compact_term_pool pool;
  if (index == NULL || !index->owns_term_pool || index->active == 0 ||
      index->record_count - 1 == index->active)
    return FALSE;
  compact_unit_index_compact_all_stale(index);
  pool = index->term_pool;
  map = compact_term_rebase_map_init();
  compact_unit_index_retain_live_clauses(index, map);
  compact_term_pool_compact_retained(pool, map);
  compact_unit_index_rebase_term_pool(index, pool, map);
  compact_term_rebase_map_free(map);
  update_peak(index);
  return TRUE;
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
    index->records[i].tokens = compact_term_rebase_slice(
      map, index->records[i].tokens);
  for (i = 1; i < index->node_count; i++)
    if (compact_term_slice_length(index->nodes[i].tokens) != 0)
      index->nodes[i].tokens = compact_term_rebase_slice(
        map, index->nodes[i].tokens);
  index->term_pool = pool;
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
  int32_t first_code,
  Term *bindings, struct cui_new_bindings *new_bindings,
  uint32_t *next_position)
{
  struct cui_node *edge = &index->nodes[node];
  const int32_t *tokens = query_slice_tokens(index, edge->tokens);
  uint32_t length = query_slice_length(edge->tokens);
  uint32_t i = 0;
  new_bindings->low = 0;
  new_bindings->high = 0;
  /* The generalization traversal has already read and classified the first
     token.  For its equal rigid branch, the target is known rigid with this
     symbol; consume both without resolving and comparing them a second time. */
  if (first_code >= 0) {
    if (length == 0 || position >= end)
      return FALSE;
    position++;
    i = 1;
  }
  for (; i < length; i++) {
    int32_t code = tokens[i];
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
        if (variable < 64)
          new_bindings->low |= UINT64_C(1) << variable;
        else
          new_bindings->high |= UINT64_C(1) << (variable - 64);
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
                                         struct cui_new_bindings new_bindings)
{
  while (new_bindings.low != 0) {
    unsigned variable = (unsigned) __builtin_ctzll(new_bindings.low);
    bindings[variable] = NULL;
    new_bindings.low &= new_bindings.low - 1;
  }
  while (new_bindings.high != 0) {
    unsigned variable =
      64U + (unsigned) __builtin_ctzll(new_bindings.high);
    bindings[variable] = NULL;
    new_bindings.high &= new_bindings.high - 1;
  }
}

static void ensure_generalization_stack(Compact_unit_index index,
                                        size_t required)
{
  size_t capacity = index->generalization_stack_capacity;
  if (capacity >= required)
    return;
  while (capacity < required)
    capacity = grow_capacity(
      capacity,
      sizeof(*index->generalization_stack),
      "compact_unit_index: generalization stack overflow");
  index->generalization_stack = safe_realloc(
    index->generalization_stack,
    capacity * sizeof(*index->generalization_stack));
  index->generalization_stack_capacity = capacity;
}

static void initialize_generalization_frame(
  Compact_unit_index index, struct cui_generalization_frame *frame,
  uint32_t node, uint32_t position, uint32_t end,
  struct cui_new_bindings new_bindings)
{
  frame->node = node;
  frame->position = position;
  frame->new_bindings = new_bindings;
  if (position == end) {
    frame->next_child = CUI_NONE;
    frame->wanted = -1;
  }
  else {
    Term target = index->query[position].term;
    frame->next_child = index->nodes[node].first_child;
    frame->wanted = VARIABLE(target) ? -1 : (int32_t) SYMNUM(target);
  }
}

static unsigned long long generalization_iterative(
  Compact_unit_index index, uint32_t root, uint32_t end, Term *bindings,
  unsigned long long exclude_id, struct cui_query_work *work)
{
  struct cui_new_bindings no_bindings = { 0, 0 };
  size_t depth = 1;

  ensure_generalization_stack(index, (size_t) end + 1);
  initialize_generalization_frame(
    index, &index->generalization_stack[0], root, 0, end,
    no_bindings);

  /*
   * Siblings use code_compare() order: every stored-variable edge first,
   * followed by rigid symbols in numeric order.  A stored pattern can
   * generalize the target at POSITION only through a variable edge, or
   * (when the target is rigid) through the one edge with the same symbol.
   * Trying every other sibling used to dominate mature unit indexes even
   * though match_generalization_edge() rejected each at its first token.
   * Repeated stored variables still require visiting every variable edge;
   * their bindings remain authoritative in match_generalization_edge().
   */
  while (depth != 0) {
    struct cui_generalization_frame *frame =
      &index->generalization_stack[depth - 1];

    if (frame->position == end) {
      uint32_t posting;
      for (posting = index->nodes[frame->node].first_posting;
           posting != CUI_NONE; posting = index->postings[posting].next) {
        struct cui_record *record =
          &index->records[index->postings[posting].record];
        work->postings++;
        if (record->active)
          work->live++;
        else
          work->dead++;
        if (record->active && record->proof_id != exclude_id)
          return record->proof_id;
      }
      undo_generalization_bindings(bindings, frame->new_bindings);
      depth--;
    }
    else if (frame->next_child == CUI_NONE) {
      undo_generalization_bindings(bindings, frame->new_bindings);
      depth--;
    }
    else {
      uint32_t child = frame->next_child;
      int32_t code;

      /* Advance the parent before descending so a pop resumes at the exact
         next sibling, just as the recursive for-loop did. */
      frame->next_child = index->nodes[child].next_sibling;
      code = query_first_code(index, child);
      if (code < 0 || (frame->wanted >= 0 && code == frame->wanted)) {
        struct cui_new_bindings new_bindings;
        uint32_t next_position = frame->position;
        work->nodes++;
        if (match_generalization_edge(index, child, frame->position, end, code,
                                      bindings, &new_bindings,
                                      &next_position)) {
          if (depth >= index->generalization_stack_capacity)
            fatal_error("compact_unit_index: invalid generalization depth");
          initialize_generalization_frame(
            index, &index->generalization_stack[depth], child,
            next_position, end, new_bindings);
          depth++;
        }
        else
          undo_generalization_bindings(bindings, new_bindings);
      }
      else if (frame->wanted < 0 || code > frame->wanted)
        frame->next_child = CUI_NONE;
    }
  }
  return 0;
}

unsigned long long compact_unit_generalization_first(
  Compact_unit_index index, Term target, BOOL sign,
  unsigned long long exclude_id)
{
  size_t count = 0;
  Term bindings[MAX_VARS];
  struct cui_query_work work;
  unsigned long long result;
  if (index == NULL || target == NULL)
    return 0;
  memset(&work, 0, sizeof(work));
  compact_query_timer_start(&index->generalization_timer);
  index->generalization_queries++;
  prepare_query_term_access(index);
  memset(bindings, 0, sizeof(bindings));
  flatten_query(index, target, &count);
  if (count == 0)
    result = 0;
  else
    result = generalization_iterative(
      index, index->roots[sign ? 1 : 0], (uint32_t) count,
      bindings, exclude_id, &work);
  compact_profile_note(&index->generalization_profile,
                       work.postings,
                       work.nodes + work.postings,
                       work.live, work.dead, 0,
                       result == 0 ? 0 : 1, 0);
  compact_profile_note_exact(&index->generalization_profile,
                             work.postings,
                             result == 0 ? 0 : 1, 0);
  update_peak(index);
  compact_query_timer_stop(&index->generalization_timer);
  return result;
}

static uint32_t token_term_end(const int32_t *tokens, uint32_t position,
                               uint32_t end)
{
  int32_t code;
  int i;
  if (position >= end)
    return UINT32_MAX;
  code = tokens[position++];
  if (code < 0)
    return position;
  for (i = 0; i < sn_to_arity(code); i++) {
    position = token_term_end(tokens, position, end);
    if (position == UINT32_MAX)
      return UINT32_MAX;
  }
  return position;
}

static BOOL pattern_matches_tokens(const int32_t *tokens, Term pattern,
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
    uint32_t finish = token_term_end(tokens, start, end);
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
          memcmp(tokens + starts[variable], tokens + start,
                 new_length * sizeof(*tokens)) != 0)
        return FALSE;
    }
    *position = finish;
    return TRUE;
  }
  code = tokens[(*position)++];
  if (code < 0 || code != SYMNUM(pattern))
    return FALSE;
  for (i = 0; i < ARITY(pattern); i++)
    if (!pattern_matches_tokens(tokens, ARG(pattern, i), position, end,
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

static void append_result_id(Compact_unit_index index,
                             unsigned long long proof_id,
                             size_t *found)
{
  if (*found == index->result_capacity) {
    index->result_capacity = grow_capacity(
      index->result_capacity, sizeof(*index->result_ids),
      "compact_unit_index: result overflow");
    index->result_ids = safe_realloc(
      index->result_ids,
      index->result_capacity * sizeof(*index->result_ids));
  }
  index->result_ids[(*found)++] = proof_id;
}

/* Retrieve stored instances of a resident pattern from the shared serialized
   term tree.  A pattern variable may cover any complete stored subterm;
   repeated-variable equality is left to pattern_matches_tokens() at terminal
   postings.  A stored variable cannot satisfy a rigid pattern position. */
static void collect_instance_tree_candidates(
  Compact_unit_index index, uint32_t node, uint32_t query_position,
  uint32_t query_end, size_t pending, Term pattern,
  unsigned long long exclude_id, size_t *found,
  unsigned long long *visited, unsigned long long *live,
  unsigned long long *dead)
{
  struct cui_node *edge = &index->nodes[node];
  uint32_t edge_length = query_slice_length(edge->tokens);
  const int32_t *edge_tokens = edge_length == 0 ? NULL :
    query_slice_tokens(index, edge->tokens);
  uint32_t at;
  uint32_t child;

  (*visited)++;
  index->instance_tree_nodes_examined++;
  for (at = 0; at < edge_length; at++) {
    int32_t code = edge_tokens[at];
    if (pending != 0) {
      int arity = code < 0 ? 0 : sn_to_arity(code);
      pending--;
      if ((size_t) arity > SIZE_MAX - pending)
        fatal_error("compact_unit_index: instance tree term overflow");
      pending += (size_t) arity;
      if (pending == 0) {
        if (query_position >= query_end)
          return;
        query_position = index->query[query_position].end;
      }
    }
    else {
      Term resident;
      if (query_position >= query_end)
        return;
      resident = index->query[query_position].term;
      if (VARIABLE(resident)) {
        if (code < 0)
          query_position = index->query[query_position].end;
        else {
          pending = (size_t) sn_to_arity(code);
          if (pending == 0)
            query_position = index->query[query_position].end;
        }
      }
      else {
        if (code < 0 || code != SYMNUM(resident))
          return;
        query_position++;
      }
    }
  }

  if (query_position == query_end && pending == 0) {
    uint32_t posting;
    for (posting = edge->first_posting; posting != CUI_NONE;
         posting = index->postings[posting].next) {
      struct cui_record *record =
        &index->records[index->postings[posting].record];
      const int32_t *record_tokens;
      uint32_t record_length;
      uint32_t position;
      uint32_t starts[MAX_VARS], ends[MAX_VARS];
      unsigned variable;
      index->instance_tree_postings_examined++;
      if (!record->active) {
        (*dead)++;
        continue;
      }
      (*live)++;
      if (record->proof_id == exclude_id)
        continue;
      index->instance_exact_tests++;
      for (variable = 0; variable < MAX_VARS; variable++)
        starts[variable] = UINT32_MAX;
      record_tokens = query_slice_tokens(index, record->tokens);
      record_length = query_slice_length(record->tokens);
      position = 0;
      if (pattern_matches_tokens(record_tokens, pattern, &position,
                                 record_length, starts, ends) &&
          position == record_length)
        append_result_id(index, record->proof_id, found);
    }
    return;
  }

  for (child = edge->first_child; child != CUI_NONE;
       child = index->nodes[child].next_sibling)
    collect_instance_tree_candidates(
      index, child, query_position, query_end, pending, pattern, exclude_id,
      found, visited, live, dead);
}

unsigned long long *compact_unit_instance_ids(
  Compact_unit_index index, Term pattern, BOOL sign,
  unsigned long long exclude_id, size_t *count)
{
  uint64_t wanted;
  size_t found = 0;
  size_t i;
  unsigned long long tests_before, visited = 0, live = 0, dead = 0;
  if (count == NULL)
    return NULL;
  *count = 0;
  if (index == NULL || pattern == NULL)
    return NULL;
  compact_query_timer_start(&index->instance_timer);
  index->instance_queries++;
  prepare_query_term_access(index);
  tests_before = index->instance_exact_tests;
  if (index->strategy == COMPACT_UNIT_CODE_TREE ||
      index->strategy == COMPACT_UNIT_ADAPTIVE) {
    size_t query_count = 0;
    uint32_t child;
    index->instance_tree_queries++;
    flatten_query(index, pattern, &query_count);
    if (query_count > UINT32_MAX)
      fatal_error("compact_unit_index: instance-tree query overflow");
    for (child = index->nodes[index->roots[sign ? 1 : 0]].first_child;
         child != CUI_NONE; child = index->nodes[child].next_sibling)
      collect_instance_tree_candidates(
        index, child, 0, (uint32_t) query_count, 0, pattern, exclude_id,
        &found, &visited, &live, &dead);
  }
  else {
    wanted = resident_symbol_mask(pattern);
    for (i = 1; i < index->record_count; i++) {
      struct cui_record *record = &index->records[i];
      const int32_t *record_tokens;
      uint32_t record_length;
      uint32_t position;
      uint32_t starts[MAX_VARS], ends[MAX_VARS];
      unsigned j;
      visited++;
      if (record->active)
        live++;
      else
        dead++;
      if (!record->active || record->sign != (unsigned char) sign ||
          record->proof_id == exclude_id ||
          (record->symbol_mask & wanted) != wanted)
        continue;
      index->instance_exact_tests++;
      for (j = 0; j < MAX_VARS; j++)
        starts[j] = UINT32_MAX;
      record_tokens = query_slice_tokens(index, record->tokens);
      record_length = query_slice_length(record->tokens);
      position = 0;
      if (pattern_matches_tokens(record_tokens, pattern, &position,
                                 record_length, starts, ends) &&
          position == record_length) {
        append_result_id(index, record->proof_id, &found);
      }
    }
  }
  if (found > 1) {
    clock_start(index->sort_clock);
    qsort(index->result_ids, found, sizeof(*index->result_ids),
          descending_id_compare);
    clock_stop(index->sort_clock);
  }
  compact_profile_note(
    &index->instance_profile,
    index->instance_exact_tests - tests_before,
    visited +
      ((index->strategy == COMPACT_UNIT_CODE_TREE ||
        index->strategy == COMPACT_UNIT_ADAPTIVE) ? live + dead : 0),
    live, dead, 0, found, 0);
  compact_profile_note_exact(
    &index->instance_profile,
    index->instance_exact_tests - tests_before, found, 0);
  update_peak(index);
  compact_query_timer_stop(&index->instance_timer);
  if (found == 0)
    return NULL;
  {
    unsigned long long *result = safe_malloc(found * sizeof(*result));
    memcpy(result, index->result_ids, found * sizeof(*result));
    *count = found;
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
  const int32_t *tokens;
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
    code = state->tokens[expr.value.position];
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
  return expr.token ? state->tokens[expr.value.position] :
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
      position = token_term_end(state->tokens, position, expr.token_end);
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
  state.tokens = query_slice_tokens(index, record->tokens);
  resident.token = FALSE;
  resident.value.resident = query;
  resident.token_end = 0;
  token.token = TRUE;
  token.value.position = 0;
  token.token_end = query_slice_length(record->tokens);
  return unify_exprs(&state, resident, token);
}

struct cui_code_tree_work {
  unsigned long long nodes;
  unsigned long long variable_parents;
  unsigned long long variable_children;
  unsigned long long pending_parents;
  unsigned long long pending_children;
  unsigned long long rigid_parents;
  unsigned long long rigid_children;
  unsigned long long rigid_sibling_checks;
};

/* Traverse the existing radix-compressed term-code tree as a safe unification
   filter.  Repeated-variable and occurs-check constraints are deliberately
   deferred to resident_unifies_record(); ignoring them can add candidates but
   cannot omit an answer.  PENDING is the number of serialized stored-term
   children still covered by the current resident query variable. */
static void collect_code_tree_candidates(
  Compact_unit_index index, uint32_t node, uint32_t query_position,
  uint32_t query_end, size_t pending, Term query,
  unsigned long long exclude_id, size_t *found,
  struct cui_code_tree_work *work, unsigned long long *live,
  unsigned long long *dead)
{
  struct cui_node *edge = &index->nodes[node];
  uint32_t edge_length = query_slice_length(edge->tokens);
  const int32_t *edge_tokens = edge_length == 0 ? NULL :
    query_slice_tokens(index, edge->tokens);
  uint32_t at;
  uint32_t child;

  work->nodes++;
  for (at = 0; at < edge_length; at++) {
    int32_t code = edge_tokens[at];
    if (pending != 0) {
      int arity = code < 0 ? 0 : sn_to_arity(code);
      pending--;
      if ((size_t) arity > SIZE_MAX - pending)
        fatal_error("compact_unit_index: code-tree term overflow");
      pending += (size_t) arity;
      if (pending == 0) {
        if (query_position >= query_end)
          return;
        query_position = index->query[query_position].end;
      }
    }
    else {
      Term resident;
      if (query_position >= query_end)
        return;
      resident = index->query[query_position].term;
      if (code < 0)
        query_position = index->query[query_position].end;
      else if (VARIABLE(resident)) {
        pending = (size_t) sn_to_arity(code);
        if (pending == 0)
          query_position = index->query[query_position].end;
      }
      else {
        if (code != SYMNUM(resident))
          return;
        query_position++;
      }
    }
  }

  if (query_position == query_end && pending == 0) {
    uint32_t posting;
    for (posting = edge->first_posting; posting != CUI_NONE;
         posting = index->postings[posting].next) {
      struct cui_record *record =
        &index->records[index->postings[posting].record];
      index->code_tree_postings_examined++;
      if (!record->active) {
        (*dead)++;
        continue;
      }
      (*live)++;
      if (record->proof_id == exclude_id)
        continue;
      index->unifier_exact_tests++;
      if (resident_unifies_record(index, query, record))
        append_result_id(index, record->proof_id, found);
    }
    return;
  }

  /* Siblings are kept in code_compare() order: stored variables first, then
     fixed symbols.  At an ordinary resident symbol only a variable edge or
     the one equal-symbol edge can unify.  Calling the recursive matcher for
     every other sibling made mature indexes examine about a thousand nodes
     per conflict query merely to reject most at their first token.  A
     resident variable (or a stored subtree currently covered by one) still
     visits every child, preserving the complete unification answer set. */
  if (pending != 0) {
    work->pending_parents++;
    for (child = edge->first_child; child != CUI_NONE;
         child = index->nodes[child].next_sibling) {
      work->pending_children++;
      collect_code_tree_candidates(index, child, query_position, query_end,
                                   pending, query, exclude_id, found, work,
                                   live, dead);
    }
  }
  else if (query_position < query_end &&
           VARIABLE(index->query[query_position].term)) {
    work->variable_parents++;
    for (child = edge->first_child; child != CUI_NONE;
         child = index->nodes[child].next_sibling) {
      work->variable_children++;
      collect_code_tree_candidates(index, child, query_position, query_end,
                                   pending, query, exclude_id, found, work,
                                   live, dead);
    }
  }
  else if (query_position < query_end) {
    int wanted = SYMNUM(index->query[query_position].term);
    work->rigid_parents++;
    for (child = edge->first_child; child != CUI_NONE;
         child = index->nodes[child].next_sibling) {
      int32_t code = query_first_code(index, child);
      work->rigid_sibling_checks++;
      if (code < 0 || code == wanted) {
        work->rigid_children++;
        collect_code_tree_candidates(
          index, child, query_position, query_end, pending, query,
          exclude_id, found, work, live, dead);
      }
      else if (code > wanted)
        break;
    }
  }
}

struct cui_feature_choice {
  uint64_t exact_key;
  uint64_t variable_shape;
  unsigned long long score;
  unsigned symbol;
  size_t depth;
  size_t variable_count;
  BOOL found;
};

static void ensure_u64_scratch(uint64_t **values, size_t *capacity,
                               size_t needed, const char *message)
{
  while (needed > *capacity) {
    *capacity = grow_capacity(*capacity, sizeof(**values), message);
    *values = safe_realloc(*values, *capacity * sizeof(**values));
  }
}

static void ensure_unsigned_scratch(unsigned **values, size_t *capacity,
                                    size_t needed, const char *message)
{
  while (needed > *capacity) {
    *capacity = grow_capacity(*capacity, sizeof(**values), message);
    *values = safe_realloc(*values, *capacity * sizeof(**values));
  }
}

static unsigned long long feature_posting_count(Compact_unit_index index,
                                                uint64_t key)
{
  uint32_t bucket = find_feature_bucket(index, key);
  return bucket == CUI_NONE ? 0 : index->feature_buckets[bucket].count;
}

static void select_unifier_feature(Compact_unit_index index, Term term,
                                   BOOL sign, uint64_t path, size_t depth,
                                   uint64_t excluded_exact_key1,
                                   uint64_t excluded_exact_key2,
                                   struct cui_feature_choice *choice)
{
  int i;
  unsigned long long score;
  uint64_t exact_key;
  if (choice->found && choice->score == 0)
    return;
  ensure_u64_scratch(&index->path_stack, &index->path_capacity, depth + 1,
                     "compact_unit_index: query path overflow");
  ensure_unsigned_scratch(&index->child_stack, &index->child_capacity,
                          depth + 1,
                          "compact_unit_index: query child path overflow");
  index->path_stack[depth] = path;
  if (VARIABLE(term)) {
    choice->variable_shape ^= mix64(
      path ^ UINT64_C(0x71756572795f7661));
    return;
  }
  if (depth != 0 &&
      (index->feature_depth == 0 || depth <= index->feature_depth)) {
    size_t ancestor;
    exact_key = exact_feature_key(path, (unsigned) SYMNUM(term), sign);
    score = feature_posting_count(index, exact_key);
    for (ancestor = 1; ancestor <= depth; ancestor++) {
      unsigned long long n = feature_posting_count(
        index, variable_feature_key(index->path_stack[ancestor], sign));
      if (ULLONG_MAX - score < n)
        score = ULLONG_MAX;
      else
        score += n;
    }
    if (exact_key != excluded_exact_key1 &&
        exact_key != excluded_exact_key2 &&
        (!choice->found || score < choice->score)) {
      ensure_u64_scratch(&index->selected_variable_keys,
                         &index->selected_variable_capacity, depth,
                         "compact_unit_index: selected path overflow");
      for (ancestor = 1; ancestor <= depth; ancestor++)
        index->selected_variable_keys[ancestor - 1] =
          variable_feature_key(index->path_stack[ancestor], sign);
      ensure_unsigned_scratch(&index->selected_path,
                              &index->selected_path_capacity, depth,
                              "compact_unit_index: selected path overflow");
      if (depth != 0)
        memcpy(index->selected_path, index->child_stack,
               depth * sizeof(*index->selected_path));
      choice->exact_key = exact_key;
      choice->symbol = (unsigned) SYMNUM(term);
      choice->depth = depth;
      choice->score = score;
      choice->variable_count = depth;
      choice->found = TRUE;
    }
  }
  if (index->feature_depth != 0 && depth >= index->feature_depth)
    return;
  for (i = 0; i < ARITY(term); i++) {
    index->child_stack[depth] = (unsigned) i;
    select_unifier_feature(index, ARG(term, i), sign,
                           child_path(path, (unsigned) i), depth + 1,
                           excluded_exact_key1, excluded_exact_key2,
                           choice);
  }
}

static void begin_position_query(Compact_unit_index index)
{
  size_t i;
  index->query_stamp++;
  if (index->query_stamp == 0) {
    for (i = 1; i < index->record_count; i++)
      index->records[i].query_stamp = 0;
    index->query_stamp = 1;
  }
}

/* A rigid query position is a necessary condition for unification: the
   stored term must contain the same symbol there, or a variable at that
   position or one of its ancestors.  Check that condition directly in the
   compact prefix stream so additional features can refine the rarest posting
   union without decoding additional posting unions. */
static BOOL record_satisfies_position(
  Compact_unit_index index, const struct cui_record *record,
  const struct cui_feature_choice *choice, const unsigned *selected_path)
{
  const int32_t *tokens = query_slice_tokens(index, record->tokens);
  uint32_t end = query_slice_length(record->tokens);
  uint32_t position = 0;
  size_t level;
  for (level = 0; level < choice->depth; level++) {
    int32_t code;
    unsigned child, i;
    int arity;
    if (position >= end)
      return FALSE;
    code = tokens[position++];
    if (code < 0)
      return TRUE;
    arity = sn_to_arity(code);
    child = selected_path[level];
    if (child >= (unsigned) arity)
      return FALSE;
    for (i = 0; i < child; i++) {
      position = token_term_end(tokens, position, end);
      if (position == UINT32_MAX)
        return FALSE;
    }
  }
  return position < end &&
    (tokens[position] < 0 || tokens[position] == (int32_t) choice->symbol);
}

static void collect_position_bucket(
  Compact_unit_index index, uint64_t key, Term query, BOOL sign,
  unsigned long long exclude_id, size_t *found,
  unsigned long long *visited, unsigned long long *live,
  unsigned long long *dead, unsigned long long *duplicates,
  const struct cui_feature_choice *required_choice,
  const unsigned *required_path,
  const struct cui_feature_choice *tertiary_choice,
  const unsigned *tertiary_path)
{
  uint32_t bucket = find_feature_bucket(index, key);
  uint32_t chunk;
  uint32_t record_index = 0;
  if (bucket == CUI_NONE)
    return;
  for (chunk = index->feature_buckets[bucket].first_chunk;
       chunk != CUI_NONE; chunk = index->feature_chunks[chunk].next) {
    const struct cui_feature_chunk *c = &index->feature_chunks[chunk];
    size_t at = 0;
    while (at < c->used) {
      uint32_t delta = 0;
      unsigned shift = 0;
      unsigned char byte;
      struct cui_record *record;
      do {
        if (at >= c->used || shift >= 35)
          fatal_error("compact_unit_index: malformed feature posting");
        byte = c->data[at++];
        delta |= (uint32_t) (byte & 0x7fU) << shift;
        shift += 7;
      } while ((byte & 0x80U) != 0);
      if (delta > UINT32_MAX - record_index)
        fatal_error("compact_unit_index: feature posting overflow");
      record_index += delta;
      if (record_index == CUI_NONE ||
          record_index >= index->record_count)
        fatal_error("compact_unit_index: invalid feature posting");
      record = &index->records[record_index];
      (*visited)++;
      index->position_postings_examined++;
      if (record->query_stamp == index->query_stamp) {
        (*duplicates)++;
        index->position_duplicate_postings++;
        continue;
      }
      record->query_stamp = index->query_stamp;
      if (!record->active) {
        (*dead)++;
        continue;
      }
      (*live)++;
      if (record->sign != (unsigned char) sign ||
          record->proof_id == exclude_id ||
          query_slice_length(record->tokens) == 0 ||
          record->root_symbol != (unsigned) SYMNUM(query))
        continue;
      if (required_choice != NULL) {
        index->position_refinement_checks++;
        if (!record_satisfies_position(index, record, required_choice,
                                       required_path)) {
          index->position_refinement_rejects++;
          continue;
        }
      }
      if (tertiary_choice != NULL) {
        index->position_refinement_checks++;
        index->position_tertiary_checks++;
        if (!record_satisfies_position(index, record, tertiary_choice,
                                       tertiary_path)) {
          index->position_refinement_rejects++;
          index->position_tertiary_rejects++;
          continue;
        }
      }
      index->unifier_exact_tests++;
      if (resident_unifies_record(index, query, record))
        append_result_id(index, record->proof_id, found);
    }
  }
}

static struct cui_adaptive_route *adaptive_route(
  Compact_unit_index index, Term query, BOOL sign,
  const struct cui_feature_choice *choice, BOOL *hit)
{
  uint64_t key = mix64(choice->exact_key ^ choice->variable_shape ^
                       ((uint64_t) (unsigned) SYMNUM(query) << 1) ^
                       (sign ? UINT64_C(1) : UINT64_C(0)));
  size_t slot;
  struct cui_adaptive_route *route;
  if (key == 0)
    key = 1;
  slot = (size_t) key & (CUI_ADAPTIVE_ROUTE_CAPACITY - 1);
  route = &index->adaptive_routes[slot];
  *hit = route->key == key;
  if (!*hit) {
    if (route->key != 0)
      index->adaptive_route_replacements++;
    route->key = key;
    route->tree_nodes = 0;
  }
  return route;
}

unsigned long long *compact_unit_unifier_ids(
  Compact_unit_index index, Term query, BOOL sign,
  unsigned long long exclude_id, size_t *count)
{
  size_t found = 0;
  uint32_t i;
  int query_root;
  unsigned long long tests_before, visited = 0, live = 0, dead = 0;
  unsigned long long duplicates = 0;
  struct cui_code_tree_work tree_work;
  struct cui_feature_choice choice, second_choice, third_choice;
  struct cui_adaptive_route *route = NULL;
  BOOL route_hit = FALSE;
  BOOL use_tree, use_position, have_second_choice = FALSE;
  BOOL have_third_choice = FALSE;
  if (count == NULL)
    return NULL;
  *count = 0;
  if (index == NULL || query == NULL || VARIABLE(query))
    return NULL;
  compact_query_timer_start(&index->unifier_timer);
  index->unifier_queries++;
  tests_before = index->unifier_exact_tests;
  query_root = SYMNUM(query);
  if ((size_t) query_root >= index->unifier_symbol_capacity) {
    compact_profile_note(&index->unifier_profile, 0, 0, 0, 0, 0, 0, 0);
    compact_profile_note_exact(&index->unifier_profile, 0, 0, 0);
    compact_query_timer_stop(&index->unifier_timer);
    return NULL;
  }
  prepare_query_term_access(index);
  memset(&choice, 0, sizeof(choice));
  memset(&second_choice, 0, sizeof(second_choice));
  memset(&third_choice, 0, sizeof(third_choice));
  memset(&tree_work, 0, sizeof(tree_work));
  use_tree = index->strategy == COMPACT_UNIT_CODE_TREE;
  use_position = index->strategy == COMPACT_UNIT_POSITION;
  if (index->strategy == COMPACT_UNIT_POSITION ||
      index->strategy == COMPACT_UNIT_ADAPTIVE)
    select_unifier_feature(index, query, sign,
                           UINT64_C(0x726f6f745f706174), 0, 0, 0, &choice);
  if (index->strategy == COMPACT_UNIT_ADAPTIVE) {
    index->adaptive_queries++;
    route = choice.found ?
      adaptive_route(index, query, sign, &choice, &route_hit) : NULL;
    if (route != NULL) {
      if (route_hit)
        index->adaptive_route_hits++;
      else
        index->adaptive_route_misses++;
    }
    /* Even an empty position union has a lookup cost.  Measure a route's
       tree traversal once, and keep remeasuring cheap routes as the index
       grows.  This prevents shallow CHAT-like trees from paying for a
       feature lookup whose complete code-tree rejection is only a handful
       of nodes, without weakening the zero-union answer. */
    use_position = choice.found && route_hit && route->tree_nodes != 0 &&
      (choice.score == 0 ?
         route->tree_nodes > CUI_ADAPTIVE_EMPTY_TREE_FLOOR :
         (choice.score <=
            ULLONG_MAX / CUI_ADAPTIVE_POSITION_FACTOR &&
          choice.score * CUI_ADAPTIVE_POSITION_FACTOR < route->tree_nodes));
    use_tree = !use_position;
    if (use_position) {
      index->adaptive_position_choices++;
      if (choice.score == 0)
        index->adaptive_position_empty_choices++;
    }
    else
      index->adaptive_tree_choices++;
  }
  if (use_tree) {
    size_t query_count = 0;
    uint32_t child;
    uint32_t root = index->roots[sign ? 1 : 0];
    index->code_tree_queries++;
    flatten_query(index, query, &query_count);
    if (query_count > UINT32_MAX)
      fatal_error("compact_unit_index: code-tree query overflow");
    /* QUERY is rigid at its root.  Prune the synthetic root's ordered
       children here so every query does not pay an otherwise empty recursive
       visit.  Stored-variable roots and the equal rigid root are the only
       compatible branches. */
    for (child = index->nodes[root].first_child; child != CUI_NONE;
         child = index->nodes[child].next_sibling) {
      int32_t code = query_first_code(index, child);
      if (code < 0 || code == query_root)
        collect_code_tree_candidates(
          index, child, 0, (uint32_t) query_count, 0, query, exclude_id,
          &found, &tree_work, &live, &dead);
      else if (code > query_root)
        break;
    }
    visited = tree_work.nodes;
    index->code_tree_nodes_examined += tree_work.nodes;
    index->code_tree_variable_parents += tree_work.variable_parents;
    index->code_tree_variable_children += tree_work.variable_children;
    index->code_tree_pending_parents += tree_work.pending_parents;
    index->code_tree_pending_children += tree_work.pending_children;
    index->code_tree_rigid_parents += tree_work.rigid_parents;
    index->code_tree_rigid_children += tree_work.rigid_children;
    index->code_tree_rigid_sibling_checks += tree_work.rigid_sibling_checks;
    if (route != NULL)
      route->tree_nodes = tree_work.nodes;
  }
  else if (use_position) {
    size_t key_at;
    index->position_queries++;
    /* SCORE is the sum of every exact/ancestor-variable posting that can
       satisfy this rigid query position.  Zero is therefore a complete
       negative answer; avoid even the empty bucket probes and query-stamp
       update on the overwhelmingly common position fast-negative path. */
    if (choice.found && choice.score != 0) {
      ensure_u64_scratch(&index->first_variable_keys,
                         &index->first_variable_capacity,
                         choice.variable_count,
                         "compact_unit_index: first path overflow");
      if (choice.variable_count != 0)
        memcpy(index->first_variable_keys, index->selected_variable_keys,
               choice.variable_count * sizeof(*index->first_variable_keys));
      select_unifier_feature(index, query, sign,
                             UINT64_C(0x726f6f745f706174), 0,
                             choice.exact_key, 0, &second_choice);
      have_second_choice = second_choice.found;
      if (have_second_choice) {
        index->position_refinement_queries++;
        /* Feature selection reuses SELECTED_PATH.  Preserve the second path
           before selecting a third condition; both are checked directly
           against each first-union record, without decoding more postings. */
        ensure_unsigned_scratch(&index->refinement_path,
                                &index->refinement_path_capacity,
                                second_choice.depth,
                                "compact_unit_index: refinement path overflow");
        if (second_choice.depth != 0)
          memcpy(index->refinement_path, index->selected_path,
                 second_choice.depth * sizeof(*index->refinement_path));
        select_unifier_feature(index, query, sign,
                               UINT64_C(0x726f6f745f706174), 0,
                               choice.exact_key, second_choice.exact_key,
                               &third_choice);
        have_third_choice = third_choice.found;
        if (have_third_choice)
          index->position_tertiary_queries++;
      }
      begin_position_query(index);
      collect_position_bucket(
        index, choice.exact_key, query, sign, exclude_id, &found, &visited,
        &live, &dead, &duplicates,
        have_second_choice ? &second_choice : NULL,
        have_second_choice ? index->refinement_path : NULL,
        have_third_choice ? &third_choice : NULL,
        have_third_choice ? index->selected_path : NULL);
      for (key_at = 0; key_at < choice.variable_count; key_at++)
        collect_position_bucket(
          index, index->first_variable_keys[key_at], query, sign,
          exclude_id, &found, &visited, &live, &dead, &duplicates,
          have_second_choice ? &second_choice : NULL,
          have_second_choice ? index->refinement_path : NULL,
          have_third_choice ? &third_choice : NULL,
          have_third_choice ? index->selected_path : NULL);
    }
  }
  if (index->strategy == COMPACT_UNIT_ROOT_SCAN ||
      (index->strategy == COMPACT_UNIT_POSITION && !choice.found)) {
    if (index->strategy == COMPACT_UNIT_POSITION)
      index->position_fallback_queries++;
    for (i = index->unifier_heads[sign ? 1 : 0][query_root];
         i != CUI_NONE; i = index->records[i].next_root) {
      struct cui_record *record = &index->records[i];
      visited++;
      if (record->active)
        live++;
      else
        dead++;
      if (!record->active || record->sign != (unsigned char) sign ||
          record->proof_id == exclude_id ||
          query_slice_length(record->tokens) == 0 ||
          record->root_symbol != (unsigned) query_root)
        continue;
      index->unifier_exact_tests++;
      if (resident_unifies_record(index, query, record)) {
        append_result_id(index, record->proof_id, &found);
      }
    }
  }
  if (found > 1) {
    clock_start(index->sort_clock);
    qsort(index->result_ids, found, sizeof(*index->result_ids),
          descending_id_compare);
    clock_stop(index->sort_clock);
  }
  compact_profile_note(
    &index->unifier_profile,
    index->unifier_exact_tests - tests_before,
    visited, live, dead, duplicates, found, 0);
  compact_profile_note_exact(
    &index->unifier_profile,
    index->unifier_exact_tests - tests_before, found, 0);
  update_peak(index);
  compact_query_timer_stop(&index->unifier_timer);
  if (found == 0) {
    return NULL;
  }
  {
    unsigned long long *result = safe_malloc(found * sizeof(*result));
    memcpy(result, index->result_ids, found * sizeof(*result));
    *count = found;
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
  stats->strategy = index->strategy;
  stats->feature_depth = index->feature_depth;
  stats->active = index->active;
  stats->peak = index->peak;
  stats->retired = index->retired;
  stats->physical = index->record_count - 1;
  stats->compactions = index->compactions;
  stats->bytes_reclaimed = index->bytes_reclaimed;
  stats->generalization_queries = index->generalization_queries;
  stats->instance_queries = index->instance_queries;
  stats->instance_exact_tests = index->instance_exact_tests;
  stats->instance_tree_queries = index->instance_tree_queries;
  stats->instance_tree_nodes_examined = index->instance_tree_nodes_examined;
  stats->instance_tree_postings_examined =
    index->instance_tree_postings_examined;
  stats->unifier_queries = index->unifier_queries;
  stats->unifier_exact_tests = index->unifier_exact_tests;
  stats->position_queries = index->position_queries;
  stats->position_fallback_queries = index->position_fallback_queries;
  stats->position_postings_examined = index->position_postings_examined;
  stats->position_duplicate_postings = index->position_duplicate_postings;
  stats->code_tree_queries = index->code_tree_queries;
  stats->code_tree_nodes_examined = index->code_tree_nodes_examined;
  stats->code_tree_postings_examined = index->code_tree_postings_examined;
  stats->code_tree_variable_parents = index->code_tree_variable_parents;
  stats->code_tree_variable_children = index->code_tree_variable_children;
  stats->code_tree_pending_parents = index->code_tree_pending_parents;
  stats->code_tree_pending_children = index->code_tree_pending_children;
  stats->code_tree_rigid_parents = index->code_tree_rigid_parents;
  stats->code_tree_rigid_children = index->code_tree_rigid_children;
  stats->code_tree_rigid_sibling_checks =
    index->code_tree_rigid_sibling_checks;
  stats->adaptive_queries = index->adaptive_queries;
  stats->adaptive_tree_choices = index->adaptive_tree_choices;
  stats->adaptive_position_choices = index->adaptive_position_choices;
  stats->adaptive_position_empty_choices =
    index->adaptive_position_empty_choices;
  stats->position_refinement_queries = index->position_refinement_queries;
  stats->position_refinement_checks = index->position_refinement_checks;
  stats->position_refinement_rejects = index->position_refinement_rejects;
  stats->position_tertiary_queries = index->position_tertiary_queries;
  stats->position_tertiary_checks = index->position_tertiary_checks;
  stats->position_tertiary_rejects = index->position_tertiary_rejects;
  stats->adaptive_route_hits = index->adaptive_route_hits;
  stats->adaptive_route_misses = index->adaptive_route_misses;
  stats->adaptive_route_replacements = index->adaptive_route_replacements;
  stats->adaptive_route_bytes = index->adaptive_routes == NULL ? 0 :
    CUI_ADAPTIVE_ROUTE_CAPACITY * sizeof(*index->adaptive_routes);
  stats->feature_items = index->feature_bucket_count == 0 ? 0 :
    index->feature_bucket_count - 1;
  stats->feature_posting_items = index->feature_posting_count;
  stats->generalization_profile = index->generalization_profile;
  stats->instance_profile = index->instance_profile;
  stats->unifier_profile = index->unifier_profile;
  stats->generalization_seconds =
    index->generalization_timer.estimated_seconds;
  stats->instance_seconds = index->instance_timer.estimated_seconds;
  stats->unifier_seconds = index->unifier_timer.estimated_seconds;
  stats->generalization_timing_eligible =
    index->generalization_timer.eligible;
  stats->generalization_timing_samples =
    index->generalization_timer.samples;
  stats->instance_timing_eligible = index->instance_timer.eligible;
  stats->instance_timing_samples = index->instance_timer.samples;
  stats->unifier_timing_eligible = index->unifier_timer.eligible;
  stats->unifier_timing_samples = index->unifier_timer.samples;
  stats->timing_sample_rate = COMPACT_TIMING_SAMPLE_RATE;
  stats->sort_seconds = clock_seconds(index->sort_clock);
  stats->maintenance_seconds = clock_seconds(index->maintenance_clock);
  stats->node_items = index->node_count;
  stats->posting_items = index->posting_count;
  stats->node_bytes = index->node_capacity * sizeof(*index->nodes);
  stats->posting_bytes = index->posting_capacity * sizeof(*index->postings);
  stats->record_bytes = index->record_capacity * sizeof(*index->records);
  stats->root_bytes = index->unifier_symbol_capacity *
    (sizeof(*index->unifier_heads[0]) +
     sizeof(*index->unifier_heads[1]));
  stats->feature_bytes =
    index->feature_bucket_capacity * sizeof(*index->feature_buckets) +
    index->feature_hash_capacity * sizeof(*index->feature_hash) +
    index->feature_chunk_capacity * sizeof(*index->feature_chunks);
  stats->token_bytes = index->owns_term_pool ? terms.token_bytes : 0;
  stats->hash_bytes = compact_id_map_bytes(index->id_map);
  stats->scratch_bytes =
    index->query_capacity * sizeof(*index->query) +
    index->generalization_stack_capacity *
      sizeof(*index->generalization_stack) +
    index->result_capacity * sizeof(*index->result_ids) +
    index->path_capacity * sizeof(*index->path_stack) +
    index->child_capacity * sizeof(*index->child_stack) +
    index->selected_path_capacity * sizeof(*index->selected_path) +
    index->refinement_path_capacity * sizeof(*index->refinement_path) +
    index->selected_variable_capacity *
      sizeof(*index->selected_variable_keys) +
    index->first_variable_capacity * sizeof(*index->first_variable_keys);
  stats->total_bytes = index_bytes(index);
  stats->peak_bytes = index->peak_bytes;
}

unsigned long long compact_unit_index_active_records(Compact_unit_index index)
{
  return index == NULL ? 0 : index->active;
}

unsigned long long compact_unit_index_physical_records(
  Compact_unit_index index)
{
  return index == NULL || index->record_count == 0 ? 0 :
    index->record_count - 1;
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
  safe_free(index->feature_buckets);
  safe_free(index->feature_hash);
  safe_free(index->feature_chunks);
  if (index->owns_term_pool)
    compact_term_pool_free(index->term_pool);
  compact_id_map_free(index->id_map);
  safe_free(index->query);
  safe_free(index->generalization_stack);
  safe_free(index->result_ids);
  safe_free(index->path_stack);
  safe_free(index->child_stack);
  safe_free(index->selected_path);
  safe_free(index->refinement_path);
  safe_free(index->selected_variable_keys);
  safe_free(index->first_variable_keys);
  safe_free(index->adaptive_routes);
  free_clock(index->sort_clock);
  free_clock(index->maintenance_clock);
  safe_free(index);
}
