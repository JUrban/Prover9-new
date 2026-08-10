#if defined(__linux__) && !defined(__EMSCRIPTEN__)
#define _GNU_SOURCE
#endif
#include "compact_term_pool.h"
#include "compact_id_map.h"

#include <errno.h>
#include <stdint.h>
#include <stdio.h>
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
  Compact_id_map directory;
  size_t directory_count;
  unsigned long long cached_proof_id;
  uint32_t cached_clause_offset;
  uint32_t cached_clause_length;
  unsigned long long serializations;
  unsigned long long lookups;
  unsigned long long hits;
  unsigned long long reused_tokens;
  unsigned long long token_growths;
  unsigned long long token_copy_bytes;
  unsigned long long rebase_growths;
  unsigned long long rebase_copy_bytes;
  unsigned long long streamed_rebases;
  unsigned long long file_sorted_rebases;
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
#if defined(__linux__) && !defined(__EMSCRIPTEN__)
  FILE *entries_file;
#endif
  size_t count;
  size_t capacity;
  size_t entries_mapping_bytes;
  BOOL entries_mapped;
  unsigned long long growths;
  unsigned long long copy_bytes;
  Compact_term_pool retained_source;
  Compact_id_set retained_ids;
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
    compact_id_map_bytes(pool->directory) +
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

static BOOL directory_get(Compact_term_pool pool,
                          unsigned long long proof_id,
                          uint32_t *offset, uint32_t *length)
{
  uint32_t values[2];
  if (pool->cached_proof_id == proof_id) {
    values[0] = pool->cached_clause_offset + 1;
    values[1] = pool->cached_clause_length;
  }
  else {
    if (!compact_id_map_get(pool->directory, proof_id, values))
      return FALSE;
    if (values[0] == 0)
      fatal_error("compact_term_pool: invalid directory offset sentinel");
    pool->cached_proof_id = proof_id;
    pool->cached_clause_offset = values[0] - 1;
    pool->cached_clause_length = values[1];
  }
  if (offset != NULL)
    *offset = values[0] - 1;
  if (length != NULL)
    *length = values[1];
  return TRUE;
}

static BOOL directory_put(Compact_term_pool pool,
                          unsigned long long proof_id,
                          uint32_t offset, uint32_t length)
{
  uint32_t values[2];
  BOOL inserted;
  if (offset == UINT32_MAX)
    fatal_error("compact_term_pool: directory offset exceeds sentinel");
  values[0] = offset + 1;
  values[1] = length;
  inserted = compact_id_map_put(pool->directory, proof_id, values);
  pool->cached_proof_id = proof_id;
  pool->cached_clause_offset = offset;
  pool->cached_clause_length = length;
  return inserted;
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
  if (directory_put(pool, proof_id, offset,
                    (uint32_t) pool->token_count - offset))
    pool->directory_count++;
  pool->serializations++;
  update_peak(pool);
}

Compact_term_pool compact_term_pool_init(void)
{
  Compact_term_pool pool = safe_calloc(1, sizeof(*pool));
  pool->directory = compact_id_map_init(2);
  update_peak(pool);
  return pool;
}

Compact_term_rebase_map compact_term_rebase_map_init(void)
{
  return safe_calloc(1, sizeof(struct compact_term_rebase_map));
}

/* A late Osborn compaction retains roughly 150,000 clauses.  Growing this
   temporary vector with realloc briefly overlapped its old and new 2/4-MiB
   allocations and set the process RSS high-water mark even though the term
   tokens themselves already use mremap.  Give the vector the same Linux
   page-table-only growth path; the portable fallback keeps realloc and
   reports the bytes that may have been copied. */
static void resize_rebase_entries(Compact_term_rebase_map map, size_t capacity)
{
  size_t bytes = capacity * sizeof(*map->entries);
#if defined(__linux__) && !defined(__EMSCRIPTEN__)
  long page_size = sysconf(_SC_PAGESIZE);
  size_t mapped_bytes;
  void *p;
  if (page_size <= 0)
    fatal_error("compact_term_pool: cannot determine rebase page size");
  if (bytes > SIZE_MAX - (size_t) page_size + 1)
    fatal_error("compact_term_pool: rebase mapping overflow");
  mapped_bytes = ((bytes + (size_t) page_size - 1) /
                  (size_t) page_size) * (size_t) page_size;
  if (!map->entries_mapped && map->entries != NULL)
    fatal_error("compact_term_pool: mixed rebase allocation modes");
  if (map->entries == NULL)
    p = mmap(NULL, mapped_bytes, PROT_READ | PROT_WRITE,
             MAP_PRIVATE | MAP_ANONYMOUS, -1, 0);
  else
    p = mremap(map->entries, map->entries_mapping_bytes, mapped_bytes,
               MREMAP_MAYMOVE);
  if (p == MAP_FAILED)
    fatal_error("compact_term_pool: cannot resize rebase mapping");
  map->entries = p;
  map->entries_mapping_bytes = mapped_bytes;
  map->entries_mapped = TRUE;
#else
  if (map->capacity != 0)
    map->copy_bytes += map->capacity * sizeof(*map->entries);
  map->entries = safe_realloc(map->entries, bytes);
#endif
  map->capacity = capacity;
  map->growths++;
}

static struct compact_term_rebase_entry *append_rebase_entry(
  Compact_term_rebase_map map)
{
  if (map->count == map->capacity) {
    size_t next = map->capacity == 0 ? 64 : map->capacity * 2;
    if (next < map->capacity ||
        next > SIZE_MAX / sizeof(*map->entries))
      fatal_error("compact_term_pool: rebase map overflow");
    resize_rebase_entries(map, next);
  }
  return &map->entries[map->count++];
}

BOOL compact_term_pool_copy_clause(Compact_term_pool destination,
                                   Compact_term_pool source,
                                   Compact_term_rebase_map map,
                                   unsigned long long proof_id)
{
  uint32_t old_offset, new_offset, length;
  struct compact_term_rebase_entry *entry;
  if (destination == NULL || source == NULL || map == NULL ||
      proof_id == 0 || map->finalized || source->directory_count == 0 ||
      map->mode == REBASE_MODE_RETAINED)
    return FALSE;
  map->mode = REBASE_MODE_COPY;
  if (!directory_get(source, proof_id, &old_offset, &length))
    return FALSE;
  if (directory_get(destination, proof_id, NULL, NULL))
    return TRUE;
  new_offset = compact_term_pool_append(
    destination, source->tokens + old_offset, length);
  if (!directory_put(destination, proof_id, new_offset, length))
    fatal_error("compact_term_pool: duplicate copied clause");
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
  if (map == NULL || source == NULL || proof_id == 0 || map->finalized ||
      source->directory_count == 0 || map->mode == REBASE_MODE_COPY)
    return FALSE;
  if (map->retained_source == NULL) {
    map->retained_source = source;
    map->retained_ids = compact_id_set_init();
    map->mode = REBASE_MODE_RETAINED;
  }
  else if (map->retained_source != source)
    return FALSE;
  if (!directory_get(source, proof_id, NULL, NULL))
    return FALSE;
  if (!compact_id_set_add(map->retained_ids, proof_id))
    return TRUE;
  /* The retained-ID bitset is already an exact first pass over the union
     of all three indexes.  Count here and materialize the interval vector
     once at its final size in compact_term_pool_compact_retained(); growing
     a power-of-two vector retained almost 110,000 unused late-proof slots. */
  map->count++;
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

struct retained_measure_context {
  Compact_term_pool pool;
  size_t count;
  size_t token_count;
};

static void measure_retained_clause(unsigned long long proof_id,
                                    void *context)
{
  struct retained_measure_context *measure = context;
  uint32_t length;
  if (!directory_get(measure->pool, proof_id, NULL, &length) ||
      length > SIZE_MAX - measure->token_count)
    fatal_error("compact_term_pool: corrupt retained size prediction");
  measure->token_count += length;
  measure->count++;
}

struct retained_materialize_context {
  Compact_term_pool pool;
  Compact_term_rebase_map map;
  size_t count;
};

static void materialize_retained_clause(unsigned long long proof_id,
                                        void *context)
{
  struct retained_materialize_context *materialize = context;
  struct compact_term_rebase_entry *entry;
  uint32_t offset, length;
  if (materialize->count >= materialize->map->count ||
      !directory_get(materialize->pool, proof_id, &offset, &length))
    fatal_error("compact_term_pool: corrupt retained ID set");
  entry = &materialize->map->entries[materialize->count++];
  entry->destination.proof_id = proof_id;
  entry->old_offset = offset;
  entry->length = length;
}

#if defined(__linux__) && !defined(__EMSCRIPTEN__)

/* Retained proof IDs normally rise in exactly the order in which their
   immutable token intervals were appended.  Stream that already sorted
   production case through an unlinked file so that the late 2--3 MiB rebase
   vector is not resident while the old token pages still exist.  Rewritten
   or non-monotone proof-ID histories are ordered by the bounded file radix
   pass below; the established in-memory qsort remains the I/O fallback. */
struct retained_stream_context {
  Compact_term_pool pool;
  FILE *file;
  size_t count;
  size_t token_count;
  uint64_t previous_end;
  BOOL monotone;
  BOOL write_failed;
};

static void stream_retained_clause(unsigned long long proof_id,
                                   void *context)
{
  struct retained_stream_context *stream = context;
  struct compact_term_rebase_entry entry;
  uint32_t offset, length;
  if (!directory_get(stream->pool, proof_id, &offset, &length) ||
      (uint64_t) offset + length > stream->pool->token_count)
    fatal_error("compact_term_pool: corrupt streamed retained interval");
  if (stream->count != 0 && offset < stream->previous_end)
    stream->monotone = FALSE;
  if (stream->token_count > UINT32_MAX ||
      length > UINT32_MAX - stream->token_count)
    fatal_error("compact_term_pool: compacted offsets exceed 32 bits");
  entry.destination.proof_id = proof_id;
  entry.old_offset = offset;
  entry.length = length;
  if (!stream->write_failed &&
      fwrite(&entry, sizeof(entry), 1, stream->file) != 1)
    stream->write_failed = TRUE;
  stream->previous_end = (uint64_t) offset + length;
  stream->token_count += length;
  stream->count++;
}

static BOOL read_rebase_entries(int fd,
                                struct compact_term_rebase_entry *entries,
                                size_t first, size_t count)
{
  unsigned char *bytes = (unsigned char *) entries;
  size_t done = 0;
  size_t total = count * sizeof(*entries);
  off_t offset = (off_t) (first * sizeof(*entries));
  while (done < total) {
    ssize_t got = pread(fd, bytes + done, total - done,
                        offset + (off_t) done);
    if (got < 0 && errno == EINTR)
      continue;
    if (got <= 0)
      return FALSE;
    done += (size_t) got;
  }
  return TRUE;
}

static BOOL write_rebase_entry(int fd,
                               const struct compact_term_rebase_entry *entry,
                               size_t at)
{
  const unsigned char *bytes = (const unsigned char *) entry;
  size_t done = 0;
  size_t total = sizeof(*entry);
  off_t offset = (off_t) (at * sizeof(*entry));
  while (done < total) {
    ssize_t put = pwrite(fd, bytes + done, total - done,
                         offset + (off_t) done);
    if (put < 0 && errno == EINTR)
      continue;
    if (put <= 0)
      return FALSE;
    done += (size_t) put;
  }
  return TRUE;
}

/* Four stable byte-wise counting passes sort arbitrary 32-bit token offsets
   while retaining only one 4-KiB entry buffer and two 256-counter arrays.
   Four is even, so the final order is back in SOURCE and SCRATCH remains
   available for the transformed old-to-new map. */
static BOOL radix_sort_rebase_file(FILE *source, FILE *scratch, size_t count)
{
  enum { REBASE_RADIX = 256, REBASE_IO_ENTRIES = 256 };
  struct compact_term_rebase_entry buffer[REBASE_IO_ENTRIES];
  size_t counts[REBASE_RADIX], cursors[REBASE_RADIX];
  int source_fd = fileno(source), scratch_fd = fileno(scratch);
  unsigned pass;
  for (pass = 0; pass < 4; pass++) {
    int input_fd = (pass & 1) == 0 ? source_fd : scratch_fd;
    int output_fd = (pass & 1) == 0 ? scratch_fd : source_fd;
    unsigned shift = pass * 8;
    size_t first, radix, total = 0;
    memset(counts, 0, sizeof(counts));
    for (first = 0; first < count; first += REBASE_IO_ENTRIES) {
      size_t i;
      size_t amount = count - first < REBASE_IO_ENTRIES ?
        count - first : REBASE_IO_ENTRIES;
      if (!read_rebase_entries(input_fd, buffer, first, amount))
        return FALSE;
      for (i = 0; i < amount; i++)
        counts[(buffer[i].old_offset >> shift) & 0xffU]++;
    }
    for (radix = 0; radix < REBASE_RADIX; radix++) {
      cursors[radix] = total;
      total += counts[radix];
    }
    if (total != count)
      return FALSE;
    for (first = 0; first < count; first += REBASE_IO_ENTRIES) {
      size_t i;
      size_t amount = count - first < REBASE_IO_ENTRIES ?
        count - first : REBASE_IO_ENTRIES;
      if (!read_rebase_entries(input_fd, buffer, first, amount))
        return FALSE;
      for (i = 0; i < amount; i++) {
        unsigned radix = (buffer[i].old_offset >> shift) & 0xffU;
        if (!write_rebase_entry(output_fd, &buffer[i], cursors[radix]++))
          return FALSE;
      }
    }
  }
  return TRUE;
}

static BOOL compact_retained_streamed(Compact_term_pool pool,
                                      Compact_term_rebase_map map)
{
  enum { REBASE_STREAM_ENTRIES = 256 };
  struct compact_term_rebase_entry buffer[REBASE_STREAM_ENTRIES];
  struct retained_stream_context stream;
  unsigned long long old_bytes;
  FILE *source_file, *destination_file;
  size_t remaining, token_count, token_capacity, mapping_bytes;
  void *entries;
  BOOL file_sorted;

  source_file = tmpfile();
  if (source_file == NULL)
    return FALSE;
  if (map->count > SIZE_MAX / sizeof(*map->entries)) {
    fclose(source_file);
    fatal_error("compact_term_pool: streamed rebase map overflow");
  }
  memset(&stream, 0, sizeof(stream));
  stream.pool = pool;
  stream.file = source_file;
  stream.monotone = TRUE;
  compact_id_set_foreach(map->retained_ids, stream_retained_clause, &stream);
  if (stream.count != map->count)
    fatal_error("compact_term_pool: incomplete streamed retained ID set");
  if (stream.write_failed || fflush(source_file) != 0) {
    fclose(source_file);
    return FALSE;
  }
  destination_file = tmpfile();
  if (destination_file == NULL) {
    fclose(source_file);
    return FALSE;
  }
  file_sorted = !stream.monotone;
  if (file_sorted &&
      !radix_sort_rebase_file(source_file, destination_file, map->count)) {
    fclose(destination_file);
    fclose(source_file);
    return FALSE;
  }

  old_bytes = pool_bytes(pool);
  compact_id_map_free(pool->directory);
  pool->directory = compact_id_map_init(2);
  pool->directory_count = 0;
  pool->cached_proof_id = 0;

  rewind(source_file);
  rewind(destination_file);
  remaining = map->count;
  token_count = 0;
  while (remaining != 0) {
    size_t i;
    size_t count = remaining < REBASE_STREAM_ENTRIES ?
      remaining : REBASE_STREAM_ENTRIES;
    if (fread(buffer, sizeof(*buffer), count, source_file) != count)
      fatal_error("compact_term_pool: cannot read streamed rebase map");
    for (i = 0; i < count; i++) {
      struct compact_term_rebase_entry *entry = &buffer[i];
      unsigned long long proof_id = entry->destination.proof_id;
      if (entry->old_offset != token_count)
        memmove(pool->tokens + token_count,
                pool->tokens + entry->old_offset,
                (size_t) entry->length * sizeof(*pool->tokens));
      if (!directory_put(pool, proof_id, (uint32_t) token_count,
                         entry->length))
        fatal_error("compact_term_pool: duplicate streamed proof ID");
      pool->directory_count++;
      entry->destination.new_offset = token_count;
      token_count += entry->length;
    }
    if (fwrite(buffer, sizeof(*buffer), count, destination_file) != count)
      fatal_error("compact_term_pool: cannot write streamed rebase map");
    remaining -= count;
  }
  if (fflush(destination_file) != 0)
    fatal_error("compact_term_pool: cannot flush streamed rebase map");
  fclose(source_file);

  pool->token_count = token_count;
  token_capacity = compacted_token_capacity(token_count);
  (void) resize_tokens(pool, token_capacity);

  if (map->count > SIZE_MAX / sizeof(*map->entries))
    fatal_error("compact_term_pool: streamed rebase map overflow");
  mapping_bytes = map->count * sizeof(*map->entries);
  entries = mmap(NULL, mapping_bytes, PROT_READ, MAP_PRIVATE,
                 fileno(destination_file), 0);
  if (entries == MAP_FAILED)
    fatal_error("compact_term_pool: cannot map streamed rebase map");
  map->entries = entries;
  map->entries_file = destination_file;
  map->entries_mapping_bytes = mapping_bytes;
  map->entries_mapped = TRUE;
  map->capacity = map->count;
  map->growths++;

  pool->rebase_growths += map->growths;
  pool->rebase_copy_bytes += map->copy_bytes;
  pool->streamed_rebases++;
  if (file_sorted)
    pool->file_sorted_rebases++;
  pool->compactions++;
  if (old_bytes > pool_bytes(pool))
    pool->bytes_reclaimed += old_bytes - pool_bytes(pool);
  compact_id_set_free(map->retained_ids);
  map->retained_ids = NULL;
  map->finalized = TRUE;
  return TRUE;
}

#endif

unsigned long long compact_term_pool_retained_reclaimable_bytes(
  Compact_term_pool pool, Compact_term_rebase_map map)
{
  unsigned long long current, compacted;
  struct retained_measure_context measure;
  size_t token_capacity;
  if (pool == NULL || map == NULL || map->finalized ||
      map->mode != REBASE_MODE_RETAINED ||
      map->retained_source != pool || map->count == 0)
    return 0;
  memset(&measure, 0, sizeof(measure));
  measure.pool = pool;
  compact_id_set_foreach(map->retained_ids, measure_retained_clause,
                         &measure);
  if (measure.count != map->count)
    fatal_error("compact_term_pool: incomplete retained size prediction");
  token_capacity = compacted_token_capacity(measure.token_count);
  current = pool_bytes(pool);
  compacted = sizeof(*pool) +
    (unsigned long long) token_capacity * sizeof(*pool->tokens) +
    compact_id_set_projected_map_bytes(map->retained_ids, 2) +
    (unsigned long long) pool->profile_capacity *
      (sizeof(*pool->profile_hashes) + sizeof(*pool->profile_offsets) +
       sizeof(*pool->profile_lengths));
  return current > compacted ? current - compacted : 0;
}

void compact_term_pool_compact_retained(Compact_term_pool pool,
                                        Compact_term_rebase_map map)
{
  unsigned long long old_bytes;
  struct retained_materialize_context materialize;
  size_t i, token_count = 0;
  size_t token_capacity;
  if (pool == NULL || map == NULL || map->finalized ||
      map->mode != REBASE_MODE_RETAINED ||
      map->retained_source != pool || map->count == 0)
    fatal_error("compact_term_pool: invalid retained compaction");
  if (map->entries != NULL || map->capacity != 0)
    fatal_error("compact_term_pool: retained entries already materialized");
#if defined(__linux__) && !defined(__EMSCRIPTEN__)
  if (compact_retained_streamed(pool, map))
    return;
#endif
  resize_rebase_entries(map, map->count);
  memset(&materialize, 0, sizeof(materialize));
  materialize.pool = pool;
  materialize.map = map;
  compact_id_set_foreach(map->retained_ids, materialize_retained_clause,
                         &materialize);
  if (materialize.count != map->count)
    fatal_error("compact_term_pool: incomplete retained ID set");
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
  compact_id_map_free(pool->directory);
  pool->directory = compact_id_map_init(2);
  pool->directory_count = 0;
  pool->cached_proof_id = 0;

  token_count = 0;
  for (i = 0; i < map->count; i++) {
    struct compact_term_rebase_entry *entry = &map->entries[i];
    unsigned long long proof_id = entry->destination.proof_id;
    if (entry->old_offset != token_count)
      memmove(pool->tokens + token_count,
              pool->tokens + entry->old_offset,
              (size_t) entry->length * sizeof(*pool->tokens));
    if (!directory_put(pool, proof_id, (uint32_t) token_count,
                       entry->length))
      fatal_error("compact_term_pool: duplicate retained proof ID");
    pool->directory_count++;
    entry->destination.new_offset = token_count;
    token_count += entry->length;
  }
  pool->token_count = token_count;
  token_capacity = compacted_token_capacity(token_count);
  (void) resize_tokens(pool, token_capacity);
  pool->rebase_growths += map->growths;
  pool->rebase_copy_bytes += map->copy_bytes;
  pool->compactions++;
  if (old_bytes > pool_bytes(pool))
    pool->bytes_reclaimed += old_bytes - pool_bytes(pool);
  compact_id_set_free(map->retained_ids);
  map->retained_ids = NULL;
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
#if defined(__linux__) && !defined(__EMSCRIPTEN__)
  if (map->entries_mapped) {
    if (map->entries != NULL &&
        munmap(map->entries, map->entries_mapping_bytes) != 0)
      fatal_error("compact_term_pool: cannot release rebase mapping");
  }
  else
    safe_free(map->entries);
  if (map->entries_file != NULL)
    fclose(map->entries_file);
#else
  safe_free(map->entries);
#endif
  compact_id_set_free(map->retained_ids);
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
  destination->rebase_growths += source->rebase_growths;
  destination->rebase_copy_bytes += source->rebase_copy_bytes;
  destination->streamed_rebases += source->streamed_rebases;
  destination->file_sorted_rebases += source->file_sorted_rebases;
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
  uint32_t offset, clause_offset, clause_length;
  if (pool == NULL || proof_id == 0 || literals == NULL || target == NULL ||
      length == NULL)
    fatal_error("compact_term_pool_intern: invalid request");
  pool->lookups++;
  if (directory_get(pool, proof_id, &clause_offset, &clause_length)) {
    offset = find_term(pool, clause_offset, clause_length, target, length);
    if (offset != UINT32_MAX) {
      pool->hits++;
      pool->reused_tokens += *length;
      return offset;
    }
  }
  serialize_clause(pool, proof_id, literals);
  if (!directory_get(pool, proof_id, &clause_offset, &clause_length))
    fatal_error("compact_term_pool_intern: missing serialized clause");
  offset = find_term(pool, clause_offset, clause_length, target, length);
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
  stats->rebase_growths = pool->rebase_growths;
  stats->rebase_copy_bytes = pool->rebase_copy_bytes;
  stats->streamed_rebases = pool->streamed_rebases;
  stats->file_sorted_rebases = pool->file_sorted_rebases;
  stats->directory_bytes = compact_id_map_bytes(pool->directory);
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
  compact_id_map_free(pool->directory);
  safe_free(pool->profile_hashes);
  safe_free(pool->profile_offsets);
  safe_free(pool->profile_lengths);
  safe_free(pool);
}
