#include "compact_back_demod.h"

#include <stdint.h>
#include <stdlib.h>
#include <string.h>

#define CBD_NONE 0U
#define CBD_TOMBSTONE UINT64_MAX
#define CBD_POSTING_BLOCK_PAYLOAD 248

struct cbd_posting_block {
  uint32_t next;
  uint16_t used;
  uint16_t count;
  unsigned char data[CBD_POSTING_BLOCK_PAYLOAD];
};

struct cbd_local_occurrence {
  uint32_t symbol;
  uint32_t offset;
};

struct cbd_record {
  unsigned long long proof_id;
  uint32_t token_offset;
  uint32_t token_length;
  uint32_t query_stamp;
  unsigned char active;
};

struct compact_back_demod_index {
  struct cbd_posting_block *posting_blocks;
  size_t posting_block_count;
  size_t posting_block_capacity;
  size_t posting_count;
  size_t posting_stream_used;
  uint32_t *posting_heads;
  uint32_t *posting_tails;
  uint32_t *posting_last_records;
  uint32_t *posting_last_occurrences;
  size_t symbol_capacity;
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
  unsigned long long *hash_keys;
  uint32_t *hash_values;
  size_t hash_capacity;
  size_t hash_count;
  size_t hash_tombstones;
  unsigned long long *results;
  size_t result_capacity;
  uint32_t query_stamp;
  unsigned long long active;
  unsigned long long peak;
  unsigned long long retired;
  unsigned long long queries;
  unsigned long long candidates;
  unsigned long long exact_tests;
  unsigned long long symbol_occurrences;
  unsigned long long peak_bytes;
};

struct cbd_symbol_set {
  uint32_t fixed[32];
  uint32_t *values;
  size_t count;
  size_t capacity;
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

static unsigned long long index_bytes(Compact_back_demod_index index)
{
  struct compact_term_pool_stats terms;
  if (index == NULL)
    return 0;
  compact_term_pool_get_stats(index->term_pool, &terms);
  return sizeof(*index) +
    index->posting_block_capacity * sizeof(*index->posting_blocks) +
    index->symbol_capacity *
      (sizeof(*index->posting_heads) + sizeof(*index->posting_tails) +
       sizeof(*index->posting_last_records) +
       sizeof(*index->posting_last_occurrences)) +
    index->occurrence_capacity * sizeof(*index->occurrences) +
    index->record_capacity * sizeof(*index->records) +
    (index->owns_term_pool ? terms.total_bytes : 0) +
    index->hash_capacity *
      (sizeof(*index->hash_keys) + sizeof(*index->hash_values)) +
    index->result_capacity * sizeof(*index->results);
}

static void update_peak(Compact_back_demod_index index)
{
  unsigned long long bytes = index_bytes(index);
  if (bytes > index->peak_bytes)
    index->peak_bytes = bytes;
  if (index->active > index->peak)
    index->peak = index->active;
}

static size_t hash_slot(Compact_back_demod_index index, uint64_t id,
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
    if (inserting && key == CBD_TOMBSTONE && tombstone == SIZE_MAX)
      tombstone = at;
    at = (at + 1) & mask;
  }
}

static void rehash(Compact_back_demod_index index, size_t capacity)
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
    if (old_keys[i] != 0 && old_keys[i] != CBD_TOMBSTONE) {
      size_t at = hash_slot(index, old_keys[i], TRUE);
      index->hash_keys[at] = old_keys[i];
      index->hash_values[at] = old_values[i];
    }
  safe_free(old_keys);
  safe_free(old_values);
}

static void ensure_hash(Compact_back_demod_index index)
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
        fatal_error("compact_back_demod: hash overflow");
      rehash(index, index->hash_capacity * 2);
    }
  }
}

static uint32_t lookup_record(Compact_back_demod_index index,
                              unsigned long long proof_id)
{
  size_t at;
  if (index == NULL || proof_id == 0 || index->hash_capacity == 0)
    return CBD_NONE;
  at = hash_slot(index, proof_id, FALSE);
  return index->hash_keys[at] == proof_id ?
    index->hash_values[at] : CBD_NONE;
}

static void ensure_symbols(Compact_back_demod_index index, unsigned symbol)
{
  size_t old_capacity;
  if ((size_t) symbol < index->symbol_capacity)
    return;
  old_capacity = index->symbol_capacity;
  while ((size_t) symbol >= index->symbol_capacity)
    index->symbol_capacity = grow_capacity(
      index->symbol_capacity, sizeof(*index->posting_heads),
      "compact_back_demod: symbol table overflow");
  index->posting_heads = safe_realloc(
    index->posting_heads,
    index->symbol_capacity * sizeof(*index->posting_heads));
  index->posting_tails = safe_realloc(
    index->posting_tails,
    index->symbol_capacity * sizeof(*index->posting_tails));
  index->posting_last_records = safe_realloc(
    index->posting_last_records,
    index->symbol_capacity * sizeof(*index->posting_last_records));
  index->posting_last_occurrences = safe_realloc(
    index->posting_last_occurrences,
    index->symbol_capacity * sizeof(*index->posting_last_occurrences));
  memset(index->posting_heads + old_capacity, 0,
         (index->symbol_capacity - old_capacity) *
           sizeof(*index->posting_heads));
  memset(index->posting_tails + old_capacity, 0,
         (index->symbol_capacity - old_capacity) *
           sizeof(*index->posting_tails));
  memset(index->posting_last_records + old_capacity, 0,
         (index->symbol_capacity - old_capacity) *
           sizeof(*index->posting_last_records));
  memset(index->posting_last_occurrences + old_capacity, 0,
         (index->symbol_capacity - old_capacity) *
           sizeof(*index->posting_last_occurrences));
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
    index->occurrence_capacity = grow_capacity(
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
  ENSURE_ARRAY(index, posting_blocks, posting_block_count,
               posting_block_capacity,
               "compact_back_demod: posting block overflow");
  if (index->posting_block_count > UINT32_MAX)
    fatal_error("compact_back_demod: posting block offsets exceed 32 bits");
  block = (uint32_t) index->posting_block_count++;
  memset(&index->posting_blocks[block], 0,
         sizeof(index->posting_blocks[block]));
  return block;
}

static void append_symbol_record(Compact_back_demod_index index,
                                 uint32_t record, unsigned symbol,
                                 uint32_t occurrence_offset,
                                 uint32_t occurrence_length)
{
  unsigned char encoded[15];
  size_t length = 0;
  uint32_t block;
  struct cbd_posting_block *tail;
  ensure_symbols(index, symbol);
  if (record <= index->posting_last_records[symbol])
    fatal_error("compact_back_demod: nonmonotone posting record");
  if (occurrence_offset < index->posting_last_occurrences[symbol])
    fatal_error("compact_back_demod: nonmonotone posting occurrence");
  if (occurrence_length == 0)
    fatal_error("compact_back_demod: empty posting occurrence list");
  length += encode_u32(encoded + length,
                       record - index->posting_last_records[symbol]);
  length += encode_u32(
    encoded + length,
    occurrence_offset - index->posting_last_occurrences[symbol]);
  length += encode_u32(encoded + length, occurrence_length);
  if (length > CBD_POSTING_BLOCK_PAYLOAD)
    fatal_error("compact_back_demod: oversized posting entry");
  block = index->posting_tails[symbol];
  if (block == CBD_NONE ||
      index->posting_blocks[block].used + length >
        CBD_POSTING_BLOCK_PAYLOAD) {
    uint32_t added = new_posting_block(index);
    if (block == CBD_NONE)
      index->posting_heads[symbol] = added;
    else
      index->posting_blocks[block].next = added;
    index->posting_tails[symbol] = added;
    block = added;
  }
  tail = &index->posting_blocks[block];
  memcpy(tail->data + tail->used, encoded, length);
  tail->used += (uint16_t) length;
  tail->count++;
  index->posting_last_records[symbol] = record;
  index->posting_last_occurrences[symbol] = occurrence_offset;
  index->posting_count++;
  index->posting_stream_used += length;
}

static void note_symbol(struct cbd_symbol_set *set, uint32_t symbol,
                        uint32_t offset)
{
  size_t i;
  for (i = 0; i < set->count; i++)
    if (set->values[i] == symbol)
      break;
  if (i == set->count) {
    if (set->count == set->capacity) {
      size_t next = grow_capacity(set->capacity, sizeof(*set->values),
                                  "compact_back_demod: symbol set overflow");
      if (set->values == set->fixed) {
        set->values = safe_malloc(next * sizeof(*set->values));
        memcpy(set->values, set->fixed,
               set->count * sizeof(*set->values));
      }
      else
        set->values = safe_realloc(set->values,
                                   next * sizeof(*set->values));
      set->capacity = next;
    }
    set->values[set->count++] = symbol;
  }
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
      note_symbol(symbols, (uint32_t) code,
                  offset + i - base);
      index->symbol_occurrences++;
    }
  }
}

static int increasing_local_offset(const void *left, const void *right)
{
  const struct cbd_local_occurrence *a = left;
  const struct cbd_local_occurrence *b = right;
  if (a->offset != b->offset)
    return a->offset < b->offset ? -1 : 1;
  return a->symbol < b->symbol ? -1 : a->symbol > b->symbol ? 1 : 0;
}

Compact_back_demod_index compact_back_demod_init_with_pool(
  Compact_term_pool pool)
{
  Compact_back_demod_index index = safe_calloc(1, sizeof(*index));
  if (pool == NULL)
    fatal_error("compact_back_demod_init_with_pool: null term pool");
  index->term_pool = pool;
  index->tokens = compact_term_pool_tokens(pool);
  index->token_limit = compact_term_pool_token_count(pool);
  if (new_posting_block(index) != CBD_NONE)
    fatal_error("compact_back_demod: invalid posting block sentinel");
  ENSURE_ARRAY(index, records, record_count, record_capacity,
               "compact_back_demod: record overflow");
  memset(&index->records[0], 0, sizeof(index->records[0]));
  index->record_count = 1;
  update_peak(index);
  return index;
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

BOOL compact_back_demod_add(Compact_back_demod_index index, Topform clause)
{
  uint32_t record_index;
  struct cbd_record *record;
  struct cbd_symbol_set symbols;
  Literals literal;
  uint32_t token_end = 0;
  BOOL have_tokens = FALSE;
  size_t at_hash;
  if (index == NULL || clause == NULL || clause->id == 0 ||
      clause->literals == NULL ||
      lookup_record(index, clause->id) != CBD_NONE)
    return FALSE;
  ENSURE_ARRAY(index, records, record_count, record_capacity,
               "compact_back_demod: record overflow");
  if (index->record_count > UINT32_MAX)
    fatal_error("compact_back_demod: record offsets exceed 32 bits");
  record_index = (uint32_t) index->record_count++;
  record = &index->records[record_index];
  memset(record, 0, sizeof(*record));
  record->proof_id = clause->id;
  record->active = TRUE;
  memset(&symbols, 0, sizeof(symbols));
  symbols.values = symbols.fixed;
  symbols.capacity = sizeof(symbols.fixed) / sizeof(symbols.fixed[0]);
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
  qsort(symbols.occurrence_values, symbols.occurrence_count,
        sizeof(*symbols.occurrence_values), increasing_local_offset);
  {
    size_t i;
    for (i = 0; i < symbols.count; i++) {
      size_t j;
      uint32_t previous = 0;
      uint32_t occurrence_offset = (uint32_t) index->occurrence_count;
      BOOL first = TRUE;
      for (j = 0; j < symbols.occurrence_count; j++)
        if (symbols.occurrence_values[j].symbol == symbols.values[i]) {
          uint32_t offset = symbols.occurrence_values[j].offset;
          if (!first && offset == previous)
            continue;
          append_occurrence_delta(index, first ? offset : offset - previous);
          previous = offset;
          first = FALSE;
        }
      append_symbol_record(
        index, record_index, symbols.values[i], occurrence_offset,
        (uint32_t) index->occurrence_count - occurrence_offset);
    }
  }
  if (symbols.values != symbols.fixed)
    safe_free(symbols.values);
  if (symbols.occurrence_values != symbols.occurrence_fixed)
    safe_free(symbols.occurrence_values);
  ensure_hash(index);
  at_hash = hash_slot(index, clause->id, TRUE);
  if (index->hash_keys[at_hash] == CBD_TOMBSTONE)
    index->hash_tombstones--;
  index->hash_keys[at_hash] = clause->id;
  index->hash_values[at_hash] = record_index;
  index->hash_count++;
  index->active++;
  update_peak(index);
  return TRUE;
}

BOOL compact_back_demod_remove(Compact_back_demod_index index,
                               unsigned long long proof_id)
{
  uint32_t record = lookup_record(index, proof_id);
  size_t at;
  if (record == CBD_NONE || !index->records[record].active)
    return FALSE;
  index->records[record].active = FALSE;
  at = hash_slot(index, proof_id, FALSE);
  index->hash_keys[at] = CBD_TOMBSTONE;
  index->hash_values[at] = CBD_NONE;
  index->hash_count--;
  index->hash_tombstones++;
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

static void collect_symbol(Compact_back_demod_index index, Term pattern,
                           unsigned long long exclude_id, size_t *count)
{
  uint32_t block;
  uint32_t record_index = 0;
  uint32_t occurrence_offset = 0;
  unsigned symbol;
  if (VARIABLE(pattern)) {
    size_t at;
    for (at = 1; at < index->record_count; at++)
      collect_record(index, (uint32_t) at, exclude_id, count);
    return;
  }
  symbol = (unsigned) SYMNUM(pattern);
  if ((size_t) symbol >= index->symbol_capacity)
    return;
  for (block = index->posting_heads[symbol]; block != CBD_NONE;
       block = index->posting_blocks[block].next) {
    const struct cbd_posting_block *current;
    uint16_t position = 0;
    uint16_t entries = 0;
    if (block >= index->posting_block_count)
      fatal_error("compact_back_demod: corrupt posting block");
    current = &index->posting_blocks[block];
    while (position < current->used) {
      uint32_t delta = decode_posting_value(current, &position);
      uint32_t occurrence_delta;
      uint32_t occurrence_length;
      struct cbd_record *record;
      if (delta > UINT32_MAX - record_index)
        fatal_error("compact_back_demod: posting record overflow");
      record_index += delta;
      occurrence_delta = decode_posting_value(current, &position);
      if (occurrence_delta > UINT32_MAX - occurrence_offset)
        fatal_error("compact_back_demod: posting occurrence overflow");
      occurrence_offset += occurrence_delta;
      occurrence_length = decode_posting_value(current, &position);
      if (record_index == CBD_NONE || record_index >= index->record_count)
        fatal_error("compact_back_demod: corrupt posting record");
      record = &index->records[record_index];
      if (record->active && record->proof_id != exclude_id &&
          record->query_stamp != index->query_stamp &&
          posting_contains_pattern(index, occurrence_offset,
                                   occurrence_length, record, pattern,
                                   (int32_t) symbol))
        collect_record(index, record_index, exclude_id, count);
      entries++;
    }
    if (position != current->used || entries != current->count)
      fatal_error("compact_back_demod: corrupt posting block contents");
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
  *count = 0;
  if (index == NULL || demod == NULL || demod->literals == NULL)
    return NULL;
  index->tokens = compact_term_pool_tokens(index->term_pool);
  index->token_limit = compact_term_pool_token_count(index->term_pool);
  atom = demod->literals->atom;
  alpha = ARG(atom, 0);
  beta = ARG(atom, 1);
  begin_query(index);
  if (type == ORIENTED || type == LEX_DEP_LR || type == LEX_DEP_BOTH)
    collect_symbol(index, alpha, demod->id, count);
  if (type == LEX_DEP_RL || type == LEX_DEP_BOTH)
    collect_symbol(index, beta, demod->id, count);
  if (*count > 1)
    qsort(index->results, *count, sizeof(*index->results), decreasing_id);
  answer = *count == 0 ? NULL : safe_malloc(*count * sizeof(*answer));
  if (*count != 0)
    memcpy(answer, index->results, *count * sizeof(*answer));
  index->queries++;
  index->candidates += *count;
  update_peak(index);
  return answer;
}

void compact_back_demod_note_exact_tests(Compact_back_demod_index index,
                                         size_t count)
{
  if (index != NULL)
    index->exact_tests += count;
}

void compact_back_demod_get_stats(Compact_back_demod_index index,
                                  struct compact_back_demod_stats *stats)
{
  struct compact_term_pool_stats terms;
  memset(stats, 0, sizeof(*stats));
  if (index == NULL)
    return;
  compact_term_pool_get_stats(index->term_pool, &terms);
  stats->active = index->active;
  stats->peak = index->peak;
  stats->retired = index->retired;
  stats->physical = index->record_count - 1;
  stats->queries = index->queries;
  stats->candidates = index->candidates;
  stats->exact_tests = index->exact_tests;
  stats->posting_groups = index->posting_count;
  stats->symbol_occurrences = index->symbol_occurrences;
  stats->posting_bytes =
    index->posting_block_capacity * sizeof(*index->posting_blocks) +
    index->symbol_capacity *
      (sizeof(*index->posting_heads) + sizeof(*index->posting_tails) +
       sizeof(*index->posting_last_records) +
       sizeof(*index->posting_last_occurrences)) +
    index->occurrence_capacity * sizeof(*index->occurrences);
  stats->posting_stream_used = index->posting_stream_used;
  stats->posting_stream_bytes =
    index->posting_block_capacity * sizeof(*index->posting_blocks);
  stats->occurrence_bytes =
    index->occurrence_capacity * sizeof(*index->occurrences);
  stats->occurrence_stream_bytes = index->occurrence_count;
  stats->record_bytes = index->record_capacity * sizeof(*index->records);
  stats->root_bytes = 0;
  stats->token_bytes = index->owns_term_pool ? terms.token_bytes : 0;
  stats->hash_bytes = index->hash_capacity *
    (sizeof(*index->hash_keys) + sizeof(*index->hash_values));
  stats->scratch_bytes = index->result_capacity * sizeof(*index->results);
  stats->total_bytes = index_bytes(index);
  stats->peak_bytes = index->peak_bytes;
}

void compact_back_demod_free(Compact_back_demod_index index)
{
  if (index == NULL)
    return;
  safe_free(index->posting_blocks);
  safe_free(index->posting_heads);
  safe_free(index->posting_tails);
  safe_free(index->posting_last_records);
  safe_free(index->posting_last_occurrences);
  safe_free(index->occurrences);
  safe_free(index->records);
  if (index->owns_term_pool)
    compact_term_pool_free(index->term_pool);
  safe_free(index->hash_keys);
  safe_free(index->hash_values);
  safe_free(index->results);
  safe_free(index);
}
