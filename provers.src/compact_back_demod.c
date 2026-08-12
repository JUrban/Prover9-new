#include "compact_back_demod.h"
#include "compact_id_map.h"

#include <limits.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#define CBD_NONE 0U
#define CBD_POSTING_BLOCK_PAYLOAD 16
#define CBD_PATH_DEPTH 3
#define CBD_TREE_TERMINAL UINT32_C(0x80000000)
#define CBD_TREE_INDEX_MASK UINT32_C(0x7fffffff)
#define CBD_TREE_DIRECT_RECORD UINT32_C(0x80000000)
#define CBD_POSITION_BLOCK_PAYLOAD 24
#define CBD_TREE_CHILD_CACHE_MAX_BYTES (UINT64_C(8) * 1024 * 1024)
#define CBD_TREE_CHILD_CACHE_MIN_SCAN 8
#define CBD_ROUTE_PROFILE_CAPACITY 4096

enum cbd_route {
  CBD_ROUTE_MASK,
  CBD_ROUTE_TREE,
  CBD_ROUTE_POSITION,
  CBD_ROUTE_COUNT
};

static unsigned Compaction_stale_pct = 25;
static Compact_back_demod_strategy Back_demod_strategy =
  COMPACT_BACK_DEMOD_MASK8;
static unsigned Back_demod_tree_min_tokens = 8;
static unsigned Back_demod_tree_admit_work = 4096;
static unsigned Back_demod_tree_build_factor = 8;
static unsigned Back_demod_position_admit_work = 4096;
static unsigned Back_demod_position_min_gain = 4;
static unsigned Back_demod_position_build_factor = 8;
static BOOL Back_demod_position_admission = FALSE;
static unsigned Back_demod_position_budget_pct = 20;
static unsigned long long Back_demod_position_budget_bytes =
  UINT64_C(64) * 1024 * 1024;
static unsigned long long Back_demod_tree_budget_bytes =
  UINT64_C(64) * 1024 * 1024;

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
  Compact_term_slice tokens;
  uint32_t next_sibling;
  /* Complete serialized terms are prefix-free.  Nonterminals store their
     first child here; terminals store a flagged posting-list index. */
  uint32_t child_or_posting;
};

struct cbd_tree_posting_list {
  uint32_t posting_head;
  uint32_t posting_tail;
  uint32_t inline_record;
  uint32_t inline_relative;
};

/* This is a bounded positive cache, not an authoritative tree directory.
   A missing or collided entry falls back to the ordered sibling chain. */
struct cbd_tree_child_cache_entry {
  uint32_t parent;
  int32_t code;
  uint32_t child;
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

struct cbd_tree_root_state {
  unsigned long long fallback_work;
  unsigned long long next_check_work;
  unsigned long long physical_groups;
  unsigned char admitted;
  unsigned char rejected;
};

/* A bounded performance cache, never an answer authority.  Every route is a
   complete candidate generator, so losing an entry to collision can only
   discard calibration.  The two entries in each hash set retain the more
   frequently observed query shapes deterministically. */
struct cbd_route_profile {
  uint64_t key;
  unsigned long long cost[CBD_ROUTE_COUNT];
  unsigned long long population[CBD_ROUTE_COUNT];
  unsigned long long last_sample[CBD_ROUTE_COUNT];
  unsigned long long queries;
  uint32_t samples[CBD_ROUTE_COUNT];
  unsigned long long position_generation;
  unsigned char preferred;
  unsigned char occupied;
};

/* Exact rigid-position postings are admitted lazily.  The 64-bit path is a
   deterministic hash of child numbers below ROOT_SYMBOL.  A collision only
   merges safe posting lists and therefore adds final compact matches; it can
   never omit a possible redex. */
struct cbd_position_bucket {
  uint64_t path;
  uint32_t root_symbol;
  uint32_t symbol;
  uint32_t next_root;
  uint32_t posting_head;
  uint32_t posting_tail;
  uint32_t inline_record;
  uint32_t inline_occurrence;
  uint32_t inline_length;
  uint32_t last_record;
  uint32_t last_occurrence;
  uint32_t posting_count;
  uint64_t *membership;
  size_t membership_capacity;
  unsigned char active;
};

struct cbd_position_block {
  uint32_t next;
  uint16_t used;
  uint16_t count;
  unsigned char data[CBD_POSITION_BLOCK_PAYLOAD];
};

struct cbd_position_query_feature {
  uint64_t path;
  uint32_t symbol;
  uint32_t bucket;
  unsigned long long matching_records;
  unsigned long long joint_records;
};

struct cbd_position_probation {
  uint64_t path;
  unsigned long long work;
  uint32_t root_symbol;
  uint32_t symbol;
  uint32_t hits;
  unsigned char occupied;
};

struct cbd_record {
  unsigned long long proof_id;
  Compact_term_slice tokens;
  uint32_t query_stamp;
  unsigned char active;
};

typedef char compact_back_demod_tree_node_must_remain_16_bytes[
  sizeof(struct cbd_tree_node) == 16 ? 1 : -1];
typedef char compact_back_demod_tree_list_must_remain_16_bytes[
  sizeof(struct cbd_tree_posting_list) == 16 ? 1 : -1];
typedef char compact_back_demod_record_must_remain_24_bytes[
  sizeof(struct cbd_record) == 24 ? 1 : -1];
typedef char compact_back_demod_route_profile_must_remain_120_bytes[
  sizeof(struct cbd_route_profile) == 120 ? 1 : -1];

struct compact_back_demod_index {
  Compact_back_demod_strategy strategy;
  struct cbd_posting_block *posting_blocks;
  size_t posting_block_count;
  size_t posting_block_capacity;
  size_t posting_count;
  size_t posting_stream_used;
  uint32_t *symbol_buckets;
  uint64_t *symbol_hashes;
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
  struct cbd_tree_child_cache_entry *tree_child_cache;
  size_t tree_child_cache_capacity;
  uint64_t *tree_child_cache_parents;
  size_t tree_child_cache_parent_words;
  size_t tree_child_cache_parent_count;
  BOOL tree_child_cache_growth_blocked;
  struct cbd_tree_root_state *tree_roots;
  size_t tree_root_capacity;
  struct cbd_position_bucket *position_buckets;
  size_t position_bucket_count;
  size_t position_bucket_capacity;
  uint32_t *position_bucket_hash;
  size_t position_bucket_hash_capacity;
  uint32_t *position_root_buckets;
  size_t position_root_capacity;
  struct cbd_position_block *position_blocks;
  size_t position_block_count;
  size_t position_block_capacity;
  struct cbd_position_query_feature *position_query;
  size_t position_query_capacity;
  struct cbd_position_probation *position_probation;
  size_t position_probation_capacity;
  struct cbd_route_profile *route_profiles;
  size_t route_profile_capacity;
  unsigned long long route_profile_occupied;
  unsigned long long route_profile_collisions;
  unsigned long long route_profile_replacements;
  unsigned long long route_choices[CBD_ROUTE_COUNT];
  unsigned long long route_probes[CBD_ROUTE_COUNT];
  unsigned long long route_switches;
  unsigned long long route_reversions;
  unsigned long long route_hysteresis_holds;
  unsigned long long route_observed_cost[CBD_ROUTE_COUNT];
  unsigned long long route_estimated_cost[CBD_ROUTE_COUNT];
  unsigned long long route_candidates[CBD_ROUTE_COUNT];
  unsigned long long position_generation;
  unsigned char *occurrences;
  size_t occurrence_count;
  size_t occurrence_capacity;
  struct cbd_record *records;
  size_t record_count;
  size_t record_capacity;
  Compact_term_pool term_pool;
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
  unsigned long long tree_sibling_checks;
  unsigned long long tree_child_cache_lookups;
  unsigned long long tree_child_cache_hits;
  unsigned long long tree_child_cache_misses;
  unsigned long long tree_child_cache_replacements;
  unsigned long long tree_child_cache_growth_denials;
  unsigned long long tree_posting_count;
  unsigned long long tree_posting_blocks;
  unsigned long long tree_posting_stream_used;
  unsigned long long tree_budget_exhaustions;
  unsigned long long tree_root_admissions;
  unsigned long long tree_root_rejections;
  unsigned long long tree_root_cost_deferrals;
  unsigned long long tree_root_censuses;
  unsigned long long tree_root_census_occurrences;
  unsigned long long tree_root_demotions;
  unsigned long long tree_root_backfill_groups;
  unsigned long long tree_root_backfill_occurrences;
  unsigned long long tree_fallback_work;
  unsigned tree_min_tokens;
  unsigned tree_admit_work;
  unsigned tree_build_factor;
  unsigned long long tree_budget_bytes;
  BOOL tree_complete;
  unsigned long long position_posting_count;
  unsigned long long position_queries;
  unsigned long long position_intersection_queries;
  unsigned long long position_dense_intersection_queries;
  unsigned long long position_intersection_scans;
  unsigned long long position_intersection_bit_checks;
  unsigned long long position_bitmap_word_checks;
  unsigned long long position_intersection_records;
  unsigned long long position_records_examined;
  unsigned long long position_admissions;
  unsigned long long position_rejections;
  unsigned long long position_demotions;
  unsigned long long position_cost_deferrals;
  unsigned long long position_probation_updates;
  unsigned long long position_probation_replacements;
  unsigned long long position_backfill_records;
  unsigned long long position_budget_exhaustions;
  unsigned position_admit_work;
  unsigned position_min_gain;
  unsigned position_build_factor;
  unsigned position_budget_pct;
  unsigned long long position_budget_bytes;
  unsigned long long position_budget_high_water;
  unsigned long long position_bitmap_bytes;
  BOOL position_complete;
  BOOL position_rebuilding;
  BOOL position_admission_enabled;
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
  unsigned long long query_input_fingerprint;
  unsigned long long query_output_fingerprint;
  struct compact_query_profile query_profile;
  struct compact_query_timer lookup_timer;
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

static void append_admitted_position_features(
  Compact_back_demod_index index, uint32_t record_index);

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

static BOOL strategy_uses_hot_tree(Compact_back_demod_strategy strategy)
{
  return strategy == COMPACT_BACK_DEMOD_HOT_ROOT_TREE ||
    strategy == COMPACT_BACK_DEMOD_ADAPTIVE;
}

static BOOL strategy_uses_position(Compact_back_demod_strategy strategy)
{
  return strategy == COMPACT_BACK_DEMOD_POSITION ||
    strategy == COMPACT_BACK_DEMOD_ADAPTIVE;
}

static BOOL strategy_uses_tree(Compact_back_demod_strategy strategy)
{
  return strategy == COMPACT_BACK_DEMOD_CODE_TREE ||
    strategy == COMPACT_BACK_DEMOD_HYBRID_TREE ||
    strategy_uses_hot_tree(strategy);
}

static BOOL strategy_uses_paths(Compact_back_demod_strategy strategy)
{
  return strategy != COMPACT_BACK_DEMOD_CODE_TREE;
}

static BOOL strategy_uses_mask8(Compact_back_demod_strategy strategy)
{
  return strategy == COMPACT_BACK_DEMOD_MASK8 ||
    strategy == COMPACT_BACK_DEMOD_HYBRID_TREE ||
    strategy_uses_hot_tree(strategy) || strategy_uses_position(strategy);
}

static unsigned long long position_estimated_bytes(
  Compact_back_demod_index index)
{
  return index->position_bucket_capacity *
      sizeof(*index->position_buckets) +
    index->position_bucket_hash_capacity *
      sizeof(*index->position_bucket_hash) +
    index->position_root_capacity * sizeof(*index->position_root_buckets) +
    index->position_block_capacity * sizeof(*index->position_blocks) +
    index->position_probation_capacity * sizeof(*index->position_probation) +
    index->position_bitmap_bytes;
}

static unsigned long long tree_estimated_bytes(
  Compact_back_demod_index index)
{
  unsigned long long blocks = index->tree_posting_blocks *
    sizeof(*index->posting_blocks);
  return index->tree_node_capacity * sizeof(*index->tree_nodes) +
    index->tree_posting_list_capacity *
      sizeof(*index->tree_posting_lists) + blocks +
    index->tree_child_cache_capacity *
      sizeof(*index->tree_child_cache) +
    index->tree_child_cache_parent_words *
      sizeof(*index->tree_child_cache_parents) +
    index->tree_root_capacity * sizeof(*index->tree_roots) +
    index->query_capacity * sizeof(*index->query);
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
    index->symbol_capacity * sizeof(*index->symbol_hashes) +
    index->path_bucket_capacity * sizeof(*index->path_buckets) +
    index->path_bucket_hash_capacity * sizeof(*index->path_bucket_hash) +
    index->tree_node_capacity * sizeof(*index->tree_nodes) +
    index->tree_posting_list_capacity *
      sizeof(*index->tree_posting_lists) +
    index->tree_child_cache_capacity *
      sizeof(*index->tree_child_cache) +
    index->tree_child_cache_parent_words *
      sizeof(*index->tree_child_cache_parents) +
    index->tree_root_capacity * sizeof(*index->tree_roots) +
    index->position_bucket_capacity * sizeof(*index->position_buckets) +
    index->position_bucket_hash_capacity *
      sizeof(*index->position_bucket_hash) +
    index->position_root_capacity * sizeof(*index->position_root_buckets) +
    index->position_block_capacity * sizeof(*index->position_blocks) +
    index->position_probation_capacity * sizeof(*index->position_probation) +
    index->route_profile_capacity * sizeof(*index->route_profiles) +
    index->position_bitmap_bytes +
    index->occurrence_capacity * sizeof(*index->occurrences) +
    index->record_capacity * sizeof(*index->records) +
    (index->owns_term_pool ? terms.total_bytes : 0) +
    compact_id_map_bytes(index->id_map) +
    index->result_capacity * sizeof(*index->results) +
    index->query_capacity * sizeof(*index->query) +
    index->position_query_capacity * sizeof(*index->position_query);
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
  index->symbol_hashes = safe_realloc(
    index->symbol_hashes,
    index->symbol_capacity * sizeof(*index->symbol_hashes));
  memset(index->symbol_hashes + old_capacity, 0,
         (index->symbol_capacity - old_capacity) *
           sizeof(*index->symbol_hashes));
  if (strategy_uses_hot_tree(index->strategy)) {
    size_t old_roots = index->tree_root_capacity;
    index->tree_root_capacity = index->symbol_capacity;
    index->tree_roots = safe_realloc(
      index->tree_roots,
      index->tree_root_capacity * sizeof(*index->tree_roots));
    memset(index->tree_roots + old_roots, 0,
           (index->tree_root_capacity - old_roots) *
             sizeof(*index->tree_roots));
  }
  if (strategy_uses_position(index->strategy)) {
    size_t old_roots = index->position_root_capacity;
    index->position_root_capacity = index->symbol_capacity;
    index->position_root_buckets = safe_realloc(
      index->position_root_buckets,
      index->position_root_capacity * sizeof(*index->position_root_buckets));
    memset(index->position_root_buckets + old_roots, 0,
           (index->position_root_capacity - old_roots) *
             sizeof(*index->position_root_buckets));
  }
}

/* Symbol numbers include constants introduced while parsing option terms.
   They are exact array keys, but must not determine lossy signature bits or
   adaptive-probation collisions: adding an unrelated option value would then
   change index work for the same theorem. */
static uint64_t stable_symbol_hash(Compact_back_demod_index index,
                                   uint32_t symbol)
{
  const unsigned char *name;
  uint64_t value = UINT64_C(1469598103934665603);
  ensure_symbols(index, symbol);
  if (index->symbol_hashes[symbol] != 0)
    return index->symbol_hashes[symbol];
  name = (const unsigned char *) sn_to_str((int) symbol);
  while (*name != '\0') {
    value ^= *name++;
    value *= UINT64_C(1099511628211);
  }
  value ^= (unsigned) sn_to_arity((int) symbol);
  value = hash_id(value);
  if (value == 0)
    value = 1;
  index->symbol_hashes[symbol] = value;
  return value;
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

static uint32_t new_posting_block(Compact_back_demod_index index,
                                  BOOL tree_block)
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
  if (index->strategy == COMPACT_BACK_DEMOD_CODE_TREE &&
      block > CBD_TREE_INDEX_MASK)
    fatal_error("compact_back_demod: tree posting blocks exceed 31 bits");
  memset(&index->posting_blocks[block], 0,
         sizeof(index->posting_blocks[block]));
  if (tree_block)
    index->tree_posting_blocks++;
  return block;
}

static uint64_t path_bucket_key(Compact_back_demod_index index,
                                uint32_t symbol, cbd_path_mask mask)
{
  return hash_id(stable_symbol_hash(index, symbol) ^ hash_id(mask));
}

static size_t path_bucket_hash_slot(Compact_back_demod_index index,
                                    uint32_t symbol, cbd_path_mask mask)
{
  size_t at = (size_t) hash_id(path_bucket_key(index, symbol, mask)) &
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

static uint64_t position_feature_key(Compact_back_demod_index index,
                                     uint32_t root_symbol, uint64_t path,
                                     uint32_t symbol)
{
  return hash_id(path ^
    (stable_symbol_hash(index, root_symbol) *
      UINT64_C(0x9e3779b97f4a7c15)) ^
    (stable_symbol_hash(index, symbol) * UINT64_C(0x85ebca77c2b2ae63)));
}

static size_t position_hash_slot(Compact_back_demod_index index,
                                 uint32_t root_symbol, uint64_t path,
                                 uint32_t symbol)
{
  size_t at = (size_t) position_feature_key(
    index, root_symbol, path, symbol) &
    (index->position_bucket_hash_capacity - 1);
  for (;;) {
    uint32_t bucket = index->position_bucket_hash[at];
    if (bucket == CBD_NONE ||
        (index->position_buckets[bucket].root_symbol == root_symbol &&
         index->position_buckets[bucket].path == path &&
         index->position_buckets[bucket].symbol == symbol))
      return at;
    at = (at + 1) & (index->position_bucket_hash_capacity - 1);
  }
}

static void rehash_position_buckets(Compact_back_demod_index index,
                                    size_t capacity)
{
  uint32_t *old_hash = index->position_bucket_hash;
  size_t i;
  index->position_bucket_hash = safe_calloc(
    capacity, sizeof(*index->position_bucket_hash));
  index->position_bucket_hash_capacity = capacity;
  for (i = 1; i < index->position_bucket_count; i++) {
    struct cbd_position_bucket *bucket = &index->position_buckets[i];
    size_t at = position_hash_slot(index, bucket->root_symbol,
                                   bucket->path, bucket->symbol);
    index->position_bucket_hash[at] = (uint32_t) i;
  }
  safe_free(old_hash);
}

static uint32_t lookup_position_bucket(Compact_back_demod_index index,
                                       uint32_t root_symbol, uint64_t path,
                                       uint32_t symbol)
{
  size_t at;
  if (index->position_bucket_hash_capacity == 0)
    return CBD_NONE;
  at = position_hash_slot(index, root_symbol, path, symbol);
  return index->position_bucket_hash[at];
}

static uint32_t add_position_bucket(Compact_back_demod_index index,
                                    uint32_t root_symbol, uint64_t path,
                                    uint32_t symbol)
{
  size_t at;
  uint32_t bucket;
  ensure_symbols(index, root_symbol);
  if (index->position_bucket_hash_capacity == 0)
    rehash_position_buckets(index, 128);
  else if ((index->position_bucket_count + 1) * 20 >=
           index->position_bucket_hash_capacity * 17) {
    if (index->position_bucket_hash_capacity > SIZE_MAX / 2)
      fatal_error("compact_back_demod: position hash overflow");
    rehash_position_buckets(index,
                            index->position_bucket_hash_capacity * 2);
  }
  at = position_hash_slot(index, root_symbol, path, symbol);
  bucket = index->position_bucket_hash[at];
  if (bucket != CBD_NONE)
    return bucket;
  if (index->position_bucket_count == index->position_bucket_capacity) {
    index->position_bucket_capacity = grow_record_capacity(
      index->position_bucket_capacity, sizeof(*index->position_buckets),
      "compact_back_demod: position bucket overflow");
    index->position_buckets = safe_realloc(
      index->position_buckets,
      index->position_bucket_capacity * sizeof(*index->position_buckets));
  }
  if (index->position_bucket_count > UINT32_MAX)
    fatal_error("compact_back_demod: position bucket offsets exceed 32 bits");
  bucket = (uint32_t) index->position_bucket_count++;
  memset(&index->position_buckets[bucket], 0,
         sizeof(index->position_buckets[bucket]));
  index->position_buckets[bucket].root_symbol = root_symbol;
  index->position_buckets[bucket].path = path;
  index->position_buckets[bucket].symbol = symbol;
  index->position_buckets[bucket].active = TRUE;
  index->position_buckets[bucket].next_root =
    index->position_root_buckets[root_symbol];
  index->position_root_buckets[root_symbol] = bucket;
  index->position_bucket_hash[at] = bucket;
  return bucket;
}

static uint32_t new_position_block(Compact_back_demod_index index)
{
  uint32_t block;
  if (index->position_block_count == index->position_block_capacity) {
    index->position_block_capacity = grow_record_capacity(
      index->position_block_capacity, sizeof(*index->position_blocks),
      "compact_back_demod: position block overflow");
    index->position_blocks = safe_realloc(
      index->position_blocks,
      index->position_block_capacity * sizeof(*index->position_blocks));
  }
  if (index->position_block_count > UINT32_MAX)
    fatal_error("compact_back_demod: position block offsets exceed 32 bits");
  block = (uint32_t) index->position_block_count++;
  memset(&index->position_blocks[block], 0,
         sizeof(index->position_blocks[block]));
  return block;
}

static void append_position_group(Compact_back_demod_index index,
                                  uint32_t bucket_index, uint32_t record,
                                  uint32_t occurrence_offset,
                                  uint32_t occurrence_length)
{
  unsigned char encoded[15];
  size_t length = 0;
  uint32_t block;
  struct cbd_position_bucket *bucket =
    &index->position_buckets[bucket_index];
  struct cbd_position_block *tail;
  if (record <= bucket->last_record)
    fatal_error("compact_back_demod: nonmonotone position posting");
  if (occurrence_offset < bucket->last_occurrence || occurrence_length == 0)
    fatal_error("compact_back_demod: invalid position occurrence posting");
  if (bucket->inline_record == CBD_NONE) {
    bucket->inline_record = record;
    bucket->inline_occurrence = occurrence_offset;
    bucket->inline_length = occurrence_length;
    bucket->last_record = record;
    bucket->last_occurrence = occurrence_offset;
    bucket->posting_count = 1;
    index->position_posting_count++;
    return;
  }
  length += encode_u32(encoded + length, record - bucket->last_record);
  length += encode_u32(encoded + length,
                       occurrence_offset - bucket->last_occurrence);
  length += encode_u32(encoded + length, occurrence_length);
  block = bucket->posting_tail;
  if (block == CBD_NONE ||
      index->position_blocks[block].used + length >
        CBD_POSITION_BLOCK_PAYLOAD) {
    uint32_t added = new_position_block(index);
    if (block == CBD_NONE)
      bucket->posting_head = added;
    else
      index->position_blocks[block].next = added;
    bucket->posting_tail = added;
    block = added;
  }
  tail = &index->position_blocks[block];
  memcpy(tail->data + tail->used, encoded, length);
  tail->used += (uint16_t) length;
  tail->count++;
  bucket->last_record = record;
  bucket->last_occurrence = occurrence_offset;
  bucket->posting_count++;
  index->position_posting_count++;
}

static size_t projected_record_capacity(size_t capacity, size_t needed,
                                        size_t item_size,
                                        const char *message)
{
  while (needed > capacity)
    capacity = grow_record_capacity(capacity, item_size, message);
  return capacity;
}

static size_t projected_position_bitmap_capacity(size_t capacity,
                                                 uint32_t record_index)
{
  size_t needed = (size_t) record_index / 64 + 1;
  return projected_record_capacity(
    capacity, needed, sizeof(uint64_t),
    "compact_back_demod: position bitmap overflow");
}

static unsigned long long position_bitmap_growth(
  struct cbd_position_bucket *bucket, uint32_t record_index)
{
  size_t projected = projected_position_bitmap_capacity(
    bucket->membership_capacity, record_index);
  return (unsigned long long) (projected - bucket->membership_capacity) *
    sizeof(*bucket->membership);
}

static void add_position_bitmap_record(Compact_back_demod_index index,
                                       struct cbd_position_bucket *bucket,
                                       uint32_t record_index)
{
  size_t projected = projected_position_bitmap_capacity(
    bucket->membership_capacity, record_index);
  if (projected != bucket->membership_capacity) {
    size_t old = bucket->membership_capacity;
    unsigned long long added = (unsigned long long) (projected - old) *
      sizeof(*bucket->membership);
    if (ULLONG_MAX - index->position_bitmap_bytes < added)
      fatal_error("compact_back_demod: position bitmap byte overflow");
    bucket->membership = safe_realloc(
      bucket->membership, projected * sizeof(*bucket->membership));
    memset(bucket->membership + old, 0,
           (projected - old) * sizeof(*bucket->membership));
    bucket->membership_capacity = projected;
    index->position_bitmap_bytes += added;
  }
  bucket->membership[record_index / 64] |=
    UINT64_C(1) << (record_index % 64);
}

static BOOL position_bitmap_contains(struct cbd_position_bucket *bucket,
                                     uint32_t record_index)
{
  size_t word = (size_t) record_index / 64;
  return word < bucket->membership_capacity &&
    (bucket->membership[word] &
     (UINT64_C(1) << (record_index % 64))) != 0;
}

static void free_position_buckets(struct cbd_position_bucket *buckets,
                                  size_t count)
{
  size_t i;
  if (buckets == NULL)
    return;
  for (i = 1; i < count; i++)
    safe_free(buckets[i].membership);
  safe_free(buckets);
}

static size_t projected_position_hash_capacity(
  Compact_back_demod_index index, size_t bucket_count)
{
  size_t capacity = index->position_bucket_hash_capacity;
  if (capacity == 0)
    capacity = 128;
  while ((bucket_count + 1) * 20 >= capacity * 17) {
    if (capacity > SIZE_MAX / 2)
      fatal_error("compact_back_demod: projected position hash overflow");
    capacity *= 2;
  }
  return capacity;
}

static unsigned long long projected_position_bytes(
  Compact_back_demod_index index, size_t added_buckets,
  size_t added_blocks, unsigned long long added_bitmap_bytes)
{
  size_t buckets = projected_record_capacity(
    index->position_bucket_capacity,
    index->position_bucket_count + added_buckets,
    sizeof(*index->position_buckets),
    "compact_back_demod: projected position bucket overflow");
  size_t blocks = projected_record_capacity(
    index->position_block_capacity,
    index->position_block_count + added_blocks,
    sizeof(*index->position_blocks),
    "compact_back_demod: projected position block overflow");
  size_t hash = projected_position_hash_capacity(
    index, index->position_bucket_count + added_buckets);
  return buckets * sizeof(*index->position_buckets) +
    blocks * sizeof(*index->position_blocks) +
    hash * sizeof(*index->position_bucket_hash) +
    index->position_root_capacity * sizeof(*index->position_root_buckets) +
    index->position_probation_capacity * sizeof(*index->position_probation) +
    index->position_bitmap_bytes + added_bitmap_bytes;
}

static unsigned long long position_budget_limit(
  Compact_back_demod_index index)
{
  unsigned long long base, relative = ULLONG_MAX, limit;
  unsigned long long current = position_estimated_bytes(index);
  unsigned long long total = index_bytes(index);
  base = total >= current ? total - current : 0;
  if (index->position_budget_pct != 0) {
    relative = base > ULLONG_MAX / index->position_budget_pct ?
      ULLONG_MAX : base * index->position_budget_pct / 100;
  }
  if (index->position_budget_bytes == 0)
    limit = relative;
  else if (index->position_budget_pct == 0)
    limit = index->position_budget_bytes;
  else
    limit = index->position_budget_bytes < relative ?
      index->position_budget_bytes : relative;
  /* The percentage is an admission ramp, not a compaction trap.  Preserve
     the largest deterministic allowance already earned by this index so a
     smaller rebuilt base cannot globally invalidate complete features. */
  if (limit > index->position_budget_high_water)
    index->position_budget_high_water = limit;
  return index->position_budget_high_water;
}

static BOOL position_budget_allows(Compact_back_demod_index index,
                                   unsigned long long projected)
{
  return projected <= position_budget_limit(index);
}

static void ensure_position_probation(Compact_back_demod_index index)
{
  unsigned long long bytes, limit;
  size_t slots = 1;
  if (index->position_probation_capacity != 0)
    return;
  limit = position_budget_limit(index);
  bytes = limit == ULLONG_MAX ? UINT64_C(131072) : limit / 4;
  if (bytes > UINT64_C(131072))
    bytes = UINT64_C(131072);
  while (slots <= SIZE_MAX / 2 &&
         (unsigned long long) (slots * 2) <=
           bytes / sizeof(*index->position_probation))
    slots *= 2;
  if (slots < 64)
    return;
  index->position_probation = safe_calloc(
    slots, sizeof(*index->position_probation));
  index->position_probation_capacity = slots;
  update_peak(index);
}

static BOOL same_position_probation(struct cbd_position_probation *entry,
                                    uint32_t root_symbol, uint64_t path,
                                    uint32_t symbol)
{
  return entry->occupied && entry->root_symbol == root_symbol &&
    entry->path == path && entry->symbol == symbol;
}

static unsigned long long note_position_probation(
  Compact_back_demod_index index, uint32_t root_symbol, uint64_t path,
  uint32_t symbol, unsigned long long work, unsigned *hits)
{
  uint64_t key = position_feature_key(
    index, root_symbol, path, symbol);
  size_t mask, first, second;
  struct cbd_position_probation *entry, *alternative;
  ensure_position_probation(index);
  if (index->position_probation_capacity == 0) {
    *hits = 0;
    return 0;
  }
  mask = index->position_probation_capacity - 1;
  first = (size_t) key & mask;
  second = (size_t) hash_id(key ^ UINT64_C(0xd6e8feb86659fd93)) & mask;
  entry = &index->position_probation[first];
  alternative = &index->position_probation[second];
  if (same_position_probation(entry, root_symbol, path, symbol)) {
    /* use ENTRY */
  }
  else if (same_position_probation(alternative, root_symbol, path, symbol))
    entry = alternative;
  else if (!entry->occupied) {
    /* use empty ENTRY */
  }
  else if (!alternative->occupied)
    entry = alternative;
  else {
    if (alternative->work < entry->work)
      entry = alternative;
    index->position_probation_replacements++;
    memset(entry, 0, sizeof(*entry));
  }
  if (!entry->occupied) {
    entry->occupied = TRUE;
    entry->root_symbol = root_symbol;
    entry->path = path;
    entry->symbol = symbol;
  }
  entry->work = ULLONG_MAX - entry->work < work ?
    ULLONG_MAX : entry->work + work;
  if (entry->hits != UINT32_MAX)
    entry->hits++;
  index->position_probation_updates++;
  *hits = entry->hits;
  return entry->work;
}

static void clear_position_probation(Compact_back_demod_index index,
                                     uint32_t root_symbol, uint64_t path,
                                     uint32_t symbol)
{
  uint64_t key;
  size_t mask, first, second;
  if (index->position_probation_capacity == 0)
    return;
  key = position_feature_key(index, root_symbol, path, symbol);
  mask = index->position_probation_capacity - 1;
  first = (size_t) key & mask;
  second = (size_t) hash_id(key ^ UINT64_C(0xd6e8feb86659fd93)) & mask;
  if (same_position_probation(&index->position_probation[first],
                              root_symbol, path, symbol))
    memset(&index->position_probation[first], 0,
           sizeof(index->position_probation[first]));
  if (second != first &&
      same_position_probation(&index->position_probation[second],
                              root_symbol, path, symbol))
    memset(&index->position_probation[second], 0,
           sizeof(index->position_probation[second]));
}

static int tree_code_compare(int32_t a, int32_t b)
{
  BOOL av = a < 0, bv = b < 0;
  if (av != bv)
    return av ? -1 : 1;
  return a < b ? -1 : a > b ? 1 : 0;
}

static int32_t tree_first_code(Compact_back_demod_index index,
                               uint32_t node);

static size_t tree_child_cache_slot(Compact_back_demod_index index,
                                    uint32_t parent, int32_t code,
                                    size_t capacity)
{
  uint64_t symbol = code < 0 ?
    hash_id(UINT64_C(0x243f6a8885a308d3) ^ (uint32_t) code) :
    stable_symbol_hash(index, (uint32_t) code);
  return (size_t) hash_id(symbol ^
    ((uint64_t) parent * UINT64_C(0x9e3779b97f4a7c15))) &
    (capacity - 1);
}

static BOOL tree_child_cache_parent_enabled(
  Compact_back_demod_index index, uint32_t parent)
{
  size_t word = (size_t) parent / 64;
  return word < index->tree_child_cache_parent_words &&
    (index->tree_child_cache_parents[word] &
     (UINT64_C(1) << (parent % 64))) != 0;
}

static unsigned long long tree_child_cache_max_bytes(
  Compact_back_demod_index index)
{
  unsigned long long max_bytes = CBD_TREE_CHILD_CACHE_MAX_BYTES;
  if (index->tree_budget_bytes != 0 &&
      index->tree_budget_bytes / 8 < max_bytes)
    max_bytes = index->tree_budget_bytes / 8;
  return max_bytes;
}

static BOOL tree_child_cache_enable_parent(Compact_back_demod_index index,
                                           uint32_t parent)
{
  size_t word = (size_t) parent / 64;
  if (word >= index->tree_child_cache_parent_words) {
    size_t old_words = index->tree_child_cache_parent_words;
    size_t new_words = ((size_t) index->tree_node_count + 63) / 64;
    unsigned long long estimated = tree_estimated_bytes(index);
    unsigned long long cache_bytes, delta;
    if (new_words <= word)
      new_words = word + 1;
    if (new_words > SIZE_MAX / sizeof(*index->tree_child_cache_parents))
      fatal_error("compact_back_demod: tree child parent map overflow");
    delta = (new_words - old_words) *
      sizeof(*index->tree_child_cache_parents);
    cache_bytes = index->tree_child_cache_capacity *
        sizeof(*index->tree_child_cache) +
      old_words * sizeof(*index->tree_child_cache_parents);
    if (cache_bytes > tree_child_cache_max_bytes(index) ||
        delta > tree_child_cache_max_bytes(index) - cache_bytes ||
        (index->tree_budget_bytes != 0 &&
        (estimated > index->tree_budget_bytes ||
         delta > index->tree_budget_bytes - estimated))) {
      index->tree_child_cache_growth_denials++;
      return FALSE;
    }
    index->tree_child_cache_parents = safe_realloc(
      index->tree_child_cache_parents,
      new_words * sizeof(*index->tree_child_cache_parents));
    memset(index->tree_child_cache_parents + old_words, 0,
           (new_words - old_words) *
             sizeof(*index->tree_child_cache_parents));
    index->tree_child_cache_parent_words = new_words;
  }
  if (!tree_child_cache_parent_enabled(index, parent)) {
    index->tree_child_cache_parents[word] |=
      UINT64_C(1) << (parent % 64);
    index->tree_child_cache_parent_count++;
  }
  return TRUE;
}

static void tree_child_cache_put(Compact_back_demod_index index,
                                 uint32_t parent, int32_t code,
                                 uint32_t child, BOOL count_replacement)
{
  struct cbd_tree_child_cache_entry *entry;
  size_t slot;
  if (index->tree_child_cache_capacity == 0 ||
      !tree_child_cache_parent_enabled(index, parent))
    return;
  slot = tree_child_cache_slot(
    index, parent, code, index->tree_child_cache_capacity);
  entry = &index->tree_child_cache[slot];
  if (count_replacement && entry->child != CBD_NONE &&
      (entry->parent != parent || entry->code != code))
    index->tree_child_cache_replacements++;
  entry->parent = parent;
  entry->code = code;
  entry->child = child;
}

static void maybe_grow_tree_child_cache(Compact_back_demod_index index)
{
  struct cbd_tree_child_cache_entry *old, *replacement;
  size_t old_capacity, desired = 64, max_entries, i;
  unsigned long long max_bytes = tree_child_cache_max_bytes(index);
  unsigned long long estimated, delta;
  if (index->tree_child_cache_growth_blocked)
    return;
  if (index->tree_child_cache_parent_words >
      max_bytes / sizeof(*index->tree_child_cache_parents))
    max_bytes = 0;
  else
    max_bytes -= index->tree_child_cache_parent_words *
      sizeof(*index->tree_child_cache_parents);
  max_entries = (size_t) (max_bytes / sizeof(*index->tree_child_cache));
  while (desired <= SIZE_MAX / 2 && desired <= index->tree_node_count / 4)
    desired *= 2;
  while (desired > max_entries && desired > 1)
    desired /= 2;
  old_capacity = index->tree_child_cache_capacity;
  if (desired <= old_capacity || desired < 64) {
    if (desired <= old_capacity)
      return;
    index->tree_child_cache_growth_blocked = TRUE;
    index->tree_child_cache_growth_denials++;
    return;
  }
  estimated = tree_estimated_bytes(index);
  while (desired > old_capacity) {
    delta = (desired - old_capacity) *
      sizeof(*index->tree_child_cache);
    if (index->tree_budget_bytes == 0 ||
        (estimated <= index->tree_budget_bytes &&
         delta <= index->tree_budget_bytes - estimated))
      break;
    desired /= 2;
  }
  if (desired <= old_capacity) {
    index->tree_child_cache_growth_blocked = TRUE;
    index->tree_child_cache_growth_denials++;
    return;
  }
  replacement = safe_calloc(desired, sizeof(*replacement));
  old = index->tree_child_cache;
  index->tree_child_cache = replacement;
  index->tree_child_cache_capacity = desired;
  for (i = 0; i < old_capacity; i++)
    if (old[i].child != CBD_NONE)
      tree_child_cache_put(index, old[i].parent, old[i].code,
                           old[i].child, FALSE);
  safe_free(old);
}

static uint32_t tree_child_cache_get(Compact_back_demod_index index,
                                     uint32_t parent, int32_t code)
{
  struct cbd_tree_child_cache_entry *entry;
  size_t slot;
  index->tree_child_cache_lookups++;
  if (index->tree_child_cache_capacity == 0) {
    index->tree_child_cache_misses++;
    return CBD_NONE;
  }
  slot = tree_child_cache_slot(
    index, parent, code, index->tree_child_cache_capacity);
  entry = &index->tree_child_cache[slot];
  if (entry->child != CBD_NONE && entry->parent == parent &&
      entry->code == code) {
    if (entry->child >= index->tree_node_count ||
        tree_first_code(index, entry->child) != code)
      fatal_error("compact_back_demod: stale tree child cache");
    index->tree_child_cache_hits++;
    return entry->child;
  }
  index->tree_child_cache_misses++;
  return CBD_NONE;
}

static uint32_t new_tree_node(Compact_back_demod_index index,
                              Compact_term_slice tokens)
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
  index->tree_nodes[node].tokens = tokens;
  return node;
}

static int32_t tree_first_code(Compact_back_demod_index index,
                               uint32_t node)
{
  struct cbd_tree_node *n = &index->tree_nodes[node];
  if (compact_term_slice_length(n->tokens) == 0)
    fatal_error("compact_back_demod: empty nonroot tree edge");
  return compact_term_pool_slice_tokens(index->term_pool, n->tokens)[0];
}

static BOOL tree_is_terminal(Compact_back_demod_index index, uint32_t node)
{
  return (index->tree_nodes[node].child_or_posting &
          CBD_TREE_TERMINAL) != 0;
}

static uint32_t tree_first_child(Compact_back_demod_index index,
                                 uint32_t node)
{
  return tree_is_terminal(index, node) ? CBD_NONE :
    index->tree_nodes[node].child_or_posting;
}

static void tree_set_first_child(Compact_back_demod_index index,
                                 uint32_t node, uint32_t child)
{
  if (tree_is_terminal(index, node) || child > CBD_TREE_INDEX_MASK)
    fatal_error("compact_back_demod: invalid tree child");
  index->tree_nodes[node].child_or_posting = child;
}

static uint32_t tree_posting_list(Compact_back_demod_index index,
                                  uint32_t node)
{
  return tree_is_terminal(index, node) ?
    index->tree_nodes[node].child_or_posting & CBD_TREE_INDEX_MASK : CBD_NONE;
}

static void tree_set_posting_list(Compact_back_demod_index index,
                                  uint32_t node, uint32_t list)
{
  if (tree_first_child(index, node) != CBD_NONE ||
      list == CBD_NONE || list > CBD_TREE_INDEX_MASK)
    fatal_error("compact_back_demod: invalid tree terminal");
  index->tree_nodes[node].child_or_posting = CBD_TREE_TERMINAL | list;
}

static uint32_t insert_tree_path(Compact_back_demod_index index,
                                 Compact_term_slice slice)
{
  uint32_t parent = CBD_NONE;
  uint32_t position = 0;
  uint32_t length = compact_term_slice_length(slice);
  const int32_t *tokens = compact_term_pool_slice_tokens(
    index->term_pool, slice);
  while (position < length) {
    uint32_t current = tree_first_child(index, parent);
    uint32_t previous = CBD_NONE;
    int32_t wanted = tokens[position];
    while (current != CBD_NONE &&
           tree_code_compare(tree_first_code(index, current), wanted) < 0) {
      previous = current;
      current = index->tree_nodes[current].next_sibling;
    }
    if (current == CBD_NONE || tree_first_code(index, current) != wanted) {
      Compact_term_slice suffix;
      uint32_t added;
      if (!compact_term_slice_subslice(
            slice, position, length - position, &suffix))
        fatal_error("compact_back_demod: invalid tree suffix");
      added = new_tree_node(index, suffix);
      if (previous == CBD_NONE) {
        index->tree_nodes[added].next_sibling =
          tree_first_child(index, parent);
        tree_set_first_child(index, parent, added);
      }
      else {
        index->tree_nodes[added].next_sibling =
          index->tree_nodes[previous].next_sibling;
        index->tree_nodes[previous].next_sibling = added;
      }
      tree_child_cache_put(index, parent, wanted, added, TRUE);
      return added;
    }
    else {
      Compact_term_slice old_slice = index->tree_nodes[current].tokens;
      const int32_t *old_tokens = compact_term_pool_slice_tokens(
        index->term_pool, old_slice);
      uint32_t old_length = compact_term_slice_length(old_slice);
      uint32_t common = 0;
      while (common < old_length && position + common < length &&
             old_tokens[common] == tokens[position + common])
        common++;
      if (common == old_length) {
        if (tree_is_terminal(index, current) &&
            position + common < length)
          fatal_error("compact_back_demod: serialized term is a tree prefix");
        position += common;
        parent = current;
      }
      else {
        uint32_t old_next = index->tree_nodes[current].next_sibling;
        Compact_term_slice prefix, old_suffix;
        uint32_t split;
        uint32_t added;
        if (common == 0)
          fatal_error("compact_back_demod: invalid zero-length tree split");
        if (!compact_term_slice_subslice(old_slice, 0, common, &prefix) ||
            !compact_term_slice_subslice(
              old_slice, common, old_length - common, &old_suffix))
          fatal_error("compact_back_demod: invalid tree split slices");
        split = new_tree_node(index, prefix);
        index->tree_nodes[split].next_sibling = old_next;
        if (previous == CBD_NONE)
          tree_set_first_child(index, parent, split);
        else
          index->tree_nodes[previous].next_sibling = split;
        tree_child_cache_put(index, parent, wanted, split, TRUE);
        index->tree_nodes[current].tokens = old_suffix;
        index->tree_nodes[current].next_sibling = CBD_NONE;
        tree_set_first_child(index, split, current);
        tree_child_cache_put(index, split,
                             tree_first_code(index, current), current, TRUE);
        position += common;
        if (position == length)
          fatal_error("compact_back_demod: serialized term prefixes tree term");
        {
          Compact_term_slice suffix;
          if (!compact_term_slice_subslice(
                slice, position, length - position, &suffix))
            fatal_error("compact_back_demod: invalid added tree suffix");
          added = new_tree_node(index, suffix);
        }
        if (tree_code_compare(tree_first_code(index, added),
                              tree_first_code(index, current)) < 0) {
          index->tree_nodes[added].next_sibling = current;
          tree_set_first_child(index, split, added);
        }
        else
          index->tree_nodes[current].next_sibling = added;
        tree_child_cache_put(index, split,
                             tree_first_code(index, added), added, TRUE);
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
  if (strategy_uses_hot_tree(index->strategy)) {
    struct cbd_tree_root_state *state;
    if ((size_t) symbol >= index->tree_root_capacity)
      fatal_error("compact_back_demod: missing root work state");
    state = &index->tree_roots[symbol];
    if (state->physical_groups != ULLONG_MAX)
      state->physical_groups++;
  }
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
    uint32_t added = new_posting_block(index, FALSE);
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
                               uint32_t record, Compact_term_slice term)
{
  unsigned char encoded[5];
  size_t length = 0;
  uint32_t node = insert_tree_path(index, term);
  uint32_t list_index = tree_posting_list(index, node);
  uint32_t block;
  struct cbd_tree_posting_list *list;
  struct cbd_posting_block *tail;
  if (list_index == CBD_NONE) {
    list_index = new_tree_posting_list(index);
    tree_set_posting_list(index, node, list_index);
  }
  list = &index->tree_posting_lists[list_index];
  if (list->inline_record == CBD_NONE) {
    unsigned long long record_offset;
    unsigned long long term_offset;
    list->inline_record = record;
    if (record >= index->record_count)
      fatal_error("compact_back_demod: invalid tree representative record");
    record_offset = compact_term_slice_offset(index->records[record].tokens);
    term_offset = compact_term_slice_offset(term);
    if (term_offset < record_offset ||
        term_offset - record_offset > UINT32_MAX)
      fatal_error("compact_back_demod: tree representative offset overflow");
    list->inline_relative = (uint32_t) (term_offset - record_offset);
    index->posting_count++;
    index->tree_posting_count++;
    return;
  }
  if (record <= list->inline_record)
    fatal_error("compact_back_demod: nonmonotone tree posting record");
  length += encode_u32(encoded + length, record);
  if (length > CBD_POSTING_BLOCK_PAYLOAD)
    fatal_error("compact_back_demod: oversized tree posting entry");
  block = list->posting_tail;
  if ((list->posting_head & CBD_TREE_DIRECT_RECORD) != 0) {
    unsigned char first[5];
    size_t first_length = encode_u32(
      first, list->posting_head & CBD_TREE_INDEX_MASK);
    uint32_t added = new_posting_block(index, TRUE);
    if (first_length + length > CBD_POSTING_BLOCK_PAYLOAD)
      fatal_error("compact_back_demod: oversized direct tree postings");
    memcpy(index->posting_blocks[added].data, first, first_length);
    memcpy(index->posting_blocks[added].data + first_length,
           encoded, length);
    index->posting_blocks[added].used = (uint16_t) (first_length + length);
    index->posting_blocks[added].count = 2;
    list->posting_head = added;
    list->posting_tail = added;
    index->posting_count++;
    index->tree_posting_count++;
    index->posting_stream_used += first_length + length;
    index->tree_posting_stream_used += first_length + length;
    return;
  }
  if (list->posting_head == CBD_NONE) {
    if (record > CBD_TREE_INDEX_MASK)
      fatal_error("compact_back_demod: tree record exceeds direct encoding");
    list->posting_head = CBD_TREE_DIRECT_RECORD | record;
    index->posting_count++;
    index->tree_posting_count++;
    return;
  }
  if (block == CBD_NONE ||
      index->posting_blocks[block].used + length >
        CBD_POSTING_BLOCK_PAYLOAD) {
    uint32_t added = new_posting_block(index, TRUE);
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
  index->posting_count++;
  index->tree_posting_count++;
  index->posting_stream_used += length;
  index->tree_posting_stream_used += length;
}

static uint64_t signature_child_path(uint64_t path, unsigned child)
{
  return hash_id(path ^ (UINT64_C(0x9e3779b97f4a7c15) + child));
}

static cbd_path_mask path_feature_bits(Compact_back_demod_index index,
                                       uint64_t path, uint32_t symbol)
{
  uint64_t stable = stable_symbol_hash(index, symbol);
  if (strategy_uses_mask8(index->strategy)) {
    uint32_t mixed = (uint32_t) path * UINT32_C(0x9e3779b1) ^
                     (uint32_t) stable * UINT32_C(0x85ebca6b) ^
                     (uint32_t) (stable >> 32);
    mixed ^= mixed >> 16;
    return UINT64_C(1) << (mixed % 8U);
  }
  else {
    uint64_t mixed = hash_id(
      path ^ (stable * UINT64_C(0x85ebca77c2b2ae63)));
    unsigned first = (unsigned) (mixed & 31U);
    unsigned second = (unsigned) ((mixed >> 32) & 31U);
    if (second == first)
      second = (second + 15U) & 31U;
    return (UINT32_C(1) << first) | (UINT32_C(1) << second);
  }
}

static cbd_path_mask token_path_mask_rec(Compact_back_demod_index index,
                                         const int32_t *tokens,
                                         uint32_t end,
                                         uint32_t *position, unsigned depth,
                                         uint64_t path)
{
  int32_t code;
  int i, arity;
  cbd_path_mask mask = 0;
  if (*position >= end)
    fatal_error("compact_back_demod: corrupt path token offset");
  code = tokens[(*position)++];
  arity = code < 0 ? 0 : sn_to_arity(code);
  for (i = 0; i < arity; i++) {
    uint64_t child_path =
      strategy_uses_mask8(index->strategy) ?
        (uint32_t) path * 17U + (uint32_t) i + 1U :
        signature_child_path(path, (unsigned) i);
    if (*position >= end)
      fatal_error("compact_back_demod: corrupt path child offset");
    if ((!strategy_uses_mask8(index->strategy) ||
         depth < CBD_PATH_DEPTH) && tokens[*position] >= 0)
      mask |= path_feature_bits(
        index,
        child_path, (uint32_t) tokens[*position]);
    mask |= token_path_mask_rec(index, tokens, end, position,
                                depth + 1, child_path);
  }
  return !strategy_uses_mask8(index->strategy) ||
         depth < CBD_PATH_DEPTH ? mask : 0;
}

static cbd_path_mask token_path_mask(Compact_back_demod_index index,
                                     const int32_t *tokens,
                                     uint32_t offset, uint32_t end)
{
  uint32_t position = offset;
  return token_path_mask_rec(index, tokens, end, &position, 0, 0);
}

static cbd_path_mask term_path_mask_rec(Term term, unsigned depth,
                                        uint64_t path,
                                        Compact_back_demod_index index)
{
  cbd_path_mask mask = 0;
  int i;
  if (VARIABLE(term) ||
      (strategy_uses_mask8(index->strategy) &&
       depth >= CBD_PATH_DEPTH))
    return 0;
  for (i = 0; i < ARITY(term); i++) {
    Term child = ARG(term, i);
    uint64_t child_path =
      strategy_uses_mask8(index->strategy) ?
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

static uint32_t token_term_end(const int32_t *tokens, uint32_t position,
                               uint32_t end);
static unsigned long long occurrence_items(
  Compact_back_demod_index index, uint32_t offset, uint32_t length);

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
                               Compact_term_slice term,
                               unsigned long long base,
                               struct cbd_symbol_set *symbols)
{
  const int32_t *tokens = compact_term_pool_slice_tokens(
    index->term_pool, term);
  unsigned long long term_offset = compact_term_slice_offset(term);
  uint32_t length = compact_term_slice_length(term);
  unsigned long long relative_base;
  uint32_t i;
  if (term_offset < base ||
      term_offset - base > COMPACT_TERM_SLICE_LENGTH_MAX ||
      length > COMPACT_TERM_SLICE_LENGTH_MAX - (term_offset - base))
    fatal_error("compact_back_demod: pooled clause-relative offset overflow");
  relative_base = term_offset - base;
  for (i = 0; i < length; i++) {
    int32_t code = tokens[i];
    if (code >= 0) {
      uint32_t term_end = token_term_end(tokens, i, length);
      note_symbol(symbols, (uint32_t) code,
                  (uint32_t) relative_base + i,
                  term_end - i,
                  index->strategy == COMPACT_BACK_DEMOD_CODE_TREE ? 0 :
                    token_path_mask(index, tokens, i, length));
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

static int compare_tree_occurrence(const int32_t *tokens,
                                   const struct cbd_local_occurrence *a,
                                   const struct cbd_local_occurrence *b)
{
  uint32_t common = a->length < b->length ? a->length : b->length;
  uint32_t i;
  for (i = 0; i < common; i++) {
    int32_t ac = tokens[a->offset + i];
    int32_t bc = tokens[b->offset + i];
    if (ac != bc)
      return ac < bc ? -1 : 1;
  }
  if (a->length != b->length)
    return a->length < b->length ? -1 : 1;
  return a->offset < b->offset ? -1 : a->offset > b->offset ? 1 : 0;
}

static void sift_tree_occurrences(const int32_t *tokens,
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
        compare_tree_occurrence(tokens, &values[child],
                                &values[child + 1]) < 0)
      child++;
    if (compare_tree_occurrence(tokens, &values[root],
                                &values[child]) >= 0)
      return;
    saved = values[root];
    values[root] = values[child];
    values[child] = saved;
    root = child;
  }
}

static void sort_tree_occurrences(const int32_t *tokens,
                                  struct cbd_local_occurrence *values,
                                  size_t count)
{
  size_t start, end;
  if (count < 2)
    return;
  for (start = count / 2; start != 0; start--)
    sift_tree_occurrences(tokens, values, start - 1, count);
  for (end = count - 1; end != 0; end--) {
    struct cbd_local_occurrence saved = values[0];
    values[0] = values[end];
    values[end] = saved;
    sift_tree_occurrences(tokens, values, 0, end);
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
  index->tree_min_tokens = Back_demod_tree_min_tokens;
  index->tree_admit_work = Back_demod_tree_admit_work;
  index->tree_build_factor = Back_demod_tree_build_factor;
  index->tree_budget_bytes = Back_demod_tree_budget_bytes;
  index->tree_complete = TRUE;
  index->position_admit_work = Back_demod_position_admit_work;
  index->position_min_gain = Back_demod_position_min_gain;
  index->position_build_factor = Back_demod_position_build_factor;
  index->position_budget_pct = Back_demod_position_budget_pct;
  index->position_budget_bytes = Back_demod_position_budget_bytes;
  index->position_complete = TRUE;
  index->position_admission_enabled = Back_demod_position_admission;
  if (strategy == COMPACT_BACK_DEMOD_ADAPTIVE) {
    index->route_profile_capacity = CBD_ROUTE_PROFILE_CAPACITY;
    index->route_profiles = safe_calloc(
      index->route_profile_capacity, sizeof(*index->route_profiles));
  }
  index->id_map = compact_id_map_init(1);
  index->maintenance_clock = clock_init("compact_back_demod_maintenance");
  if (new_posting_block(index, FALSE) != CBD_NONE)
    fatal_error("compact_back_demod: invalid posting block sentinel");
  if (strategy_uses_tree(strategy)) {
    if (new_tree_node(index, 0) != CBD_NONE ||
        new_tree_posting_list(index) != CBD_NONE)
      fatal_error("compact_back_demod: invalid tree sentinel");
  }
  if (strategy_uses_paths(strategy)) {
    ENSURE_ARRAY(index, path_buckets, path_bucket_count,
                 path_bucket_capacity,
                 "compact_back_demod: path bucket overflow");
    memset(&index->path_buckets[0], 0, sizeof(index->path_buckets[0]));
    index->path_bucket_count = 1;
  }
  if (strategy_uses_position(strategy)) {
    ENSURE_ARRAY(index, position_buckets, position_bucket_count,
                 position_bucket_capacity,
                 "compact_back_demod: position bucket overflow");
    memset(&index->position_buckets[0], 0,
           sizeof(index->position_buckets[0]));
    index->position_bucket_count = 1;
    if (new_position_block(index) != CBD_NONE)
      fatal_error("compact_back_demod: invalid position block sentinel");
  }
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
      strategy != COMPACT_BACK_DEMOD_CODE_TREE &&
      strategy != COMPACT_BACK_DEMOD_HYBRID_TREE &&
      strategy != COMPACT_BACK_DEMOD_HOT_ROOT_TREE &&
      strategy != COMPACT_BACK_DEMOD_POSITION &&
      strategy != COMPACT_BACK_DEMOD_ADAPTIVE)
    fatal_error("compact_back_demod: invalid strategy");
  Back_demod_strategy = strategy;
}

void compact_back_demod_set_tree_min_tokens(unsigned tokens)
{
  if (tokens == 0)
    fatal_error("compact_back_demod: tree minimum must be positive");
  Back_demod_tree_min_tokens = tokens;
}

void compact_back_demod_set_tree_budget_kb(unsigned kilobytes)
{
  Back_demod_tree_budget_bytes =
    (unsigned long long) kilobytes * 1024;
}

void compact_back_demod_set_tree_admit_work(unsigned groups)
{
  if (groups == 0)
    fatal_error("compact_back_demod: tree admission work must be positive");
  Back_demod_tree_admit_work = groups;
}

void compact_back_demod_set_tree_build_factor(unsigned factor)
{
  if (factor == 0)
    fatal_error("compact_back_demod: tree build factor must be positive");
  Back_demod_tree_build_factor = factor;
}

void compact_back_demod_set_position_options(unsigned admit_work,
                                             unsigned min_gain,
                                             unsigned build_factor,
                                             unsigned budget_kb,
                                             unsigned budget_pct,
                                             BOOL admission_enabled)
{
  if (admit_work == 0 || min_gain == 0 || build_factor == 0 ||
      budget_pct > 1000)
    fatal_error("compact_back_demod: invalid position options");
  Back_demod_position_admit_work = admit_work;
  Back_demod_position_min_gain = min_gain;
  Back_demod_position_build_factor = build_factor;
  Back_demod_position_budget_bytes =
    (unsigned long long) budget_kb * 1024;
  Back_demod_position_budget_pct = budget_pct;
  Back_demod_position_admission = admission_enabled;
}

BOOL compact_back_demod_add(Compact_back_demod_index index, Topform clause)
{
  uint32_t record_index;
  struct cbd_record *record;
  struct cbd_symbol_set symbols;
  Literals literal;
  unsigned long long record_base = 0;
  unsigned long long token_end = 0;
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
      Compact_term_slice term = compact_term_pool_intern_slice(
        index->term_pool, clause->id, clause->literals, ARG(atom, arg));
      unsigned long long offset = compact_term_slice_offset(term);
      uint32_t length = compact_term_slice_length(term);
      if (!have_tokens) {
        record_base = offset;
        token_end = offset + length;
        have_tokens = TRUE;
      }
      else {
        if (offset < record_base)
          fatal_error("compact_back_demod: nonmonotone pooled clause slice");
        if (offset + length > token_end)
          token_end = offset + length;
      }
      collect_term_slice(index, term, record_base, &symbols);
    }
  }
  if ((have_tokens &&
       token_end - record_base > COMPACT_TERM_SLICE_LENGTH_MAX) ||
      !compact_term_slice_encode(
        have_tokens ? record_base : 0,
        have_tokens ? (uint32_t) (token_end - record_base) : 0,
        &record->tokens))
    fatal_error("compact_back_demod: pooled clause slice overflow");
  if (strategy_uses_paths(index->strategy)) {
    qsort(symbols.occurrence_values, symbols.occurrence_count,
          sizeof(*symbols.occurrence_values), increasing_local_bucket);
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

  append_admitted_position_features(index, record_index);

  if (strategy_uses_tree(index->strategy) &&
      (index->tree_complete ||
       strategy_uses_hot_tree(index->strategy))) {
    size_t i = 0;
    size_t eligible = 0;
    if (strategy_uses_hot_tree(index->strategy)) {
      for (i = 0; i < symbols.occurrence_count; i++) {
        unsigned symbol = symbols.occurrence_values[i].symbol;
        if ((size_t) symbol < index->tree_root_capacity &&
            index->tree_roots[symbol].admitted)
          eligible++;
      }
      if (eligible == 0)
        goto tree_index_done;
    }
    const int32_t *record_tokens = compact_term_pool_slice_tokens(
      index->term_pool, record->tokens);
    sort_tree_occurrences(record_tokens,
                          symbols.occurrence_values,
                          symbols.occurrence_count);
    if (index->strategy == COMPACT_BACK_DEMOD_HYBRID_TREE) {
      unsigned long long worst;
      for (i = 0; i < symbols.occurrence_count; i++)
        if (symbols.occurrence_values[i].length >= index->tree_min_tokens)
          eligible++;
      worst = eligible > (ULLONG_MAX - 4096) / 96 ? ULLONG_MAX :
        4096 + (unsigned long long) eligible * 96;
      if (eligible != 0 && index->tree_budget_bytes != 0 &&
          (tree_estimated_bytes(index) > index->tree_budget_bytes ||
           worst > index->tree_budget_bytes -
             tree_estimated_bytes(index))) {
        index->tree_complete = FALSE;
        index->tree_budget_exhaustions++;
      }
    }
    else if (strategy_uses_hot_tree(index->strategy)) {
      unsigned long long worst;
      worst = eligible > (ULLONG_MAX - 4096) / 96 ? ULLONG_MAX :
        4096 + (unsigned long long) eligible * 96;
      if (eligible != 0 && index->tree_budget_bytes != 0 &&
          (tree_estimated_bytes(index) > index->tree_budget_bytes ||
           worst > index->tree_budget_bytes -
             tree_estimated_bytes(index))) {
        /* A hot-root tree is only an optimization over the complete path
           index.  If later growth will not fit, demote the affected roots;
           do not globally disable unrelated trees and retain their bytes. */
        for (i = 0; i < symbols.occurrence_count; i++) {
          unsigned symbol = symbols.occurrence_values[i].symbol;
          if ((size_t) symbol < index->tree_root_capacity &&
              index->tree_roots[symbol].admitted) {
            index->tree_roots[symbol].admitted = FALSE;
            index->tree_roots[symbol].rejected = TRUE;
            index->tree_root_demotions++;
          }
        }
        index->tree_budget_exhaustions++;
      }
    }
    i = 0;
    while ((index->tree_complete ||
            strategy_uses_hot_tree(index->strategy)) &&
           i < symbols.occurrence_count) {
      size_t j = i;
      uint32_t relative = symbols.occurrence_values[i].offset;
      uint32_t length = symbols.occurrence_values[i].length;
      while (j < symbols.occurrence_count &&
             symbols.occurrence_values[j].length == length &&
             memcmp(record_tokens + relative,
                    record_tokens + symbols.occurrence_values[j].offset,
                    (size_t) length * sizeof(*record_tokens)) == 0)
        j++;
      if (index->strategy == COMPACT_BACK_DEMOD_CODE_TREE ||
          (index->strategy == COMPACT_BACK_DEMOD_HYBRID_TREE &&
           length >= index->tree_min_tokens) ||
          (strategy_uses_hot_tree(index->strategy) &&
           (size_t) symbols.occurrence_values[i].symbol <
             index->tree_root_capacity &&
           index->tree_roots[
             symbols.occurrence_values[i].symbol].admitted))
        {
          Compact_term_slice term;
          if (!compact_term_slice_subslice(
                record->tokens, relative, length, &term))
            fatal_error("compact_back_demod: invalid occurrence slice");
          append_tree_record(index, record_index, term);
        }
      i = j;
    }
  }
tree_index_done:
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

static uint32_t token_term_end(const int32_t *tokens, uint32_t position,
                               uint32_t end)
{
  int32_t code;
  int i, arity;
  if (position >= end)
    fatal_error("compact_back_demod: corrupt term token offset");
  code = tokens[position++];
  arity = code < 0 ? 0 : sn_to_arity(code);
  for (i = 0; i < arity; i++)
    position = token_term_end(tokens, position, end);
  return position;
}

struct cbd_binding {
  uint32_t offset;
  uint32_t end;
  unsigned char bound;
};

static BOOL match_token_term(const int32_t *tokens, uint32_t token_end,
                             Term pattern,
                             uint32_t *position,
                             struct cbd_binding *bindings)
{
  uint32_t at = *position;
  int i;
  if (at >= token_end)
    fatal_error("compact_back_demod: corrupt match token offset");
  if (VARIABLE(pattern)) {
    int variable = VARNUM(pattern);
    uint32_t end;
    if (variable < 0 || variable >= MAX_VARS)
      fatal_error("compact_back_demod: pattern variable exceeds MAX_VARS");
    end = token_term_end(tokens, at, token_end);
    if (bindings[variable].bound) {
      size_t old_length = bindings[variable].end - bindings[variable].offset;
      size_t new_length = end - at;
      if (old_length != new_length ||
          memcmp(tokens + bindings[variable].offset,
                 tokens + at,
                 new_length * sizeof(*tokens)) != 0)
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
  if (tokens[at] < 0 || tokens[at] != SYMNUM(pattern))
    return FALSE;
  *position = at + 1;
  for (i = 0; i < ARITY(pattern); i++)
    if (!match_token_term(tokens, token_end, ARG(pattern, i),
                          position, bindings))
      return FALSE;
  return TRUE;
}

static BOOL occurrence_matches(const int32_t *tokens, uint32_t token_end,
                               Term pattern, uint32_t token_offset)
{
  struct cbd_binding bindings[MAX_VARS];
  uint32_t position = token_offset;
  memset(bindings, 0, sizeof(bindings));
  return match_token_term(tokens, token_end, pattern, &position, bindings);
}

static uint64_t position_child_path(uint64_t path, unsigned child)
{
  return hash_id(path ^
    (UINT64_C(0x9e3779b97f4a7c15) * ((uint64_t) child + 1)));
}

static void ensure_position_query(Compact_back_demod_index index,
                                  size_t needed)
{
  while (needed > index->position_query_capacity) {
    index->position_query_capacity = grow_record_capacity(
      index->position_query_capacity, sizeof(*index->position_query),
      "compact_back_demod: position query overflow");
    index->position_query = safe_realloc(
      index->position_query,
      index->position_query_capacity * sizeof(*index->position_query));
  }
}

static void collect_pattern_position_features_rec(
  Compact_back_demod_index index, Term term, uint64_t path, size_t *count)
{
  int i;
  if (VARIABLE(term))
    return;
  for (i = 0; i < ARITY(term); i++) {
    Term child = ARG(term, i);
    uint64_t child_path = position_child_path(path, (unsigned) i);
    if (!VARIABLE(child)) {
      size_t j;
      uint32_t symbol = (uint32_t) SYMNUM(child);
      for (j = 0; j < *count; j++)
        if (index->position_query[j].path == child_path &&
            index->position_query[j].symbol == symbol)
          break;
      if (j == *count) {
        ensure_position_query(index, *count + 1);
        memset(&index->position_query[*count], 0,
               sizeof(index->position_query[*count]));
        index->position_query[*count].path = child_path;
        index->position_query[*count].symbol = symbol;
        (*count)++;
      }
      collect_pattern_position_features_rec(
        index, child, child_path, count);
    }
  }
}

static size_t collect_pattern_position_features(
  Compact_back_demod_index index, Term pattern)
{
  size_t count = 0;
  if (!VARIABLE(pattern))
    collect_pattern_position_features_rec(index, pattern, 0, &count);
  return count;
}

static uint32_t mark_subject_position_features_rec(
  const int32_t *tokens, uint32_t token_end,
  uint32_t position, uint64_t path,
  struct cbd_position_query_feature *features, size_t feature_count,
  unsigned char *matched)
{
  int32_t code;
  int i, arity;
  if (position >= token_end)
    fatal_error("compact_back_demod: corrupt position subject");
  code = tokens[position++];
  arity = code < 0 ? 0 : sn_to_arity(code);
  for (i = 0; i < arity; i++) {
    uint32_t child_start = position;
    uint64_t child_path = position_child_path(path, (unsigned) i);
    int32_t child_code;
    size_t j;
    if (child_start >= token_end)
      fatal_error("compact_back_demod: corrupt position child");
    child_code = tokens[child_start];
    if (child_code >= 0)
      for (j = 0; j < feature_count; j++)
        if (!matched[j] && features[j].path == child_path &&
            features[j].symbol == (uint32_t) child_code)
          matched[j] = TRUE;
    position = mark_subject_position_features_rec(
      tokens, token_end, child_start, child_path,
      features, feature_count, matched);
  }
  return position;
}

static void record_position_features(
  Compact_back_demod_index index, struct cbd_record *record,
  uint32_t root_symbol, struct cbd_position_query_feature *features,
  size_t feature_count, unsigned char *matched)
{
  const int32_t *tokens;
  uint32_t i, end = compact_term_slice_length(record->tokens);
  memset(matched, 0, feature_count);
  if (end == 0)
    return;
  tokens = compact_term_pool_slice_tokens(index->term_pool, record->tokens);
  for (i = 0; i < end; i++)
    if (tokens[i] >= 0 && (uint32_t) tokens[i] == root_symbol)
      (void) mark_subject_position_features_rec(
        tokens, end, i, 0, features, feature_count, matched);
}

static void record_position_occurrences(
  Compact_back_demod_index index, struct cbd_record *record,
  uint32_t root_symbol, struct cbd_position_query_feature *feature,
  struct cbd_symbol_set *occurrences)
{
  const int32_t *tokens;
  uint32_t i, end = compact_term_slice_length(record->tokens);
  if (end == 0)
    return;
  tokens = compact_term_pool_slice_tokens(index->term_pool, record->tokens);
  for (i = 0; i < end; i++)
    if (tokens[i] >= 0 && (uint32_t) tokens[i] == root_symbol) {
      unsigned char matched = FALSE;
      (void) mark_subject_position_features_rec(
        tokens, end, i, 0, feature, 1, &matched);
      if (matched)
        note_symbol(occurrences, root_symbol,
                    i, 0, 0);
    }
}

static BOOL append_record_position_feature(
  Compact_back_demod_index index, uint32_t bucket_index,
  uint32_t record_index)
{
  struct cbd_position_bucket *bucket =
    &index->position_buckets[bucket_index];
  struct cbd_position_query_feature feature;
  struct cbd_symbol_set occurrences;
  uint32_t stream_offset, previous = 0;
  size_t i;
  memset(&feature, 0, sizeof(feature));
  feature.path = bucket->path;
  feature.symbol = bucket->symbol;
  memset(&occurrences, 0, sizeof(occurrences));
  occurrences.occurrence_values = occurrences.occurrence_fixed;
  occurrences.occurrence_capacity =
    sizeof(occurrences.occurrence_fixed) /
      sizeof(occurrences.occurrence_fixed[0]);
  record_position_occurrences(
    index, &index->records[record_index], bucket->root_symbol,
    &feature, &occurrences);
  if (occurrences.occurrence_count == 0) {
    if (occurrences.occurrence_values != occurrences.occurrence_fixed)
      safe_free(occurrences.occurrence_values);
    return FALSE;
  }
  add_position_bitmap_record(index, bucket, record_index);
  stream_offset = (uint32_t) index->occurrence_count;
  for (i = 0; i < occurrences.occurrence_count; i++) {
    uint32_t offset = occurrences.occurrence_values[i].offset;
    append_occurrence_delta(index, i == 0 ? offset : offset - previous);
    previous = offset;
  }
  append_position_group(
    index, bucket_index, record_index, stream_offset,
    (uint32_t) index->occurrence_count - stream_offset);
  if (occurrences.occurrence_values != occurrences.occurrence_fixed)
    safe_free(occurrences.occurrence_values);
  return TRUE;
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
  const int32_t *tokens = compact_term_pool_slice_tokens(
    index->term_pool, record->tokens);
  uint32_t token_length = compact_term_slice_length(record->tokens);
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
    if (relative >= token_length || tokens[relative] != symbol)
      fatal_error("compact_back_demod: corrupt occurrence offset");
    if (occurrence_matches(tokens, token_length, pattern, relative))
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

static void examine_tree_group(Compact_back_demod_index index,
                               uint32_t record_index,
                               unsigned long long exclude_id,
                               size_t *count)
{
  struct cbd_record *record;
  if (record_index == CBD_NONE || record_index >= index->record_count)
    fatal_error("compact_back_demod: corrupt tree posting record");
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
  collect_record(index, record_index, exclude_id, count);
}

static void collect_tree_posting_list(
  Compact_back_demod_index index, struct cbd_tree_posting_list *list,
  Term pattern, unsigned long long exclude_id, size_t *count)
{
  struct cbd_record *representative;
  const int32_t *tokens;
  uint32_t token_length;
  uint32_t block;
  if (list->inline_record == CBD_NONE ||
      list->inline_record >= index->record_count)
    fatal_error("compact_back_demod: missing inline tree posting");
  representative = &index->records[list->inline_record];
  tokens = compact_term_pool_slice_tokens(index->term_pool,
                                           representative->tokens);
  token_length = compact_term_slice_length(representative->tokens);
  if (list->inline_relative >= token_length)
    fatal_error("compact_back_demod: corrupt tree representative offset");
  index->occurrences_examined++;
  if (!occurrence_matches(tokens, token_length, pattern,
                          list->inline_relative))
    return;
  index->posting_bytes_decoded += sizeof(uint32_t);
  index->query_bytes_decoded += sizeof(uint32_t);
  examine_tree_group(index, list->inline_record, exclude_id, count);
  if ((list->posting_head & CBD_TREE_DIRECT_RECORD) != 0) {
    index->posting_bytes_decoded += sizeof(uint32_t);
    index->query_bytes_decoded += sizeof(uint32_t);
    examine_tree_group(index,
                       list->posting_head & CBD_TREE_INDEX_MASK,
                       exclude_id, count);
    return;
  }
  for (block = list->posting_head; block != CBD_NONE;
       block = index->posting_blocks[block].next) {
    const struct cbd_posting_block *current;
    uint16_t position = 0;
    uint16_t entries = 0;
    if (block >= index->posting_block_count)
      fatal_error("compact_back_demod: corrupt tree posting block");
    current = &index->posting_blocks[block];
    index->posting_bytes_decoded += current->used;
    index->query_bytes_decoded += current->used;
    while (position < current->used) {
      uint32_t record_index = decode_posting_value(current, &position);
      examine_tree_group(index, record_index, exclude_id, count);
      entries++;
    }
    if (position != current->used || entries != current->count)
      fatal_error("compact_back_demod: corrupt tree posting block contents");
  }
}

static unsigned long long backfill_root_group(
  Compact_back_demod_index index, uint32_t record_index,
  uint32_t occurrence_offset, uint32_t occurrence_length, BOOL build)
{
  struct cbd_record *record;
  uint32_t position = occurrence_offset;
  uint32_t end;
  uint32_t relative = 0;
  unsigned long long occurrences = 0;
  struct cbd_symbol_set terms;
  const int32_t *tokens;
  uint32_t token_length;
  if (record_index == CBD_NONE || record_index >= index->record_count ||
      occurrence_offset > index->occurrence_count ||
      occurrence_length > index->occurrence_count - occurrence_offset)
    fatal_error("compact_back_demod: corrupt root backfill group");
  if (!build)
    return occurrence_items(index, occurrence_offset, occurrence_length);
  index->tree_root_backfill_groups++;
  record = &index->records[record_index];
  if (!record->active)
    return 0;
  tokens = compact_term_pool_slice_tokens(index->term_pool, record->tokens);
  token_length = compact_term_slice_length(record->tokens);
  memset(&terms, 0, sizeof(terms));
  terms.occurrence_values = terms.occurrence_fixed;
  terms.occurrence_capacity = sizeof(terms.occurrence_fixed) /
    sizeof(terms.occurrence_fixed[0]);
  end = occurrence_offset + occurrence_length;
  while (position < end) {
    uint32_t delta = decode_occurrence_delta(index, &position, end);
    uint32_t token_end;
    if (delta > UINT32_MAX - relative)
      fatal_error("compact_back_demod: root backfill offset overflow");
    relative += delta;
    if (relative >= token_length)
      fatal_error("compact_back_demod: corrupt root backfill occurrence");
    token_end = token_term_end(tokens, relative, token_length);
    note_symbol(&terms, (uint32_t) tokens[relative], relative,
                token_end - relative, 0);
    occurrences++;
  }
  sort_tree_occurrences(tokens,
                        terms.occurrence_values, terms.occurrence_count);
  {
    size_t i = 0;
    while (i < terms.occurrence_count) {
      size_t j = i;
      uint32_t local = terms.occurrence_values[i].offset;
      uint32_t length = terms.occurrence_values[i].length;
      while (j < terms.occurrence_count &&
             terms.occurrence_values[j].length == length &&
             memcmp(tokens + local,
                    tokens + terms.occurrence_values[j].offset,
                    (size_t) length * sizeof(*tokens)) == 0)
        j++;
      {
        Compact_term_slice term;
        if (!compact_term_slice_subslice(
              record->tokens, local, length, &term))
          fatal_error("compact_back_demod: invalid root backfill slice");
        append_tree_record(index, record_index, term);
      }
      i = j;
    }
  }
  if (terms.occurrence_values != terms.occurrence_fixed)
    safe_free(terms.occurrence_values);
  index->tree_root_backfill_occurrences += occurrences;
  return occurrences;
}

static unsigned long long process_root_postings(
  Compact_back_demod_index index, unsigned symbol, BOOL build)
{
  uint32_t bucket_index;
  unsigned long long occurrences = 0;
  if ((size_t) symbol >= index->symbol_capacity)
    return 0;
  for (bucket_index = index->symbol_buckets[symbol];
       bucket_index != CBD_NONE;
       bucket_index = index->path_buckets[bucket_index].next) {
    struct cbd_path_bucket *bucket = &index->path_buckets[bucket_index];
    uint32_t block;
    uint32_t record_index = bucket->inline_record;
    uint32_t occurrence_offset = bucket->inline_occurrence;
    occurrences += backfill_root_group(
      index, record_index, occurrence_offset, bucket->inline_length, build);
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
          fatal_error("compact_back_demod: root backfill posting overflow");
        record_index += delta;
        occurrence_offset += occurrence_delta;
        occurrences += backfill_root_group(
          index, record_index, occurrence_offset, occurrence_length, build);
        entries++;
      }
      if (position != current->used || entries != current->count)
        fatal_error("compact_back_demod: corrupt root backfill block");
    }
  }
  return occurrences;
}

static void maybe_admit_hot_root(Compact_back_demod_index index,
                                 Term pattern,
                                 unsigned long long query_work)
{
  unsigned symbol;
  struct cbd_tree_root_state *state;
  unsigned long long occurrences, worst, estimated, required_work;
  if (!strategy_uses_hot_tree(index->strategy) ||
      VARIABLE(pattern))
    return;
  symbol = (unsigned) SYMNUM(pattern);
  if ((size_t) symbol >= index->tree_root_capacity)
    return;
  state = &index->tree_roots[symbol];
  if (ULLONG_MAX - state->fallback_work < query_work)
    state->fallback_work = ULLONG_MAX;
  else
    state->fallback_work += query_work;
  if (ULLONG_MAX - index->tree_fallback_work < query_work)
    index->tree_fallback_work = ULLONG_MAX;
  else
    index->tree_fallback_work += query_work;
  if (state->admitted || state->rejected ||
      state->fallback_work <
        (state->next_check_work == 0 ? index->tree_admit_work :
         state->next_check_work))
    return;
  clock_start(index->maintenance_clock);
  occurrences = process_root_postings(index, symbol, FALSE);
  index->tree_root_censuses++;
  if (ULLONG_MAX - index->tree_root_census_occurrences < occurrences)
    index->tree_root_census_occurrences = ULLONG_MAX;
  else
    index->tree_root_census_occurrences += occurrences;
  required_work = occurrences > ULLONG_MAX / index->tree_build_factor ?
    ULLONG_MAX : occurrences * index->tree_build_factor;
  if (required_work < index->tree_admit_work)
    required_work = index->tree_admit_work;
  if (state->fallback_work < required_work) {
    /* Count the current root before committing memory, then wait until the
       measured fallback cost can amortize that root's present construction
       cost.  A later census accounts for growth since this observation. */
    state->next_check_work = required_work;
    index->tree_root_cost_deferrals++;
    clock_stop(index->maintenance_clock);
    return;
  }
  worst = occurrences > (ULLONG_MAX - 4096) / 96 ? ULLONG_MAX :
    4096 + occurrences * 96;
  estimated = tree_estimated_bytes(index);
  if (index->tree_budget_bytes != 0 &&
      (estimated > index->tree_budget_bytes ||
       worst > index->tree_budget_bytes - estimated)) {
    state->rejected = TRUE;
    index->tree_root_rejections++;
  }
  else {
    (void) process_root_postings(index, symbol, TRUE);
    state->admitted = TRUE;
    state->next_check_work = 0;
    index->tree_root_admissions++;
    update_peak(index);
  }
  clock_stop(index->maintenance_clock);
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
   pattern variable consumes exactly one complete stored subterm.  A terminal
   represents one exact token sequence, so one direct match against its
   retained representative enforces repeated-variable equality for every
   clause posting in that terminal. */
static void collect_tree_candidates(Compact_back_demod_index index,
                                    uint32_t node,
                                    uint32_t query_position,
                                    uint32_t query_end,
                                    size_t pending,
                                    unsigned long long exclude_id,
                                    size_t *count)
{
  struct cbd_tree_node *edge = &index->tree_nodes[node];
  const int32_t *tokens = compact_term_pool_slice_tokens(
    index->term_pool, edge->tokens);
  uint32_t token_length = compact_term_slice_length(edge->tokens);
  uint32_t at, child;
  index->tree_nodes_examined++;
  for (at = 0; at < token_length; at++) {
    int32_t code = tokens[at];
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
    uint32_t posting_list = tree_posting_list(index, node);
    struct cbd_tree_posting_list *list;
    if (posting_list == CBD_NONE)
      return;
    if (posting_list >= index->tree_posting_list_count)
      fatal_error("compact_back_demod: corrupt tree posting list");
    list = &index->tree_posting_lists[posting_list];
    collect_tree_posting_list(index, list,
                              index->query[0].term, exclude_id, count);
    return;
  }

  child = tree_first_child(index, node);
  if (pending == 0 && query_position < query_end &&
      !VARIABLE(index->query[query_position].term)) {
    int32_t wanted = SYMNUM(index->query[query_position].term);
    uint32_t cached = tree_child_cache_parent_enabled(index, node) ?
      tree_child_cache_get(index, node, wanted) : CBD_NONE;
    unsigned scanned = 0;
    /* Sibling edges are ordered by their first token and radix insertion
       gives them distinct first tokens.  A rigid query token can therefore
       enter only the equal-code edge; the bounded positive cache avoids the
       chain on a hit, while a miss retains this complete fallback. */
    if (cached != CBD_NONE)
      collect_tree_candidates(index, cached, query_position, query_end,
                              pending, exclude_id, count);
    else
      while (child != CBD_NONE) {
        int32_t actual = tree_first_code(index, child);
        scanned++;
        index->tree_sibling_checks++;
        if (tree_code_compare(actual, wanted) < 0)
          child = index->tree_nodes[child].next_sibling;
        else {
          if (actual == wanted) {
            if (scanned >= CBD_TREE_CHILD_CACHE_MIN_SCAN &&
                tree_child_cache_enable_parent(index, node))
              maybe_grow_tree_child_cache(index);
            tree_child_cache_put(index, node, wanted, child, TRUE);
            collect_tree_candidates(index, child, query_position, query_end,
                                    pending, exclude_id, count);
          }
          break;
        }
      }
  }
  else
    for (; child != CBD_NONE;
         child = index->tree_nodes[child].next_sibling) {
      index->tree_sibling_checks++;
      collect_tree_candidates(index, child, query_position, query_end,
                              pending, exclude_id, count);
    }
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
  child = tree_child_cache_parent_enabled(index, CBD_NONE) ?
    tree_child_cache_get(index, CBD_NONE, symbol) : CBD_NONE;
  if (child != CBD_NONE) {
    collect_tree_candidates(index, child, 0, (uint32_t) query_count,
                            0, exclude_id, count);
    return;
  }
  {
    unsigned scanned = 0;
    for (child = tree_first_child(index, CBD_NONE);
       child != CBD_NONE; child = index->tree_nodes[child].next_sibling) {
      int32_t root = tree_first_code(index, child);
      scanned++;
      if (root < symbol)
        continue;
      if (root > symbol)
        break;
      if (scanned >= CBD_TREE_CHILD_CACHE_MIN_SCAN &&
          tree_child_cache_enable_parent(index, CBD_NONE))
        maybe_grow_tree_child_cache(index);
      tree_child_cache_put(index, CBD_NONE, symbol, child, TRUE);
      collect_tree_candidates(index, child, 0, (uint32_t) query_count,
                              0, exclude_id, count);
    }
  }
}

static uint32_t decode_position_value(
  const struct cbd_position_block *block, uint16_t *position)
{
  uint32_t value = 0;
  unsigned shift = 0;
  while (*position < block->used) {
    unsigned char byte = block->data[(*position)++];
    if (shift == 28 && (byte & 0xf0U) != 0)
      fatal_error("compact_back_demod: corrupt position delta");
    value |= (uint32_t) (byte & 0x7fU) << shift;
    if ((byte & 0x80U) == 0)
      return value;
    shift += 7;
    if (shift > 28)
      fatal_error("compact_back_demod: corrupt position value");
  }
  fatal_error("compact_back_demod: truncated position value");
  return 0;
}

struct cbd_position_iterator {
  Compact_back_demod_index index;
  struct cbd_position_bucket *bucket;
  uint32_t record;
  uint32_t occurrence;
  uint32_t block;
  uint16_t position;
  uint16_t entries;
  unsigned char first;
};

static void note_position_intersection_scan(Compact_back_demod_index index,
                                            uint32_t record_index)
{
  struct cbd_record *record;
  if (record_index == CBD_NONE || record_index >= index->record_count)
    fatal_error("compact_back_demod: corrupt position intersection record");
  record = &index->records[record_index];
  index->position_records_examined++;
  index->position_intersection_scans++;
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
}

static void init_position_iterator(Compact_back_demod_index index,
                                   uint32_t bucket_index,
                                   struct cbd_position_iterator *iterator)
{
  memset(iterator, 0, sizeof(*iterator));
  iterator->index = index;
  iterator->bucket = &index->position_buckets[bucket_index];
  iterator->record = iterator->bucket->inline_record;
  iterator->occurrence = iterator->bucket->inline_occurrence;
  iterator->block = iterator->bucket->posting_head;
  iterator->first = TRUE;
}

static BOOL next_position_record(struct cbd_position_iterator *iterator,
                                 uint32_t *record,
                                 uint32_t *occurrence,
                                 uint32_t *length)
{
  Compact_back_demod_index index = iterator->index;
  if (iterator->first) {
    iterator->first = FALSE;
    if (iterator->record == CBD_NONE)
      return FALSE;
    if (iterator->bucket->inline_length == 0)
      fatal_error("compact_back_demod: empty position intersection inline");
    index->posting_bytes_decoded += 3 * sizeof(uint32_t);
    index->query_bytes_decoded += 3 * sizeof(uint32_t);
    *record = iterator->record;
    *occurrence = iterator->occurrence;
    *length = iterator->bucket->inline_length;
    note_position_intersection_scan(index, *record);
    return TRUE;
  }
  while (iterator->block != CBD_NONE) {
    const struct cbd_position_block *current;
    uint32_t delta, occurrence_delta;
    if (iterator->block >= index->position_block_count)
      fatal_error("compact_back_demod: corrupt position intersection block");
    current = &index->position_blocks[iterator->block];
    if (iterator->position == 0) {
      index->posting_bytes_decoded += current->used;
      index->query_bytes_decoded += current->used;
    }
    if (iterator->position == current->used) {
      if (iterator->entries != current->count)
        fatal_error("compact_back_demod: corrupt position intersection count");
      iterator->block = current->next;
      iterator->position = 0;
      iterator->entries = 0;
      continue;
    }
    delta = decode_position_value(current, &iterator->position);
    occurrence_delta = decode_position_value(current, &iterator->position);
    *length = decode_position_value(current, &iterator->position);
    if (delta > UINT32_MAX - iterator->record ||
        occurrence_delta > UINT32_MAX - iterator->occurrence ||
        *length == 0)
      fatal_error("compact_back_demod: position intersection overflow");
    iterator->record += delta;
    iterator->occurrence += occurrence_delta;
    iterator->entries++;
    *record = iterator->record;
    *occurrence = iterator->occurrence;
    note_position_intersection_scan(index, *record);
    return TRUE;
  }
  return FALSE;
}

static void collect_position_intersection_record(
  Compact_back_demod_index index, uint32_t record_index,
  uint32_t occurrence_offset, uint32_t occurrence_length, Term pattern,
  int32_t symbol, unsigned long long exclude_id, size_t *count)
{
  struct cbd_record *record = &index->records[record_index];
  if (record->active && record->proof_id != exclude_id &&
      record->query_stamp != index->query_stamp &&
      posting_contains_pattern(index, occurrence_offset,
                               occurrence_length, record, pattern, symbol))
    collect_record(index, record_index, exclude_id, count);
}

static BOOL record_contains_pattern(Compact_back_demod_index index,
                                    struct cbd_record *record,
                                    Term pattern, int32_t symbol)
{
  const int32_t *tokens;
  uint32_t at, end = compact_term_slice_length(record->tokens);
  if (end == 0)
    return FALSE;
  tokens = compact_term_pool_slice_tokens(index->term_pool, record->tokens);
  for (at = 0; at < end; at++)
    if (tokens[at] == symbol) {
      index->occurrences_examined++;
      if (occurrence_matches(tokens, end, pattern, at))
        return TRUE;
    }
  return FALSE;
}

static size_t position_bitmap_word_limit(
  Compact_back_demod_index index, size_t feature_count,
  size_t *selected_count)
{
  size_t i, limit = SIZE_MAX;
  *selected_count = 0;
  for (i = 0; i < feature_count; i++) {
    uint32_t bucket = index->position_query[i].bucket;
    if (bucket != CBD_NONE && index->position_buckets[bucket].active) {
      size_t capacity =
        index->position_buckets[bucket].membership_capacity;
      if (capacity < limit)
        limit = capacity;
      (*selected_count)++;
    }
  }
  if (*selected_count != 0) {
    size_t logical_words = index->record_count / 64 +
      (index->record_count % 64 != 0);
    if (limit > logical_words)
      limit = logical_words;
  }
  return *selected_count == 0 ? 0 : limit;
}

static BOOL use_dense_position_intersection(
  Compact_back_demod_index index, uint32_t first_bucket,
  size_t feature_count)
{
  size_t selected, words = position_bitmap_word_limit(
    index, feature_count, &selected);
  unsigned long long work;
  if (selected < 2)
    return FALSE;
  work = words > ULLONG_MAX / selected ? ULLONG_MAX :
    (unsigned long long) words * selected;
  return work <= index->position_buckets[first_bucket].posting_count;
}

static void collect_position_bitmap_intersection(
  Compact_back_demod_index index, uint32_t first_bucket,
  size_t feature_count, Term pattern, unsigned long long exclude_id,
  size_t *count)
{
  size_t selected, words = position_bitmap_word_limit(
    index, feature_count, &selected);
  size_t word, i;
  int32_t symbol =
    (int32_t) index->position_buckets[first_bucket].root_symbol;
  index->position_queries++;
  index->position_intersection_queries++;
  index->position_dense_intersection_queries++;
  for (word = 0; word < words; word++) {
    uint64_t common = UINT64_MAX;
    for (i = 0; i < feature_count; i++) {
      uint32_t bucket_index = index->position_query[i].bucket;
      if (bucket_index != CBD_NONE &&
          index->position_buckets[bucket_index].active) {
        struct cbd_position_bucket *bucket =
          &index->position_buckets[bucket_index];
        common &= bucket->membership[word];
        index->position_bitmap_word_checks++;
        index->query_work++;
        index->query_bytes_decoded += sizeof(*bucket->membership);
        if (common == 0)
          break;
      }
    }
    while (common != 0) {
      unsigned bit = (unsigned) __builtin_ctzll(common);
      size_t candidate = word * 64 + bit;
      struct cbd_record *record;
      common &= common - 1;
      if (candidate == 0 || candidate >= index->record_count)
        continue;
      record = &index->records[candidate];
      index->position_records_examined++;
      index->position_intersection_records++;
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
          record_contains_pattern(index, record, pattern, symbol))
        collect_record(index, (uint32_t) candidate, exclude_id, count);
    }
  }
}

static void collect_position_intersection(
  Compact_back_demod_index index, uint32_t first_bucket,
  size_t feature_count, Term pattern, unsigned long long exclude_id,
  size_t *count)
{
  struct cbd_position_iterator first;
  uint32_t first_record, first_occurrence, first_length;
  BOOL have_first;
  size_t i;
  index->position_queries++;
  index->position_intersection_queries++;
  init_position_iterator(index, first_bucket, &first);
  have_first = next_position_record(
    &first, &first_record, &first_occurrence, &first_length);
  while (have_first) {
    BOOL member = TRUE;
    for (i = 0; i < feature_count && member; i++) {
      uint32_t bucket = index->position_query[i].bucket;
      if (bucket != CBD_NONE && bucket != first_bucket &&
          index->position_buckets[bucket].active) {
        index->position_intersection_bit_checks++;
        member = position_bitmap_contains(
          &index->position_buckets[bucket], first_record);
      }
    }
    if (member) {
      index->position_intersection_records++;
      collect_position_intersection_record(
        index, first_record, first_occurrence, first_length, pattern,
        (int32_t) index->position_buckets[first_bucket].root_symbol,
        exclude_id, count);
    }
    have_first = next_position_record(
      &first, &first_record, &first_occurrence, &first_length);
  }
}

static void collect_position_bucket(Compact_back_demod_index index,
                                    uint32_t bucket_index, Term pattern,
                                    unsigned long long exclude_id,
                                    size_t *count)
{
  struct cbd_position_bucket *bucket =
    &index->position_buckets[bucket_index];
  uint32_t record_index = bucket->inline_record;
  uint32_t occurrence_offset = bucket->inline_occurrence;
  uint32_t block;
  index->position_queries++;
  if (record_index == CBD_NONE)
    return;
  if (bucket->inline_length == 0)
    fatal_error("compact_back_demod: missing position occurrence list");
  index->position_records_examined++;
  index->posting_bytes_decoded += 3 * sizeof(uint32_t);
  index->query_bytes_decoded += 3 * sizeof(uint32_t);
  examine_posting_group(index, record_index, occurrence_offset,
                        bucket->inline_length, pattern,
                        (int32_t) bucket->root_symbol, exclude_id, count);
  for (block = bucket->posting_head; block != CBD_NONE;
       block = index->position_blocks[block].next) {
    const struct cbd_position_block *current;
    uint16_t position = 0;
    uint16_t entries = 0;
    if (block >= index->position_block_count)
      fatal_error("compact_back_demod: corrupt position block");
    current = &index->position_blocks[block];
    index->posting_bytes_decoded += current->used;
    index->query_bytes_decoded += current->used;
    while (position < current->used) {
      uint32_t delta = decode_position_value(current, &position);
      uint32_t occurrence_delta;
      uint32_t occurrence_length;
      if (delta > UINT32_MAX - record_index)
        fatal_error("compact_back_demod: position record overflow");
      record_index += delta;
      occurrence_delta = decode_position_value(current, &position);
      if (occurrence_delta > UINT32_MAX - occurrence_offset)
        fatal_error("compact_back_demod: position occurrence overflow");
      occurrence_offset += occurrence_delta;
      occurrence_length = decode_position_value(current, &position);
      index->position_records_examined++;
      examine_posting_group(index, record_index, occurrence_offset,
                            occurrence_length, pattern,
                            (int32_t) bucket->root_symbol,
                            exclude_id, count);
      entries++;
    }
    if (position != current->used || entries != current->count)
      fatal_error("compact_back_demod: corrupt position block contents");
  }
}

static BOOL record_has_position_bucket(
  Compact_back_demod_index index, struct cbd_record *record,
  struct cbd_position_bucket *bucket)
{
  struct cbd_position_query_feature feature;
  unsigned char matched = FALSE;
  memset(&feature, 0, sizeof(feature));
  feature.path = bucket->path;
  feature.symbol = bucket->symbol;
  record_position_features(index, record, bucket->root_symbol,
                           &feature, 1, &matched);
  return matched;
}

static void append_admitted_position_features(
  Compact_back_demod_index index, uint32_t record_index)
{
  struct cbd_record *record = &index->records[record_index];
  size_t i, added_blocks = 0;
  unsigned long long bitmap_growth = 0;
  if (!strategy_uses_position(index->strategy) ||
      !index->position_complete || index->position_bucket_count <= 1)
    return;
  for (i = 1; i < index->position_bucket_count; i++) {
    struct cbd_position_bucket *bucket = &index->position_buckets[i];
    if (bucket->active && record_has_position_bucket(index, record, bucket)) {
      unsigned long long growth = position_bitmap_growth(
        bucket, record_index);
      if (ULLONG_MAX - bitmap_growth < growth)
        fatal_error("compact_back_demod: projected bitmap byte overflow");
      bitmap_growth += growth;
      if (bucket->inline_record != CBD_NONE)
        added_blocks++;
    }
  }
  if (!index->position_rebuilding &&
      (added_blocks != 0 || bitmap_growth != 0) &&
      !position_budget_allows(index, projected_position_bytes(
        index, 0, added_blocks, bitmap_growth))) {
    /* Position postings are optional refinements over the complete path
       index.  Demote only features matched by this record; unrelated features
       remain complete and useful until compaction reclaims the dead arrays. */
    for (i = 1; i < index->position_bucket_count; i++) {
      struct cbd_position_bucket *bucket = &index->position_buckets[i];
      if (bucket->active && record_has_position_bucket(index, record, bucket)) {
        bucket->active = FALSE;
        index->position_demotions++;
        if (index->position_generation != ULLONG_MAX)
          index->position_generation++;
      }
    }
    index->position_budget_exhaustions++;
    return;
  }
  for (i = 1; i < index->position_bucket_count; i++) {
    if (index->position_buckets[i].active)
      (void) append_record_position_feature(
        index, (uint32_t) i, record_index);
  }
}

static void maybe_admit_position_feature(Compact_back_demod_index index,
                                         Term pattern,
                                         unsigned long long query_work)
{
  size_t feature_count, i, best = SIZE_MAX;
  unsigned char *matched;
  uint32_t root;
  unsigned long long matches, blocks, build_floor, bitmap_bytes;
  if (!strategy_uses_position(index->strategy) ||
      !index->position_complete || !index->position_admission_enabled ||
      VARIABLE(pattern) || query_work == 0)
    return;
  build_floor = index->active >
      ULLONG_MAX / index->position_build_factor ? ULLONG_MAX :
    index->active * index->position_build_factor;
  if (build_floor < index->position_admit_work)
    build_floor = index->position_admit_work;
  feature_count = collect_pattern_position_features(index, pattern);
  if (feature_count == 0)
    return;
  root = (uint32_t) SYMNUM(pattern);
  {
    size_t qualified = 0;
    for (i = 0; i < feature_count; i++) {
      unsigned hits;
      unsigned long long work = note_position_probation(
        index, root, index->position_query[i].path,
        index->position_query[i].symbol, query_work, &hits);
      if (hits >= 2 && work >= build_floor) {
        if (qualified != i)
          index->position_query[qualified] = index->position_query[i];
        qualified++;
      }
    }
    feature_count = qualified;
  }
  if (feature_count == 0) {
    index->position_cost_deferrals++;
    return;
  }
  for (i = 0; i < feature_count; i++) {
    index->position_query[i].bucket = lookup_position_bucket(
      index, root, index->position_query[i].path,
      index->position_query[i].symbol);
    index->position_query[i].matching_records = 0;
    index->position_query[i].joint_records = 0;
  }
  matched = safe_malloc(feature_count);
  clock_start(index->maintenance_clock);
  for (i = 1; i < index->record_count; i++)
    if (index->records[i].active) {
      size_t j;
      BOOL baseline_match = TRUE;
      for (j = 0; j < feature_count && baseline_match; j++) {
        uint32_t bucket = index->position_query[j].bucket;
        if (bucket != CBD_NONE && index->position_buckets[bucket].active &&
            !position_bitmap_contains(
              &index->position_buckets[bucket], (uint32_t) i))
          baseline_match = FALSE;
      }
      record_position_features(index, &index->records[i], root,
                               index->position_query, feature_count, matched);
      index->position_records_examined++;
      for (j = 0; j < feature_count; j++)
        if (matched[j]) {
          index->position_query[j].matching_records++;
          if (baseline_match)
            index->position_query[j].joint_records++;
        }
    }
  for (i = 0; i < feature_count; i++)
    if (index->position_query[i].bucket == CBD_NONE &&
        (best == SIZE_MAX ||
         index->position_query[i].joint_records <
           index->position_query[best].joint_records))
      best = i;
  if (best == SIZE_MAX) {
    for (i = 0; i < feature_count; i++)
      clear_position_probation(index, root,
                               index->position_query[i].path,
                               index->position_query[i].symbol);
    safe_free(matched);
    clock_stop(index->maintenance_clock);
    return;
  }
  matches = index->position_query[best].joint_records;
  if (matches > query_work / index->position_min_gain) {
    index->position_rejections++;
    for (i = 0; i < feature_count; i++)
      clear_position_probation(index, root,
                               index->position_query[i].path,
                               index->position_query[i].symbol);
    safe_free(matched);
    clock_stop(index->maintenance_clock);
    return;
  }
  blocks = index->position_query[best].matching_records <= 1 ? 0 :
    index->position_query[best].matching_records - 1;
  bitmap_bytes = (unsigned long long) projected_position_bitmap_capacity(
    0, (uint32_t) (index->record_count - 1)) * sizeof(uint64_t);
  if (blocks > SIZE_MAX ||
      !position_budget_allows(index, projected_position_bytes(
        index, 1, (size_t) blocks, bitmap_bytes))) {
    index->position_rejections++;
    index->position_budget_exhaustions++;
    for (i = 0; i < feature_count; i++)
      clear_position_probation(index, root,
                               index->position_query[i].path,
                               index->position_query[i].symbol);
    safe_free(matched);
    clock_stop(index->maintenance_clock);
    return;
  }
  {
    uint32_t bucket = add_position_bucket(
      index, root, index->position_query[best].path,
      index->position_query[best].symbol);
    struct cbd_position_query_feature feature = index->position_query[best];
    for (i = 1; i < index->record_count; i++)
      if (index->records[i].active) {
        unsigned char selected = FALSE;
        record_position_features(index, &index->records[i], root,
                                 &feature, 1, &selected);
        index->position_backfill_records++;
        if (selected)
          (void) append_record_position_feature(
            index, bucket, (uint32_t) i);
      }
  }
  index->position_admissions++;
  if (index->position_generation != ULLONG_MAX)
    index->position_generation++;
  for (i = 0; i < feature_count; i++)
    clear_position_probation(index, root,
                             index->position_query[i].path,
                             index->position_query[i].symbol);
  update_peak(index);
  safe_free(matched);
  clock_stop(index->maintenance_clock);
}

static uint32_t best_position_bucket(Compact_back_demod_index index,
                                     Term pattern, size_t *selected,
                                     size_t *feature_count)
{
  size_t count, i;
  uint32_t root, best = CBD_NONE;
  *selected = 0;
  *feature_count = 0;
  if (!strategy_uses_position(index->strategy) ||
      !index->position_complete || VARIABLE(pattern))
    return CBD_NONE;
  count = collect_pattern_position_features(index, pattern);
  *feature_count = count;
  root = (uint32_t) SYMNUM(pattern);
  for (i = 0; i < count; i++) {
    uint32_t bucket = lookup_position_bucket(
      index, root, index->position_query[i].path,
      index->position_query[i].symbol);
    index->position_query[i].bucket = bucket;
    if (bucket != CBD_NONE && index->position_buckets[bucket].active) {
      (*selected)++;
      if (best == CBD_NONE ||
          index->position_buckets[bucket].posting_count <
            index->position_buckets[best].posting_count)
        best = bucket;
    }
  }
  return best;
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

static uint64_t query_term_fingerprint(Compact_back_demod_index index,
                                       Term term)
{
  uint64_t value;
  int i;
  if (VARIABLE(term))
    return hash_id(UINT64_C(0x6a09e667f3bcc909) ^
                   (unsigned) VARNUM(term));
  value = hash_id(UINT64_C(0xbb67ae8584caa73b) ^
                  stable_symbol_hash(index, (uint32_t) SYMNUM(term)) ^
                  ((uint64_t) ARITY(term) << 32));
  for (i = 0; i < ARITY(term); i++)
    value = hash_id(value ^ query_term_fingerprint(index, ARG(term, i)) ^
                    (UINT64_C(0x9e3779b97f4a7c15) *
                     ((uint64_t) i + 1)));
  return value;
}

static uint64_t route_pattern_fingerprint_rec(
  Compact_back_demod_index index, Term term,
  uint32_t variable_names[MAX_VARS], uint32_t *next_variable)
{
  uint64_t value;
  int i;
  if (VARIABLE(term)) {
    int variable = VARNUM(term);
    uint32_t canonical;
    if (variable < 0 || variable >= MAX_VARS)
      return hash_id(UINT64_C(0x243f6a8885a308d3) ^
                     (uint32_t) variable);
    canonical = variable_names[variable];
    if (canonical == UINT32_MAX) {
      canonical = (*next_variable)++;
      variable_names[variable] = canonical;
    }
    return hash_id(UINT64_C(0x243f6a8885a308d3) ^ canonical);
  }
  value = hash_id(UINT64_C(0x13198a2e03707344) ^
                  stable_symbol_hash(index, (uint32_t) SYMNUM(term)) ^
                  ((uint64_t) ARITY(term) << 32));
  for (i = 0; i < ARITY(term); i++)
    value = hash_id(value ^ route_pattern_fingerprint_rec(
                      index, ARG(term, i), variable_names, next_variable) ^
                    (UINT64_C(0x9e3779b97f4a7c15) *
                     ((uint64_t) i + 1)));
  return value;
}

static uint64_t route_pattern_fingerprint(Compact_back_demod_index index,
                                          Term pattern)
{
  uint32_t variable_names[MAX_VARS];
  uint32_t next_variable = 0;
  uint64_t key;
  size_t i;
  for (i = 0; i < MAX_VARS; i++)
    variable_names[i] = UINT32_MAX;
  key = route_pattern_fingerprint_rec(
    index, pattern, variable_names, &next_variable);
  return key == 0 ? 1 : key;
}

static struct cbd_route_profile *route_profile_for_pattern(
  Compact_back_demod_index index, Term pattern)
{
  uint64_t key;
  size_t sets, first;
  struct cbd_route_profile *a, *b, *entry;
  if (index->route_profile_capacity < 2)
    return NULL;
  key = route_pattern_fingerprint(index, pattern);
  sets = index->route_profile_capacity / 2;
  first = ((size_t) key & (sets - 1)) * 2;
  a = &index->route_profiles[first];
  b = a + 1;
  if (a->occupied && a->key == key)
    return a;
  if (b->occupied && b->key == key)
    return b;
  if (!a->occupied)
    entry = a;
  else if (!b->occupied)
    entry = b;
  else {
    index->route_profile_collisions++;
    if (a->queries < b->queries)
      entry = a;
    else if (b->queries < a->queries)
      entry = b;
    else
      entry = a->key > b->key ? a : b;
    index->route_profile_replacements++;
  }
  if (!entry->occupied)
    index->route_profile_occupied++;
  memset(entry, 0, sizeof(*entry));
  entry->occupied = TRUE;
  entry->key = key;
  entry->preferred = CBD_ROUTE_MASK;
  entry->position_generation = index->position_generation;
  return entry;
}

static unsigned long long saturating_add(unsigned long long a,
                                         unsigned long long b)
{
  return ULLONG_MAX - a < b ? ULLONG_MAX : a + b;
}

static unsigned long long route_scaled_cost(unsigned long long cost,
                                            unsigned long long old_population,
                                            unsigned long long population)
{
  if (old_population == 0 || population == old_population)
    return cost;
  if (population == 0)
    return 0;
  if (cost > ULLONG_MAX / population)
    return ULLONG_MAX;
  return cost * population / old_population;
}

static unsigned long long route_ewma(unsigned long long old,
                                     unsigned long long observed)
{
  unsigned long long difference, step;
  if (old == observed)
    return old;
  difference = old > observed ? old - observed : observed - old;
  step = difference / 8 + (difference % 8 != 0);
  return observed > old ? saturating_add(old, step) : old - step;
}

static BOOL route_materially_better(unsigned long long alternative,
                                    unsigned long long current)
{
  unsigned long long threshold = current / 5 * 4 +
    (current % 5) * 4 / 5;
  return alternative < current && alternative <= threshold;
}

static unsigned long long route_root_population(
  Compact_back_demod_index index, Term pattern)
{
  unsigned symbol;
  if (VARIABLE(pattern))
    return index->record_count - 1;
  symbol = (unsigned) SYMNUM(pattern);
  return (size_t) symbol < index->tree_root_capacity ?
    index->tree_roots[symbol].physical_groups : 0;
}

static unsigned long long route_position_population(
  Compact_back_demod_index index, uint32_t first_bucket,
  size_t selected_positions, size_t feature_count)
{
  unsigned long long postings;
  if (first_bucket == CBD_NONE)
    return 0;
  postings = index->position_buckets[first_bucket].posting_count;
  if (selected_positions > 1 && use_dense_position_intersection(
        index, first_bucket, feature_count)) {
    size_t selected, words = position_bitmap_word_limit(
      index, feature_count, &selected);
    return words > ULLONG_MAX / selected ? ULLONG_MAX :
      (unsigned long long) words * selected;
  }
  return selected_positions > 1 &&
      postings > ULLONG_MAX / selected_positions ? ULLONG_MAX :
    postings * selected_positions;
}

struct cbd_route_work_snapshot {
  unsigned long long query_work;
  unsigned long long tree_nodes;
  unsigned long long tree_siblings;
  unsigned long long tree_child_lookups;
  unsigned long long occurrences;
  unsigned long long position_bit_checks;
  unsigned long long position_records;
  unsigned long long bytes;
};

static struct cbd_route_work_snapshot route_work_snapshot(
  Compact_back_demod_index index)
{
  struct cbd_route_work_snapshot value;
  value.query_work = index->query_work;
  value.tree_nodes = index->tree_nodes_examined;
  value.tree_siblings = index->tree_sibling_checks;
  value.tree_child_lookups = index->tree_child_cache_lookups;
  value.occurrences = index->occurrences_examined;
  value.position_bit_checks = index->position_intersection_bit_checks +
    index->position_bitmap_word_checks;
  value.position_records = index->position_records_examined;
  value.bytes = index->query_bytes_decoded;
  return value;
}

static unsigned long long route_observed_work(
  const struct cbd_route_work_snapshot *before,
  const struct cbd_route_work_snapshot *after)
{
  unsigned long long work = after->query_work - before->query_work;
  work = saturating_add(work, after->tree_nodes - before->tree_nodes);
  work = saturating_add(work, after->tree_siblings - before->tree_siblings);
  work = saturating_add(
    work, after->tree_child_lookups - before->tree_child_lookups);
  work = saturating_add(work, after->occurrences - before->occurrences);
  work = saturating_add(
    work, after->position_bit_checks - before->position_bit_checks);
  work = saturating_add(
    work, after->position_records - before->position_records);
  work = saturating_add(work, (after->bytes - before->bytes + 15) / 16);
  return work;
}

static unsigned long long route_estimate(
  const struct cbd_route_profile *profile, enum cbd_route route,
  unsigned long long population)
{
  return profile->samples[route] == 0 ? ULLONG_MAX :
    route_scaled_cost(profile->cost[route], profile->population[route],
                      population);
}

static enum cbd_route route_raw_best(
  const struct cbd_route_profile *profile,
  const unsigned char available[CBD_ROUTE_COUNT],
  const unsigned long long population[CBD_ROUTE_COUNT])
{
  enum cbd_route route, best = CBD_ROUTE_MASK;
  unsigned long long best_cost = ULLONG_MAX;
  for (route = CBD_ROUTE_MASK; route < CBD_ROUTE_COUNT; route++)
    if (available[route]) {
      unsigned long long cost = route_estimate(
        profile, route, population[route]);
      if (cost < best_cost ||
          (cost == best_cost && route == profile->preferred)) {
        best = route;
        best_cost = cost;
      }
    }
  return best;
}

static void route_update_preference(
  Compact_back_demod_index index, struct cbd_route_profile *profile,
  const unsigned char available[CBD_ROUTE_COUNT],
  const unsigned long long population[CBD_ROUTE_COUNT])
{
  enum cbd_route current = (enum cbd_route) profile->preferred;
  enum cbd_route best = route_raw_best(profile, available, population);
  unsigned long long current_cost, best_cost;
  if (!available[current]) {
    profile->preferred = (unsigned char) best;
    index->route_switches++;
    if (best == CBD_ROUTE_MASK)
      index->route_reversions++;
    return;
  }
  if (best == current)
    return;
  current_cost = route_estimate(profile, current, population[current]);
  best_cost = route_estimate(profile, best, population[best]);
  if (route_materially_better(best_cost, current_cost)) {
    profile->preferred = (unsigned char) best;
    index->route_switches++;
    if (best == CBD_ROUTE_MASK)
      index->route_reversions++;
  }
  else
    index->route_hysteresis_holds++;
}

static enum cbd_route choose_adaptive_route(
  Compact_back_demod_index index, struct cbd_route_profile *profile,
  const unsigned char available[CBD_ROUTE_COUNT],
  const unsigned long long population[CBD_ROUTE_COUNT], BOOL *probe)
{
  enum cbd_route route, chosen;
  *probe = FALSE;
  if (profile->queries != ULLONG_MAX)
    profile->queries++;
  route_update_preference(index, profile, available, population);
  chosen = (enum cbd_route) profile->preferred;

  /* A new shape establishes its complete mask baseline first.  Then train a
     newly available position (which can cure variable-prefix fanout) before
     the tree. */
  if (available[CBD_ROUTE_MASK] &&
      profile->samples[CBD_ROUTE_MASK] == 0)
    chosen = CBD_ROUTE_MASK;
  else if (available[CBD_ROUTE_POSITION] &&
           profile->samples[CBD_ROUTE_POSITION] == 0) {
    chosen = CBD_ROUTE_POSITION;
    *probe = chosen != (enum cbd_route) profile->preferred;
  }
  else if (available[CBD_ROUTE_TREE] &&
           profile->samples[CBD_ROUTE_TREE] == 0) {
    chosen = CBD_ROUTE_TREE;
    *probe = chosen != (enum cbd_route) profile->preferred;
  }

  /* Recalibrate on powers-of-two physical growth or shrinkage, not elapsed
     queries.  A stable mature index therefore pays no exploration tax, while
     a day-long growing index still samples every materially new scale. */
  if (profile->samples[chosen] != 0)
    for (route = CBD_ROUTE_MASK; route < CBD_ROUTE_COUNT; route++)
      if (available[route] && route != profile->preferred &&
          profile->samples[route] != 0 &&
          ((profile->population[route] != 0 &&
            population[route] >= profile->population[route] &&
            population[route] - profile->population[route] >=
              profile->population[route]) ||
           (population[route] < profile->population[route] &&
            profile->population[route] - population[route] >=
              population[route]))) {
        chosen = route;
        *probe = TRUE;
        break;
      }

  if (*probe) {
    index->route_probes[chosen]++;
  }
  index->route_choices[chosen]++;
  for (route = CBD_ROUTE_MASK; route < CBD_ROUTE_COUNT; route++)
    if (available[route] && profile->samples[route] != 0)
      index->route_estimated_cost[route] = saturating_add(
        index->route_estimated_cost[route],
        route_estimate(profile, route, population[route]));
  return chosen;
}

static void note_adaptive_route(
  Compact_back_demod_index index, struct cbd_route_profile *profile,
  enum cbd_route route, unsigned long long population,
  unsigned long long observed, size_t candidates,
  const unsigned char available[CBD_ROUTE_COUNT],
  const unsigned long long populations[CBD_ROUTE_COUNT])
{
  if (profile->samples[route] == 0)
    profile->cost[route] = observed;
  else
    profile->cost[route] = route_ewma(profile->cost[route], observed);
  profile->population[route] = population;
  profile->last_sample[route] = profile->queries;
  if (profile->samples[route] != UINT32_MAX)
    profile->samples[route]++;
  index->route_observed_cost[route] = saturating_add(
    index->route_observed_cost[route], observed);
  index->route_candidates[route] = saturating_add(
    index->route_candidates[route], candidates);
  route_update_preference(index, profile, available, populations);
}

static BOOL query_event_trace_enabled(void)
{
  static int enabled = -1;
  if (enabled < 0) {
    const char *value = getenv("P9_COMPACT_BACK_TRACE");
    enabled = value != NULL && value[0] != '\0' && strcmp(value, "0") != 0;
  }
  return enabled != 0;
}

static uint64_t note_query_input_fingerprint(Compact_back_demod_index index,
                                             Topform demod, int type,
                                             Term alpha, Term beta)
{
  uint64_t value = hash_id(demod->id ^ ((uint64_t) (unsigned) type << 56));
  if (type == ORIENTED || type == LEX_DEP_LR || type == LEX_DEP_BOTH)
    value = hash_id(value ^ query_term_fingerprint(index, alpha));
  if (type == LEX_DEP_RL || type == LEX_DEP_BOTH)
    value = hash_id(value ^ query_term_fingerprint(index, beta) ^
                    UINT64_C(0x3c6ef372fe94f82b));
  index->query_input_fingerprint = hash_id(
    index->query_input_fingerprint ^ value ^
    (index->queries + UINT64_C(0xa54ff53a5f1d36f1)));
  return value;
}

static uint64_t note_query_output_fingerprint(Compact_back_demod_index index,
                                              size_t count)
{
  size_t i;
  uint64_t value = hash_id(index->query_work ^
    ((uint64_t) count << 32) ^ index->query_bytes_decoded);
  for (i = 0; i < count; i++)
    value = hash_id(value ^ index->results[i] ^
                    (UINT64_C(0x510e527fade682d1) *
                     ((uint64_t) i + 1)));
  index->query_output_fingerprint = hash_id(
    index->query_output_fingerprint ^ value ^
    (index->queries + UINT64_C(0x1f83d9abfb41bd6b)));
  return value;
}

static size_t pattern_min_tokens(Term term)
{
  size_t count = 1;
  int i;
  if (VARIABLE(term))
    return count;
  for (i = 0; i < ARITY(term); i++) {
    size_t child = pattern_min_tokens(ARG(term, i));
    if (child > SIZE_MAX - count)
      fatal_error("compact_back_demod: pattern size overflow");
    count += child;
  }
  return count;
}

static BOOL use_tree_for_pattern(Compact_back_demod_index index,
                                 Term pattern)
{
  return index->strategy == COMPACT_BACK_DEMOD_CODE_TREE ||
    (index->strategy == COMPACT_BACK_DEMOD_HYBRID_TREE &&
     index->tree_complete &&
     pattern_min_tokens(pattern) >= index->tree_min_tokens) ||
    (strategy_uses_hot_tree(index->strategy) &&
     !VARIABLE(pattern) &&
     (size_t) SYMNUM(pattern) < index->tree_root_capacity &&
     index->tree_roots[SYMNUM(pattern)].admitted);
}

static void collect_pattern(Compact_back_demod_index index, Term pattern,
                            unsigned long long exclude_id, size_t *count)
{
  unsigned long long before = index->query_work;
  size_t selected_positions;
  size_t position_feature_count;
  uint32_t position_bucket = best_position_bucket(
    index, pattern, &selected_positions, &position_feature_count);
  if (index->strategy == COMPACT_BACK_DEMOD_ADAPTIVE &&
      !VARIABLE(pattern)) {
    struct cbd_route_profile *profile = route_profile_for_pattern(
      index, pattern);
    struct cbd_route_work_snapshot work_before, work_after;
    unsigned char available[CBD_ROUTE_COUNT] = {TRUE, FALSE, FALSE};
    unsigned long long population[CBD_ROUTE_COUNT];
    enum cbd_route route;
    BOOL probe;
    size_t count_before = *count;
    if (profile == NULL)
      fatal_error("compact_back_demod: missing adaptive route table");
    if (profile->position_generation != index->position_generation) {
      profile->cost[CBD_ROUTE_POSITION] = 0;
      profile->population[CBD_ROUTE_POSITION] = 0;
      profile->last_sample[CBD_ROUTE_POSITION] = 0;
      profile->samples[CBD_ROUTE_POSITION] = 0;
      profile->position_generation = index->position_generation;
    }
    available[CBD_ROUTE_TREE] = use_tree_for_pattern(index, pattern);
    available[CBD_ROUTE_POSITION] = position_bucket != CBD_NONE;
    population[CBD_ROUTE_MASK] = route_root_population(index, pattern);
    population[CBD_ROUTE_TREE] = population[CBD_ROUTE_MASK];
    population[CBD_ROUTE_POSITION] = route_position_population(
      index, position_bucket, selected_positions, position_feature_count);
    route = choose_adaptive_route(
      index, profile, available, population, &probe);
    work_before = route_work_snapshot(index);
    if (route == CBD_ROUTE_POSITION) {
      if (selected_positions > 1 && use_dense_position_intersection(
            index, position_bucket, position_feature_count))
        collect_position_bitmap_intersection(
          index, position_bucket, position_feature_count, pattern,
          exclude_id, count);
      else if (selected_positions > 1)
        collect_position_intersection(
          index, position_bucket, position_feature_count, pattern,
          exclude_id, count);
      else
        collect_position_bucket(index, position_bucket, pattern,
                                exclude_id, count);
    }
    else if (route == CBD_ROUTE_TREE)
      collect_tree(index, pattern, exclude_id, count);
    else
      collect_symbol(index, pattern, exclude_id, count);
    work_after = route_work_snapshot(index);
    {
      unsigned long long observed = route_observed_work(
        &work_before, &work_after);
      note_adaptive_route(
        index, profile, route, population[route], observed,
        *count - count_before, available, population);
      if (route == CBD_ROUTE_MASK)
        maybe_admit_hot_root(
          index, pattern, index->query_work - before);
      /* Any expensive complete route can fund a missing selective position.
         Position itself can likewise reveal that another intersection key is
         needed as the population evolves. */
      maybe_admit_position_feature(index, pattern, observed);
    }
    return;
  }
  if (position_bucket != CBD_NONE) {
    unsigned long long observed;
    if (selected_positions > 1 && use_dense_position_intersection(
          index, position_bucket, position_feature_count))
      collect_position_bitmap_intersection(
        index, position_bucket, position_feature_count, pattern,
        exclude_id, count);
    else if (selected_positions > 1)
      collect_position_intersection(
        index, position_bucket, position_feature_count, pattern,
        exclude_id, count);
    else
      collect_position_bucket(index, position_bucket, pattern,
                              exclude_id, count);
    observed = index->query_work - before;
    if (observed >= index->position_admit_work)
      maybe_admit_position_feature(index, pattern, observed);
  }
  else if (use_tree_for_pattern(index, pattern)) {
    unsigned long long nodes_before = index->tree_nodes_examined;
    unsigned long long observed;
    collect_tree(index, pattern, exclude_id, count);
    observed = index->tree_nodes_examined - nodes_before;
    if (ULLONG_MAX - observed < index->query_work - before)
      observed = ULLONG_MAX;
    else
      observed += index->query_work - before;
    /* In adaptive mode, a tree which fans out before a selective rigid
       position supplies the evidence for a complementary position posting. */
    maybe_admit_position_feature(index, pattern, observed);
  }
  else {
    collect_symbol(index, pattern, exclude_id, count);
    maybe_admit_hot_root(index, pattern, index->query_work - before);
    maybe_admit_position_feature(index, pattern,
                                 index->query_work - before);
  }
}

unsigned long long *compact_back_demod_candidate_ids(
  Compact_back_demod_index index, Topform demod, int type, size_t *count)
{
  Term atom, alpha, beta;
  uint64_t input_fingerprint, output_fingerprint;
  unsigned long long *answer;
  unsigned long long occurrences_before;
  *count = 0;
  if (index == NULL || demod == NULL || demod->literals == NULL)
    return NULL;
  compact_query_timer_start(&index->lookup_timer);
  index->query_work = 0;
  index->query_live = 0;
  index->query_dead = 0;
  index->query_duplicates = 0;
  index->query_bytes_decoded = 0;
  occurrences_before = index->occurrences_examined;
  atom = demod->literals->atom;
  alpha = ARG(atom, 0);
  beta = ARG(atom, 1);
  input_fingerprint = note_query_input_fingerprint(
    index, demod, type, alpha, beta);
  begin_query(index);
  if (type == ORIENTED || type == LEX_DEP_LR || type == LEX_DEP_BOTH)
    collect_pattern(index, alpha, demod->id, count);
  if (type == LEX_DEP_RL || type == LEX_DEP_BOTH)
    collect_pattern(index, beta, demod->id, count);
  if (*count > 1)
    qsort(index->results, *count, sizeof(*index->results), decreasing_id);
  output_fingerprint = note_query_output_fingerprint(index, *count);
  if (query_event_trace_enabled())
    fprintf(stderr,
            "CBD_QUERY sequence=%llu demod=%llu type=%d input=%016llx "
            "active=%llu physical=%llu work=%llu candidates=%llu "
            "output=%016llx.\n",
            index->queries + 1, demod->id, type,
            (unsigned long long) input_fingerprint, index->active,
            (unsigned long long) (index->record_count - 1),
            index->query_work, (unsigned long long) *count,
            (unsigned long long) output_fingerprint);
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
  compact_query_timer_stop(&index->lookup_timer);
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

static Compact_term_slice tree_list_term(
  Compact_back_demod_index index, const struct cbd_tree_posting_list *list)
{
  struct cbd_record *record;
  const int32_t *tokens;
  uint32_t length, end;
  Compact_term_slice term;
  if (list->inline_record == CBD_NONE ||
      list->inline_record >= index->record_count)
    fatal_error("compact_back_demod: corrupt tree representative record");
  record = &index->records[list->inline_record];
  tokens = compact_term_pool_slice_tokens(index->term_pool, record->tokens);
  length = compact_term_slice_length(record->tokens);
  if (list->inline_relative >= length)
    fatal_error("compact_back_demod: corrupt tree representative offset");
  end = token_term_end(tokens, list->inline_relative, length);
  if (!compact_term_slice_subslice(
        record->tokens, list->inline_relative,
        end - list->inline_relative, &term))
    fatal_error("compact_back_demod: invalid tree representative slice");
  return term;
}

static void copy_live_tree_record(Compact_back_demod_index source,
                                  Compact_back_demod_index replacement,
                                  const uint32_t *record_map,
                                  uint32_t record_index,
                                  Compact_term_slice term)
{
  uint32_t mapped;
  if (record_index == CBD_NONE || record_index >= source->record_count ||
      compact_term_slice_length(term) == 0)
    fatal_error("compact_back_demod: corrupt compacted tree posting");
  (void) compact_term_pool_slice_tokens(source->term_pool, term);
  mapped = record_map[record_index];
  if (mapped == CBD_NONE)
    return;
  append_tree_record(replacement, mapped, term);
}

static void copy_hot_root_states(Compact_back_demod_index destination,
                                 Compact_back_demod_index source)
{
  if (!strategy_uses_hot_tree(source->strategy) ||
      source->tree_root_capacity == 0)
    return;
  if (source->tree_root_capacity - 1 > UINT_MAX)
    fatal_error("compact_back_demod: root-state symbol overflow");
  ensure_symbols(destination,
                 (unsigned) source->tree_root_capacity - 1);
  memcpy(destination->tree_roots, source->tree_roots,
         source->tree_root_capacity * sizeof(*source->tree_roots));
  {
    size_t i;
    /* The replacement path index repopulates this physical-work metric. */
    for (i = 0; i < destination->tree_root_capacity; i++)
      destination->tree_roots[i].physical_groups = 0;
  }
}

static void copy_route_profiles(Compact_back_demod_index destination,
                                Compact_back_demod_index source)
{
  if (source->route_profile_capacity == 0)
    return;
  if (destination->route_profile_capacity !=
      source->route_profile_capacity)
    fatal_error("compact_back_demod: route-profile capacity mismatch");
  memcpy(destination->route_profiles, source->route_profiles,
         source->route_profile_capacity * sizeof(*source->route_profiles));
  destination->route_profile_occupied = source->route_profile_occupied;
  destination->route_profile_collisions = source->route_profile_collisions;
  destination->route_profile_replacements =
    source->route_profile_replacements;
  memcpy(destination->route_choices, source->route_choices,
         sizeof(source->route_choices));
  memcpy(destination->route_probes, source->route_probes,
         sizeof(source->route_probes));
  destination->route_switches = source->route_switches;
  destination->route_reversions = source->route_reversions;
  destination->route_hysteresis_holds = source->route_hysteresis_holds;
  memcpy(destination->route_observed_cost, source->route_observed_cost,
         sizeof(source->route_observed_cost));
  memcpy(destination->route_estimated_cost, source->route_estimated_cost,
         sizeof(source->route_estimated_cost));
  memcpy(destination->route_candidates, source->route_candidates,
         sizeof(source->route_candidates));
  destination->position_generation = source->position_generation;
}

static void copy_position_definitions(Compact_back_demod_index destination,
                                      Compact_back_demod_index source)
{
  size_t i;
  if (!strategy_uses_position(source->strategy))
    return;
  destination->position_complete = source->position_complete;
  destination->position_budget_high_water =
    source->position_budget_high_water;
  if (!source->position_complete)
    return;
  for (i = 1; i < source->position_bucket_count; i++) {
    struct cbd_position_bucket *bucket = &source->position_buckets[i];
    if (bucket->active)
      (void) add_position_bucket(destination, bucket->root_symbol,
                                 bucket->path, bucket->symbol);
  }
  if (source->position_probation_capacity != 0) {
    destination->position_probation = safe_malloc(
      source->position_probation_capacity *
        sizeof(*source->position_probation));
    memcpy(destination->position_probation, source->position_probation,
           source->position_probation_capacity *
             sizeof(*source->position_probation));
    destination->position_probation_capacity =
      source->position_probation_capacity;
  }
}

static void finish_position_rebuild(Compact_back_demod_index index)
{
  if (!strategy_uses_position(index->strategy))
    return;
  index->position_rebuilding = FALSE;
  if (index->position_complete &&
      !position_budget_allows(index, position_estimated_bytes(index))) {
    index->position_complete = FALSE;
    index->position_budget_exhaustions++;
  }
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
  replacement->tree_complete = index->tree_complete;
  copy_hot_root_states(replacement, index);
  copy_position_definitions(replacement, index);
  copy_route_profiles(replacement, index);
  replacement->position_rebuilding = TRUE;
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
      append_admitted_position_features(replacement, added);
    }
  finish_position_rebuild(replacement);
  if (strategy_uses_tree(index->strategy) && index->tree_complete) {
    for (i = 1; i < index->tree_posting_list_count; i++) {
      struct cbd_tree_posting_list *list =
        &index->tree_posting_lists[i];
      Compact_term_slice term;
      uint32_t block;
      uint32_t record_index = list->inline_record;
      term = tree_list_term(index, list);
      if (strategy_uses_hot_tree(index->strategy)) {
        unsigned symbol = (unsigned) compact_term_pool_slice_tokens(
          index->term_pool, term)[0];
        if ((size_t) symbol >= index->tree_root_capacity ||
            !index->tree_roots[symbol].admitted)
          continue;
      }
      if (record_index == CBD_NONE || compact_term_slice_length(term) == 0)
        fatal_error("compact_back_demod: corrupt compacted tree inline posting");
      copy_live_tree_record(index, replacement, record_map, record_index,
                            term);
      if ((list->posting_head & CBD_TREE_DIRECT_RECORD) != 0) {
        copy_live_tree_record(
          index, replacement, record_map,
          list->posting_head & CBD_TREE_INDEX_MASK, term);
        continue;
      }
      for (block = list->posting_head; block != CBD_NONE;
           block = index->posting_blocks[block].next) {
        const struct cbd_posting_block *current =
          &index->posting_blocks[block];
        uint16_t position = 0;
        uint16_t entries = 0;
        while (position < current->used) {
          record_index = decode_posting_value(current, &position);
          copy_live_tree_record(index, replacement, record_map, record_index,
                                term);
          entries++;
        }
        if (position != current->used || entries != current->count)
          fatal_error("compact_back_demod: corrupt compacted tree block");
      }
    }
    if (!strategy_uses_paths(index->strategy))
      replacement->symbol_occurrences = index->symbol_occurrences;
  }
  if (strategy_uses_paths(index->strategy))
    for (i = 1; i < index->path_bucket_count; i++) {
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
  free_clock(replacement->maintenance_clock);
  replacement->lookup_timer = old.lookup_timer;
  replacement->maintenance_clock = old.maintenance_clock;
  *index = *replacement;
  safe_free(replacement);
  safe_free(old.posting_blocks);
  safe_free(old.symbol_buckets);
  safe_free(old.symbol_hashes);
  safe_free(old.path_buckets);
  safe_free(old.path_bucket_hash);
  safe_free(old.tree_nodes);
  safe_free(old.tree_posting_lists);
  safe_free(old.tree_child_cache);
  safe_free(old.tree_child_cache_parents);
  safe_free(old.tree_roots);
  free_position_buckets(old.position_buckets, old.position_bucket_count);
  safe_free(old.position_bucket_hash);
  safe_free(old.position_root_buckets);
  safe_free(old.position_blocks);
  safe_free(old.position_query);
  safe_free(old.position_probation);
  safe_free(old.route_profiles);
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
  index->tree_sibling_checks = old.tree_sibling_checks;
  index->tree_child_cache_lookups = old.tree_child_cache_lookups;
  index->tree_child_cache_hits = old.tree_child_cache_hits;
  index->tree_child_cache_misses = old.tree_child_cache_misses;
  index->tree_child_cache_replacements =
    old.tree_child_cache_replacements;
  index->tree_child_cache_growth_denials =
    old.tree_child_cache_growth_denials;
  index->tree_budget_exhaustions += old.tree_budget_exhaustions;
  index->tree_root_admissions = old.tree_root_admissions;
  index->tree_root_rejections = old.tree_root_rejections;
  index->tree_root_cost_deferrals = old.tree_root_cost_deferrals;
  index->tree_root_censuses = old.tree_root_censuses;
  index->tree_root_census_occurrences =
    old.tree_root_census_occurrences;
  index->tree_root_demotions = old.tree_root_demotions;
  index->tree_root_backfill_groups = old.tree_root_backfill_groups;
  index->tree_root_backfill_occurrences =
    old.tree_root_backfill_occurrences;
  index->tree_fallback_work = old.tree_fallback_work;
  index->position_queries = old.position_queries;
  index->position_intersection_queries =
    old.position_intersection_queries;
  index->position_dense_intersection_queries =
    old.position_dense_intersection_queries;
  index->position_intersection_scans = old.position_intersection_scans;
  index->position_intersection_bit_checks =
    old.position_intersection_bit_checks;
  index->position_bitmap_word_checks = old.position_bitmap_word_checks;
  index->position_intersection_records = old.position_intersection_records;
  index->position_records_examined = old.position_records_examined;
  index->position_admissions = old.position_admissions;
  index->position_rejections = old.position_rejections;
  index->position_demotions = old.position_demotions;
  index->position_cost_deferrals = old.position_cost_deferrals;
  index->position_probation_updates = old.position_probation_updates;
  index->position_probation_replacements =
    old.position_probation_replacements;
  index->position_backfill_records = old.position_backfill_records;
  index->position_budget_exhaustions += old.position_budget_exhaustions;
  index->inactive_groups_examined = old.inactive_groups_examined;
  index->duplicate_groups_examined = old.duplicate_groups_examined;
  index->posting_bytes_decoded = old.posting_bytes_decoded;
  index->worst_query_id = old.worst_query_id;
  index->worst_query_groups = old.worst_query_groups;
  index->worst_query_occurrences = old.worst_query_occurrences;
  index->worst_query_candidates = old.worst_query_candidates;
  index->query_input_fingerprint = old.query_input_fingerprint;
  index->query_output_fingerprint = old.query_output_fingerprint;
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
  safe_free(old.symbol_hashes);
  safe_free(old.path_buckets);
  safe_free(old.path_bucket_hash);
  safe_free(old.tree_nodes);
  safe_free(old.tree_posting_lists);
  safe_free(old.tree_child_cache);
  safe_free(old.tree_child_cache_parents);
  safe_free(old.occurrences);
  safe_free(old.records);
  compact_id_map_free(old.id_map);
  safe_free(old.results);
  safe_free(old.query);
  replacement = compact_back_demod_init_with_pool_strategy(
    old.term_pool, old.strategy);
  copy_hot_root_states(replacement, &old);
  copy_position_definitions(replacement, &old);
  copy_route_profiles(replacement, &old);
  replacement->position_rebuilding = TRUE;
  safe_free(old.tree_roots);
  free_position_buckets(old.position_buckets, old.position_bucket_count);
  safe_free(old.position_bucket_hash);
  safe_free(old.position_root_buckets);
  safe_free(old.position_blocks);
  safe_free(old.position_query);
  safe_free(old.position_probation);
  safe_free(old.route_profiles);
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
  finish_position_rebuild(replacement);
  if (id_file != NULL)
    fclose(id_file);
  safe_free(ids);
  free_clock(replacement->maintenance_clock);
  replacement->lookup_timer = old.lookup_timer;
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
  index->tree_sibling_checks = old.tree_sibling_checks;
  index->tree_child_cache_lookups = old.tree_child_cache_lookups;
  index->tree_child_cache_hits = old.tree_child_cache_hits;
  index->tree_child_cache_misses = old.tree_child_cache_misses;
  index->tree_child_cache_replacements =
    old.tree_child_cache_replacements;
  index->tree_child_cache_growth_denials =
    old.tree_child_cache_growth_denials;
  index->tree_budget_exhaustions += old.tree_budget_exhaustions;
  index->tree_root_admissions = old.tree_root_admissions;
  index->tree_root_rejections = old.tree_root_rejections;
  index->tree_root_cost_deferrals = old.tree_root_cost_deferrals;
  index->tree_root_censuses = old.tree_root_censuses;
  index->tree_root_census_occurrences =
    old.tree_root_census_occurrences;
  index->tree_root_demotions = old.tree_root_demotions;
  index->tree_root_backfill_groups = old.tree_root_backfill_groups;
  index->tree_root_backfill_occurrences =
    old.tree_root_backfill_occurrences;
  index->tree_fallback_work = old.tree_fallback_work;
  index->position_queries = old.position_queries;
  index->position_intersection_queries =
    old.position_intersection_queries;
  index->position_dense_intersection_queries =
    old.position_dense_intersection_queries;
  index->position_intersection_scans = old.position_intersection_scans;
  index->position_intersection_bit_checks =
    old.position_intersection_bit_checks;
  index->position_bitmap_word_checks = old.position_bitmap_word_checks;
  index->position_intersection_records = old.position_intersection_records;
  index->position_records_examined = old.position_records_examined;
  index->position_admissions = old.position_admissions;
  index->position_rejections = old.position_rejections;
  index->position_demotions = old.position_demotions;
  index->position_cost_deferrals = old.position_cost_deferrals;
  index->position_probation_updates = old.position_probation_updates;
  index->position_probation_replacements =
    old.position_probation_replacements;
  index->position_backfill_records = old.position_backfill_records;
  index->position_budget_exhaustions += old.position_budget_exhaustions;
  index->inactive_groups_examined = old.inactive_groups_examined;
  index->duplicate_groups_examined = old.duplicate_groups_examined;
  index->posting_bytes_decoded = old.posting_bytes_decoded;
  index->worst_query_id = old.worst_query_id;
  index->worst_query_groups = old.worst_query_groups;
  index->worst_query_occurrences = old.worst_query_occurrences;
  index->worst_query_candidates = old.worst_query_candidates;
  index->query_input_fingerprint = old.query_input_fingerprint;
  index->query_output_fingerprint = old.query_output_fingerprint;
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
    if (compact_term_slice_length(index->records[i].tokens) != 0)
      index->records[i].tokens = compact_term_rebase_slice(
        map, index->records[i].tokens);
  for (i = 1; i < index->tree_node_count; i++)
    if (compact_term_slice_length(index->tree_nodes[i].tokens) != 0)
      index->tree_nodes[i].tokens = compact_term_rebase_slice(
        map, index->tree_nodes[i].tokens);
  index->term_pool = pool;
  update_peak(index);
}

void compact_back_demod_get_stats(Compact_back_demod_index index,
                                  struct compact_back_demod_stats *stats)
{
  size_t i;
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
  stats->path_buckets = index->path_bucket_count == 0 ? 0 :
    index->path_bucket_count - 1;
  stats->tree_nodes = index->tree_node_count == 0 ? 0 :
    index->tree_node_count - 1;
  stats->tree_terminals = index->tree_posting_list_count == 0 ? 0 :
    index->tree_posting_list_count - 1;
  stats->tree_queries = index->tree_queries;
  stats->tree_nodes_examined = index->tree_nodes_examined;
  stats->tree_sibling_checks = index->tree_sibling_checks;
  stats->tree_child_cache_lookups = index->tree_child_cache_lookups;
  stats->tree_child_cache_hits = index->tree_child_cache_hits;
  stats->tree_child_cache_misses = index->tree_child_cache_misses;
  stats->tree_child_cache_replacements =
    index->tree_child_cache_replacements;
  stats->tree_child_cache_growth_denials =
    index->tree_child_cache_growth_denials;
  stats->tree_child_cache_parents = index->tree_child_cache_parent_count;
  stats->tree_child_cache_bytes = index->tree_child_cache_capacity *
      sizeof(*index->tree_child_cache) +
    index->tree_child_cache_parent_words *
      sizeof(*index->tree_child_cache_parents);
  stats->tree_posting_groups = index->tree_posting_count;
  stats->tree_budget_bytes = index->tree_budget_bytes;
  stats->tree_estimated_bytes = tree_estimated_bytes(index);
  stats->tree_budget_exhaustions = index->tree_budget_exhaustions;
  stats->tree_root_admissions = index->tree_root_admissions;
  stats->tree_root_rejections = index->tree_root_rejections;
  stats->tree_root_cost_deferrals = index->tree_root_cost_deferrals;
  stats->tree_root_censuses = index->tree_root_censuses;
  stats->tree_root_census_occurrences =
    index->tree_root_census_occurrences;
  stats->tree_root_demotions = index->tree_root_demotions;
  stats->tree_root_backfill_groups = index->tree_root_backfill_groups;
  stats->tree_root_backfill_occurrences =
    index->tree_root_backfill_occurrences;
  stats->tree_fallback_work = index->tree_fallback_work;
  stats->tree_min_tokens = index->tree_min_tokens;
  stats->tree_admit_work = index->tree_admit_work;
  stats->tree_build_factor = index->tree_build_factor;
  stats->tree_complete = index->tree_complete;
  stats->position_physical_features = index->position_bucket_count == 0 ? 0 :
    index->position_bucket_count - 1;
  stats->position_features = 0;
  for (i = 1; i < index->position_bucket_count; i++)
    if (index->position_buckets[i].active)
      stats->position_features++;
  stats->position_postings = index->position_posting_count;
  stats->position_queries = index->position_queries;
  stats->position_intersection_queries =
    index->position_intersection_queries;
  stats->position_dense_intersection_queries =
    index->position_dense_intersection_queries;
  stats->position_intersection_scans = index->position_intersection_scans;
  stats->position_intersection_bit_checks =
    index->position_intersection_bit_checks;
  stats->position_bitmap_word_checks = index->position_bitmap_word_checks;
  stats->position_intersection_records =
    index->position_intersection_records;
  stats->position_records_examined = index->position_records_examined;
  stats->position_admissions = index->position_admissions;
  stats->position_rejections = index->position_rejections;
  stats->position_demotions = index->position_demotions;
  stats->position_cost_deferrals = index->position_cost_deferrals;
  stats->position_probation_updates = index->position_probation_updates;
  stats->position_probation_replacements =
    index->position_probation_replacements;
  stats->position_backfill_records = index->position_backfill_records;
  stats->position_budget_bytes = index->position_budget_bytes;
  stats->position_effective_budget_bytes =
    index->position_budget_high_water;
  stats->position_estimated_bytes = position_estimated_bytes(index);
  stats->position_probation_bytes = index->position_probation_capacity *
    sizeof(*index->position_probation);
  stats->position_bitmap_bytes = index->position_bitmap_bytes;
  stats->position_budget_exhaustions = index->position_budget_exhaustions;
  stats->position_budget_pct = index->position_budget_pct;
  stats->position_admit_work = index->position_admit_work;
  stats->position_min_gain = index->position_min_gain;
  stats->position_build_factor = index->position_build_factor;
  stats->position_admission_enabled = index->position_admission_enabled;
  stats->position_complete = index->position_complete;
  stats->route_profile_capacity = index->route_profile_capacity;
  stats->route_profile_occupied = index->route_profile_occupied;
  stats->route_profile_bytes = index->route_profile_capacity *
    sizeof(*index->route_profiles);
  stats->route_profile_collisions = index->route_profile_collisions;
  stats->route_profile_replacements = index->route_profile_replacements;
  stats->route_mask_choices = index->route_choices[CBD_ROUTE_MASK];
  stats->route_tree_choices = index->route_choices[CBD_ROUTE_TREE];
  stats->route_position_choices = index->route_choices[CBD_ROUTE_POSITION];
  stats->route_mask_probes = index->route_probes[CBD_ROUTE_MASK];
  stats->route_tree_probes = index->route_probes[CBD_ROUTE_TREE];
  stats->route_position_probes = index->route_probes[CBD_ROUTE_POSITION];
  stats->route_switches = index->route_switches;
  stats->route_reversions = index->route_reversions;
  stats->route_hysteresis_holds = index->route_hysteresis_holds;
  stats->route_mask_observed_cost =
    index->route_observed_cost[CBD_ROUTE_MASK];
  stats->route_tree_observed_cost =
    index->route_observed_cost[CBD_ROUTE_TREE];
  stats->route_position_observed_cost =
    index->route_observed_cost[CBD_ROUTE_POSITION];
  stats->route_mask_estimated_cost =
    index->route_estimated_cost[CBD_ROUTE_MASK];
  stats->route_tree_estimated_cost =
    index->route_estimated_cost[CBD_ROUTE_TREE];
  stats->route_position_estimated_cost =
    index->route_estimated_cost[CBD_ROUTE_POSITION];
  stats->route_mask_candidates = index->route_candidates[CBD_ROUTE_MASK];
  stats->route_tree_candidates = index->route_candidates[CBD_ROUTE_TREE];
  stats->route_position_candidates =
    index->route_candidates[CBD_ROUTE_POSITION];
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
  stats->query_input_fingerprint = index->query_input_fingerprint;
  stats->query_output_fingerprint = index->query_output_fingerprint;
  stats->query_profile = index->query_profile;
  stats->lookup_seconds = index->lookup_timer.estimated_seconds;
  stats->lookup_timing_eligible = index->lookup_timer.eligible;
  stats->lookup_timing_samples = index->lookup_timer.samples;
  stats->timing_sample_rate = COMPACT_TIMING_SAMPLE_RATE;
  stats->maintenance_seconds = clock_seconds(index->maintenance_clock);
  stats->materialized_file_snapshots = index->materialized_file_snapshots;
  stats->materialized_snapshot_ids = index->materialized_snapshot_ids;
  stats->posting_bytes =
    index->posting_block_capacity * sizeof(*index->posting_blocks) +
    index->occurrence_capacity * sizeof(*index->occurrences) +
    index->position_block_capacity * sizeof(*index->position_blocks);
  stats->posting_stream_used = index->posting_stream_used;
  stats->posting_stream_bytes =
    index->posting_block_capacity * sizeof(*index->posting_blocks);
  stats->occurrence_bytes =
    index->occurrence_capacity * sizeof(*index->occurrences);
  stats->occurrence_stream_bytes = index->occurrence_count;
  stats->record_bytes = index->record_capacity * sizeof(*index->records);
  stats->root_bytes =
    index->symbol_capacity * sizeof(*index->symbol_buckets) +
    index->symbol_capacity * sizeof(*index->symbol_hashes) +
    index->path_bucket_capacity * sizeof(*index->path_buckets) +
    index->path_bucket_hash_capacity * sizeof(*index->path_bucket_hash) +
    index->tree_node_capacity * sizeof(*index->tree_nodes) +
    index->tree_posting_list_capacity *
      sizeof(*index->tree_posting_lists) +
    index->tree_child_cache_capacity *
      sizeof(*index->tree_child_cache) +
    index->tree_child_cache_parent_words *
      sizeof(*index->tree_child_cache_parents) +
    index->tree_root_capacity * sizeof(*index->tree_roots) +
    index->position_bucket_capacity * sizeof(*index->position_buckets) +
    index->position_bucket_hash_capacity *
      sizeof(*index->position_bucket_hash) +
    index->position_root_capacity * sizeof(*index->position_root_buckets);
  stats->root_bytes += index->position_probation_capacity *
    sizeof(*index->position_probation);
  stats->root_bytes += index->route_profile_capacity *
    sizeof(*index->route_profiles);
  stats->token_bytes = index->owns_term_pool ? terms.token_bytes : 0;
  stats->hash_bytes = compact_id_map_bytes(index->id_map);
  stats->scratch_bytes = index->result_capacity * sizeof(*index->results) +
    index->query_capacity * sizeof(*index->query) +
    index->position_query_capacity * sizeof(*index->position_query);
  stats->total_bytes = index_bytes(index);
  stats->peak_bytes = index->peak_bytes;
}

void compact_back_demod_free(Compact_back_demod_index index)
{
  if (index == NULL)
    return;
  safe_free(index->posting_blocks);
  safe_free(index->symbol_buckets);
  safe_free(index->symbol_hashes);
  safe_free(index->path_buckets);
  safe_free(index->path_bucket_hash);
  safe_free(index->tree_nodes);
  safe_free(index->tree_posting_lists);
  safe_free(index->tree_child_cache);
  safe_free(index->tree_child_cache_parents);
  safe_free(index->tree_roots);
  free_position_buckets(index->position_buckets,
                        index->position_bucket_count);
  safe_free(index->position_bucket_hash);
  safe_free(index->position_root_buckets);
  safe_free(index->position_blocks);
  safe_free(index->position_query);
  safe_free(index->position_probation);
  safe_free(index->route_profiles);
  safe_free(index->occurrences);
  safe_free(index->records);
  if (index->owns_term_pool)
    compact_term_pool_free(index->term_pool);
  compact_id_map_free(index->id_map);
  safe_free(index->results);
  safe_free(index->query);
  free_clock(index->maintenance_clock);
  safe_free(index);
}
