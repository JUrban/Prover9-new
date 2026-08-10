#include "compact_term_pool.h"

#include <stdint.h>
#include <string.h>

struct compact_term_pool {
  int32_t *tokens;
  size_t token_count;
  size_t token_capacity;
  unsigned long long *proof_ids;
  uint32_t *clause_offsets;
  uint32_t *clause_lengths;
  size_t directory_capacity;
  size_t directory_count;
  unsigned long long serializations;
  unsigned long long lookups;
  unsigned long long hits;
  unsigned long long reused_tokens;
  unsigned long long token_growths;
  unsigned long long token_copy_bytes;
  unsigned long long peak_bytes;
  BOOL sharing_profile_enabled;
  uint64_t *profile_hashes;
  uint32_t *profile_offsets;
  uint32_t *profile_lengths;
  size_t profile_capacity;
  size_t profile_count;
  unsigned long long profile_term_occurrences;
  unsigned long long profile_child_references;
  unsigned long long profile_atom_roots;
};

static size_t grow_token_capacity(size_t current)
{
  size_t increment;
  size_t next;
  if (current == 0)
    return 64;
  increment = current / 12;
  if (increment < 64)
    increment = 64;
  if (increment > SIZE_MAX - current)
    fatal_error("compact_term_pool: token capacity overflow");
  next = current + increment;
  if (next > SIZE_MAX / sizeof(int32_t))
    fatal_error("compact_term_pool: token capacity overflow");
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

static void profile_rehash(Compact_term_pool pool, size_t capacity)
{
  uint64_t *old_hashes = pool->profile_hashes;
  uint32_t *old_offsets = pool->profile_offsets;
  uint32_t *old_lengths = pool->profile_lengths;
  size_t old_capacity = pool->profile_capacity;
  size_t i;
  pool->profile_hashes = safe_calloc(capacity,
                                     sizeof(*pool->profile_hashes));
  pool->profile_offsets = safe_calloc(capacity,
                                      sizeof(*pool->profile_offsets));
  pool->profile_lengths = safe_calloc(capacity,
                                      sizeof(*pool->profile_lengths));
  pool->profile_capacity = capacity;
  for (i = 0; i < old_capacity; i++)
    if (old_hashes[i] != 0) {
      size_t at = (size_t) old_hashes[i] & (capacity - 1);
      while (pool->profile_hashes[at] != 0)
        at = (at + 1) & (capacity - 1);
      pool->profile_hashes[at] = old_hashes[i];
      pool->profile_offsets[at] = old_offsets[i];
      pool->profile_lengths[at] = old_lengths[i];
    }
  safe_free(old_hashes);
  safe_free(old_offsets);
  safe_free(old_lengths);
}

static void ensure_profile_directory(Compact_term_pool pool)
{
  if (pool->profile_capacity == 0)
    profile_rehash(pool, 128);
  else if ((pool->profile_count + 1) * 20 >=
           pool->profile_capacity * 17) {
    if (pool->profile_capacity > SIZE_MAX / 2)
      fatal_error("compact_term_pool: sharing profile overflow");
    profile_rehash(pool, pool->profile_capacity * 2);
  }
}

static uint64_t profile_term(Compact_term_pool pool, Term term,
                             uint32_t *position)
{
  uint32_t start = *position;
  uint64_t hash;
  uint32_t length;
  size_t at;
  int i;
  int32_t code;
  if (*position >= pool->token_count)
    fatal_error("compact_term_pool: sharing profile token overflow");
  code = pool->tokens[(*position)++];
  hash = hash_id((uint64_t) (uint32_t) code +
                 UINT64_C(0x9e3779b97f4a7c15));
  pool->profile_term_occurrences++;
  for (i = 0; i < ARITY(term); i++) {
    uint64_t child = profile_term(pool, ARG(term, i), position);
    hash = hash_id(hash ^ child ^
                   (UINT64_C(0x517cc1b727220a95) + (uint64_t) i));
  }
  length = *position - start;
  if (hash == 0)
    hash = 1;
  ensure_profile_directory(pool);
  at = (size_t) hash & (pool->profile_capacity - 1);
  while (pool->profile_hashes[at] != 0) {
    if (pool->profile_hashes[at] == hash &&
        pool->profile_lengths[at] == length &&
        memcmp(pool->tokens + pool->profile_offsets[at],
               pool->tokens + start,
               (size_t) length * sizeof(*pool->tokens)) == 0)
      return hash;
    at = (at + 1) & (pool->profile_capacity - 1);
  }
  pool->profile_hashes[at] = hash;
  pool->profile_offsets[at] = start;
  pool->profile_lengths[at] = length;
  pool->profile_count++;
  pool->profile_child_references += (unsigned) ARITY(term);
  return hash;
}

static unsigned long long pool_bytes(Compact_term_pool pool)
{
  if (pool == NULL)
    return 0;
  return sizeof(*pool) +
    pool->token_capacity * sizeof(*pool->tokens) +
    pool->directory_capacity *
      (sizeof(*pool->proof_ids) + sizeof(*pool->clause_offsets) +
       sizeof(*pool->clause_lengths)) +
    pool->profile_capacity *
      (sizeof(*pool->profile_hashes) + sizeof(*pool->profile_offsets) +
       sizeof(*pool->profile_lengths));
}

static void update_peak(Compact_term_pool pool)
{
  unsigned long long bytes = pool_bytes(pool);
  if (bytes > pool->peak_bytes)
    pool->peak_bytes = bytes;
}

static size_t directory_slot(Compact_term_pool pool,
                             unsigned long long proof_id)
{
  size_t mask = pool->directory_capacity - 1;
  size_t at = (size_t) hash_id(proof_id) & mask;
  while (pool->proof_ids[at] != 0 && pool->proof_ids[at] != proof_id)
    at = (at + 1) & mask;
  return at;
}

static void rehash(Compact_term_pool pool, size_t capacity)
{
  unsigned long long *old_ids = pool->proof_ids;
  uint32_t *old_offsets = pool->clause_offsets;
  uint32_t *old_lengths = pool->clause_lengths;
  size_t old_capacity = pool->directory_capacity;
  size_t i;
  pool->proof_ids = safe_calloc(capacity, sizeof(*pool->proof_ids));
  pool->clause_offsets = safe_calloc(capacity,
                                     sizeof(*pool->clause_offsets));
  pool->clause_lengths = safe_calloc(capacity,
                                     sizeof(*pool->clause_lengths));
  pool->directory_capacity = capacity;
  for (i = 0; i < old_capacity; i++)
    if (old_ids[i] != 0) {
      size_t at = directory_slot(pool, old_ids[i]);
      pool->proof_ids[at] = old_ids[i];
      pool->clause_offsets[at] = old_offsets[i];
      pool->clause_lengths[at] = old_lengths[i];
    }
  safe_free(old_ids);
  safe_free(old_offsets);
  safe_free(old_lengths);
}

static void ensure_directory(Compact_term_pool pool)
{
  if (pool->directory_capacity == 0)
    rehash(pool, 128);
  else if ((pool->directory_count + 1) * 20 >=
           pool->directory_capacity * 17) {
    if (pool->directory_capacity > SIZE_MAX / 2)
      fatal_error("compact_term_pool: directory overflow");
    rehash(pool, pool->directory_capacity * 2);
  }
}

static void ensure_tokens(Compact_term_pool pool, size_t extra)
{
  size_t needed;
  if (extra > SIZE_MAX - pool->token_count)
    fatal_error("compact_term_pool: token overflow");
  needed = pool->token_count + extra;
  if (needed > UINT32_MAX)
    fatal_error("compact_term_pool: token offsets exceed 32 bits");
  while (needed > pool->token_capacity) {
    size_t old_capacity = pool->token_capacity;
    pool->token_capacity = grow_token_capacity(pool->token_capacity);
    pool->tokens = safe_realloc(
      pool->tokens, pool->token_capacity * sizeof(*pool->tokens));
    pool->token_growths++;
    pool->token_copy_bytes +=
      old_capacity * sizeof(*pool->tokens);
  }
}

static void append_term(Compact_term_pool pool, Term term)
{
  size_t capacity = 128, top = 0;
  Term fixed[128];
  Term *stack = fixed;
  stack[top++] = term;
  while (top != 0) {
    Term current = stack[--top];
    int i;
    ensure_tokens(pool, 1);
    pool->tokens[pool->token_count++] = VARIABLE(current) ?
      -(int32_t) VARNUM(current) - 1 : (int32_t) SYMNUM(current);
    for (i = ARITY(current) - 1; i >= 0; i--) {
      if (top == capacity) {
        capacity *= 2;
        if (stack == fixed) {
          stack = safe_malloc(capacity * sizeof(*stack));
          memcpy(stack, fixed, top * sizeof(*stack));
        }
        else
          stack = safe_realloc(stack, capacity * sizeof(*stack));
      }
      stack[top++] = ARG(current, i);
    }
  }
  if (stack != fixed)
    safe_free(stack);
}

static BOOL term_equals_tokens(Compact_term_pool pool, Term term,
                               uint32_t *position, uint32_t end)
{
  int32_t code;
  int i;
  if (*position >= end)
    return FALSE;
  code = pool->tokens[(*position)++];
  if (VARIABLE(term))
    return code == -(int32_t) VARNUM(term) - 1;
  if (code < 0 || code != SYMNUM(term))
    return FALSE;
  for (i = 0; i < ARITY(term); i++)
    if (!term_equals_tokens(pool, ARG(term, i), position, end))
      return FALSE;
  return TRUE;
}

static uint32_t find_term(Compact_term_pool pool, uint32_t offset,
                          uint32_t length, Term target,
                          uint32_t *target_length)
{
  uint32_t at;
  uint32_t end = offset + length;
  int32_t root = VARIABLE(target) ?
    -(int32_t) VARNUM(target) - 1 : (int32_t) SYMNUM(target);
  for (at = offset; at < end; at++)
    if (pool->tokens[at] == root) {
      uint32_t after = at;
      if (term_equals_tokens(pool, target, &after, end)) {
        *target_length = after - at;
        return at;
      }
    }
  return UINT32_MAX;
}

static void serialize_clause(Compact_term_pool pool,
                             unsigned long long proof_id,
                             Literals literals)
{
  uint32_t offset = (uint32_t) pool->token_count;
  Literals literal;
  size_t at;
  for (literal = literals; literal != NULL; literal = literal->next) {
    uint32_t atom_offset = (uint32_t) pool->token_count;
    append_term(pool, literal->atom);
    if (pool->sharing_profile_enabled) {
      uint32_t position = atom_offset;
      (void) profile_term(pool, literal->atom, &position);
      if (position != pool->token_count)
        fatal_error("compact_term_pool: sharing profile atom mismatch");
      pool->profile_atom_roots++;
    }
  }
  ensure_directory(pool);
  at = directory_slot(pool, proof_id);
  if (pool->proof_ids[at] == 0) {
    pool->proof_ids[at] = proof_id;
    pool->directory_count++;
  }
  pool->clause_offsets[at] = offset;
  pool->clause_lengths[at] = (uint32_t) pool->token_count - offset;
  pool->serializations++;
  update_peak(pool);
}

Compact_term_pool compact_term_pool_init(void)
{
  Compact_term_pool pool = safe_calloc(1, sizeof(*pool));
  update_peak(pool);
  return pool;
}

void compact_term_pool_enable_sharing_profile(Compact_term_pool pool)
{
  if (pool == NULL)
    fatal_error("compact_term_pool_enable_sharing_profile: null pool");
  if (pool->token_count != 0 || pool->directory_count != 0)
    fatal_error("compact_term_pool: sharing profile enabled after use");
  pool->sharing_profile_enabled = TRUE;
  ensure_profile_directory(pool);
  update_peak(pool);
}

uint32_t compact_term_pool_intern(Compact_term_pool pool,
                                  unsigned long long proof_id,
                                  Literals literals, Term target,
                                  uint32_t *length)
{
  size_t at;
  uint32_t offset;
  if (pool == NULL || proof_id == 0 || literals == NULL || target == NULL ||
      length == NULL)
    fatal_error("compact_term_pool_intern: invalid request");
  pool->lookups++;
  if (pool->directory_capacity != 0) {
    at = directory_slot(pool, proof_id);
    if (pool->proof_ids[at] == proof_id) {
      offset = find_term(pool, pool->clause_offsets[at],
                         pool->clause_lengths[at], target, length);
      if (offset != UINT32_MAX) {
        pool->hits++;
        pool->reused_tokens += *length;
        return offset;
      }
    }
  }
  serialize_clause(pool, proof_id, literals);
  at = directory_slot(pool, proof_id);
  offset = find_term(pool, pool->clause_offsets[at],
                     pool->clause_lengths[at], target, length);
  if (offset == UINT32_MAX)
    fatal_error("compact_term_pool_intern: target is not a clause subterm");
  return offset;
}

uint32_t compact_term_pool_append(Compact_term_pool pool,
                                  const int32_t *tokens, uint32_t length)
{
  uint32_t offset;
  if (pool == NULL || (tokens == NULL && length != 0))
    fatal_error("compact_term_pool_append: invalid request");
  offset = (uint32_t) pool->token_count;
  ensure_tokens(pool, length);
  if (length != 0)
    memcpy(pool->tokens + pool->token_count, tokens,
           (size_t) length * sizeof(*pool->tokens));
  pool->token_count += length;
  update_peak(pool);
  return offset;
}

const int32_t *compact_term_pool_tokens(Compact_term_pool pool)
{
  return pool == NULL ? NULL : pool->tokens;
}

size_t compact_term_pool_token_count(Compact_term_pool pool)
{
  return pool == NULL ? 0 : pool->token_count;
}

void compact_term_pool_get_stats(Compact_term_pool pool,
                                 struct compact_term_pool_stats *stats)
{
  if (stats == NULL)
    return;
  memset(stats, 0, sizeof(*stats));
  if (pool == NULL)
    return;
  stats->clause_entries = pool->directory_count;
  stats->serializations = pool->serializations;
  stats->lookups = pool->lookups;
  stats->hits = pool->hits;
  stats->reused_tokens = pool->reused_tokens;
  stats->logical_tokens = pool->token_count;
  stats->token_bytes = pool->token_capacity * sizeof(*pool->tokens);
  stats->token_growths = pool->token_growths;
  stats->token_copy_bytes = pool->token_copy_bytes;
  stats->directory_bytes = pool->directory_capacity *
    (sizeof(*pool->proof_ids) + sizeof(*pool->clause_offsets) +
     sizeof(*pool->clause_lengths));
  stats->total_bytes = pool_bytes(pool);
  stats->peak_bytes = pool->peak_bytes;
  stats->sharing_profile_enabled = pool->sharing_profile_enabled;
  stats->profile_term_occurrences = pool->profile_term_occurrences;
  stats->profile_unique_terms = pool->profile_count;
  stats->profile_child_references = pool->profile_child_references;
  stats->profile_atom_roots = pool->profile_atom_roots;
  stats->profile_dag_payload_bytes =
    (pool->profile_count + pool->profile_child_references +
     pool->profile_atom_roots) * sizeof(uint32_t);
  stats->profile_table_bytes = pool->profile_capacity *
    (sizeof(*pool->profile_hashes) + sizeof(*pool->profile_offsets) +
     sizeof(*pool->profile_lengths));
}

void compact_term_pool_free(Compact_term_pool pool)
{
  if (pool == NULL)
    return;
  safe_free(pool->tokens);
  safe_free(pool->proof_ids);
  safe_free(pool->clause_offsets);
  safe_free(pool->clause_lengths);
  safe_free(pool->profile_hashes);
  safe_free(pool->profile_offsets);
  safe_free(pool->profile_lengths);
  safe_free(pool);
}
