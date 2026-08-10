#if defined(__linux__) && !defined(__EMSCRIPTEN__)
#define _GNU_SOURCE
#endif
#include "compact_term_pool.h"

#include <stdint.h>
#include <string.h>
#if defined(__linux__) && !defined(__EMSCRIPTEN__)
#include <sys/mman.h>
#include <unistd.h>
#endif

struct compact_term_pool {
  int32_t *tokens;
  size_t token_count;
  size_t token_capacity;
  size_t token_mapping_bytes;
  BOOL tokens_mapped;
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
  unsigned long long compactions;
  unsigned long long bytes_reclaimed;
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

struct compact_term_rebase_entry {
  union {
    unsigned long long proof_id;
    unsigned long long new_offset;
  } destination;
  uint32_t old_offset;
  uint32_t length;
};

struct compact_term_rebase_map {
  struct compact_term_rebase_entry *entries;
  size_t count;
  size_t capacity;
  Compact_term_pool retained_source;
  uint64_t *retained_slots;
  size_t retained_slot_words;
  unsigned mode;
  BOOL finalized;
};

enum compact_term_rebase_mode {
  REBASE_MODE_UNSET = 0,
  REBASE_MODE_COPY = 1,
  REBASE_MODE_RETAINED = 2
};

static size_t grow_token_capacity(size_t current)
{
  size_t increment;
  size_t next;
  if (current == 0)
    return 64;
  increment = current / 8;
  if (increment < 64)
    increment = 64;
  if (increment > SIZE_MAX - current)
    fatal_error("compact_term_pool: token capacity overflow");
  next = current + increment;
  if (next > SIZE_MAX / sizeof(int32_t))
    fatal_error("compact_term_pool: token capacity overflow");
  return next;
}

/* Linux can resize an anonymous token mapping by moving page tables instead
   of allocating and copying both the old and new multi-megabyte arrays.  The
   fallback retains the established realloc behavior on other platforms. */
static BOOL resize_tokens(Compact_term_pool pool, size_t capacity)
{
  size_t bytes = capacity * sizeof(*pool->tokens);
#if defined(__linux__) && !defined(__EMSCRIPTEN__)
  long page_size = sysconf(_SC_PAGESIZE);
  size_t mapped_bytes;
  void *p;
  if (page_size <= 0)
    fatal_error("compact_term_pool: cannot determine page size");
  if (bytes > SIZE_MAX - (size_t) page_size + 1)
    fatal_error("compact_term_pool: token mapping overflow");
  mapped_bytes = bytes == 0 ? 0 :
    ((bytes + (size_t) page_size - 1) / (size_t) page_size) *
      (size_t) page_size;
  if (mapped_bytes == pool->token_mapping_bytes) {
    pool->token_capacity = capacity;
    return FALSE;
  }
  if (!pool->tokens_mapped && pool->tokens != NULL)
    fatal_error("compact_term_pool: mixed token allocation modes");
  if (mapped_bytes == 0) {
    if (pool->tokens != NULL &&
        munmap(pool->tokens, pool->token_mapping_bytes) != 0)
      fatal_error("compact_term_pool: cannot release token mapping");
    pool->tokens = NULL;
    pool->token_mapping_bytes = 0;
    pool->token_capacity = 0;
    pool->tokens_mapped = FALSE;
    return FALSE;
  }
  if (pool->tokens == NULL)
    p = mmap(NULL, mapped_bytes, PROT_READ | PROT_WRITE,
             MAP_PRIVATE | MAP_ANONYMOUS, -1, 0);
  else
    p = mremap(pool->tokens, pool->token_mapping_bytes, mapped_bytes,
               MREMAP_MAYMOVE);
  if (p == MAP_FAILED)
    fatal_error("compact_term_pool: cannot resize token mapping");
  pool->tokens = p;
  pool->token_mapping_bytes = mapped_bytes;
  pool->token_capacity = capacity;
  pool->tokens_mapped = TRUE;
  return FALSE;
#else
  size_t old_capacity = pool->token_capacity;
  pool->tokens = safe_realloc(pool->tokens, bytes);
  pool->token_capacity = capacity;
  return old_capacity != 0;
#endif
}

static void release_tokens(Compact_term_pool pool)
{
#if defined(__linux__) && !defined(__EMSCRIPTEN__)
  if (pool->tokens_mapped) {
    if (pool->tokens != NULL &&
        munmap(pool->tokens, pool->token_mapping_bytes) != 0)
      fatal_error("compact_term_pool: cannot release token mapping");
  }
  else
    safe_free(pool->tokens);
#else
  safe_free(pool->tokens);
#endif
  pool->tokens = NULL;
  pool->token_capacity = 0;
  pool->token_mapping_bytes = 0;
  pool->tokens_mapped = FALSE;
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
    size_t next_capacity = grow_token_capacity(pool->token_capacity);
    BOOL copied = resize_tokens(pool, next_capacity);
    pool->token_growths++;
    if (copied)
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

Compact_term_rebase_map compact_term_rebase_map_init(void)
{
  return safe_calloc(1, sizeof(struct compact_term_rebase_map));
}

static struct compact_term_rebase_entry *append_rebase_entry(
  Compact_term_rebase_map map)
{
  if (map->count == map->capacity) {
    size_t next = map->capacity == 0 ? 64 : map->capacity * 2;
    if (next < map->capacity ||
        next > SIZE_MAX / sizeof(*map->entries))
      fatal_error("compact_term_pool: rebase map overflow");
    map->entries = safe_realloc(
      map->entries, next * sizeof(*map->entries));
    map->capacity = next;
  }
  return &map->entries[map->count++];
}

BOOL compact_term_pool_copy_clause(Compact_term_pool destination,
                                   Compact_term_pool source,
                                   Compact_term_rebase_map map,
                                   unsigned long long proof_id)
{
  size_t source_at, destination_at;
  uint32_t old_offset, new_offset, length;
  struct compact_term_rebase_entry *entry;
  if (destination == NULL || source == NULL || map == NULL ||
      proof_id == 0 || map->finalized || source->directory_capacity == 0 ||
      map->mode == REBASE_MODE_RETAINED)
    return FALSE;
  map->mode = REBASE_MODE_COPY;
  source_at = directory_slot(source, proof_id);
  if (source->proof_ids[source_at] != proof_id)
    return FALSE;
  if (destination->directory_capacity != 0) {
    destination_at = directory_slot(destination, proof_id);
    if (destination->proof_ids[destination_at] == proof_id)
      return TRUE;
  }
  old_offset = source->clause_offsets[source_at];
  length = source->clause_lengths[source_at];
  new_offset = compact_term_pool_append(
    destination, source->tokens + old_offset, length);
  ensure_directory(destination);
  destination_at = directory_slot(destination, proof_id);
  destination->proof_ids[destination_at] = proof_id;
  destination->clause_offsets[destination_at] = new_offset;
  destination->clause_lengths[destination_at] = length;
  destination->directory_count++;
  destination->serializations++;
  entry = append_rebase_entry(map);
  entry->destination.new_offset = new_offset;
  entry->old_offset = old_offset;
  entry->length = length;
  update_peak(destination);
  return TRUE;
}

BOOL compact_term_rebase_map_retain_clause(Compact_term_rebase_map map,
                                           Compact_term_pool source,
                                           unsigned long long proof_id)
{
  size_t at, word;
  uint64_t bit;
  struct compact_term_rebase_entry *entry;
  if (map == NULL || source == NULL || proof_id == 0 || map->finalized ||
      source->directory_capacity == 0 || map->mode == REBASE_MODE_COPY)
    return FALSE;
  if (map->retained_source == NULL) {
    map->retained_source = source;
    map->retained_slot_words =
      (source->directory_capacity + 63) / 64;
    map->retained_slots = safe_calloc(
      map->retained_slot_words, sizeof(*map->retained_slots));
    map->mode = REBASE_MODE_RETAINED;
  }
  else if (map->retained_source != source)
    return FALSE;
  at = directory_slot(source, proof_id);
  if (source->proof_ids[at] != proof_id)
    return FALSE;
  word = at / 64;
  bit = UINT64_C(1) << (at % 64);
  if ((map->retained_slots[word] & bit) != 0)
    return TRUE;
  map->retained_slots[word] |= bit;
  entry = append_rebase_entry(map);
  entry->destination.proof_id = proof_id;
  entry->old_offset = source->clause_offsets[at];
  entry->length = source->clause_lengths[at];
  return TRUE;
}

static int increasing_old_offset(const void *left, const void *right)
{
  const struct compact_term_rebase_entry *a = left;
  const struct compact_term_rebase_entry *b = right;
  return a->old_offset < b->old_offset ? -1 :
         a->old_offset > b->old_offset ? 1 : 0;
}

void compact_term_rebase_map_finalize(Compact_term_rebase_map map)
{
  size_t i;
  if (map == NULL || map->finalized)
    return;
  if (map->mode == REBASE_MODE_RETAINED)
    fatal_error("compact_term_pool: retained map requires in-place compaction");
  qsort(map->entries, map->count, sizeof(*map->entries),
        increasing_old_offset);
  for (i = 1; i < map->count; i++)
    if ((uint64_t) map->entries[i-1].old_offset +
          map->entries[i-1].length > map->entries[i].old_offset)
      fatal_error("compact_term_pool: overlapping rebase intervals");
  map->finalized = TRUE;
}

static size_t compacted_token_capacity(size_t needed)
{
  size_t capacity = 0;
  while (capacity < needed)
    capacity = grow_token_capacity(capacity);
  return capacity;
}

static size_t compacted_directory_capacity(size_t entries)
{
  size_t capacity = 128;
  while ((entries + 1) * 20 >= capacity * 17) {
    if (capacity > SIZE_MAX / 2)
      fatal_error("compact_term_pool: compacted directory overflow");
    capacity *= 2;
  }
  return capacity;
}

void compact_term_pool_compact_retained(Compact_term_pool pool,
                                        Compact_term_rebase_map map)
{
  unsigned long long old_bytes;
  size_t i, token_count = 0, token_capacity, directory_capacity;
  if (pool == NULL || map == NULL || map->finalized ||
      map->mode != REBASE_MODE_RETAINED ||
      map->retained_source != pool || map->count == 0)
    fatal_error("compact_term_pool: invalid retained compaction");
  qsort(map->entries, map->count, sizeof(*map->entries),
        increasing_old_offset);
  for (i = 0; i < map->count; i++) {
    struct compact_term_rebase_entry *entry = &map->entries[i];
    if ((uint64_t) entry->old_offset + entry->length > pool->token_count)
      fatal_error("compact_term_pool: retained interval exceeds pool");
    if (i != 0 &&
        (uint64_t) map->entries[i-1].old_offset +
          map->entries[i-1].length > entry->old_offset)
      fatal_error("compact_term_pool: overlapping retained intervals");
    if (entry->length > UINT32_MAX - token_count)
      fatal_error("compact_term_pool: compacted offsets exceed 32 bits");
    token_count += entry->length;
  }

  old_bytes = pool_bytes(pool);
  directory_capacity = compacted_directory_capacity(map->count);
  pool->proof_ids = safe_realloc(
    pool->proof_ids, directory_capacity * sizeof(*pool->proof_ids));
  pool->clause_offsets = safe_realloc(
    pool->clause_offsets,
    directory_capacity * sizeof(*pool->clause_offsets));
  pool->clause_lengths = safe_realloc(
    pool->clause_lengths,
    directory_capacity * sizeof(*pool->clause_lengths));
  memset(pool->proof_ids, 0,
         directory_capacity * sizeof(*pool->proof_ids));
  memset(pool->clause_offsets, 0,
         directory_capacity * sizeof(*pool->clause_offsets));
  memset(pool->clause_lengths, 0,
         directory_capacity * sizeof(*pool->clause_lengths));
  pool->directory_capacity = directory_capacity;
  pool->directory_count = 0;

  token_count = 0;
  for (i = 0; i < map->count; i++) {
    struct compact_term_rebase_entry *entry = &map->entries[i];
    unsigned long long proof_id = entry->destination.proof_id;
    size_t at;
    if (entry->old_offset != token_count)
      memmove(pool->tokens + token_count,
              pool->tokens + entry->old_offset,
              (size_t) entry->length * sizeof(*pool->tokens));
    at = directory_slot(pool, proof_id);
    pool->proof_ids[at] = proof_id;
    pool->clause_offsets[at] = (uint32_t) token_count;
    pool->clause_lengths[at] = entry->length;
    pool->directory_count++;
    entry->destination.new_offset = token_count;
    token_count += entry->length;
  }
  pool->token_count = token_count;
  token_capacity = compacted_token_capacity(token_count);
  (void) resize_tokens(pool, token_capacity);
  pool->compactions++;
  if (old_bytes > pool_bytes(pool))
    pool->bytes_reclaimed += old_bytes - pool_bytes(pool);
  safe_free(map->retained_slots);
  map->retained_slots = NULL;
  map->retained_slot_words = 0;
  map->finalized = TRUE;
}

uint32_t compact_term_rebase_offset(Compact_term_rebase_map map,
                                    uint32_t old_offset)
{
  size_t low = 0, high;
  struct compact_term_rebase_entry *entry;
  if (map == NULL || !map->finalized || map->count == 0)
    fatal_error("compact_term_pool: unfinished or empty rebase map");
  high = map->count;
  while (low < high) {
    size_t middle = low + (high - low) / 2;
    if (map->entries[middle].old_offset <= old_offset)
      low = middle + 1;
    else
      high = middle;
  }
  if (low == 0)
    fatal_error("compact_term_pool: token offset precedes rebase map");
  entry = &map->entries[low - 1];
  if ((uint64_t) old_offset >=
      (uint64_t) entry->old_offset + entry->length)
    fatal_error("compact_term_pool: token offset is not retained");
  return (uint32_t) entry->destination.new_offset +
    (old_offset - entry->old_offset);
}

void compact_term_rebase_map_free(Compact_term_rebase_map map)
{
  if (map == NULL)
    return;
  safe_free(map->entries);
  safe_free(map->retained_slots);
  safe_free(map);
}

void compact_term_pool_finish_compaction(Compact_term_pool destination,
                                         Compact_term_pool source)
{
  unsigned long long source_bytes, destination_bytes;
  if (destination == NULL || source == NULL)
    fatal_error("compact_term_pool_finish_compaction: null pool");
  source_bytes = pool_bytes(source);
  destination_bytes = pool_bytes(destination);
  destination->serializations = source->serializations;
  destination->lookups = source->lookups;
  destination->hits = source->hits;
  destination->reused_tokens = source->reused_tokens;
  destination->token_growths += source->token_growths;
  destination->token_copy_bytes += source->token_copy_bytes;
  destination->compactions = source->compactions + 1;
  destination->bytes_reclaimed = source->bytes_reclaimed +
    (source_bytes > destination_bytes ? source_bytes - destination_bytes : 0);
  if (source->peak_bytes > destination->peak_bytes)
    destination->peak_bytes = source->peak_bytes;
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
  stats->compactions = pool->compactions;
  stats->bytes_reclaimed = pool->bytes_reclaimed;
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
  release_tokens(pool);
  safe_free(pool->proof_ids);
  safe_free(pool->clause_offsets);
  safe_free(pool->clause_lengths);
  safe_free(pool->profile_hashes);
  safe_free(pool->profile_offsets);
  safe_free(pool->profile_lengths);
  safe_free(pool);
}
