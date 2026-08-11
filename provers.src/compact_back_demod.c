#include "compact_back_demod.h"
#include "compact_id_map.h"

#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#define CBD_NONE 0U
#define CBD_POSTING_BLOCK_PAYLOAD 16
#define CBD_PATH_DEPTH 3

static unsigned Compaction_stale_pct = 25;
static Compact_back_demod_strategy Back_demod_strategy =
  COMPACT_BACK_DEMOD_MASK8;

typedef uint32_t cbd_path_mask;

struct cbd_posting_block {
  uint32_t next;
  uint16_t used;
  uint16_t count;
  unsigned char data[CBD_POSTING_BLOCK_PAYLOAD];
};

/* Occurrences with one root symbol and one structural signature share a
   posting chain.  mask8 preserves the original shallow 8-bit signature;
   signature32 uses two bits per rigid fact at arbitrary depth.  A query only
   opens buckets whose signature contains all of its fixed path features; hash
   collisions can add candidates but the structural matcher remains final. */
struct cbd_path_bucket {
  uint32_t next;
  uint32_t posting_head;
  uint32_t posting_tail;
  uint32_t inline_record;
  uint32_t inline_occurrence;
  uint32_t inline_length;
  uint32_t last_record;
  uint32_t last_occurrence;
  uint32_t symbol;
  cbd_path_mask mask;
};

/* Radix edges refer directly to the shared serialized-term pool.  Only
   terminal nodes allocate a posting-list descriptor, so common prefixes do
   not pay for empty posting metadata. */
struct cbd_tree_node {
  uint32_t token_offset;
  uint32_t token_length;
  uint32_t first_child;
  uint32_t next_sibling;
  uint32_t posting_list;
};

struct cbd_tree_posting_list {
  uint32_t posting_head;
  uint32_t posting_tail;
  uint32_t inline_record;
  uint32_t inline_occurrence;
  uint32_t inline_length;
  uint32_t last_record;
  uint32_t last_occurrence;
};

struct cbd_local_occurrence {
  uint32_t symbol;
  uint32_t offset;
  uint32_t length;
  cbd_path_mask path_mask;
};

struct cbd_query_term {
  Term term;
  uint32_t end;
};

struct cbd_record {
  unsigned long long proof_id;
  uint32_t token_offset;
  uint32_t token_length;
  uint32_t query_stamp;
  unsigned char active;
};

struct compact_back_demod_index {
  Compact_back_demod_strategy strategy;
  struct cbd_posting_block *posting_blocks;
  size_t posting_block_count;
  size_t posting_block_capacity;
  size_t posting_count;
  size_t posting_stream_used;
  uint32_t *symbol_buckets;
  size_t symbol_capacity;
  struct cbd_path_bucket *path_buckets;
  size_t path_bucket_count;
  size_t path_bucket_capacity;
  uint32_t *path_bucket_hash;
  size_t path_bucket_hash_capacity;
  struct cbd_tree_node *tree_nodes;
  size_t tree_node_count;
  size_t tree_node_capacity;
  struct cbd_tree_posting_list *tree_posting_lists;
  size_t tree_posting_list_count;
  size_t tree_posting_list_capacity;
  unsigned char *occurrences;
  size_t occurrence_count;
  size_t occurrence_capacity;
  struct cbd_record *records;
  size_t record_count;
  size_t record_capacity;
  Compact_term_pool term_pool;
  const int32_t *tokens;
  size_t token_limit;
  BOOL owns_term_pool;
  Compact_id_map id_map;
  unsigned long long *results;
  size_t result_capacity;
  struct cbd_query_term *query;
  size_t query_capacity;
  uint32_t query_stamp;
  unsigned long long active;
  unsigned long long peak;
  unsigned long long retired;
  unsigned long long compactions;
  unsigned long long bytes_reclaimed;
  unsigned long long queries;
  unsigned long long candidates;
  unsigned long long exact_tests;
  unsigned long long symbol_occurrences;
  unsigned long long posting_groups_examined;
  unsigned long long occurrences_examined;
  unsigned long long path_filter_checks;
  unsigned long long path_filter_rejects;
  unsigned long long tree_queries;
  unsigned long long tree_nodes_examined;
  unsigned long long inactive_groups_examined;
  unsigned long long duplicate_groups_examined;
  unsigned long long posting_bytes_decoded;
  unsigned long long query_work;
  unsigned long long query_live;
  unsigned long long query_dead;
  unsigned long long query_duplicates;
  unsigned long long query_bytes_decoded;
  unsigned long long worst_query_id;
  unsigned long long worst_query_groups;
  unsigned long long worst_query_occurrences;
  unsigned long long worst_query_candidates;
  struct compact_query_profile query_profile;
  Clock lookup_clock;
  Clock maintenance_clock;
  unsigned long long materialized_file_snapshots;
  unsigned long long materialized_snapshot_ids;
  unsigned long long peak_bytes;
};

struct cbd_symbol_set {
  struct cbd_local_occurrence occurrence_fixed[128];
  struct cbd_local_occurrence *occurrence_values;
  size_t occurrence_count;
  size_t occurrence_capacity;
};

static size_t grow_capacity(size_t current, size_t item_size,
                            const char *message)
{
  size_t next = current == 0 ? 64 : current * 2;
  if (next < current || next > SIZE_MAX / item_size)
    fatal_error((char *) message);
  return next;
}

static size_t grow_record_capacity(size_t current, size_t item_size,
                                   const char *message)
{
  size_t increment;
  size_t next;
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

#define ENSURE_ARRAY(index, field, count, capacity, message) do {       \
  if ((index)->count == (index)->capacity) {                            \
    (index)->capacity = grow_capacity((index)->capacity,                \
      sizeof(*(index)->field), (message));                              \
    (index)->field = safe_realloc((index)->field,                       \
      (index)->capacity * sizeof(*(index)->field));                     \
  }                                                                    \
} while (0)

static void ensure_records(Compact_back_demod_index index)
{
  if (index->record_count == index->record_capacity) {
    index->record_capacity = grow_record_capacity(
      index->record_capacity, sizeof(*index->records),
      "compact_back_demod: record overflow");
    index->records = safe_realloc(
      index->records, index->record_capacity * sizeof(*index->records));
  }
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

static unsigned long long index_bytes(Compact_back_demod_index index)
{
  struct compact_term_pool_stats terms;
  if (index == NULL)
    return 0;
  compact_term_pool_get_stats(index->term_pool, &terms);
  return sizeof(*index) +
    index->posting_block_capacity * sizeof(*index->posting_blocks) +
    index->symbol_capacity * sizeof(*index->symbol_buckets) +
    index->path_bucket_capacity * sizeof(*index->path_buckets) +
    index->path_bucket_hash_capacity * sizeof(*index->path_bucket_hash) +
    index->tree_node_capacity * sizeof(*index->tree_nodes) +
    index->tree_posting_list_capacity *
      sizeof(*index->tree_posting_lists) +
    index->occurrence_capacity * sizeof(*index->occurrences) +
    index->record_capacity * sizeof(*index->records) +
    (index->owns_term_pool ? terms.total_bytes : 0) +
    compact_id_map_bytes(index->id_map) +
    index->result_capacity * sizeof(*index->results) +
    index->query_capacity * sizeof(*index->query);
}

static void update_peak(Compact_back_demod_index index)
{
  unsigned long long bytes = index_bytes(index);
  if (bytes > index->peak_bytes)
    index->peak_bytes = bytes;
  if (index->active > index->peak)
    index->peak = index->active;
}

static uint32_t lookup_record(Compact_back_demod_index index,
                              unsigned long long proof_id)
{
  uint32_t value = CBD_NONE;
  return index != NULL &&
    compact_id_map_get(index->id_map, proof_id, &value) ? value : CBD_NONE;
}

static void ensure_symbols(Compact_back_demod_index index, unsigned symbol)
{
  size_t old_capacity;
  if ((size_t) symbol < index->symbol_capacity)
    return;
  old_capacity = index->symbol_capacity;
  while ((size_t) symbol >= index->symbol_capacity)
    index->symbol_capacity = grow_capacity(
      index->symbol_capacity, sizeof(*index->symbol_buckets),
      "compact_back_demod: symbol table overflow");
  index->symbol_buckets = safe_realloc(
    index->symbol_buckets,
    index->symbol_capacity * sizeof(*index->symbol_buckets));
  memset(index->symbol_buckets + old_capacity, 0,
         (index->symbol_capacity - old_capacity) *
           sizeof(*index->symbol_buckets));
}

static void ensure_occurrence_bytes(Compact_back_demod_index index,
                                    size_t extra)
{
  size_t needed;
  if (extra > SIZE_MAX - index->occurrence_count)
    fatal_error("compact_back_demod: occurrence stream overflow");
  needed = index->occurrence_count + extra;
  if (needed > UINT32_MAX)
    fatal_error("compact_back_demod: occurrence offsets exceed 32 bits");
  while (needed > index->occurrence_capacity) {
    index->occurrence_capacity = grow_record_capacity(
      index->occurrence_capacity, sizeof(*index->occurrences),
      "compact_back_demod: occurrence capacity overflow");
    index->occurrences = safe_realloc(
      index->occurrences,
      index->occurrence_capacity * sizeof(*index->occurrences));
  }
}

static void append_occurrence_delta(Compact_back_demod_index index,
                                    uint32_t delta)
{
  do {
    unsigned char byte = (unsigned char) (delta & 0x7fU);
    delta >>= 7;
    if (delta != 0)
      byte |= 0x80U;
    ensure_occurrence_bytes(index, 1);
    index->occurrences[index->occurrence_count++] = byte;
  } while (delta != 0);
}

static size_t encode_u32(unsigned char *destination, uint32_t value)
{
  size_t count = 0;
  do {
    unsigned char byte = (unsigned char) (value & 0x7fU);
    value >>= 7;
    if (value != 0)
      byte |= 0x80U;
    destination[count++] = byte;
  } while (value != 0);
  return count;
}

static uint32_t new_posting_block(Compact_back_demod_index index)
{
  uint32_t block;
  if (index->posting_block_count == index->posting_block_capacity) {
    index->posting_block_capacity = grow_record_capacity(
      index->posting_block_capacity, sizeof(*index->posting_blocks),
      "compact_back_demod: posting block overflow");
    index->posting_blocks = safe_realloc(
      index->posting_blocks,
      index->posting_block_capacity * sizeof(*index->posting_blocks));
  }
  if (index->posting_block_count > UINT32_MAX)
    fatal_error("compact_back_demod: posting block offsets exceed 32 bits");
  block = (uint32_t) index->posting_block_count++;
  memset(&index->posting_blocks[block], 0,
         sizeof(index->posting_blocks[block]));
  return block;
}

static uint64_t path_bucket_key(uint32_t symbol, cbd_path_mask mask)
{
  return hash_id(((uint64_t) symbol << 32) ^ hash_id(mask));
}

static size_t path_bucket_hash_slot(Compact_back_demod_index index,
                                    uint32_t symbol, cbd_path_mask mask)
{
  size_t at = (size_t) hash_id(path_bucket_key(symbol, mask)) &
    (index->path_bucket_hash_capacity - 1);
  for (;;) {
    uint32_t bucket = index->path_bucket_hash[at];
    if (bucket == CBD_NONE ||
        (index->path_buckets[bucket].symbol == symbol &&
         index->path_buckets[bucket].mask == mask))
      return at;
    at = (at + 1) & (index->path_bucket_hash_capacity - 1);
  }
}

static void rehash_path_buckets(Compact_back_demod_index index,
                                size_t capacity)
{
  uint32_t *old_hash = index->path_bucket_hash;
  size_t i;
  index->path_bucket_hash = safe_calloc(
    capacity, sizeof(*index->path_bucket_hash));
  index->path_bucket_hash_capacity = capacity;
  for (i = 1; i < index->path_bucket_count; i++) {
    struct cbd_path_bucket *bucket = &index->path_buckets[i];
    size_t at = path_bucket_hash_slot(
      index, bucket->symbol, bucket->mask);
    index->path_bucket_hash[at] = (uint32_t) i;
  }
  safe_free(old_hash);
}

static uint32_t find_or_add_path_bucket(Compact_back_demod_index index,
                                        uint32_t symbol,
                                        cbd_path_mask mask)
{
  size_t at;
  uint32_t bucket;
  ensure_symbols(index, symbol);
  if (index->path_bucket_hash_capacity == 0)
    rehash_path_buckets(index, 128);
  else if ((index->path_bucket_count + 1) * 20 >=
           index->path_bucket_hash_capacity * 17) {
    if (index->path_bucket_hash_capacity > SIZE_MAX / 2)
      fatal_error("compact_back_demod: path bucket hash overflow");
    rehash_path_buckets(index, index->path_bucket_hash_capacity * 2);
  }
  at = path_bucket_hash_slot(index, symbol, mask);
  bucket = index->path_bucket_hash[at];
  if (bucket != CBD_NONE)
    return bucket;
  if (index->path_bucket_count == index->path_bucket_capacity) {
    index->path_bucket_capacity =
      index->strategy == COMPACT_BACK_DEMOD_SIGNATURE32 ?
        grow_record_capacity(
          index->path_bucket_capacity, sizeof(*index->path_buckets),
          "compact_back_demod: path bucket overflow") :
        grow_capacity(
          index->path_bucket_capacity, sizeof(*index->path_buckets),
          "compact_back_demod: path bucket overflow");
    index->path_buckets = safe_realloc(
      index->path_buckets,
      index->path_bucket_capacity * sizeof(*index->path_buckets));
  }
  if (index->path_bucket_count > UINT32_MAX)
    fatal_error("compact_back_demod: path bucket offsets exceed 32 bits");
  bucket = (uint32_t) index->path_bucket_count++;
  memset(&index->path_buckets[bucket], 0,
         sizeof(index->path_buckets[bucket]));
  index->path_buckets[bucket].symbol = symbol;
  index->path_buckets[bucket].mask = mask;
  index->path_buckets[bucket].next = index->symbol_buckets[symbol];
  index->symbol_buckets[symbol] = bucket;
  index->path_bucket_hash[at] = bucket;
  return bucket;
}

static int tree_code_compare(int32_t a, int32_t b)
{
  BOOL av = a < 0, bv = b < 0;
  if (av != bv)
    return av ? -1 : 1;
  return a < b ? -1 : a > b ? 1 : 0;
}

static uint32_t new_tree_node(Compact_back_demod_index index,
                              uint32_t token_offset,
                              uint32_t token_length)
{
  uint32_t node;
  if (index->tree_node_count == index->tree_node_capacity) {
    index->tree_node_capacity = grow_record_capacity(
      index->tree_node_capacity, sizeof(*index->tree_nodes),
      "compact_back_demod: tree node overflow");
    index->tree_nodes = safe_realloc(
      index->tree_nodes,
      index->tree_node_capacity * sizeof(*index->tree_nodes));
  }
  if (index->tree_node_count > UINT32_MAX)
    fatal_error("compact_back_demod: tree node offsets exceed 32 bits");
  node = (uint32_t) index->tree_node_count++;
  memset(&index->tree_nodes[node], 0, sizeof(index->tree_nodes[node]));
  index->tree_nodes[node].token_offset = token_offset;
  index->tree_nodes[node].token_length = token_length;
  return node;
}

static int32_t tree_first_code(Compact_back_demod_index index,
                               uint32_t node)
{
  struct cbd_tree_node *n = &index->tree_nodes[node];
  if (n->token_length == 0)
    fatal_error("compact_back_demod: empty nonroot tree edge");
  return index->tokens[n->token_offset];
}

static uint32_t insert_tree_path(Compact_back_demod_index index,
                                 uint32_t offset, uint32_t length)
{
  uint32_t parent = CBD_NONE;
  uint32_t position = 0;
  while (position < length) {
    uint32_t current = index->tree_nodes[parent].first_child;
    uint32_t previous = CBD_NONE;
    int32_t wanted = index->tokens[offset + position];
    while (current != CBD_NONE &&
           tree_code_compare(tree_first_code(index, current), wanted) < 0) {
      previous = current;
      current = index->tree_nodes[current].next_sibling;
    }
    if (current == CBD_NONE || tree_first_code(index, current) != wanted) {
      uint32_t added = new_tree_node(index, offset + position,
                                     length - position);
      if (previous == CBD_NONE) {
        index->tree_nodes[added].next_sibling =
          index->tree_nodes[parent].first_child;
        index->tree_nodes[parent].first_child = added;
      }
      else {
        index->tree_nodes[added].next_sibling =
          index->tree_nodes[previous].next_sibling;
        index->tree_nodes[previous].next_sibling = added;
      }
      return added;
    }
    else {
      uint32_t old_offset = index->tree_nodes[current].token_offset;
      uint32_t old_length = index->tree_nodes[current].token_length;
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
        uint32_t old_next = index->tree_nodes[current].next_sibling;
        uint32_t split = new_tree_node(index, old_offset, common);
        uint32_t added;
        if (common == 0)
          fatal_error("compact_back_demod: invalid zero-length tree split");
        index->tree_nodes[split].next_sibling = old_next;
        if (previous == CBD_NONE)
          index->tree_nodes[parent].first_child = split;
        else
          index->tree_nodes[previous].next_sibling = split;
        index->tree_nodes[current].token_offset += common;
        index->tree_nodes[current].token_length -= common;
        index->tree_nodes[current].next_sibling = CBD_NONE;
        index->tree_nodes[split].first_child = current;
        position += common;
        if (position == length)
          return split;
        added = new_tree_node(index, offset + position, length - position);
        if (tree_code_compare(tree_first_code(index, added),
                              tree_first_code(index, current)) < 0) {
          index->tree_nodes[added].next_sibling = current;
          index->tree_nodes[split].first_child = added;
        }
        else
          index->tree_nodes[current].next_sibling = added;
        return added;
      }
    }
  }
  return parent;
}

static uint32_t new_tree_posting_list(Compact_back_demod_index index)
{
  uint32_t list;
  if (index->tree_posting_list_count ==
      index->tree_posting_list_capacity) {
    index->tree_posting_list_capacity = grow_record_capacity(
      index->tree_posting_list_capacity,
      sizeof(*index->tree_posting_lists),
      "compact_back_demod: tree posting-list overflow");
    index->tree_posting_lists = safe_realloc(
      index->tree_posting_lists,
      index->tree_posting_list_capacity *
        sizeof(*index->tree_posting_lists));
  }
  if (index->tree_posting_list_count > UINT32_MAX)
    fatal_error("compact_back_demod: tree posting-list offsets exceed 32 bits");
  list = (uint32_t) index->tree_posting_list_count++;
  memset(&index->tree_posting_lists[list], 0,
         sizeof(index->tree_posting_lists[list]));
  return list;
}

static void append_symbol_record(Compact_back_demod_index index,
                                 uint32_t record, unsigned symbol,
                                 cbd_path_mask mask,
                                 uint32_t occurrence_offset,
                                 uint32_t occurrence_length)
{
  unsigned char encoded[15];
  size_t length = 0;
  uint32_t bucket_index, block;
  struct cbd_path_bucket *bucket;
  struct cbd_posting_block *tail;
  bucket_index = find_or_add_path_bucket(index, symbol, mask);
  bucket = &index->path_buckets[bucket_index];
  if (record <= bucket->last_record)
    fatal_error("compact_back_demod: nonmonotone posting record");
  if (occurrence_offset < bucket->last_occurrence)
    fatal_error("compact_back_demod: nonmonotone posting occurrence");
  if (occurrence_length == 0)
    fatal_error("compact_back_demod: empty posting occurrence list");
  if (bucket->inline_length == 0) {
    bucket->inline_record = record;
    bucket->inline_occurrence = occurrence_offset;
    bucket->inline_length = occurrence_length;
    bucket->last_record = record;
    bucket->last_occurrence = occurrence_offset;
    index->posting_count++;
    return;
  }
  length += encode_u32(encoded + length,
                       record - bucket->last_record);
  length += encode_u32(
    encoded + length,
    occurrence_offset - bucket->last_occurrence);
  length += encode_u32(encoded + length, occurrence_length);
  if (length > CBD_POSTING_BLOCK_PAYLOAD)
    fatal_error("compact_back_demod: oversized posting entry");
  block = bucket->posting_tail;
  if (block == CBD_NONE ||
      index->posting_blocks[block].used + length >
        CBD_POSTING_BLOCK_PAYLOAD) {
    uint32_t added = new_posting_block(index);
    if (block == CBD_NONE)
      bucket->posting_head = added;
    else
      index->posting_blocks[block].next = added;
    bucket->posting_tail = added;
    block = added;
  }
  tail = &index->posting_blocks[block];
  memcpy(tail->data + tail->used, encoded, length);
  tail->used += (uint16_t) length;
  tail->count++;
  bucket->last_record = record;
  bucket->last_occurrence = occurrence_offset;
  index->posting_count++;
  index->posting_stream_used += length;
}

static void append_tree_record(Compact_back_demod_index index,
                               uint32_t record, uint32_t token_offset,
                               uint32_t token_length,
                               uint32_t occurrence_offset,
                               uint32_t occurrence_length)
{
  unsigned char encoded[15];
  size_t length = 0;
  uint32_t node = insert_tree_path(index, token_offset, token_length);
  uint32_t block;
  struct cbd_tree_posting_list *list;
  struct cbd_posting_block *tail;
  if (index->tree_nodes[node].posting_list == CBD_NONE)
    index->tree_nodes[node].posting_list = new_tree_posting_list(index);
  list = &index->tree_posting_lists[index->tree_nodes[node].posting_list];
  if (record <= list->last_record)
    fatal_error("compact_back_demod: nonmonotone tree posting record");
  if (occurrence_offset < list->last_occurrence)
    fatal_error("compact_back_demod: nonmonotone tree posting occurrence");
  if (occurrence_length == 0)
    fatal_error("compact_back_demod: empty tree occurrence list");
  if (list->inline_length == 0) {
    list->inline_record = record;
    list->inline_occurrence = occurrence_offset;
    list->inline_length = occurrence_length;
    list->last_record = record;
    list->last_occurrence = occurrence_offset;
    index->posting_count++;
    return;
  }
  length += encode_u32(encoded + length, record - list->last_record);
  length += encode_u32(
    encoded + length, occurrence_offset - list->last_occurrence);
  length += encode_u32(encoded + length, occurrence_length);
  if (length > CBD_POSTING_BLOCK_PAYLOAD)
    fatal_error("compact_back_demod: oversized tree posting entry");
  block = list->posting_tail;
  if (block == CBD_NONE ||
      index->posting_blocks[block].used + length >
        CBD_POSTING_BLOCK_PAYLOAD) {
    uint32_t added = new_posting_block(index);
    if (block == CBD_NONE)
      list->posting_head = added;
    else
      index->posting_blocks[block].next = added;
    list->posting_tail = added;
    block = added;
  }
  tail = &index->posting_blocks[block];
  memcpy(tail->data + tail->used, encoded, length);
  tail->used += (uint16_t) length;
  tail->count++;
  list->last_record = record;
  list->last_occurrence = occurrence_offset;
  index->posting_count++;
  index->posting_stream_used += length;
}

static uint64_t signature_child_path(uint64_t path, unsigned child)
{
  return hash_id(path ^ (UINT64_C(0x9e3779b97f4a7c15) + child));
}

static cbd_path_mask path_feature_bits(Compact_back_demod_index index,
                                       uint64_t path, uint32_t symbol)
{
  if (index->strategy == COMPACT_BACK_DEMOD_MASK8) {
    uint32_t mixed = (uint32_t) path * UINT32_C(0x9e3779b1) ^
                     symbol * UINT32_C(0x85ebca6b);
    mixed ^= mixed >> 16;
    return UINT64_C(1) << (mixed % 8U);
  }
  else {
    uint64_t mixed = hash_id(
      path ^ ((uint64_t) symbol * UINT64_C(0x85ebca77c2b2ae63)));
    unsigned first = (unsigned) (mixed & 31U);
    unsigned second = (unsigned) ((mixed >> 32) & 31U);
    if (second == first)
      second = (second + 15U) & 31U;
    return (UINT32_C(1) << first) | (UINT32_C(1) << second);
  }
}

static cbd_path_mask token_path_mask_rec(Compact_back_demod_index index,
                                         uint32_t *position, unsigned depth,
                                         uint64_t path)
{
  int32_t code;
  int i, arity;
  cbd_path_mask mask = 0;
  if ((size_t) *position >= index->token_limit)
    fatal_error("compact_back_demod: corrupt path token offset");
  code = index->tokens[(*position)++];
  arity = code < 0 ? 0 : sn_to_arity(code);
  for (i = 0; i < arity; i++) {
    uint64_t child_path =
      index->strategy == COMPACT_BACK_DEMOD_MASK8 ?
        (uint32_t) path * 17U + (uint32_t) i + 1U :
        signature_child_path(path, (unsigned) i);
    if ((size_t) *position >= index->token_limit)
      fatal_error("compact_back_demod: corrupt path child offset");
    if ((index->strategy == COMPACT_BACK_DEMOD_SIGNATURE32 ||
         depth < CBD_PATH_DEPTH) && index->tokens[*position] >= 0)
      mask |= path_feature_bits(
        index,
        child_path, (uint32_t) index->tokens[*position]);
    mask |= token_path_mask_rec(index, position, depth + 1, child_path);
  }
  return index->strategy == COMPACT_BACK_DEMOD_SIGNATURE32 ||
         depth < CBD_PATH_DEPTH ? mask : 0;
}

static cbd_path_mask token_path_mask(Compact_back_demod_index index,
                                     uint32_t offset)
{
  uint32_t position = offset;
  return token_path_mask_rec(index, &position, 0, 0);
}

static cbd_path_mask term_path_mask_rec(Term term, unsigned depth,
                                        uint64_t path,
                                        Compact_back_demod_index index)
{
  cbd_path_mask mask = 0;
  int i;
  if (VARIABLE(term) ||
      (index->strategy == COMPACT_BACK_DEMOD_MASK8 &&
       depth >= CBD_PATH_DEPTH))
    return 0;
  for (i = 0; i < ARITY(term); i++) {
    Term child = ARG(term, i);
    uint64_t child_path =
      index->strategy == COMPACT_BACK_DEMOD_MASK8 ?
        (uint32_t) path * 17U + (uint32_t) i + 1U :
        signature_child_path(path, (unsigned) i);
    if (!VARIABLE(child)) {
      mask |= path_feature_bits(index, child_path,
                                (uint32_t) SYMNUM(child));
      mask |= term_path_mask_rec(child, depth + 1, child_path, index);
    }
  }
  return mask;
}

static cbd_path_mask term_path_mask(Compact_back_demod_index index, Term term)
{
  return term_path_mask_rec(term, 0, 0, index);
}

static uint32_t token_term_end(Compact_back_demod_index index,
                               uint32_t position);

static void note_symbol(struct cbd_symbol_set *set, uint32_t symbol,
                        uint32_t offset, uint32_t length,
                        cbd_path_mask path_mask)
{
  if (set->occurrence_count == set->occurrence_capacity) {
    size_t next = grow_capacity(
      set->occurrence_capacity, sizeof(*set->occurrence_values),
      "compact_back_demod: local occurrence overflow");
    if (set->occurrence_values == set->occurrence_fixed) {
      set->occurrence_values = safe_malloc(
        next * sizeof(*set->occurrence_values));
      memcpy(set->occurrence_values, set->occurrence_fixed,
             set->occurrence_count * sizeof(*set->occurrence_values));
    }
    else
      set->occurrence_values = safe_realloc(
        set->occurrence_values, next * sizeof(*set->occurrence_values));
    set->occurrence_capacity = next;
  }
  set->occurrence_values[set->occurrence_count].symbol = symbol;
  set->occurrence_values[set->occurrence_count].offset = offset;
  set->occurrence_values[set->occurrence_count].length = length;
  set->occurrence_values[set->occurrence_count].path_mask = path_mask;
  set->occurrence_count++;
}

static void collect_term_slice(Compact_back_demod_index index,
                               uint32_t offset, uint32_t length,
                               uint32_t base,
                               struct cbd_symbol_set *symbols)
{
  uint32_t i;
  for (i = 0; i < length; i++) {
    int32_t code = index->tokens[offset + i];
    if (code >= 0) {
      uint32_t term_end = token_term_end(index, offset + i);
      note_symbol(symbols, (uint32_t) code,
                  offset + i - base,
                  term_end - (offset + i),
                  index->strategy == COMPACT_BACK_DEMOD_CODE_TREE ? 0 :
                    token_path_mask(index, offset + i));
      index->symbol_occurrences++;
    }
  }
}

static int increasing_local_bucket(const void *left, const void *right)
{
  const struct cbd_local_occurrence *a = left;
  const struct cbd_local_occurrence *b = right;
  if (a->symbol != b->symbol)
    return a->symbol < b->symbol ? -1 : 1;
  if (a->path_mask != b->path_mask)
    return a->path_mask < b->path_mask ? -1 : 1;
  if (a->offset != b->offset)
    return a->offset < b->offset ? -1 : 1;
  return 0;
}

static int compare_tree_occurrence(Compact_back_demod_index index,
                                   uint32_t base,
                                   const struct cbd_local_occurrence *a,
                                   const struct cbd_local_occurrence *b)
{
  uint32_t common = a->length < b->length ? a->length : b->length;
  uint32_t i;
  for (i = 0; i < common; i++) {
    int32_t ac = index->tokens[base + a->offset + i];
    int32_t bc = index->tokens[base + b->offset + i];
    if (ac != bc)
      return ac < bc ? -1 : 1;
  }
  if (a->length != b->length)
    return a->length < b->length ? -1 : 1;
  return a->offset < b->offset ? -1 : a->offset > b->offset ? 1 : 0;
}

static void sift_tree_occurrences(Compact_back_demod_index index,
                                  uint32_t base,
                                  struct cbd_local_occurrence *values,
                                  size_t root, size_t count)
{
  for (;;) {
    size_t child;
    struct cbd_local_occurrence saved;
    if (root > (SIZE_MAX - 1) / 2)
      return;
    child = root * 2 + 1;
    if (child >= count)
      return;
    if (child + 1 < count &&
        compare_tree_occurrence(index, base, &values[child],
                                &values[child + 1]) < 0)
      child++;
    if (compare_tree_occurrence(index, base, &values[root],
                                &values[child]) >= 0)
      return;
    saved = values[root];
    values[root] = values[child];
    values[child] = saved;
    root = child;
  }
}

static void sort_tree_occurrences(Compact_back_demod_index index,
                                  uint32_t base,
                                  struct cbd_local_occurrence *values,
                                  size_t count)
{
  size_t start, end;
  if (count < 2)
    return;
  for (start = count / 2; start != 0; start--)
    sift_tree_occurrences(index, base, values, start - 1, count);
  for (end = count - 1; end != 0; end--) {
    struct cbd_local_occurrence saved = values[0];
    values[0] = values[end];
    values[end] = saved;
    sift_tree_occurrences(index, base, values, 0, end);
  }
}

static Compact_back_demod_index compact_back_demod_init_with_pool_strategy(
  Compact_term_pool pool, Compact_back_demod_strategy strategy)
{
  Compact_back_demod_index index = safe_calloc(1, sizeof(*index));
  if (pool == NULL)
    fatal_error("compact_back_demod_init_with_pool: null term pool");
  index->term_pool = pool;
  index->strategy = strategy;
  index->tokens = compact_term_pool_tokens(pool);
  index->id_map = compact_id_map_init(1);
  index->lookup_clock = clock_init("compact_back_demod_lookup");
  index->maintenance_clock = clock_init("compact_back_demod_maintenance");
  index->token_limit = compact_term_pool_token_count(pool);
  if (new_posting_block(index) != CBD_NONE)
    fatal_error("compact_back_demod: invalid posting block sentinel");
  if (strategy == COMPACT_BACK_DEMOD_CODE_TREE) {
    if (new_tree_node(index, 0, 0) != CBD_NONE ||
        new_tree_posting_list(index) != CBD_NONE)
      fatal_error("compact_back_demod: invalid tree sentinel");
  }
  ENSURE_ARRAY(index, path_buckets, path_bucket_count,
               path_bucket_capacity,
               "compact_back_demod: path bucket overflow");
  memset(&index->path_buckets[0], 0, sizeof(index->path_buckets[0]));
  index->path_bucket_count = 1;
  ensure_records(index);
  memset(&index->records[0], 0, sizeof(index->records[0]));
  index->record_count = 1;
  update_peak(index);
  return index;
}

Compact_back_demod_index compact_back_demod_init_with_pool(
  Compact_term_pool pool)
{
  return compact_back_demod_init_with_pool_strategy(
    pool, Back_demod_strategy);
}

Compact_back_demod_index compact_back_demod_init(void)
{
  Compact_term_pool pool = compact_term_pool_init();
  Compact_back_demod_index index =
    compact_back_demod_init_with_pool(pool);
  index->owns_term_pool = TRUE;
  update_peak(index);
  return index;
}

void compact_back_demod_set_strategy(Compact_back_demod_strategy strategy)
{
  if (strategy != COMPACT_BACK_DEMOD_MASK8 &&
      strategy != COMPACT_BACK_DEMOD_SIGNATURE32 &&
      strategy != COMPACT_BACK_DEMOD_CODE_TREE)
    fatal_error("compact_back_demod: invalid strategy");
  Back_demod_strategy = strategy;
}

BOOL compact_back_demod_add(Compact_back_demod_index index, Topform clause)
{
  uint32_t record_index;
  struct cbd_record *record;
  struct cbd_symbol_set symbols;
  Literals literal;
  uint32_t token_end = 0;
  BOOL have_tokens = FALSE;
  if (index == NULL || clause == NULL || clause->id == 0 ||
      clause->literals == NULL ||
      lookup_record(index, clause->id) != CBD_NONE)
    return FALSE;
  ensure_records(index);
  if (index->record_count > UINT32_MAX)
    fatal_error("compact_back_demod: record offsets exceed 32 bits");
  record_index = (uint32_t) index->record_count++;
  record = &index->records[record_index];
  memset(record, 0, sizeof(*record));
  record->proof_id = clause->id;
  record->active = TRUE;
  memset(&symbols, 0, sizeof(symbols));
  symbols.occurrence_values = symbols.occurrence_fixed;
  symbols.occurrence_capacity = sizeof(symbols.occurrence_fixed) /
    sizeof(symbols.occurrence_fixed[0]);
  for (literal = clause->literals; literal != NULL; literal = literal->next) {
    Term atom = literal->atom;
    int arg;
    for (arg = 0; arg < ARITY(atom); arg++) {
      uint32_t length;
      uint32_t offset = compact_term_pool_intern(
        index->term_pool, clause->id, clause->literals, ARG(atom, arg),
        &length);
      index->tokens = compact_term_pool_tokens(index->term_pool);
      index->token_limit = compact_term_pool_token_count(index->term_pool);
      if (!have_tokens) {
        record->token_offset = offset;
        token_end = offset + length;
        have_tokens = TRUE;
      }
      else {
        if (offset < record->token_offset)
          fatal_error("compact_back_demod: nonmonotone pooled clause slice");
        if (offset + length > token_end)
          token_end = offset + length;
      }
      collect_term_slice(index, offset, length, record->token_offset,
                         &symbols);
    }
  }
  record->token_length = have_tokens ? token_end - record->token_offset : 0;
  if (index->strategy == COMPACT_BACK_DEMOD_CODE_TREE)
    sort_tree_occurrences(index, record->token_offset,
                          symbols.occurrence_values,
                          symbols.occurrence_count);
  else
    qsort(symbols.occurrence_values, symbols.occurrence_count,
          sizeof(*symbols.occurrence_values), increasing_local_bucket);
  if (index->strategy == COMPACT_BACK_DEMOD_CODE_TREE) {
    size_t i = 0;
    while (i < symbols.occurrence_count) {
      size_t j = i;
      uint32_t relative = symbols.occurrence_values[i].offset;
      uint32_t length = symbols.occurrence_values[i].length;
      uint32_t previous = 0;
      uint32_t occurrence_offset = (uint32_t) index->occurrence_count;
      BOOL first = TRUE;
      while (j < symbols.occurrence_count &&
             symbols.occurrence_values[j].length == length &&
             memcmp(index->tokens + record->token_offset + relative,
                    index->tokens + record->token_offset +
                      symbols.occurrence_values[j].offset,
                    (size_t) length * sizeof(*index->tokens)) == 0) {
        uint32_t offset = symbols.occurrence_values[j].offset;
        if (first || offset != previous) {
          append_occurrence_delta(
            index, first ? offset : offset - previous);
          previous = offset;
          first = FALSE;
        }
        j++;
      }
      append_tree_record(
        index, record_index, record->token_offset + relative, length,
        occurrence_offset,
        (uint32_t) index->occurrence_count - occurrence_offset);
      i = j;
    }
  }
  else {
    size_t i = 0;
    while (i < symbols.occurrence_count) {
      size_t j = i;
      uint32_t symbol = symbols.occurrence_values[i].symbol;
      cbd_path_mask mask = symbols.occurrence_values[i].path_mask;
      uint32_t previous = 0;
      uint32_t occurrence_offset = (uint32_t) index->occurrence_count;
      BOOL first = TRUE;
      while (j < symbols.occurrence_count &&
             symbols.occurrence_values[j].symbol == symbol &&
             symbols.occurrence_values[j].path_mask == mask) {
        uint32_t offset = symbols.occurrence_values[j].offset;
        if (first || offset != previous) {
          append_occurrence_delta(
            index, first ? offset : offset - previous);
          previous = offset;
          first = FALSE;
        }
        j++;
      }
      append_symbol_record(
        index, record_index, symbol, mask, occurrence_offset,
        (uint32_t) index->occurrence_count - occurrence_offset);
      i = j;
    }
  }
  if (symbols.occurrence_values != symbols.occurrence_fixed)
    safe_free(symbols.occurrence_values);
  if (!compact_id_map_put(index->id_map, clause->id, &record_index))
    fatal_error("compact_back_demod: duplicate proof ID");
  index->active++;
  update_peak(index);
  return TRUE;
}

BOOL compact_back_demod_remove(Compact_back_demod_index index,
                               unsigned long long proof_id)
{
  uint32_t record = lookup_record(index, proof_id);
  if (record == CBD_NONE || !index->records[record].active)
    return FALSE;
  index->records[record].active = FALSE;
  if (!compact_id_map_remove(index->id_map, proof_id))
    fatal_error("compact_back_demod: missing proof ID on removal");
  index->active--;
  index->retired++;
  return TRUE;
}

static void ensure_results(Compact_back_demod_index index, size_t needed)
{
  while (needed > index->result_capacity) {
    index->result_capacity = grow_capacity(
      index->result_capacity, sizeof(*index->results),
      "compact_back_demod: result overflow");
    index->results = safe_realloc(
      index->results, index->result_capacity * sizeof(*index->results));
  }
}

static void begin_query(Compact_back_demod_index index)
{
  size_t i;
  index->query_stamp++;
  if (index->query_stamp != 0)
    return;
  for (i = 1; i < index->record_count; i++)
    index->records[i].query_stamp = 0;
  index->query_stamp = 1;
}

static void collect_record(Compact_back_demod_index index, uint32_t at,
                           unsigned long long exclude_id, size_t *count)
{
  struct cbd_record *record = &index->records[at];
  if (!record->active || record->proof_id == exclude_id ||
      record->query_stamp == index->query_stamp)
    return;
  record->query_stamp = index->query_stamp;
  ensure_results(index, *count + 1);
  index->results[(*count)++] = record->proof_id;
}

static uint32_t token_term_end(Compact_back_demod_index index,
                               uint32_t position)
{
  int32_t code;
  int i, arity;
  if ((size_t) position >= index->token_limit)
    fatal_error("compact_back_demod: corrupt term token offset");
  code = index->tokens[position++];
  arity = code < 0 ? 0 : sn_to_arity(code);
  for (i = 0; i < arity; i++)
    position = token_term_end(index, position);
  return position;
}

struct cbd_binding {
  uint32_t offset;
  uint32_t end;
  unsigned char bound;
};

static BOOL match_token_term(Compact_back_demod_index index, Term pattern,
                             uint32_t *position,
                             struct cbd_binding *bindings)
{
  uint32_t at = *position;
  int i;
  if ((size_t) at >= index->token_limit)
    fatal_error("compact_back_demod: corrupt match token offset");
  if (VARIABLE(pattern)) {
    int variable = VARNUM(pattern);
    uint32_t end;
    if (variable < 0 || variable >= MAX_VARS)
      fatal_error("compact_back_demod: pattern variable exceeds MAX_VARS");
    end = token_term_end(index, at);
    if (bindings[variable].bound) {
      size_t old_length = bindings[variable].end - bindings[variable].offset;
      size_t new_length = end - at;
      if (old_length != new_length ||
          memcmp(index->tokens + bindings[variable].offset,
                 index->tokens + at,
                 new_length * sizeof(*index->tokens)) != 0)
        return FALSE;
    }
    else {
      bindings[variable].offset = at;
      bindings[variable].end = end;
      bindings[variable].bound = TRUE;
    }
    *position = end;
    return TRUE;
  }
  if (index->tokens[at] < 0 || index->tokens[at] != SYMNUM(pattern))
    return FALSE;
  *position = at + 1;
  for (i = 0; i < ARITY(pattern); i++)
    if (!match_token_term(index, ARG(pattern, i), position, bindings))
      return FALSE;
  return TRUE;
}

static BOOL occurrence_matches(Compact_back_demod_index index,
                               Term pattern, uint32_t token_offset)
{
  struct cbd_binding bindings[MAX_VARS];
  uint32_t position = token_offset;
  memset(bindings, 0, sizeof(bindings));
  return match_token_term(index, pattern, &position, bindings);
}

static uint32_t decode_occurrence_delta(Compact_back_demod_index index,
                                        uint32_t *position, uint32_t end)
{
  uint32_t value = 0;
  unsigned shift = 0;
  while (*position < end) {
    unsigned char byte = index->occurrences[(*position)++];
    if (shift == 28 && (byte & 0xf0U) != 0)
      fatal_error("compact_back_demod: corrupt occurrence delta");
    value |= (uint32_t) (byte & 0x7fU) << shift;
    if ((byte & 0x80U) == 0)
      return value;
    shift += 7;
    if (shift > 28)
      fatal_error("compact_back_demod: corrupt occurrence delta");
  }
  fatal_error("compact_back_demod: truncated occurrence delta");
  return 0;
}

/* A symbol posting names each clause once.  Its delta-varint stream retains
   just the matching subterm offsets, so exact probes avoid rescanning the
   rest of a large equational clause. */
static BOOL posting_contains_pattern(Compact_back_demod_index index,
                                     uint32_t occurrence_offset,
                                     uint32_t occurrence_length,
                                     struct cbd_record *record,
                                     Term pattern, int32_t symbol)
{
  uint32_t position = occurrence_offset;
  uint32_t end;
  uint32_t relative = 0;
  if (occurrence_length > UINT32_MAX - occurrence_offset ||
      occurrence_offset + occurrence_length > index->occurrence_count)
    fatal_error("compact_back_demod: corrupt posting occurrence range");
  end = occurrence_offset + occurrence_length;
  while (position < end) {
    uint32_t delta = decode_occurrence_delta(index, &position, end);
    index->occurrences_examined++;
    if (delta > UINT32_MAX - relative)
      fatal_error("compact_back_demod: occurrence offset overflow");
    relative += delta;
    if (relative >= record->token_length ||
        index->tokens[record->token_offset + relative] != symbol)
      fatal_error("compact_back_demod: corrupt occurrence offset");
    if (occurrence_matches(index, pattern,
                           record->token_offset + relative))
      return TRUE;
  }
  return FALSE;
}

static uint32_t decode_posting_value(const struct cbd_posting_block *block,
                                     uint16_t *position)
{
  uint32_t value = 0;
  unsigned shift = 0;
  while (*position < block->used) {
    unsigned char byte = block->data[(*position)++];
    if (shift == 28 && (byte & 0xf0U) != 0)
      fatal_error("compact_back_demod: corrupt posting value");
    value |= (uint32_t) (byte & 0x7fU) << shift;
    if ((byte & 0x80U) == 0)
      return value;
    shift += 7;
    if (shift > 28)
      fatal_error("compact_back_demod: corrupt posting value");
  }
  fatal_error("compact_back_demod: truncated posting value");
  return 0;
}

static void examine_posting_group(
  Compact_back_demod_index index, uint32_t record_index,
  uint32_t occurrence_offset, uint32_t occurrence_length,
  Term pattern, int32_t symbol, unsigned long long exclude_id,
  size_t *count)
{
  struct cbd_record *record;
  if (record_index == CBD_NONE || record_index >= index->record_count)
    fatal_error("compact_back_demod: corrupt posting record");
  record = &index->records[record_index];
  index->posting_groups_examined++;
  index->query_work++;
  if (record->active)
    index->query_live++;
  else {
    index->query_dead++;
    index->inactive_groups_examined++;
  }
  if (record->active && record->query_stamp == index->query_stamp) {
    index->query_duplicates++;
    index->duplicate_groups_examined++;
  }
  if (record->active && record->proof_id != exclude_id &&
      record->query_stamp != index->query_stamp &&
      posting_contains_pattern(index, occurrence_offset,
                               occurrence_length, record, pattern, symbol))
    collect_record(index, record_index, exclude_id, count);
}

static void collect_posting_list(Compact_back_demod_index index,
                                 uint32_t inline_record,
                                 uint32_t inline_occurrence,
                                 uint32_t inline_length,
                                 uint32_t posting_head,
                                 Term pattern, int32_t symbol,
                                 unsigned long long exclude_id,
                                 size_t *count)
{
  uint32_t block;
  uint32_t record_index = inline_record;
  uint32_t occurrence_offset = inline_occurrence;
  if (record_index == CBD_NONE || inline_length == 0)
    fatal_error("compact_back_demod: missing inline posting");
  index->posting_bytes_decoded += 3 * sizeof(uint32_t);
  index->query_bytes_decoded += 3 * sizeof(uint32_t);
  examine_posting_group(index, record_index, occurrence_offset,
                        inline_length, pattern, symbol, exclude_id, count);
  for (block = posting_head; block != CBD_NONE;
       block = index->posting_blocks[block].next) {
    const struct cbd_posting_block *current;
    uint16_t position = 0;
    uint16_t entries = 0;
    if (block >= index->posting_block_count)
      fatal_error("compact_back_demod: corrupt posting block");
    current = &index->posting_blocks[block];
    index->posting_bytes_decoded += current->used;
    index->query_bytes_decoded += current->used;
    while (position < current->used) {
      uint32_t delta = decode_posting_value(current, &position);
      uint32_t occurrence_delta;
      uint32_t occurrence_length;
      if (delta > UINT32_MAX - record_index)
        fatal_error("compact_back_demod: posting record overflow");
      record_index += delta;
      occurrence_delta = decode_posting_value(current, &position);
      if (occurrence_delta > UINT32_MAX - occurrence_offset)
        fatal_error("compact_back_demod: posting occurrence overflow");
      occurrence_offset += occurrence_delta;
      occurrence_length = decode_posting_value(current, &position);
      examine_posting_group(index, record_index, occurrence_offset,
                            occurrence_length, pattern, symbol,
                            exclude_id, count);
      entries++;
    }
    if (position != current->used || entries != current->count)
      fatal_error("compact_back_demod: corrupt posting block contents");
  }
}

static void collect_symbol(Compact_back_demod_index index, Term pattern,
                           unsigned long long exclude_id, size_t *count);

static void flatten_tree_query(Compact_back_demod_index index, Term term,
                               size_t *count)
{
  size_t at, i;
  if (*count == index->query_capacity) {
    index->query_capacity = grow_record_capacity(
      index->query_capacity, sizeof(*index->query),
      "compact_back_demod: tree query overflow");
    index->query = safe_realloc(
      index->query, index->query_capacity * sizeof(*index->query));
  }
  if (*count > UINT32_MAX)
    fatal_error("compact_back_demod: tree query exceeds 32 bits");
  at = (*count)++;
  index->query[at].term = term;
  for (i = 0; i < (size_t) ARITY(term); i++)
    flatten_tree_query(index, ARG(term, (int) i), count);
  if (*count > UINT32_MAX)
    fatal_error("compact_back_demod: tree query exceeds 32 bits");
  index->query[at].end = (uint32_t) *count;
}

/* Traverse serialized subject terms under one-way matching semantics.  A
   pattern variable consumes exactly one complete stored subterm.  Repeated
   variable equality is deliberately left to occurrence_matches() at a
   terminal posting so this traversal can only add, never lose, candidates. */
static void collect_tree_candidates(Compact_back_demod_index index,
                                    uint32_t node,
                                    uint32_t query_position,
                                    uint32_t query_end,
                                    size_t pending, Term pattern,
                                    int32_t symbol,
                                    unsigned long long exclude_id,
                                    size_t *count)
{
  struct cbd_tree_node *edge = &index->tree_nodes[node];
  uint32_t at, child;
  index->tree_nodes_examined++;
  for (at = 0; at < edge->token_length; at++) {
    int32_t code = index->tokens[edge->token_offset + at];
    if (pending != 0) {
      int arity = code < 0 ? 0 : sn_to_arity(code);
      pending--;
      if ((size_t) arity > SIZE_MAX - pending)
        fatal_error("compact_back_demod: tree term overflow");
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
    uint32_t posting_list = edge->posting_list;
    struct cbd_tree_posting_list *list;
    if (posting_list == CBD_NONE)
      return;
    if (posting_list >= index->tree_posting_list_count)
      fatal_error("compact_back_demod: corrupt tree posting list");
    list = &index->tree_posting_lists[posting_list];
    collect_posting_list(index, list->inline_record,
                         list->inline_occurrence, list->inline_length,
                         list->posting_head, pattern, symbol,
                         exclude_id, count);
    return;
  }

  for (child = edge->first_child; child != CBD_NONE;
       child = index->tree_nodes[child].next_sibling)
    collect_tree_candidates(index, child, query_position, query_end,
                            pending, pattern, symbol, exclude_id, count);
}

static void collect_tree(Compact_back_demod_index index, Term pattern,
                         unsigned long long exclude_id, size_t *count)
{
  size_t query_count = 0;
  uint32_t child;
  int32_t symbol;
  if (VARIABLE(pattern)) {
    collect_symbol(index, pattern, exclude_id, count);
    return;
  }
  symbol = SYMNUM(pattern);
  flatten_tree_query(index, pattern, &query_count);
  index->tree_queries++;
  for (child = index->tree_nodes[CBD_NONE].first_child;
       child != CBD_NONE; child = index->tree_nodes[child].next_sibling) {
    int32_t root = tree_first_code(index, child);
    if (root < symbol)
      continue;
    if (root > symbol)
      break;
    collect_tree_candidates(index, child, 0, (uint32_t) query_count,
                            0, pattern, symbol, exclude_id, count);
  }
}

static void collect_symbol(Compact_back_demod_index index, Term pattern,
                           unsigned long long exclude_id, size_t *count)
{
  uint32_t bucket_index;
  unsigned symbol;
  cbd_path_mask required_mask;
  if (VARIABLE(pattern)) {
    size_t at;
    for (at = 1; at < index->record_count; at++) {
      struct cbd_record *record = &index->records[at];
      index->query_work++;
      if (record->active)
        index->query_live++;
      else {
        index->query_dead++;
        index->inactive_groups_examined++;
      }
      if (record->active && record->query_stamp == index->query_stamp) {
        index->query_duplicates++;
        index->duplicate_groups_examined++;
      }
      collect_record(index, (uint32_t) at, exclude_id, count);
    }
    return;
  }
  symbol = (unsigned) SYMNUM(pattern);
  required_mask = term_path_mask(index, pattern);
  if ((size_t) symbol >= index->symbol_capacity)
    return;
  for (bucket_index = index->symbol_buckets[symbol];
       bucket_index != CBD_NONE;
       bucket_index = index->path_buckets[bucket_index].next) {
    struct cbd_path_bucket *bucket;
    if (bucket_index >= index->path_bucket_count)
      fatal_error("compact_back_demod: corrupt path bucket");
    bucket = &index->path_buckets[bucket_index];
    index->path_filter_checks++;
    if ((bucket->mask & required_mask) != required_mask) {
      index->path_filter_rejects++;
      continue;
    }
    collect_posting_list(index, bucket->inline_record,
                         bucket->inline_occurrence, bucket->inline_length,
                         bucket->posting_head, pattern, (int32_t) symbol,
                         exclude_id, count);
  }
}

static int decreasing_id(const void *left, const void *right)
{
  unsigned long long a = *(const unsigned long long *) left;
  unsigned long long b = *(const unsigned long long *) right;
  return a < b ? 1 : a > b ? -1 : 0;
}

unsigned long long *compact_back_demod_candidate_ids(
  Compact_back_demod_index index, Topform demod, int type, size_t *count)
{
  Term atom, alpha, beta;
  unsigned long long *answer;
  unsigned long long occurrences_before;
  *count = 0;
  if (index == NULL || demod == NULL || demod->literals == NULL)
    return NULL;
  clock_start(index->lookup_clock);
  index->query_work = 0;
  index->query_live = 0;
  index->query_dead = 0;
  index->query_duplicates = 0;
  index->query_bytes_decoded = 0;
  occurrences_before = index->occurrences_examined;
  index->tokens = compact_term_pool_tokens(index->term_pool);
  index->token_limit = compact_term_pool_token_count(index->term_pool);
  atom = demod->literals->atom;
  alpha = ARG(atom, 0);
  beta = ARG(atom, 1);
  begin_query(index);
  if (type == ORIENTED || type == LEX_DEP_LR || type == LEX_DEP_BOTH) {
    if (index->strategy == COMPACT_BACK_DEMOD_CODE_TREE)
      collect_tree(index, alpha, demod->id, count);
    else
      collect_symbol(index, alpha, demod->id, count);
  }
  if (type == LEX_DEP_RL || type == LEX_DEP_BOTH) {
    if (index->strategy == COMPACT_BACK_DEMOD_CODE_TREE)
      collect_tree(index, beta, demod->id, count);
    else
      collect_symbol(index, beta, demod->id, count);
  }
  if (*count > 1)
    qsort(index->results, *count, sizeof(*index->results), decreasing_id);
  answer = *count == 0 ? NULL : safe_malloc(*count * sizeof(*answer));
  if (*count != 0)
    memcpy(answer, index->results, *count * sizeof(*answer));
  index->queries++;
  index->candidates += *count;
  compact_profile_note(&index->query_profile, *count, index->query_work,
                       index->query_live, index->query_dead,
                       index->query_duplicates, *count,
                       index->query_bytes_decoded);
  if (index->query_work > index->worst_query_groups) {
    index->worst_query_id = demod->id;
    index->worst_query_groups = index->query_work;
    index->worst_query_occurrences =
      index->occurrences_examined - occurrences_before;
    index->worst_query_candidates = *count;
  }
  update_peak(index);
  clock_stop(index->lookup_clock);
  return answer;
}

void compact_back_demod_note_exact_query(
  Compact_back_demod_index index, size_t tests, size_t successes,
  size_t materializations)
{
  if (index != NULL) {
    index->exact_tests += tests;
    compact_profile_note_exact(&index->query_profile, tests, successes,
                               materializations);
  }
}

BOOL compact_back_demod_compaction_needed(Compact_back_demod_index index)
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

void compact_back_demod_set_compaction_stale_pct(unsigned percentage)
{
  if (percentage == 0 || percentage > 1000)
    fatal_error("compact_back_demod: invalid stale percentage");
  Compaction_stale_pct = percentage;
}

static unsigned long long occurrence_items(
  Compact_back_demod_index index, uint32_t offset, uint32_t length)
{
  uint32_t position = offset;
  uint32_t end = offset + length;
  unsigned long long count = 0;
  while (position < end) {
    (void) decode_occurrence_delta(index, &position, end);
    count++;
  }
  return count;
}

static void copy_live_tree_group(Compact_back_demod_index source,
                                 Compact_back_demod_index replacement,
                                 const uint32_t *record_map,
                                 uint32_t record_index,
                                 uint32_t occurrence_offset,
                                 uint32_t occurrence_length)
{
  uint32_t mapped;
  uint32_t position, end, relative;
  uint32_t token_offset, token_end;
  uint32_t new_offset;
  if (record_index == CBD_NONE || record_index >= source->record_count ||
      occurrence_offset > source->occurrence_count ||
      occurrence_length > source->occurrence_count - occurrence_offset)
    fatal_error("compact_back_demod: corrupt compacted tree posting");
  mapped = record_map[record_index];
  if (mapped == CBD_NONE)
    return;
  position = occurrence_offset;
  end = occurrence_offset + occurrence_length;
  relative = decode_occurrence_delta(source, &position, end);
  if (relative >= source->records[record_index].token_length)
    fatal_error("compact_back_demod: corrupt compacted tree occurrence");
  token_offset = source->records[record_index].token_offset + relative;
  token_end = token_term_end(source, token_offset);
  new_offset = (uint32_t) replacement->occurrence_count;
  ensure_occurrence_bytes(replacement, occurrence_length);
  memcpy(replacement->occurrences + replacement->occurrence_count,
         source->occurrences + occurrence_offset, occurrence_length);
  replacement->occurrence_count += occurrence_length;
  replacement->symbol_occurrences += occurrence_items(
    source, occurrence_offset, occurrence_length);
  append_tree_record(replacement, mapped, token_offset,
                     token_end - token_offset, new_offset,
                     occurrence_length);
}

static void compact_back_demod_compact_internal(
  Compact_back_demod_index index, BOOL force)
{
  Compact_back_demod_index replacement;
  struct compact_back_demod_index old;
  uint32_t *record_map;
  unsigned long long old_bytes, old_peak, old_peak_active;
  unsigned long long retired, compactions, reclaimed;
  unsigned long long queries, candidates, exact_tests;
  unsigned long long groups_examined, occurrences_examined;
  unsigned long long path_checks, path_rejects;
  size_t i;
  if (index == NULL ||
      (!force && !compact_back_demod_compaction_needed(index)) ||
      (force && index->record_count - 1 == index->active))
    return;
  clock_start(index->maintenance_clock);
  old_bytes = index_bytes(index);
  old_peak = index->peak_bytes;
  old_peak_active = index->peak;
  retired = index->retired;
  compactions = index->compactions;
  reclaimed = index->bytes_reclaimed;
  queries = index->queries;
  candidates = index->candidates;
  exact_tests = index->exact_tests;
  groups_examined = index->posting_groups_examined;
  occurrences_examined = index->occurrences_examined;
  path_checks = index->path_filter_checks;
  path_rejects = index->path_filter_rejects;
  replacement = compact_back_demod_init_with_pool_strategy(
    index->term_pool, index->strategy);
  replacement->tokens = compact_term_pool_tokens(index->term_pool);
  replacement->token_limit = compact_term_pool_token_count(index->term_pool);
  record_map = safe_calloc(index->record_count, sizeof(*record_map));
  for (i = 1; i < index->record_count; i++)
    if (index->records[i].active) {
      uint32_t added;
      struct cbd_record *record;
      ensure_records(replacement);
      if (replacement->record_count > UINT32_MAX)
        fatal_error("compact_back_demod: compacted record overflow");
      added = (uint32_t) replacement->record_count++;
      record = &replacement->records[added];
      *record = index->records[i];
      record->query_stamp = 0;
      record_map[i] = added;
      if (!compact_id_map_put(replacement->id_map, record->proof_id, &added))
        fatal_error("compact_back_demod: duplicate compacted proof ID");
      replacement->active++;
    }
  if (index->strategy == COMPACT_BACK_DEMOD_CODE_TREE) {
    for (i = 1; i < index->tree_posting_list_count; i++) {
      struct cbd_tree_posting_list *list =
        &index->tree_posting_lists[i];
      uint32_t block;
      uint32_t record_index = list->inline_record;
      uint32_t occurrence_offset = list->inline_occurrence;
      if (record_index == CBD_NONE || list->inline_length == 0)
        fatal_error("compact_back_demod: corrupt compacted tree inline posting");
      copy_live_tree_group(index, replacement, record_map, record_index,
                           occurrence_offset, list->inline_length);
      for (block = list->posting_head; block != CBD_NONE;
           block = index->posting_blocks[block].next) {
        const struct cbd_posting_block *current =
          &index->posting_blocks[block];
        uint16_t position = 0;
        uint16_t entries = 0;
        while (position < current->used) {
          uint32_t delta = decode_posting_value(current, &position);
          uint32_t occurrence_delta =
            decode_posting_value(current, &position);
          uint32_t occurrence_length =
            decode_posting_value(current, &position);
          if (delta > UINT32_MAX - record_index ||
              occurrence_delta > UINT32_MAX - occurrence_offset)
            fatal_error("compact_back_demod: compacted tree posting overflow");
          record_index += delta;
          occurrence_offset += occurrence_delta;
          copy_live_tree_group(index, replacement, record_map, record_index,
                               occurrence_offset, occurrence_length);
          entries++;
        }
        if (position != current->used || entries != current->count)
          fatal_error("compact_back_demod: corrupt compacted tree block");
      }
    }
  }
  else for (i = 1; i < index->path_bucket_count; i++) {
    struct cbd_path_bucket *bucket = &index->path_buckets[i];
    uint32_t block;
    uint32_t record_index = bucket->inline_record;
    uint32_t occurrence_offset = bucket->inline_occurrence;
    if (record_index == CBD_NONE || record_index >= index->record_count ||
        bucket->inline_length == 0 ||
        occurrence_offset > index->occurrence_count ||
        bucket->inline_length > index->occurrence_count - occurrence_offset)
      fatal_error("compact_back_demod: corrupt compacted inline posting");
    if (record_map[record_index] != CBD_NONE) {
      uint32_t new_offset = (uint32_t) replacement->occurrence_count;
      ensure_occurrence_bytes(replacement, bucket->inline_length);
      memcpy(replacement->occurrences + replacement->occurrence_count,
             index->occurrences + occurrence_offset,
             bucket->inline_length);
      replacement->occurrence_count += bucket->inline_length;
      replacement->symbol_occurrences += occurrence_items(
        index, occurrence_offset, bucket->inline_length);
      append_symbol_record(replacement, record_map[record_index],
                           bucket->symbol, bucket->mask, new_offset,
                           bucket->inline_length);
    }
    for (block = bucket->posting_head; block != CBD_NONE;
         block = index->posting_blocks[block].next) {
      const struct cbd_posting_block *current =
        &index->posting_blocks[block];
      uint16_t position = 0;
      uint16_t entries = 0;
      while (position < current->used) {
        uint32_t delta = decode_posting_value(current, &position);
        uint32_t occurrence_delta =
          decode_posting_value(current, &position);
        uint32_t occurrence_length =
          decode_posting_value(current, &position);
        if (delta > UINT32_MAX - record_index ||
            occurrence_delta > UINT32_MAX - occurrence_offset)
          fatal_error("compact_back_demod: compacted posting overflow");
        record_index += delta;
        occurrence_offset += occurrence_delta;
        if (record_index == CBD_NONE || record_index >= index->record_count ||
            occurrence_offset > index->occurrence_count ||
            occurrence_length > index->occurrence_count - occurrence_offset)
          fatal_error("compact_back_demod: corrupt compacted posting");
        if (record_map[record_index] != CBD_NONE) {
          uint32_t new_offset = (uint32_t) replacement->occurrence_count;
          ensure_occurrence_bytes(replacement, occurrence_length);
          memcpy(replacement->occurrences + replacement->occurrence_count,
                 index->occurrences + occurrence_offset, occurrence_length);
          replacement->occurrence_count += occurrence_length;
          replacement->symbol_occurrences += occurrence_items(
            index, occurrence_offset, occurrence_length);
          append_symbol_record(replacement, record_map[record_index],
                               bucket->symbol, bucket->mask, new_offset,
                               occurrence_length);
        }
        entries++;
      }
      if (position != current->used || entries != current->count)
        fatal_error("compact_back_demod: corrupt compacted posting block");
    }
  }
  safe_free(record_map);
  update_peak(replacement);
  old = *index;
  free_clock(replacement->lookup_clock);
  free_clock(replacement->maintenance_clock);
  replacement->lookup_clock = old.lookup_clock;
  replacement->maintenance_clock = old.maintenance_clock;
  *index = *replacement;
  safe_free(replacement);
  safe_free(old.posting_blocks);
  safe_free(old.symbol_buckets);
  safe_free(old.path_buckets);
  safe_free(old.path_bucket_hash);
  safe_free(old.tree_nodes);
  safe_free(old.tree_posting_lists);
  safe_free(old.occurrences);
  safe_free(old.records);
  compact_id_map_free(old.id_map);
  safe_free(old.results);
  safe_free(old.query);
  index->retired = retired;
  index->compactions = compactions + 1;
  index->bytes_reclaimed = reclaimed +
    (old_bytes > index_bytes(index) ? old_bytes - index_bytes(index) : 0);
  index->queries = queries;
  index->candidates = candidates;
  index->exact_tests = exact_tests;
  index->posting_groups_examined = groups_examined;
  index->occurrences_examined = occurrences_examined;
  index->path_filter_checks = path_checks;
  index->path_filter_rejects = path_rejects;
  index->tree_queries = old.tree_queries;
  index->tree_nodes_examined = old.tree_nodes_examined;
  index->inactive_groups_examined = old.inactive_groups_examined;
  index->duplicate_groups_examined = old.duplicate_groups_examined;
  index->posting_bytes_decoded = old.posting_bytes_decoded;
  index->worst_query_id = old.worst_query_id;
  index->worst_query_groups = old.worst_query_groups;
  index->worst_query_occurrences = old.worst_query_occurrences;
  index->worst_query_candidates = old.worst_query_candidates;
  index->query_profile = old.query_profile;
  if (old_peak > index->peak_bytes)
    index->peak_bytes = old_peak;
  if (old_peak_active > index->peak)
    index->peak = old_peak_active;
  clock_stop(index->maintenance_clock);
}

void compact_back_demod_compact(Compact_back_demod_index index)
{
  compact_back_demod_compact_internal(index, FALSE);
}

void compact_back_demod_compact_materialized(
  Compact_back_demod_index index,
  Compact_back_demod_materializer materialize,
  Compact_back_demod_materialized_releaser release,
  Compact_back_demod_materialized_batch_adviser advise,
  void *context)
{
  enum { MATERIALIZED_ID_BATCH = 4096 };
  Compact_back_demod_index replacement;
  struct compact_back_demod_index old;
  unsigned long long id_batch[MATERIALIZED_ID_BATCH];
  unsigned long long *ids = NULL;
  unsigned long long old_bytes, old_peak, old_peak_active;
  unsigned long long retired, compactions, reclaimed;
  unsigned long long queries, candidates, exact_tests;
  unsigned long long groups_examined, occurrences_examined;
  unsigned long long path_checks, path_rejects;
  unsigned long long file_snapshots, snapshot_ids;
  FILE *id_file = NULL;
  size_t i, buffered = 0, count = 0, rebuilt = 0;
  BOOL file_snapshot = FALSE;
  if (index == NULL || materialize == NULL ||
      !compact_back_demod_compaction_needed(index))
    return;
  clock_start(index->maintenance_clock);
  if (index->active > SIZE_MAX / sizeof(*ids))
    fatal_error("compact_back_demod: materialized ID snapshot overflow");
  if (index->active != 0)
    id_file = tmpfile();
  if (id_file != NULL) {
    for (i = 1; i < index->record_count; i++)
      if (index->records[i].active) {
        id_batch[buffered++] = index->records[i].proof_id;
        count++;
        if (buffered == MATERIALIZED_ID_BATCH) {
          if (fwrite(id_batch, sizeof(*id_batch), buffered, id_file) !=
              buffered)
            break;
          buffered = 0;
        }
      }
    if (i == index->record_count &&
        (buffered == 0 ||
         fwrite(id_batch, sizeof(*id_batch), buffered, id_file) == buffered) &&
        fflush(id_file) == 0 && fseek(id_file, 0, SEEK_SET) == 0)
      file_snapshot = TRUE;
    else {
      fclose(id_file);
      id_file = NULL;
      count = 0;
    }
  }
  if (!file_snapshot) {
    ids = index->active == 0 ? NULL :
      safe_malloc((size_t) index->active * sizeof(*ids));
    for (i = 1; i < index->record_count; i++)
      if (index->records[i].active)
        ids[count++] = index->records[i].proof_id;
  }
  if (count != index->active)
    fatal_error("compact_back_demod: active ID snapshot mismatch");

  old_bytes = index_bytes(index);
  old_peak = index->peak_bytes;
  old_peak_active = index->peak;
  retired = index->retired;
  compactions = index->compactions;
  reclaimed = index->bytes_reclaimed;
  queries = index->queries;
  candidates = index->candidates;
  exact_tests = index->exact_tests;
  groups_examined = index->posting_groups_examined;
  occurrences_examined = index->occurrences_examined;
  path_checks = index->path_filter_checks;
  path_rejects = index->path_filter_rejects;
  file_snapshots = index->materialized_file_snapshots;
  snapshot_ids = index->materialized_snapshot_ids;

  /* Stable IDs plus the shared clause/term archives are the complete rebuild
     recipe.  Drop all old posting, occurrence, record, and hash arrays before
     the first replacement allocation, then materialize at most one clause at
     a time in original record order. */
  old = *index;
  safe_free(old.posting_blocks);
  safe_free(old.symbol_buckets);
  safe_free(old.path_buckets);
  safe_free(old.path_bucket_hash);
  safe_free(old.tree_nodes);
  safe_free(old.tree_posting_lists);
  safe_free(old.occurrences);
  safe_free(old.records);
  compact_id_map_free(old.id_map);
  safe_free(old.results);
  safe_free(old.query);
  replacement = compact_back_demod_init_with_pool_strategy(
    old.term_pool, old.strategy);
  replacement->owns_term_pool = old.owns_term_pool;
  while (rebuilt < count) {
    unsigned long long *batch;
    size_t amount = count - rebuilt < MATERIALIZED_ID_BATCH ?
      count - rebuilt : MATERIALIZED_ID_BATCH;
    if (file_snapshot) {
      if (fread(id_batch, sizeof(*id_batch), amount, id_file) != amount)
        fatal_error("compact_back_demod: cannot read materialized ID batch");
      batch = id_batch;
    }
    else
      batch = ids + rebuilt;
    for (i = 0; i < amount; i++) {
      Topform clause = materialize(batch[i], context);
      if (clause == NULL)
        fatal_error("compact_back_demod: cannot materialize rebuild clause");
      if (!compact_back_demod_add(replacement, clause))
        fatal_error("compact_back_demod: cannot rebuild materialized clause");
      if (release != NULL)
        release(clause, context);
    }
    if (advise != NULL)
      advise(batch, amount, context);
    rebuilt += amount;
  }
  if (id_file != NULL)
    fclose(id_file);
  safe_free(ids);
  free_clock(replacement->lookup_clock);
  free_clock(replacement->maintenance_clock);
  replacement->lookup_clock = old.lookup_clock;
  replacement->maintenance_clock = old.maintenance_clock;
  *index = *replacement;
  safe_free(replacement);
  index->retired = retired;
  index->compactions = compactions + 1;
  index->bytes_reclaimed = reclaimed +
    (old_bytes > index_bytes(index) ? old_bytes - index_bytes(index) : 0);
  index->queries = queries;
  index->candidates = candidates;
  index->exact_tests = exact_tests;
  index->posting_groups_examined = groups_examined;
  index->occurrences_examined = occurrences_examined;
  index->path_filter_checks = path_checks;
  index->path_filter_rejects = path_rejects;
  index->tree_queries = old.tree_queries;
  index->tree_nodes_examined = old.tree_nodes_examined;
  index->inactive_groups_examined = old.inactive_groups_examined;
  index->duplicate_groups_examined = old.duplicate_groups_examined;
  index->posting_bytes_decoded = old.posting_bytes_decoded;
  index->worst_query_id = old.worst_query_id;
  index->worst_query_groups = old.worst_query_groups;
  index->worst_query_occurrences = old.worst_query_occurrences;
  index->worst_query_candidates = old.worst_query_candidates;
  index->query_profile = old.query_profile;
  index->materialized_file_snapshots =
    file_snapshots + (file_snapshot ? 1 : 0);
  index->materialized_snapshot_ids = snapshot_ids + count;
  if (old_peak > index->peak_bytes)
    index->peak_bytes = old_peak;
  if (old_peak_active > index->peak)
    index->peak = old_peak_active;
  clock_stop(index->maintenance_clock);
}

void compact_back_demod_compact_all_stale(Compact_back_demod_index index)
{
  compact_back_demod_compact_internal(index, TRUE);
}

void compact_back_demod_copy_live_clauses(
  Compact_back_demod_index index, Compact_term_pool destination,
  Compact_term_rebase_map map)
{
  size_t i;
  if (index == NULL)
    return;
  for (i = 1; i < index->record_count; i++)
    if (!compact_term_pool_copy_clause(
          destination, index->term_pool, map,
          index->records[i].proof_id))
      fatal_error("compact_back_demod: cannot copy compacted pool clause");
}

void compact_back_demod_retain_live_clauses(
  Compact_back_demod_index index, Compact_term_rebase_map map)
{
  size_t i;
  if (index == NULL)
    return;
  for (i = 1; i < index->record_count; i++)
    if (!compact_term_rebase_map_retain_clause(
          map, index->term_pool, index->records[i].proof_id))
      fatal_error("compact_back_demod: cannot retain term-pool clause");
}

void compact_back_demod_rebase_term_pool(
  Compact_back_demod_index index, Compact_term_pool pool,
  Compact_term_rebase_map map)
{
  size_t i;
  if (index == NULL)
    return;
  for (i = 1; i < index->record_count; i++)
    index->records[i].token_offset = compact_term_rebase_offset(
      map, index->records[i].token_offset);
  for (i = 1; i < index->tree_node_count; i++)
    if (index->tree_nodes[i].token_length != 0)
      index->tree_nodes[i].token_offset = compact_term_rebase_offset(
        map, index->tree_nodes[i].token_offset);
  index->term_pool = pool;
  index->tokens = compact_term_pool_tokens(pool);
  index->token_limit = compact_term_pool_token_count(pool);
  update_peak(index);
}

void compact_back_demod_get_stats(Compact_back_demod_index index,
                                  struct compact_back_demod_stats *stats)
{
  struct compact_term_pool_stats terms;
  memset(stats, 0, sizeof(*stats));
  if (index == NULL)
    return;
  compact_term_pool_get_stats(index->term_pool, &terms);
  stats->strategy = index->strategy;
  stats->active = index->active;
  stats->peak = index->peak;
  stats->retired = index->retired;
  stats->physical = index->record_count - 1;
  stats->compactions = index->compactions;
  stats->bytes_reclaimed = index->bytes_reclaimed;
  stats->queries = index->queries;
  stats->candidates = index->candidates;
  stats->exact_tests = index->exact_tests;
  stats->posting_groups = index->posting_count;
  stats->path_buckets = index->path_bucket_count - 1;
  stats->tree_nodes = index->tree_node_count == 0 ? 0 :
    index->tree_node_count - 1;
  stats->tree_terminals = index->tree_posting_list_count == 0 ? 0 :
    index->tree_posting_list_count - 1;
  stats->tree_queries = index->tree_queries;
  stats->tree_nodes_examined = index->tree_nodes_examined;
  stats->symbol_occurrences = index->symbol_occurrences;
  stats->posting_groups_examined = index->posting_groups_examined;
  stats->occurrences_examined = index->occurrences_examined;
  stats->path_filter_checks = index->path_filter_checks;
  stats->path_filter_rejects = index->path_filter_rejects;
  stats->inactive_groups_examined = index->inactive_groups_examined;
  stats->duplicate_groups_examined = index->duplicate_groups_examined;
  stats->posting_bytes_decoded = index->posting_bytes_decoded;
  stats->worst_query_id = index->worst_query_id;
  stats->worst_query_groups = index->worst_query_groups;
  stats->worst_query_occurrences = index->worst_query_occurrences;
  stats->worst_query_candidates = index->worst_query_candidates;
  stats->query_profile = index->query_profile;
  stats->lookup_seconds = clock_seconds(index->lookup_clock);
  stats->maintenance_seconds = clock_seconds(index->maintenance_clock);
  stats->materialized_file_snapshots = index->materialized_file_snapshots;
  stats->materialized_snapshot_ids = index->materialized_snapshot_ids;
  stats->posting_bytes =
    index->posting_block_capacity * sizeof(*index->posting_blocks) +
    index->occurrence_capacity * sizeof(*index->occurrences);
  stats->posting_stream_used = index->posting_stream_used;
  stats->posting_stream_bytes =
    index->posting_block_capacity * sizeof(*index->posting_blocks);
  stats->occurrence_bytes =
    index->occurrence_capacity * sizeof(*index->occurrences);
  stats->occurrence_stream_bytes = index->occurrence_count;
  stats->record_bytes = index->record_capacity * sizeof(*index->records);
  stats->root_bytes =
    index->symbol_capacity * sizeof(*index->symbol_buckets) +
    index->path_bucket_capacity * sizeof(*index->path_buckets) +
    index->path_bucket_hash_capacity * sizeof(*index->path_bucket_hash) +
    index->tree_node_capacity * sizeof(*index->tree_nodes) +
    index->tree_posting_list_capacity *
      sizeof(*index->tree_posting_lists);
  stats->token_bytes = index->owns_term_pool ? terms.token_bytes : 0;
  stats->hash_bytes = compact_id_map_bytes(index->id_map);
  stats->scratch_bytes = index->result_capacity * sizeof(*index->results) +
    index->query_capacity * sizeof(*index->query);
  stats->total_bytes = index_bytes(index);
  stats->peak_bytes = index->peak_bytes;
}

void compact_back_demod_free(Compact_back_demod_index index)
{
  if (index == NULL)
    return;
  safe_free(index->posting_blocks);
  safe_free(index->symbol_buckets);
  safe_free(index->path_buckets);
  safe_free(index->path_bucket_hash);
  safe_free(index->tree_nodes);
  safe_free(index->tree_posting_lists);
  safe_free(index->occurrences);
  safe_free(index->records);
  if (index->owns_term_pool)
    compact_term_pool_free(index->term_pool);
  compact_id_map_free(index->id_map);
  safe_free(index->results);
  safe_free(index->query);
  free_clock(index->lookup_clock);
  free_clock(index->maintenance_clock);
  safe_free(index);
}
