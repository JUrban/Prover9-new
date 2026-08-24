#include "hint_term_table.h"
#include "memory.h"
#include "fatal.h"
#include <limits.h>
#include <stdint.h>
#include <string.h>

#define HINT_TERM_VARIABLE_CODE UINT32_C(0x80000000)
#define HINT_TERM_DELTA_HANDLE  UINT32_C(0x80000000)
#define HINT_TERM_HANDLE_MASK   UINT32_C(0x7fffffff)

#define HINT_TERM_RECORD_ACTIVE 1U
#define HINT_TERM_RECORD_SIGN   2U
#define HINT_TERM_RECORD_EVER   4U

struct hint_term_node {
  uint32_t code;
  uint32_t child_offset;
  uint32_t arity;
};

struct hint_term_arena {
  struct hint_term_node *nodes;
  uint32_t node_count;
  uint32_t node_capacity;
  uint32_t *children;
  uint32_t child_count;
  uint32_t child_capacity;
  uint32_t *hash_slots;
  uint32_t hash_capacity;
  uint32_t hash_count;
  uint32_t *scratch;
  uint32_t scratch_count;
  uint32_t scratch_capacity;
  unsigned long long occurrences;
  unsigned long long intern_hits;
  unsigned long long hash_peak_bytes;
  uint32_t handle_tag;
  BOOL finalized;
};

struct hint_term_table {
  struct hint_term_arena base;
  struct hint_term_arena delta;
  uint32_t *roots;
  unsigned char *record_flags;
  uint32_t record_capacity;
  unsigned long long active_records;
  unsigned long long additions;
  unsigned long long removals;
  unsigned long long reinsertions;
  unsigned long long match_attempts;
  unsigned long long match_successes;
  unsigned long long match_nodes;
  unsigned long long match_rigid_tests;
  unsigned long long match_rigid_rejects;
  unsigned long long match_first_bindings;
  unsigned long long match_repeated_tests;
  unsigned long long match_repeated_rejects;
  BOOL finalized;
};

static uint64_t hint_term_mix(uint64_t x)
{
  x ^= x >> 30;
  x *= UINT64_C(0xbf58476d1ce4e5b9);
  x ^= x >> 27;
  x *= UINT64_C(0x94d049bb133111eb);
  x ^= x >> 31;
  return x;
}

static uint64_t hint_term_key_hash(uint32_t code, uint32_t arity,
                                   const uint32_t *children)
{
  uint64_t hash = hint_term_mix(
    ((uint64_t) code << 32) ^ arity ^ UINT64_C(0x68696e747465726d));
  uint32_t i;
  for (i = 0; i < arity; i++)
    hash = hint_term_mix(hash ^ children[i] ^
      ((uint64_t) (i + 1) * UINT64_C(0x9e3779b97f4a7c15)));
  return hash;
}

static BOOL hint_term_node_equal(const struct hint_term_arena *arena,
                                 uint32_t handle, uint32_t code,
                                 uint32_t arity,
                                 const uint32_t *children)
{
  const struct hint_term_node *node;
  if (handle == 0 || handle > arena->node_count)
    return FALSE;
  node = arena->nodes + handle - 1;
  return node->code == code && node->arity == arity &&
    (arity == 0 ||
     memcmp(arena->children + node->child_offset, children,
            (size_t) arity * sizeof(uint32_t)) == 0);
}

static BOOL hint_term_reserve_nodes(struct hint_term_arena *arena,
                                    uint32_t needed)
{
  uint32_t capacity;
  if (needed <= arena->node_capacity)
    return TRUE;
  capacity = arena->node_capacity == 0 ? 1024 : arena->node_capacity;
  while (capacity < needed) {
    if (capacity > UINT32_MAX / 2)
      return FALSE;
    capacity *= 2;
  }
  arena->nodes = safe_realloc(
    arena->nodes, (size_t) capacity * sizeof(*arena->nodes));
  arena->node_capacity = capacity;
  return TRUE;
}

static BOOL hint_term_reserve_words(uint32_t **words, uint32_t *capacity,
                                    uint32_t needed)
{
  uint32_t result;
  if (needed <= *capacity)
    return TRUE;
  result = *capacity == 0 ? 1024 : *capacity;
  while (result < needed) {
    if (result > UINT32_MAX / 2)
      return FALSE;
    result *= 2;
  }
  *words = safe_realloc(*words, (size_t) result * sizeof(**words));
  *capacity = result;
  return TRUE;
}

static void hint_term_hash_insert(struct hint_term_arena *arena,
                                  uint32_t handle)
{
  const struct hint_term_node *node = arena->nodes + handle - 1;
  const uint32_t *children = node->arity == 0 ? NULL :
    arena->children + node->child_offset;
  uint64_t hash = hint_term_key_hash(node->code, node->arity, children);
  uint32_t position = (uint32_t) hash & (arena->hash_capacity - 1);
  while (arena->hash_slots[position] != 0)
    position = (position + 1) & (arena->hash_capacity - 1);
  arena->hash_slots[position] = handle;
  arena->hash_count++;
}

static BOOL hint_term_rehash(struct hint_term_arena *arena,
                             uint32_t capacity)
{
  uint32_t *old = arena->hash_slots;
  uint32_t i;
  if (capacity == 0 || (capacity & (capacity - 1)) != 0)
    return FALSE;
  arena->hash_slots = safe_calloc(capacity, sizeof(*arena->hash_slots));
  arena->hash_capacity = capacity;
  arena->hash_count = 0;
  for (i = 1; i <= arena->node_count; i++)
    hint_term_hash_insert(arena, i);
  if (old != NULL)
    safe_free(old);
  if ((unsigned long long) capacity * sizeof(*arena->hash_slots) >
      arena->hash_peak_bytes)
    arena->hash_peak_bytes =
      (unsigned long long) capacity * sizeof(*arena->hash_slots);
  return TRUE;
}

static BOOL hint_term_prepare_hash(struct hint_term_arena *arena)
{
  if (arena->hash_capacity == 0)
    return hint_term_rehash(arena, 2048);
  if ((uint64_t) (arena->hash_count + 1) * 10 >=
      (uint64_t) arena->hash_capacity * 7) {
    if (arena->hash_capacity > UINT32_MAX / 2)
      return FALSE;
    return hint_term_rehash(arena, arena->hash_capacity * 2);
  }
  return TRUE;
}

static uint32_t hint_term_intern(struct hint_term_arena *arena, Term term)
{
  uint32_t arity = VARIABLE(term) ? 0 : (uint32_t) ARITY(term);
  uint32_t code;
  uint32_t scratch_start = arena->scratch_count;
  uint64_t hash;
  uint32_t position;
  uint32_t i;
  arena->occurrences++;
  if (arena->finalized ||
      arity > UINT32_MAX - scratch_start ||
      !hint_term_reserve_words(&arena->scratch, &arena->scratch_capacity,
                               scratch_start + arity))
    return 0;
  arena->scratch_count += arity;
  for (i = 0; i < arity; i++) {
    uint32_t child = hint_term_intern(arena, ARG(term,(int) i));
    if (child == 0) {
      arena->scratch_count = scratch_start;
      return 0;
    }
    arena->scratch[scratch_start + i] = child | arena->handle_tag;
  }
  if (VARIABLE(term)) {
    unsigned variable = (unsigned) VARNUM(term);
    if (variable > HINT_TERM_HANDLE_MASK) {
      arena->scratch_count = scratch_start;
      return 0;
    }
    code = HINT_TERM_VARIABLE_CODE | (uint32_t) variable;
  }
  else {
    unsigned symbol = (unsigned) SYMNUM(term);
    if (symbol == 0 || symbol > HINT_TERM_HANDLE_MASK) {
      arena->scratch_count = scratch_start;
      return 0;
    }
    code = (uint32_t) symbol;
  }
  if (!hint_term_prepare_hash(arena)) {
    arena->scratch_count = scratch_start;
    return 0;
  }
  hash = hint_term_key_hash(
    code, arity, arena->scratch + scratch_start);
  position = (uint32_t) hash & (arena->hash_capacity - 1);
  while (arena->hash_slots[position] != 0) {
    uint32_t handle = arena->hash_slots[position];
    if (hint_term_node_equal(arena, handle, code, arity,
                             arena->scratch + scratch_start)) {
      arena->intern_hits++;
      arena->scratch_count = scratch_start;
      return handle;
    }
    position = (position + 1) & (arena->hash_capacity - 1);
  }
  if (arena->node_count == HINT_TERM_HANDLE_MASK ||
      arity > UINT32_MAX - arena->child_count ||
      !hint_term_reserve_nodes(arena, arena->node_count + 1) ||
      !hint_term_reserve_words(
        &arena->children, &arena->child_capacity,
        arena->child_count + arity)) {
    arena->scratch_count = scratch_start;
    return 0;
  }
  if (arity != 0)
    memcpy(arena->children + arena->child_count,
           arena->scratch + scratch_start,
           (size_t) arity * sizeof(uint32_t));
  arena->nodes[arena->node_count].code = code;
  arena->nodes[arena->node_count].child_offset = arena->child_count;
  arena->nodes[arena->node_count].arity = arity;
  arena->child_count += arity;
  arena->node_count++;
  arena->hash_slots[position] = arena->node_count;
  arena->hash_count++;
  arena->scratch_count = scratch_start;
  return arena->node_count;
}

static BOOL hint_term_reserve_records(Hint_term_table table, unsigned id)
{
  uint32_t capacity;
  uint32_t old;
  if (id < table->record_capacity)
    return TRUE;
  if (id == UINT_MAX)
    return FALSE;
  capacity = table->record_capacity == 0 ? 1024 : table->record_capacity;
  while (capacity <= id) {
    if (capacity > UINT32_MAX / 2)
      return FALSE;
    capacity *= 2;
  }
  old = table->record_capacity;
  table->roots = safe_realloc(
    table->roots, (size_t) capacity * sizeof(*table->roots));
  table->record_flags = safe_realloc(
    table->record_flags, (size_t) capacity * sizeof(*table->record_flags));
  memset(table->roots + old, 0,
         (size_t) (capacity - old) * sizeof(*table->roots));
  memset(table->record_flags + old, 0,
         (size_t) (capacity - old) * sizeof(*table->record_flags));
  table->record_capacity = capacity;
  return TRUE;
}

static void hint_term_arena_destroy(struct hint_term_arena *arena)
{
  if (arena->nodes != NULL) safe_free(arena->nodes);
  if (arena->children != NULL) safe_free(arena->children);
  if (arena->hash_slots != NULL) safe_free(arena->hash_slots);
  if (arena->scratch != NULL) safe_free(arena->scratch);
  memset(arena, 0, sizeof(*arena));
}

Hint_term_table hint_term_table_init(void)
{
  Hint_term_table table = safe_calloc(1, sizeof(struct hint_term_table));
  table->delta.handle_tag = HINT_TERM_DELTA_HANDLE;
  return table;
}

void hint_term_table_destroy(Hint_term_table table)
{
  if (table == NULL)
    return;
  hint_term_arena_destroy(&table->base);
  hint_term_arena_destroy(&table->delta);
  if (table->roots != NULL) safe_free(table->roots);
  if (table->record_flags != NULL) safe_free(table->record_flags);
  safe_free(table);
}

BOOL hint_term_table_add(Hint_term_table table, unsigned id,
                         BOOL positive, Term atom)
{
  struct hint_term_arena *arena;
  uint32_t local;
  uint32_t encoded;
  unsigned char old_flags;
  if (table == NULL || atom == NULL || id == 0 ||
      !hint_term_reserve_records(table, id) ||
      (table->record_flags[id] & HINT_TERM_RECORD_ACTIVE) != 0)
    return FALSE;
  old_flags = table->record_flags[id];
  arena = table->finalized ? &table->delta : &table->base;
  local = hint_term_intern(arena, atom);
  if (local == 0 || local > HINT_TERM_HANDLE_MASK)
    return FALSE;
  encoded = table->finalized ? local | HINT_TERM_DELTA_HANDLE : local;
  table->roots[id] = encoded;
  table->record_flags[id] = HINT_TERM_RECORD_ACTIVE |
    HINT_TERM_RECORD_EVER | (positive ? HINT_TERM_RECORD_SIGN : 0);
  table->active_records++;
  table->additions++;
  if (old_flags & HINT_TERM_RECORD_EVER)
    table->reinsertions++;
  return TRUE;
}

BOOL hint_term_table_remove(Hint_term_table table, unsigned id)
{
  if (table == NULL || id == 0 || id >= table->record_capacity ||
      (table->record_flags[id] & HINT_TERM_RECORD_ACTIVE) == 0)
    return FALSE;
  table->roots[id] = 0;
  table->record_flags[id] &= (unsigned char) ~HINT_TERM_RECORD_ACTIVE;
  table->active_records--;
  table->removals++;
  return TRUE;
}

void hint_term_table_finalize(Hint_term_table table)
{
  uint32_t highest;
  if (table == NULL || table->finalized)
    return;
  if (table->base.hash_slots != NULL)
    safe_free(table->base.hash_slots);
  if (table->base.scratch != NULL)
    safe_free(table->base.scratch);
  table->base.hash_slots = NULL;
  table->base.hash_capacity = table->base.hash_count = 0;
  table->base.scratch = NULL;
  table->base.scratch_capacity = table->base.scratch_count = 0;
  if (table->base.node_count != 0 &&
      table->base.node_count != table->base.node_capacity) {
    table->base.nodes = safe_realloc(
      table->base.nodes,
      (size_t) table->base.node_count * sizeof(*table->base.nodes));
    table->base.node_capacity = table->base.node_count;
  }
  if (table->base.child_count != 0 &&
      table->base.child_count != table->base.child_capacity) {
    table->base.children = safe_realloc(
      table->base.children,
      (size_t) table->base.child_count * sizeof(*table->base.children));
    table->base.child_capacity = table->base.child_count;
  }
  highest = table->record_capacity;
  while (highest != 0 && table->record_flags[highest - 1] == 0)
    highest--;
  if (highest != 0 && highest != table->record_capacity) {
    table->roots = safe_realloc(
      table->roots, (size_t) highest * sizeof(*table->roots));
    table->record_flags = safe_realloc(
      table->record_flags,
      (size_t) highest * sizeof(*table->record_flags));
    table->record_capacity = highest;
  }
  table->base.finalized = TRUE;
  table->finalized = TRUE;
}

uint32_t hint_term_table_root(Hint_term_table table, unsigned id)
{
  if (table == NULL || id == 0 || id >= table->record_capacity ||
      (table->record_flags[id] & HINT_TERM_RECORD_ACTIVE) == 0)
    return 0;
  return table->roots[id];
}

BOOL hint_term_table_positive(Hint_term_table table, unsigned id)
{
  return table != NULL && id != 0 && id < table->record_capacity &&
    (table->record_flags[id] & HINT_TERM_RECORD_ACTIVE) != 0 &&
    (table->record_flags[id] & HINT_TERM_RECORD_SIGN) != 0;
}

BOOL hint_term_table_node(Hint_term_table table, uint32_t handle,
                          struct hint_term_node_view *view)
{
  const struct hint_term_arena *arena;
  const struct hint_term_node *node;
  uint32_t local;
  if (table == NULL || view == NULL || handle == 0)
    return FALSE;
  arena = (handle & HINT_TERM_DELTA_HANDLE) != 0 ?
    &table->delta : &table->base;
  local = handle & HINT_TERM_HANDLE_MASK;
  if (local == 0 || local > arena->node_count)
    return FALSE;
  node = arena->nodes + local - 1;
  view->variable = (node->code & HINT_TERM_VARIABLE_CODE) != 0;
  view->symbol_or_variable = node->code & HINT_TERM_HANDLE_MASK;
  view->arity = node->arity;
  view->children = node->arity == 0 ? NULL :
    arena->children + node->child_offset;
  return TRUE;
}

BOOL hint_term_table_matches(Hint_term_table table, unsigned id,
                             BOOL positive, Term pattern, BOOL *matched)
{
  struct match_frame {
    Term pattern;
    uint32_t target;
  } stack[1000];
  uint32_t bindings[MAX_VARS];
  unsigned char bound[MAX_VARS];
  int top = 0;
  uint32_t root;
  if (matched == NULL || table == NULL || pattern == NULL || id == 0 ||
      id >= table->record_capacity ||
      (table->record_flags[id] & HINT_TERM_RECORD_ACTIVE) == 0)
    return FALSE;
  table->match_attempts++;
  if (((table->record_flags[id] & HINT_TERM_RECORD_SIGN) != 0) != positive) {
    *matched = FALSE;
    return TRUE;
  }
  root = table->roots[id];
  if (root == 0)
    return FALSE;
  memset(bound, 0, sizeof(bound));
  stack[top].pattern = pattern;
  stack[top++].target = root;
  while (top > 0) {
    struct match_frame frame = stack[--top];
    table->match_nodes++;
    if (VARIABLE(frame.pattern)) {
      unsigned variable = (unsigned) VARNUM(frame.pattern);
      if (variable >= MAX_VARS)
        return FALSE;
      if (!bound[variable]) {
        bound[variable] = 1;
        bindings[variable] = frame.target;
        table->match_first_bindings++;
      }
      else {
        table->match_repeated_tests++;
        if (bindings[variable] != frame.target) {
          table->match_repeated_rejects++;
          *matched = FALSE;
          return TRUE;
        }
      }
    }
    else {
      struct hint_term_node_view view;
      int i;
      table->match_rigid_tests++;
      if (!hint_term_table_node(table, frame.target, &view))
        return FALSE;
      if (view.variable || view.symbol_or_variable !=
            (unsigned) SYMNUM(frame.pattern) ||
          view.arity != (unsigned) ARITY(frame.pattern)) {
        table->match_rigid_rejects++;
        *matched = FALSE;
        return TRUE;
      }
      if (top + ARITY(frame.pattern) >
          (int) (sizeof(stack) / sizeof(stack[0])))
        return FALSE;
      for (i = ARITY(frame.pattern) - 1; i >= 0; i--) {
        stack[top].pattern = ARG(frame.pattern,i);
        stack[top++].target = view.children[i];
      }
    }
  }
  table->match_successes++;
  *matched = TRUE;
  return TRUE;
}

void hint_term_table_get_stats(Hint_term_table table,
                               struct hint_term_table_stats *stats)
{
  unsigned long long base_hash, delta_hash;
  memset(stats, 0, sizeof(*stats));
  if (table == NULL)
    return;
  stats->active_records = table->active_records;
  stats->additions = table->additions;
  stats->removals = table->removals;
  stats->reinsertions = table->reinsertions;
  stats->base_nodes = table->base.node_count;
  stats->base_children = table->base.child_count;
  stats->base_occurrences = table->base.occurrences;
  stats->base_intern_hits = table->base.intern_hits;
  stats->delta_nodes = table->delta.node_count;
  stats->delta_children = table->delta.child_count;
  stats->delta_occurrences = table->delta.occurrences;
  stats->delta_intern_hits = table->delta.intern_hits;
  stats->node_bytes =
    (unsigned long long) (table->base.node_capacity +
                          table->delta.node_capacity) *
      sizeof(struct hint_term_node);
  stats->child_bytes =
    (unsigned long long) (table->base.child_capacity +
                          table->delta.child_capacity) * sizeof(uint32_t);
  stats->record_bytes =
    (unsigned long long) table->record_capacity *
      (sizeof(uint32_t) + sizeof(unsigned char));
  base_hash = (unsigned long long) table->base.hash_capacity *
                sizeof(uint32_t);
  delta_hash = (unsigned long long) table->delta.hash_capacity *
                 sizeof(uint32_t);
  stats->hash_bytes = base_hash + delta_hash;
  stats->hash_peak_bytes = table->base.hash_peak_bytes +
                           table->delta.hash_peak_bytes;
  stats->scratch_bytes =
    (unsigned long long) (table->base.scratch_capacity +
                          table->delta.scratch_capacity) * sizeof(uint32_t);
  stats->total_bytes = stats->node_bytes + stats->child_bytes +
    stats->record_bytes + stats->hash_bytes + stats->scratch_bytes;
  stats->match_attempts = table->match_attempts;
  stats->match_successes = table->match_successes;
  stats->match_nodes = table->match_nodes;
  stats->match_rigid_tests = table->match_rigid_tests;
  stats->match_rigid_rejects = table->match_rigid_rejects;
  stats->match_first_bindings = table->match_first_bindings;
  stats->match_repeated_tests = table->match_repeated_tests;
  stats->match_repeated_rejects = table->match_repeated_rejects;
  stats->finalized = table->finalized;
}
