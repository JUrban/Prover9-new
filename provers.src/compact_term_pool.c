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

static uint64_t hash_id(uint64_t x)
{
  x ^= x >> 30;
  x *= UINT64_C(0xbf58476d1ce4e5b9);
  x ^= x >> 27;
  x *= UINT64_C(0x94d049bb133111eb);
  x ^= x >> 31;
  return x;
}

static unsigned long long pool_bytes(Compact_term_pool pool)
{
  if (pool == NULL)
    return 0;
  return sizeof(*pool) +
    pool->token_capacity * sizeof(*pool->tokens) +
    pool->directory_capacity *
      (sizeof(*pool->proof_ids) + sizeof(*pool->clause_offsets) +
       sizeof(*pool->clause_lengths));
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
    pool->token_capacity = grow_capacity(
      pool->token_capacity, sizeof(*pool->tokens),
      "compact_term_pool: token capacity overflow");
    pool->tokens = safe_realloc(
      pool->tokens, pool->token_capacity * sizeof(*pool->tokens));
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
  for (literal = literals; literal != NULL; literal = literal->next)
    append_term(pool, literal->atom);
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
  stats->directory_bytes = pool->directory_capacity *
    (sizeof(*pool->proof_ids) + sizeof(*pool->clause_offsets) +
     sizeof(*pool->clause_lengths));
  stats->total_bytes = pool_bytes(pool);
  stats->peak_bytes = pool->peak_bytes;
}

void compact_term_pool_free(Compact_term_pool pool)
{
  if (pool == NULL)
    return;
  safe_free(pool->tokens);
  safe_free(pool->proof_ids);
  safe_free(pool->clause_offsets);
  safe_free(pool->clause_lengths);
  safe_free(pool);
}
