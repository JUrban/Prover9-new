#include "compact_unit_index.h"

#include <stdint.h>
#include <string.h>

#define CUI_NONE 0U
#define CUI_TOMBSTONE UINT64_MAX

struct cui_node {
  int32_t code;
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
  int32_t *tokens;
  size_t token_count;
  size_t token_capacity;
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
  if (index == NULL)
    return 0;
  return sizeof(*index) +
    index->node_capacity * sizeof(*index->nodes) +
    index->posting_capacity * sizeof(*index->postings) +
    index->record_capacity * sizeof(*index->records) +
    index->token_capacity * sizeof(*index->tokens) +
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
    (index)->capacity = grow_capacity((index)->capacity,                \
      sizeof(*(index)->field), (message));                              \
    (index)->field = safe_realloc((index)->field,                       \
      (index)->capacity * sizeof(*(index)->field));                     \
  }                                                                    \
} while (0)

static void ensure_tokens(Compact_unit_index index, size_t extra)
{
  size_t needed;
  if (extra > SIZE_MAX - index->token_count)
    fatal_error("compact_unit_index: token overflow");
  needed = index->token_count + extra;
  if (needed > UINT32_MAX)
    fatal_error("compact_unit_index: token offsets exceed 32 bits");
  while (needed > index->token_capacity) {
    index->token_capacity = grow_capacity(
      index->token_capacity, sizeof(*index->tokens),
      "compact_unit_index: token capacity overflow");
    index->tokens = safe_realloc(
      index->tokens, index->token_capacity * sizeof(*index->tokens));
  }
}

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
  else if ((index->hash_count + index->hash_tombstones + 1) * 10 >=
           index->hash_capacity * 7)
    rehash(index, index->hash_capacity * 2);
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

static uint32_t new_node(Compact_unit_index index, int32_t code)
{
  uint32_t node;
  ENSURE_ARRAY(index, nodes, node_count, node_capacity,
               "compact_unit_index: node overflow");
  if (index->node_count > UINT32_MAX)
    fatal_error("compact_unit_index: node offsets exceed 32 bits");
  node = (uint32_t) index->node_count++;
  memset(&index->nodes[node], 0, sizeof(index->nodes[node]));
  index->nodes[node].code = code;
  return node;
}

static uint32_t trie_child(Compact_unit_index index, uint32_t parent,
                           int32_t code)
{
  uint32_t current = index->nodes[parent].first_child;
  uint32_t previous = CUI_NONE;
  while (current != CUI_NONE &&
         code_compare(index->nodes[current].code, code) < 0) {
    previous = current;
    current = index->nodes[current].next_sibling;
  }
  if (current != CUI_NONE && index->nodes[current].code == code)
    return current;
  current = new_node(index, code);
  if (previous == CUI_NONE) {
    index->nodes[current].next_sibling = index->nodes[parent].first_child;
    index->nodes[parent].first_child = current;
  }
  else {
    index->nodes[current].next_sibling = index->nodes[previous].next_sibling;
    index->nodes[previous].next_sibling = current;
  }
  return current;
}

static uint32_t append_tokens(Compact_unit_index index, Term atom,
                              uint32_t *length, uint64_t *symbol_mask)
{
  size_t offset = index->token_count;
  size_t capacity = 128, top = 0;
  Term fixed[128];
  Term *stack = fixed;
  *symbol_mask = 0;
  stack[top++] = atom;
  while (top != 0) {
    Term current = stack[--top];
    int i;
    int32_t code = VARIABLE(current) ?
      -(int32_t) VARNUM(current) - 1 : (int32_t) SYMNUM(current);
    ensure_tokens(index, 1);
    index->tokens[index->token_count++] = code;
    if (code >= 0)
      *symbol_mask |= symbol_bit((unsigned) code);
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
  *length = (uint32_t) (index->token_count - offset);
  return (uint32_t) offset;
}

static void index_record(Compact_unit_index index, uint32_t record)
{
  struct cui_record *r = &index->records[record];
  uint32_t node = index->roots[r->sign ? 1 : 0];
  uint32_t posting;
  uint32_t i;
  for (i = 0; i < r->token_length; i++)
    node = trie_child(index, node,
                      index->tokens[r->token_offset + i]);
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

Compact_unit_index compact_unit_index_init(void)
{
  Compact_unit_index index = safe_calloc(1, sizeof(*index));
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
  record->token_offset = append_tokens(index, unit->literals->atom,
                                        &record->token_length,
                                        &record->symbol_mask);
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
    int32_t code = index->nodes[child].code;
    Term query_term = index->query[position].term;
    unsigned long long found = 0;
    if (code < 0) {
      int variable = -code - 1;
      BOOL newly_bound = FALSE;
      if (variable >= MAX_VARS)
        fatal_error("compact_unit_index: variable exceeds MAX_VARS");
      if (bindings[variable] == NULL) {
        bindings[variable] = query_term;
        newly_bound = TRUE;
      }
      if (newly_bound || term_ident(bindings[variable], query_term))
        found = generalization_rec(index, child,
                                   index->query[position].end, end,
                                   bindings, exclude_id);
      if (newly_bound)
        bindings[variable] = NULL;
    }
    else if (!VARIABLE(query_term) && SYMNUM(query_term) == code)
      found = generalization_rec(index, child, position + 1, end,
                                 bindings, exclude_id);
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
  size_t i;
  int query_root;
  if (count == NULL)
    return NULL;
  *count = 0;
  if (index == NULL || query == NULL || VARIABLE(query))
    return NULL;
  index->unifier_queries++;
  query_root = SYMNUM(query);
  for (i = 1; i < index->record_count; i++) {
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
  if (stats == NULL)
    return;
  memset(stats, 0, sizeof(*stats));
  if (index == NULL)
    return;
  stats->active = index->active;
  stats->peak = index->peak;
  stats->retired = index->retired;
  stats->physical = index->record_count - 1;
  stats->generalization_queries = index->generalization_queries;
  stats->instance_queries = index->instance_queries;
  stats->instance_exact_tests = index->instance_exact_tests;
  stats->unifier_queries = index->unifier_queries;
  stats->unifier_exact_tests = index->unifier_exact_tests;
  stats->node_bytes = index->node_capacity * sizeof(*index->nodes);
  stats->posting_bytes = index->posting_capacity * sizeof(*index->postings);
  stats->record_bytes = index->record_capacity * sizeof(*index->records);
  stats->token_bytes = index->token_capacity * sizeof(*index->tokens);
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
  safe_free(index->tokens);
  safe_free(index->hash_keys);
  safe_free(index->hash_values);
  safe_free(index->query);
  safe_free(index->result_ids);
  safe_free(index);
}
