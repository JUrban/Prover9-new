#include "compact_back_demod.h"

#include <stdint.h>
#include <stdlib.h>
#include <string.h>

#define CBD_NONE 0U
#define CBD_TOMBSTONE UINT64_MAX

struct cbd_posting {
  uint32_t record;
  uint32_t next;
  uint32_t occurrence_offset;
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
  struct cbd_posting *postings;
  size_t posting_count;
  size_t posting_capacity;
  uint32_t *posting_heads;
  uint32_t *posting_tails;
  size_t symbol_capacity;
  unsigned char *occurrences;
  size_t occurrence_count;
  size_t occurrence_capacity;
  struct cbd_record *records;
  size_t record_count;
  size_t record_capacity;
  int32_t *tokens;
  size_t token_count;
  size_t token_capacity;
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
  uint32_t token_base;
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
  if (index == NULL)
    return 0;
  return sizeof(*index) +
    index->posting_capacity * sizeof(*index->postings) +
    index->symbol_capacity *
      (sizeof(*index->posting_heads) + sizeof(*index->posting_tails)) +
    index->occurrence_capacity * sizeof(*index->occurrences) +
    index->record_capacity * sizeof(*index->records) +
    index->token_capacity * sizeof(*index->tokens) +
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
  else if ((index->hash_count + index->hash_tombstones + 1) * 10 >=
           index->hash_capacity * 7) {
    if (index->hash_capacity > SIZE_MAX / 2)
      fatal_error("compact_back_demod: hash overflow");
    rehash(index, index->hash_capacity * 2);
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
  memset(index->posting_heads + old_capacity, 0,
         (index->symbol_capacity - old_capacity) *
           sizeof(*index->posting_heads));
  memset(index->posting_tails + old_capacity, 0,
         (index->symbol_capacity - old_capacity) *
           sizeof(*index->posting_tails));
}

static void ensure_tokens(Compact_back_demod_index index, size_t extra)
{
  size_t needed;
  if (extra > SIZE_MAX - index->token_count)
    fatal_error("compact_back_demod: token overflow");
  needed = index->token_count + extra;
  if (needed > UINT32_MAX)
    fatal_error("compact_back_demod: token offsets exceed 32 bits");
  while (needed > index->token_capacity) {
    index->token_capacity = grow_capacity(
      index->token_capacity, sizeof(*index->tokens),
      "compact_back_demod: token capacity overflow");
    index->tokens = safe_realloc(
      index->tokens, index->token_capacity * sizeof(*index->tokens));
  }
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

static void append_symbol_record(Compact_back_demod_index index,
                                 uint32_t record, unsigned symbol)
{
  uint32_t posting;
  ensure_symbols(index, symbol);
  ENSURE_ARRAY(index, postings, posting_count, posting_capacity,
               "compact_back_demod: posting overflow");
  if (index->posting_count > UINT32_MAX)
    fatal_error("compact_back_demod: posting offsets exceed 32 bits");
  posting = (uint32_t) index->posting_count++;
  index->postings[posting].record = record;
  index->postings[posting].next = CBD_NONE;
  index->postings[posting].occurrence_offset =
    (uint32_t) index->occurrence_count;
  if (index->posting_heads[symbol] == CBD_NONE)
    index->posting_heads[symbol] = posting;
  else
    index->postings[index->posting_tails[symbol]].next = posting;
  index->posting_tails[symbol] = posting;
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

static void append_term(Compact_back_demod_index index, Term term,
                        struct cbd_symbol_set *symbols)
{
  size_t capacity = 128, top = 0;
  Term fixed[128];
  Term *stack = fixed;
  stack[top++] = term;
  while (top != 0) {
    Term current = stack[--top];
    int i;
    int32_t code = VARIABLE(current) ?
      -(int32_t) VARNUM(current) - 1 : (int32_t) SYMNUM(current);
    ensure_tokens(index, 1);
    index->tokens[index->token_count++] = code;
    if (code >= 0) {
      note_symbol(symbols, (uint32_t) code,
                  (uint32_t) index->token_count - 1 - symbols->token_base);
      index->symbol_occurrences++;
    }
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

Compact_back_demod_index compact_back_demod_init(void)
{
  Compact_back_demod_index index = safe_calloc(1, sizeof(*index));
  ENSURE_ARRAY(index, postings, posting_count, posting_capacity,
               "compact_back_demod: posting overflow");
  memset(&index->postings[0], 0, sizeof(index->postings[0]));
  index->posting_count = 1;
  ENSURE_ARRAY(index, records, record_count, record_capacity,
               "compact_back_demod: record overflow");
  memset(&index->records[0], 0, sizeof(index->records[0]));
  index->record_count = 1;
  update_peak(index);
  return index;
}

BOOL compact_back_demod_add(Compact_back_demod_index index, Topform clause)
{
  uint32_t record_index;
  struct cbd_record *record;
  struct cbd_symbol_set symbols;
  Literals literal;
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
  record->token_offset = (uint32_t) index->token_count;
  record->active = TRUE;
  memset(&symbols, 0, sizeof(symbols));
  symbols.values = symbols.fixed;
  symbols.capacity = sizeof(symbols.fixed) / sizeof(symbols.fixed[0]);
  symbols.occurrence_values = symbols.occurrence_fixed;
  symbols.occurrence_capacity = sizeof(symbols.occurrence_fixed) /
    sizeof(symbols.occurrence_fixed[0]);
  symbols.token_base = record->token_offset;
  for (literal = clause->literals; literal != NULL; literal = literal->next) {
    Term atom = literal->atom;
    int arg;
    for (arg = 0; arg < ARITY(atom); arg++)
      append_term(index, ARG(atom, arg), &symbols);
  }
  record->token_length = (uint32_t) index->token_count - record->token_offset;
  {
    size_t i;
    for (i = 0; i < symbols.count; i++) {
      size_t j;
      uint32_t previous = 0;
      BOOL first = TRUE;
      append_symbol_record(index, record_index, symbols.values[i]);
      for (j = 0; j < symbols.occurrence_count; j++)
        if (symbols.occurrence_values[j].symbol == symbols.values[i]) {
          uint32_t offset = symbols.occurrence_values[j].offset;
          append_occurrence_delta(index, first ? offset : offset - previous);
          previous = offset;
          first = FALSE;
        }
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
  if ((size_t) position >= index->token_count)
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
  if ((size_t) at >= index->token_count)
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
                                     uint32_t posting,
                                     struct cbd_record *record,
                                     Term pattern, int32_t symbol)
{
  uint32_t position = index->postings[posting].occurrence_offset;
  uint32_t end = posting + 1 < index->posting_count ?
    index->postings[posting + 1].occurrence_offset :
    (uint32_t) index->occurrence_count;
  uint32_t relative = 0;
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

static void collect_symbol(Compact_back_demod_index index, Term pattern,
                           unsigned long long exclude_id, size_t *count)
{
  uint32_t posting;
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
  for (posting = index->posting_heads[symbol]; posting != CBD_NONE;
       posting = index->postings[posting].next) {
    struct cbd_posting *candidate = &index->postings[posting];
    struct cbd_record *record = &index->records[candidate->record];
    if (record->active && record->proof_id != exclude_id &&
        record->query_stamp != index->query_stamp &&
        posting_contains_pattern(index, posting, record, pattern,
                                 (int32_t) symbol))
      collect_record(index, candidate->record, exclude_id, count);
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
  memset(stats, 0, sizeof(*stats));
  if (index == NULL)
    return;
  stats->active = index->active;
  stats->peak = index->peak;
  stats->retired = index->retired;
  stats->physical = index->record_count - 1;
  stats->queries = index->queries;
  stats->candidates = index->candidates;
  stats->exact_tests = index->exact_tests;
  stats->posting_groups = index->posting_count - 1;
  stats->symbol_occurrences = index->symbol_occurrences;
  stats->posting_bytes = index->posting_capacity * sizeof(*index->postings) +
    index->symbol_capacity *
      (sizeof(*index->posting_heads) + sizeof(*index->posting_tails)) +
    index->occurrence_capacity * sizeof(*index->occurrences);
  stats->occurrence_bytes =
    index->occurrence_capacity * sizeof(*index->occurrences);
  stats->occurrence_stream_bytes = index->occurrence_count;
  stats->record_bytes = index->record_capacity * sizeof(*index->records);
  stats->root_bytes = 0;
  stats->token_bytes = index->token_capacity * sizeof(*index->tokens);
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
  safe_free(index->postings);
  safe_free(index->posting_heads);
  safe_free(index->posting_tails);
  safe_free(index->occurrences);
  safe_free(index->records);
  safe_free(index->tokens);
  safe_free(index->hash_keys);
  safe_free(index->hash_values);
  safe_free(index->results);
  safe_free(index);
}
