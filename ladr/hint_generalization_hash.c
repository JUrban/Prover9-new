#include "hint_generalization_hash.h"
#include "fatal.h"
#include "literals.h"
#include "memory.h"

#include <limits.h>
#include <stdint.h>
#include <stdlib.h>
#include <string.h>

#define GH_EXACT_BIT UINT32_C(0x80000000)
#define GH_ID_MASK   UINT32_C(0x7fffffff)
#define GH_MAX_COMPLETE_NODES 32U
#define GH_MAX_COMPLETE_LITERALS 8U
#define GH_MAX_PENDING \
  (GH_MAX_COMPLETE_NODES + 2 * GH_MAX_COMPLETE_LITERALS)
#define GH_TARGET_KIND_SHIFT 30U
#define GH_TARGET_PAYLOAD_MASK UINT32_C(0x3fffffff)
#define GH_MAX_TARGET_TOKENS (GH_MAX_COMPLETE_NODES + 1U)

enum gh_target_recipe_kind {
  GH_TARGET_EXACT = 0,
  GH_TARGET_PARTIAL = 1,
  GH_TARGET_COMPLETE = 2
};

/* Eight bytes per unique target.  Exact and one-hole targets reconstruct
   from the compressed active hint; exhaustive targets refer to a short
   serialized preorder stream because no single hole recipe describes them. */
struct gh_target_recipe {
  uint32_t hint_id;
  uint32_t descriptor;
};

struct gh_complete_target {
  uint32_t token_offset;
  uint16_t token_count;
  uint16_t maximum_depth;
};

struct gh_key {
  uint64_t primary;
  uint32_t check;
};

struct gh_slot {
  uint64_t primary;
  uint32_t check;
  uint32_t value;
};

struct gh_hash_state {
  uint64_t first;
  uint64_t second;
  int variables[MAX_VARS + 1];
  unsigned next_variable;
};

enum gh_pending_type {
  GH_PENDING_SIGN,
  GH_PENDING_FORCE_RIGID,
  GH_PENDING_TERM
};

struct gh_pending {
  Term term;
  unsigned char type;
  unsigned char sign;
  unsigned char depth;
};

struct gh_generation_state {
  struct gh_hash_state hash;
  Term targets[GH_MAX_COMPLETE_NODES];
  unsigned target_count;
  int target_tokens[GH_MAX_TARGET_TOKENS];
  unsigned target_token_count;
  unsigned target_maximum_depth;
  BOOL capture_unit_target;
};

struct gh_virtual_variable {
  int multiplier;
  unsigned variable;
};

struct gh_virtual_hash_state {
  struct gh_hash_state hash;
  struct gh_virtual_variable variables[2 * MAX_VARS + 2];
  unsigned variable_count;
  unsigned term_nodes;
};

struct gh_partial_candidate {
  Term term;
  unsigned depth;
  unsigned ordinal;
};

struct hint_generalization_hash {
  struct gh_slot *slots;
  size_t capacity;
  size_t count;
  unsigned complete_nodes;
  unsigned partial_per_hint;
  unsigned long long maximum_entries;
  uint32_t *hint_nodes;
  unsigned hint_capacity;
  struct gh_partial_candidate *partial;
  unsigned partial_capacity;
  unsigned partial_count;
  unsigned partial_ordinal;
  unsigned long long exact_hints;
  unsigned long long exact_new_entries;
  unsigned long long complete_hints;
  unsigned long long partial_hints;
  unsigned long long generated_attempts;
  unsigned long long generated_new_entries;
  unsigned long long duplicates;
  unsigned long long partial_cap_skips;
  unsigned long long rehashes;
  unsigned long long queries;
  unsigned long long hits;
  unsigned long long probes;
  unsigned long long maximum_probe;
  struct gh_target_recipe *target_recipes;
  unsigned target_recipe_count;
  unsigned target_recipe_capacity;
  struct gh_complete_target *complete_targets;
  unsigned complete_target_count;
  unsigned complete_target_capacity;
  int *complete_target_tokens;
  unsigned complete_target_token_count;
  unsigned complete_target_token_capacity;
  unsigned long long target_position_records;
  unsigned long long target_variable_positions;
  unsigned long long target_rigid_positions;
  unsigned long long target_exact_recipes;
  unsigned long long target_partial_recipes;
  unsigned long long target_complete_recipes;
  unsigned target_max_nodes;
  unsigned target_max_depth;
  unsigned long long target_budget_bytes;
  BOOL target_recipes_enabled;
  BOOL finalized;
};

static unsigned long long gh_target_allocated_bytes(
  Hint_generalization_hash table)
{
  return (unsigned long long) table->target_recipe_capacity *
           sizeof(*table->target_recipes) +
         (unsigned long long) table->complete_target_capacity *
           sizeof(*table->complete_targets) +
         (unsigned long long) table->complete_target_token_capacity *
           sizeof(*table->complete_target_tokens);
}

static void gh_target_check_budget(Hint_generalization_hash table,
                                   unsigned long long bytes)
{
  if (table->target_budget_bytes != 0 && bytes > table->target_budget_bytes)
    fatal_error("hash target recipe sidecar exceeds hash_target_index_kb");
}

static void gh_target_recipe_reserve(Hint_generalization_hash table,
                                     unsigned needed)
{
  unsigned capacity;
  unsigned long long bytes;
  if (needed <= table->target_recipe_capacity)
    return;
  capacity = table->target_recipe_capacity == 0 ? 1024 :
             table->target_recipe_capacity;
  while (capacity < needed) {
    unsigned next = capacity + (capacity + 1) / 2;
    if (next <= capacity)
      fatal_error("hash target recipe capacity overflow");
    capacity = next;
  }
  bytes = (unsigned long long) capacity * sizeof(*table->target_recipes) +
          (unsigned long long) table->complete_target_capacity *
            sizeof(*table->complete_targets) +
          (unsigned long long) table->complete_target_token_capacity *
            sizeof(*table->complete_target_tokens);
  gh_target_check_budget(table, bytes);
  table->target_recipes = safe_realloc(
    table->target_recipes,
    (size_t) capacity * sizeof(*table->target_recipes));
  table->target_recipe_capacity = capacity;
}

static void gh_complete_target_reserve(Hint_generalization_hash table,
                                       unsigned records,
                                       unsigned tokens)
{
  unsigned record_capacity = table->complete_target_capacity;
  unsigned token_capacity = table->complete_target_token_capacity;
  unsigned long long bytes;
  if (records > record_capacity) {
    record_capacity = record_capacity == 0 ? 64 : record_capacity;
    while (record_capacity < records) {
      unsigned next = record_capacity + (record_capacity + 1) / 2;
      if (next <= record_capacity)
        fatal_error("complete hash target capacity overflow");
      record_capacity = next;
    }
  }
  if (tokens > token_capacity) {
    token_capacity = token_capacity == 0 ? 1024 : token_capacity;
    while (token_capacity < tokens) {
      unsigned next = token_capacity + (token_capacity + 1) / 2;
      if (next <= token_capacity)
        fatal_error("complete hash target token overflow");
      token_capacity = next;
    }
  }
  bytes = (unsigned long long) table->target_recipe_capacity *
            sizeof(*table->target_recipes) +
          (unsigned long long) record_capacity *
            sizeof(*table->complete_targets) +
          (unsigned long long) token_capacity *
            sizeof(*table->complete_target_tokens);
  gh_target_check_budget(table, bytes);
  if (record_capacity != table->complete_target_capacity) {
    table->complete_targets = safe_realloc(
      table->complete_targets,
      (size_t) record_capacity * sizeof(*table->complete_targets));
    table->complete_target_capacity = record_capacity;
  }
  if (token_capacity != table->complete_target_token_capacity) {
    table->complete_target_tokens = safe_realloc(
      table->complete_target_tokens,
      (size_t) token_capacity * sizeof(*table->complete_target_tokens));
    table->complete_target_token_capacity = token_capacity;
  }
}

static BOOL gh_positive_unit_equality(Topform hint)
{
  return hint != NULL && hint->literals != NULL &&
         hint->literals->next == NULL && pos_eq(hint->literals) &&
         ARITY(hint->literals->atom) == 2;
}

static void gh_target_term_census(Term term, Term abstract, unsigned depth,
                                  unsigned *nodes, unsigned *variables,
                                  unsigned *maximum_depth)
{
  int i;
  (*nodes)++;
  if (depth > *maximum_depth)
    *maximum_depth = depth;
  if (term == abstract || VARIABLE(term)) {
    (*variables)++;
    return;
  }
  for (i = 0; i < ARITY(term); i++)
    gh_target_term_census(ARG(term, i), abstract, depth + 1,
                          nodes, variables, maximum_depth);
}

static void gh_record_target_census(Hint_generalization_hash table,
                                    Topform hint, Term abstract)
{
  unsigned nodes = 0, variables = 0, maximum_depth = 0;
  Term atom;
  int side;
  if (!table->target_recipes_enabled || !gh_positive_unit_equality(hint))
    return;
  atom = hint->literals->atom;
  for (side = 0; side < 2; side++)
    gh_target_term_census(ARG(atom, side), abstract, 1,
                          &nodes, &variables, &maximum_depth);
  table->target_position_records += nodes;
  table->target_variable_positions += variables;
  table->target_rigid_positions += nodes - variables;
  if (nodes + 1 > table->target_max_nodes)
    table->target_max_nodes = nodes + 1;
  if (maximum_depth > table->target_max_depth)
    table->target_max_depth = maximum_depth;
}

static void gh_append_target_recipe(Hint_generalization_hash table,
                                    unsigned hint_id,
                                    enum gh_target_recipe_kind kind,
                                    unsigned payload)
{
  struct gh_target_recipe *recipe;
  if (!table->target_recipes_enabled)
    return;
  if (payload > GH_TARGET_PAYLOAD_MASK)
    fatal_error("hash target recipe payload overflow");
  gh_target_recipe_reserve(table, table->target_recipe_count + 1);
  recipe = table->target_recipes + table->target_recipe_count++;
  recipe->hint_id = hint_id;
  recipe->descriptor = ((uint32_t) kind << GH_TARGET_KIND_SHIFT) | payload;
  if (kind == GH_TARGET_EXACT)
    table->target_exact_recipes++;
  else if (kind == GH_TARGET_PARTIAL)
    table->target_partial_recipes++;
  else
    table->target_complete_recipes++;
}

static uint64_t gh_mix(uint64_t x)
{
  x ^= x >> 30;
  x *= UINT64_C(0xbf58476d1ce4e5b9);
  x ^= x >> 27;
  x *= UINT64_C(0x94d049bb133111eb);
  x ^= x >> 31;
  return x;
}

static void gh_emit(struct gh_hash_state *state, uint64_t token)
{
  state->first = gh_mix(state->first ^ token ^
    UINT64_C(0x9e3779b97f4a7c15));
  state->second = gh_mix(state->second + token +
    UINT64_C(0x6a09e667f3bcc909));
}

static struct gh_hash_state gh_hash_begin(unsigned literals)
{
  struct gh_hash_state state;
  unsigned i;
  state.first = UINT64_C(0x67656e6861736831);
  state.second = UINT64_C(0x67656e6861736832);
  for (i = 0; i <= MAX_VARS; i++)
    state.variables[i] = -1;
  state.next_variable = 0;
  gh_emit(&state, UINT64_C(0x100000000) + literals);
  return state;
}

static void gh_hash_variable(struct gh_hash_state *state, unsigned variable)
{
  if (variable > MAX_VARS)
    fatal_error("generalized hint hash variable overflow");
  if (state->variables[variable] < 0)
    state->variables[variable] = (int) state->next_variable++;
  gh_emit(state, UINT64_C(0x200000000) +
                 (unsigned) state->variables[variable]);
}

static void gh_hash_rigid(struct gh_hash_state *state, Term term)
{
  gh_emit(state, UINT64_C(0x300000000) + (unsigned) SYMNUM(term));
  gh_emit(state, UINT64_C(0x400000000) + (unsigned) ARITY(term));
}

static void gh_virtual_variable(struct gh_virtual_hash_state *state,
                                Term term, Context context)
{
  int multiplier = context == NULL ? -1 : context->multiplier;
  unsigned variable = (unsigned) VARNUM(term);
  unsigned i;
  for (i = 0; i < state->variable_count; i++)
    if (state->variables[i].multiplier == multiplier &&
        state->variables[i].variable == variable) {
      gh_emit(&state->hash, UINT64_C(0x200000000) + i);
      return;
    }
  if (state->variable_count >= 2 * MAX_VARS + 2)
    fatal_error("virtual generalized-hash variable overflow");
  state->variables[state->variable_count].multiplier = multiplier;
  state->variables[state->variable_count].variable = variable;
  gh_emit(&state->hash,
          UINT64_C(0x200000000) + state->variable_count);
  state->variable_count++;
}

static void gh_hash_applied_term(struct gh_virtual_hash_state *state,
                                 Term term, Context context)
{
  int i;
  DEREFERENCE(term, context);
  state->term_nodes++;
  if (VARIABLE(term)) {
    gh_virtual_variable(state, term, context);
    return;
  }
  gh_hash_rigid(&state->hash, term);
  for (i = 0; i < ARITY(term); i++)
    gh_hash_applied_term(state, ARG(term, i), context);
}

/* Hash the apply_substitute2() result without constructing its spine. */
static void gh_hash_substituted_term(struct gh_virtual_hash_state *state,
                                     Term term, Context into_context,
                                     Ilist path,
                                     Term beta, Context from_context)
{
  int selected, i;
  if (path == NULL) {
    gh_hash_applied_term(state, beta, from_context);
    return;
  }
  if (VARIABLE(term)) {
    gh_hash_applied_term(state, term, into_context);
    return;
  }
  selected = path->i - 1;
  if (selected < 0 || selected >= ARITY(term))
    fatal_error("invalid virtual paramodulation position");
  state->term_nodes++;
  gh_hash_rigid(&state->hash, term);
  for (i = 0; i < ARITY(term); i++) {
    if (i == selected)
      gh_hash_substituted_term(state, ARG(term, i), into_context,
                               path->next, beta, from_context);
    else
      gh_hash_applied_term(state, ARG(term, i), into_context);
  }
}

static struct gh_key gh_finish_key(const struct gh_hash_state *state)
{
  struct gh_key key;
  uint64_t second = gh_mix(state->second ^ (state->first >> 1));
  key.primary = gh_mix(state->first ^ (state->second << 1));
  if (key.primary == 0)
    key.primary = 1;
  key.check = (uint32_t) (second ^ (second >> 32));
  return key;
}

static int gh_key_compare(struct gh_key first, struct gh_key second)
{
  if (first.primary != second.primary)
    return first.primary < second.primary ? -1 : 1;
  if (first.check != second.check)
    return first.check < second.check ? -1 : 1;
  return 0;
}

static void gh_hash_actual_term(struct gh_hash_state *state, Term term,
                                Term abstract)
{
  int i;
  if (term == abstract) {
    gh_hash_variable(state, MAX_VARS);
    return;
  }
  if (VARIABLE(term)) {
    gh_hash_variable(state, (unsigned) VARNUM(term));
    return;
  }
  gh_hash_rigid(state, term);
  for (i = 0; i < ARITY(term); i++)
    gh_hash_actual_term(state, ARG(term, i), abstract);
}

static struct gh_key gh_hash_literal_order(Literals *literals,
                                           const unsigned *order,
                                           unsigned count, Term abstract)
{
  struct gh_hash_state state = gh_hash_begin(count);
  unsigned i;
  for (i = 0; i < count; i++) {
    Literals literal = literals[order[i]];
    gh_emit(&state, literal->sign ? UINT64_C(0x500000001) :
                                   UINT64_C(0x500000000));
    gh_hash_actual_term(&state, literal->atom, abstract);
  }
  return gh_finish_key(&state);
}

struct gh_canonical_context {
  Literals *literals;
  unsigned count;
  unsigned order[GH_MAX_COMPLETE_LITERALS];
  BOOL used[GH_MAX_COMPLETE_LITERALS];
  Term abstract;
  struct gh_key best;
  BOOL have_best;
};

static void gh_canonical_recurse(struct gh_canonical_context *context,
                                 unsigned depth)
{
  unsigned i;
  if (depth == context->count) {
    struct gh_key key = gh_hash_literal_order(
      context->literals, context->order, context->count, context->abstract);
    if (!context->have_best || gh_key_compare(key, context->best) < 0) {
      context->best = key;
      context->have_best = TRUE;
    }
    return;
  }
  for (i = 0; i < context->count; i++) {
    if (!context->used[i]) {
      context->used[i] = TRUE;
      context->order[depth] = i;
      gh_canonical_recurse(context, depth + 1);
      context->used[i] = FALSE;
    }
  }
}

static struct gh_key gh_clause_key(Topform clause, Term abstract)
{
  Literals local[GH_MAX_COMPLETE_LITERALS], literal;
  unsigned order[GH_MAX_COMPLETE_LITERALS];
  unsigned count = 0, i;
  struct gh_canonical_context context;
  for (literal = clause->literals; literal != NULL; literal = literal->next) {
    if (count == GH_MAX_COMPLETE_LITERALS) {
      struct gh_hash_state state;
      /* Large nonunit clauses retain their established Prover9 order in
         this deliberately incomplete experiment. */
      state = gh_hash_begin(number_of_literals(clause->literals));
      for (literal = clause->literals; literal != NULL;
           literal = literal->next) {
        gh_emit(&state, literal->sign ? UINT64_C(0x500000001) :
                                       UINT64_C(0x500000000));
        gh_hash_actual_term(&state, literal->atom, abstract);
      }
      return gh_finish_key(&state);
    }
    local[count++] = literal;
  }
  if (count == 0) {
    struct gh_hash_state state = gh_hash_begin(0);
    return gh_finish_key(&state);
  }
  if (count == 1) {
    order[0] = 0;
    return gh_hash_literal_order(local, order, 1, abstract);
  }
  memset(&context, 0, sizeof(context));
  context.literals = local;
  context.count = count;
  context.abstract = abstract;
  for (i = 0; i < count; i++)
    context.used[i] = FALSE;
  gh_canonical_recurse(&context, 0);
  return context.best;
}

static size_t gh_initial_capacity(unsigned expected_hints)
{
  size_t capacity = 1024;
  while ((uint64_t) capacity * 3 < (uint64_t) expected_hints * 4) {
    if (capacity > SIZE_MAX / 2)
      fatal_error("generalized hint hash initial capacity overflow");
    capacity *= 2;
  }
  return capacity;
}

static size_t gh_slot_position(uint64_t primary, uint32_t check,
                               size_t capacity)
{
  return (size_t) gh_mix(primary ^ check) & (capacity - 1);
}

static size_t gh_slot_step(uint64_t primary, uint32_t check,
                           size_t capacity)
{
  return ((size_t) gh_mix(primary +
           ((uint64_t) check << 32) + UINT64_C(0x517cc1b727220a95)) | 1) &
         (capacity - 1);
}

static void gh_insert_existing(struct gh_slot *slots, size_t capacity,
                               struct gh_slot entry)
{
  size_t position = gh_slot_position(entry.primary, entry.check, capacity);
  size_t step = gh_slot_step(entry.primary, entry.check, capacity);
  while (slots[position].primary != 0)
    position = (position + step) & (capacity - 1);
  slots[position] = entry;
}

static void gh_rehash(Hint_generalization_hash table, size_t capacity)
{
  struct gh_slot *old = table->slots;
  size_t old_capacity = table->capacity;
  size_t i;
  table->slots = safe_calloc(capacity, sizeof(*table->slots));
  table->capacity = capacity;
  for (i = 0; i < old_capacity; i++)
    if (old[i].primary != 0)
      gh_insert_existing(table->slots, capacity, old[i]);
  if (old != NULL)
    safe_free(old);
  table->rehashes++;
}

/* 1=new entry, 0=duplicate/update, -1=generalization cap. */
static int gh_insert(Hint_generalization_hash table, struct gh_key key,
                     unsigned id, BOOL exact)
{
  size_t position;
  size_t step;
  uint32_t value;
  if (id == 0 || id > GH_ID_MASK)
    fatal_error("generalized hint hash stable ID overflow");
  if (!exact)
    table->generated_attempts++;
  position = gh_slot_position(key.primary, key.check, table->capacity);
  step = gh_slot_step(key.primary, key.check, table->capacity);
  value = id | (exact ? GH_EXACT_BIT : 0);
  while (table->slots[position].primary != 0) {
    struct gh_slot *slot = table->slots + position;
    if (slot->primary == key.primary && slot->check == key.check) {
      BOOL old_exact = (slot->value & GH_EXACT_BIT) != 0;
      unsigned old_id = slot->value & GH_ID_MASK;
      if ((exact && !old_exact) || (exact == old_exact && id < old_id))
        slot->value = value;
      table->duplicates++;
      return 0;
    }
    position = (position + step) & (table->capacity - 1);
  }
  if (!exact && table->count >= table->maximum_entries)
    return -1;
  if ((uint64_t) (table->count + 1) * 5 >=
      (uint64_t) table->capacity * 4) {
    if (table->capacity > SIZE_MAX / 2)
      fatal_error("generalized hint hash capacity overflow");
    gh_rehash(table, table->capacity * 2);
    position = gh_slot_position(key.primary, key.check, table->capacity);
    step = gh_slot_step(key.primary, key.check, table->capacity);
    while (table->slots[position].primary != 0)
      position = (position + step) & (table->capacity - 1);
  }
  table->slots[position].primary = key.primary;
  table->slots[position].check = key.check;
  table->slots[position].value = value;
  table->count++;
  if (!exact)
    table->generated_new_entries++;
  return 1;
}

static void gh_reserve_hint(Hint_generalization_hash table, unsigned id)
{
  unsigned capacity;
  if (id < table->hint_capacity)
    return;
  capacity = table->hint_capacity == 0 ? 1024 : table->hint_capacity;
  while (capacity <= id) {
    if (capacity > UINT_MAX / 2)
      fatal_error("generalized hint hash length table overflow");
    capacity *= 2;
  }
  table->hint_nodes = safe_realloc(
    table->hint_nodes, (size_t) capacity * sizeof(*table->hint_nodes));
  memset(table->hint_nodes + table->hint_capacity, 0,
         (size_t) (capacity - table->hint_capacity) *
           sizeof(*table->hint_nodes));
  table->hint_capacity = capacity;
}

Hint_generalization_hash hint_generalization_hash_init(
  unsigned complete_nodes, unsigned partial_per_hint,
  unsigned long long maximum_entries, unsigned expected_hints)
{
  Hint_generalization_hash table;
  if (complete_nodes == 0 || complete_nodes > GH_MAX_COMPLETE_NODES)
    fatal_error("hint_hash_complete_nodes must be in 1..32");
  if (maximum_entries == 0)
    fatal_error("hint_hash_max_entries must be positive");
  table = safe_calloc(1, sizeof(*table));
  table->complete_nodes = complete_nodes;
  table->partial_per_hint = partial_per_hint;
  table->maximum_entries = maximum_entries;
  gh_rehash(table, gh_initial_capacity(expected_hints));
  return table;
}

void hint_generalization_hash_enable_target_recipes(
  Hint_generalization_hash table, unsigned long long budget_bytes)
{
  if (table == NULL || table->finalized || table->count != 0)
    fatal_error("target recipes must be enabled before adding hints");
  table->target_budget_bytes = budget_bytes;
  table->target_recipes_enabled = budget_bytes != 0;
}

void hint_generalization_hash_destroy(Hint_generalization_hash table)
{
  if (table == NULL)
    return;
  if (table->slots != NULL)
    safe_free(table->slots);
  if (table->hint_nodes != NULL)
    safe_free(table->hint_nodes);
  if (table->partial != NULL)
    safe_free(table->partial);
  if (table->target_recipes != NULL)
    safe_free(table->target_recipes);
  if (table->complete_targets != NULL)
    safe_free(table->complete_targets);
  if (table->complete_target_tokens != NULL)
    safe_free(table->complete_target_tokens);
  safe_free(table);
}

BOOL hint_generalization_hash_add_exact(Hint_generalization_hash table,
                                        unsigned id, Topform hint)
{
  int inserted;
  if (table == NULL || table->finalized || hint == NULL)
    return FALSE;
  gh_reserve_hint(table, id);
  table->hint_nodes[id] = (uint32_t) clause_symbol_count(hint->literals);
  inserted = gh_insert(table, gh_clause_key(hint, NULL), id, TRUE);
  table->exact_hints++;
  if (inserted > 0)
    table->exact_new_entries++;
  if (inserted > 0 && table->target_recipes_enabled &&
      gh_positive_unit_equality(hint)) {
    gh_append_target_recipe(table, id, GH_TARGET_EXACT, 0);
    gh_record_target_census(table, hint, NULL);
  }
  if (table->count > table->maximum_entries)
    fatal_error("exact hints exceed hint_hash_max_entries");
  return TRUE;
}

BOOL hint_generalization_hash_is_complete_hint(
  Hint_generalization_hash table, unsigned id)
{
  return table != NULL && id < table->hint_capacity &&
         table->hint_nodes[id] != 0 &&
         table->hint_nodes[id] <= table->complete_nodes;
}

static void gh_capture_target_token(struct gh_generation_state *state,
                                    int token, unsigned depth)
{
  if (!state->capture_unit_target)
    return;
  if (state->target_token_count >= GH_MAX_TARGET_TOKENS)
    fatal_error("complete hash target token bound exceeded");
  state->target_tokens[state->target_token_count++] = token;
  if (depth > state->target_maximum_depth)
    state->target_maximum_depth = depth;
}

static void gh_record_complete_target(Hint_generalization_hash table,
                                      unsigned id,
                                      const struct gh_generation_state *state)
{
  struct gh_complete_target *target;
  unsigned index;
  unsigned variables = 0, i;
  if (!table->target_recipes_enabled || !state->capture_unit_target)
    return;
  if (state->target_token_count == 0 || state->target_token_count > UINT16_MAX)
    fatal_error("invalid complete hash target token stream");
  if (table->complete_target_count > GH_TARGET_PAYLOAD_MASK)
    fatal_error("complete hash target recipe overflow");
  gh_complete_target_reserve(
    table, table->complete_target_count + 1,
    table->complete_target_token_count + state->target_token_count);
  index = table->complete_target_count++;
  target = table->complete_targets + index;
  target->token_offset = table->complete_target_token_count;
  target->token_count = (uint16_t) state->target_token_count;
  target->maximum_depth = (uint16_t) state->target_maximum_depth;
  memcpy(table->complete_target_tokens + table->complete_target_token_count,
         state->target_tokens,
         state->target_token_count * sizeof(*state->target_tokens));
  table->complete_target_token_count += state->target_token_count;
  for (i = 1; i < state->target_token_count; i++)
    if (state->target_tokens[i] < 0)
      variables++;
  table->target_position_records += state->target_token_count - 1;
  table->target_variable_positions += variables;
  table->target_rigid_positions +=
    state->target_token_count - 1 - variables;
  if (state->target_token_count > table->target_max_nodes)
    table->target_max_nodes = state->target_token_count;
  if (state->target_maximum_depth > table->target_max_depth)
    table->target_max_depth = state->target_maximum_depth;
  gh_append_target_recipe(table, id, GH_TARGET_COMPLETE, index);
}

static void gh_pending_recurse(Hint_generalization_hash table, unsigned id,
                               const struct gh_pending *pending,
                               unsigned pending_count,
                               struct gh_generation_state state,
                               BOOL *complete)
{
  struct gh_pending next[GH_MAX_PENDING];
  struct gh_pending item;
  unsigned rest, i;
  if (!*complete)
    return;
  if (pending_count == 0) {
    int inserted = gh_insert(table, gh_finish_key(&state.hash), id, FALSE);
    if (inserted < 0)
      *complete = FALSE;
    else if (inserted > 0)
      gh_record_complete_target(table, id, &state);
    return;
  }
  item = pending[0];
  rest = pending_count - 1;
  if (item.type == GH_PENDING_SIGN) {
    gh_emit(&state.hash, item.sign ? UINT64_C(0x500000001) :
                                    UINT64_C(0x500000000));
    gh_pending_recurse(table, id, pending + 1, rest, state, complete);
    return;
  }
  if (item.type == GH_PENDING_TERM) {
    for (i = 0; i < state.target_count; i++) {
      if (term_ident(state.targets[i], item.term)) {
        struct gh_generation_state reused = state;
        gh_emit(&reused.hash, UINT64_C(0x200000000) + i);
        gh_capture_target_token(&reused, -(int) i - 1, item.depth);
        gh_pending_recurse(
          table, id, pending + 1, rest, reused, complete);
      }
    }
    if (state.target_count >= GH_MAX_COMPLETE_NODES) {
      *complete = FALSE;
      return;
    }
    {
      struct gh_generation_state fresh = state;
      fresh.targets[fresh.target_count] = item.term;
      gh_emit(&fresh.hash,
              UINT64_C(0x200000000) + fresh.target_count);
      gh_capture_target_token(
        &fresh, -(int) fresh.target_count - 1, item.depth);
      fresh.target_count++;
      gh_pending_recurse(
        table, id, pending + 1, rest, fresh, complete);
    }
  }
  if (!VARIABLE(item.term)) {
    struct gh_generation_state rigid = state;
    unsigned arity = ARITY(item.term);
    if (arity + rest > GH_MAX_PENDING) {
      *complete = FALSE;
      return;
    }
    gh_hash_rigid(&rigid.hash, item.term);
    gh_capture_target_token(&rigid, SYMNUM(item.term), item.depth);
    for (i = 0; i < arity; i++) {
      next[i].term = ARG(item.term, i);
      next[i].type = GH_PENDING_TERM;
      next[i].sign = 0;
      next[i].depth = item.depth + 1;
    }
    if (rest != 0)
      memcpy(next + arity, pending + 1, rest * sizeof(*next));
    gh_pending_recurse(
      table, id, next, arity + rest, rigid, complete);
  }
  else if (item.type == GH_PENDING_FORCE_RIGID)
    *complete = FALSE;
}

static void gh_generate_selected(Hint_generalization_hash table, unsigned id,
                                 Literals *literals,
                                 const unsigned *selected, unsigned count,
                                 BOOL *complete)
{
  struct gh_pending pending[GH_MAX_PENDING];
  struct gh_generation_state state;
  unsigned i;
  memset(&state, 0, sizeof(state));
  state.hash = gh_hash_begin(count);
  state.capture_unit_target = count == 1 &&
    literals[selected[0]]->sign && eq_term(literals[selected[0]]->atom) &&
    ARITY(literals[selected[0]]->atom) == 2;
  for (i = 0; i < count; i++) {
    pending[2*i].term = NULL;
    pending[2*i].type = GH_PENDING_SIGN;
    pending[2*i].sign = literals[selected[i]]->sign;
    pending[2*i].depth = 0;
    pending[2*i+1].term = literals[selected[i]]->atom;
    pending[2*i+1].type = GH_PENDING_FORCE_RIGID;
    pending[2*i+1].sign = 0;
    pending[2*i+1].depth = 0;
  }
  gh_pending_recurse(table, id, pending, 2 * count, state, complete);
}

static void gh_select_literals(Hint_generalization_hash table, unsigned id,
                               Literals *literals, unsigned literal_count,
                               unsigned wanted, unsigned depth,
                               unsigned *selected, BOOL *used,
                               BOOL *complete)
{
  unsigned i;
  if (!*complete)
    return;
  if (depth == wanted) {
    gh_generate_selected(table, id, literals, selected, wanted, complete);
    return;
  }
  for (i = 0; i < literal_count; i++) {
    if (!used[i]) {
      used[i] = TRUE;
      selected[depth] = i;
      gh_select_literals(table, id, literals, literal_count, wanted,
                         depth + 1, selected, used, complete);
      used[i] = FALSE;
    }
  }
}

BOOL hint_generalization_hash_add_complete(Hint_generalization_hash table,
                                           unsigned id, Topform hint)
{
  Literals literals[GH_MAX_COMPLETE_LITERALS], literal;
  unsigned selected[GH_MAX_COMPLETE_LITERALS];
  BOOL used[GH_MAX_COMPLETE_LITERALS] = {FALSE};
  unsigned literal_count = 0, wanted;
  BOOL complete = TRUE;
  if (table == NULL || table->finalized || hint == NULL ||
      !hint_generalization_hash_is_complete_hint(table, id))
    return FALSE;
  for (literal = hint->literals; literal != NULL; literal = literal->next) {
    if (literal_count == GH_MAX_COMPLETE_LITERALS)
      return FALSE;
    literals[literal_count++] = literal;
  }
  if (literal_count == 0) {
    table->complete_hints++;
    return TRUE;
  }
  for (wanted = 1; wanted <= literal_count && complete; wanted++)
    gh_select_literals(table, id, literals, literal_count, wanted, 0,
                       selected, used, &complete);
  if (!complete)
    return FALSE;
  table->complete_hints++;
  return TRUE;
}

static void gh_partial_reserve(Hint_generalization_hash table,
                               unsigned needed)
{
  unsigned capacity;
  if (needed <= table->partial_capacity)
    return;
  capacity = table->partial_capacity == 0 ? 64 : table->partial_capacity;
  while (capacity < needed) {
    if (capacity > UINT_MAX / 2)
      fatal_error("generalized hint partial scratch overflow");
    capacity *= 2;
  }
  table->partial = safe_realloc(
    table->partial, (size_t) capacity * sizeof(*table->partial));
  table->partial_capacity = capacity;
}

static void gh_collect_partial_term(Hint_generalization_hash table,
                                    Term term, unsigned depth)
{
  int i;
  gh_partial_reserve(table, table->partial_count + 1);
  table->partial[table->partial_count].term = term;
  table->partial[table->partial_count].depth = depth;
  table->partial[table->partial_count].ordinal = table->partial_ordinal++;
  table->partial_count++;
  if (!VARIABLE(term))
    for (i = 0; i < ARITY(term); i++)
      gh_collect_partial_term(table, ARG(term, i), depth + 1);
}

static int gh_partial_compare(const void *vfirst, const void *vsecond)
{
  const struct gh_partial_candidate *first = vfirst;
  const struct gh_partial_candidate *second = vsecond;
  if (first->depth != second->depth)
    return first->depth < second->depth ? -1 : 1;
  if (first->ordinal != second->ordinal)
    return first->ordinal < second->ordinal ? -1 : 1;
  return 0;
}

BOOL hint_generalization_hash_add_partial(Hint_generalization_hash table,
                                          unsigned id, Topform hint)
{
  Literals literal;
  unsigned i, limit;
  if (table == NULL || table->finalized || hint == NULL ||
      table->partial_per_hint == 0)
    return TRUE;
  table->partial_count = 0;
  table->partial_ordinal = 0;
  for (literal = hint->literals; literal != NULL; literal = literal->next) {
    Term atom = literal->atom;
    int child;
    if (!VARIABLE(atom))
      for (child = 0; child < ARITY(atom); child++)
        gh_collect_partial_term(table, ARG(atom, child), 1);
  }
  qsort(table->partial, table->partial_count, sizeof(*table->partial),
        gh_partial_compare);
  limit = table->partial_count < table->partial_per_hint ?
          table->partial_count : table->partial_per_hint;
  for (i = 0; i < limit; i++) {
    int inserted = gh_insert(table,
                             gh_clause_key(hint, table->partial[i].term),
                             id, FALSE);
    if (inserted < 0) {
      table->partial_cap_skips += limit - i;
      table->partial_hints++;
      return FALSE;
    }
    if (inserted > 0 && table->target_recipes_enabled &&
        gh_positive_unit_equality(hint)) {
      gh_append_target_recipe(table, id, GH_TARGET_PARTIAL,
                              table->partial[i].ordinal);
      gh_record_target_census(table, hint, table->partial[i].term);
    }
  }
  table->partial_hints++;
  return TRUE;
}

void hint_generalization_hash_finalize(Hint_generalization_hash table)
{
  if (table != NULL)
    table->finalized = TRUE;
}

static unsigned gh_lookup_key(Hint_generalization_hash table,
                              struct gh_key key,
                              unsigned long long *probe_count)
{
  size_t position = gh_slot_position(key.primary, key.check, table->capacity);
  size_t step = gh_slot_step(key.primary, key.check, table->capacity);
  unsigned long long probes = 0;
  while (table->slots[position].primary != 0) {
    probes++;
    if (table->slots[position].primary == key.primary &&
        table->slots[position].check == key.check) {
      if (probe_count != NULL)
        *probe_count = probes;
      return table->slots[position].value & GH_ID_MASK;
    }
    position = (position + step) & (table->capacity - 1);
  }
  probes++;
  if (probe_count != NULL)
    *probe_count = probes;
  return 0;
}

unsigned hint_generalization_hash_lookup(Hint_generalization_hash table,
                                         Topform clause)
{
  struct gh_key key;
  unsigned long long probes = 0;
  unsigned id;
  if (table == NULL || !table->finalized || clause == NULL)
    return 0;
  table->queries++;
  key = gh_clause_key(clause, NULL);
  id = gh_lookup_key(table, key, &probes);
  if (id != 0)
    table->hits++;
  table->probes += probes;
  if (probes > table->maximum_probe)
    table->maximum_probe = probes;
  return id;
}

static struct gh_key gh_virtual_unit_key(
  Literals from_lit, int from_side, Context from_subst,
  Literals into_lit, Ilist atom_path, Context into_subst,
  BOOL flipped, unsigned *term_nodes)
{
  struct gh_virtual_hash_state state;
  Term atom = into_lit->atom;
  Term beta = ARG(from_lit->atom, from_side == 0 ? 1 : 0);
  int output_position;
  memset(&state, 0, sizeof(state));
  state.hash = gh_hash_begin(1);
  gh_emit(&state.hash, into_lit->sign ? UINT64_C(0x500000001) :
                                           UINT64_C(0x500000000));
  state.term_nodes++;
  gh_hash_rigid(&state.hash, atom);
  for (output_position = 0; output_position < 2; output_position++) {
    int source_position = flipped ? 1 - output_position : output_position;
    if (atom_path != NULL && atom_path->i - 1 == source_position)
      gh_hash_substituted_term(&state, ARG(atom, source_position),
                               into_subst, atom_path->next,
                               beta, from_subst);
    else
      gh_hash_applied_term(&state, ARG(atom, source_position), into_subst);
  }
  if (term_nodes != NULL)
    *term_nodes = state.term_nodes;
  return gh_finish_key(&state.hash);
}

static struct gh_key gh_materialized_unit_key(Literals literal, BOOL flipped)
{
  struct gh_virtual_hash_state state;
  Term atom = literal->atom;
  int output_position;
  memset(&state, 0, sizeof(state));
  state.hash = gh_hash_begin(1);
  gh_emit(&state.hash, literal->sign ? UINT64_C(0x500000001) :
                                         UINT64_C(0x500000000));
  state.term_nodes++;
  gh_hash_rigid(&state.hash, atom);
  for (output_position = 0; output_position < 2; output_position++) {
    int source_position = flipped ? 1 - output_position : output_position;
    gh_hash_applied_term(&state, ARG(atom, source_position), NULL);
  }
  return gh_finish_key(&state.hash);
}

struct gh_term_view {
  Term term;
  Context context;
  Ilist path;
  Term beta;
  Context beta_context;
  BOOL substitute;
};

static struct gh_term_view gh_applied_view(Term term, Context context)
{
  struct gh_term_view view;
  memset(&view, 0, sizeof(view));
  view.term = term;
  view.context = context;
  return view;
}

static struct gh_term_view gh_substituted_view(
  Term term, Context context, Ilist path, Term beta, Context beta_context)
{
  struct gh_term_view view = gh_applied_view(term, context);
  view.path = path;
  view.beta = beta;
  view.beta_context = beta_context;
  view.substitute = TRUE;
  return view;
}

static void gh_normalize_view(struct gh_term_view *view)
{
  if (view->substitute && view->path == NULL) {
    view->term = view->beta;
    view->context = view->beta_context;
    view->substitute = FALSE;
  }
  else if (view->substitute && VARIABLE(view->term))
    view->substitute = FALSE;
  if (!view->substitute)
    DEREFERENCE(view->term, view->context);
}

static int gh_view_variable_number(const struct gh_term_view *view)
{
  return (view->context == NULL ? 0 :
          view->context->multiplier * MAX_VARS) + VARNUM(view->term);
}

static struct gh_term_view gh_view_child(
  const struct gh_term_view *view, int child)
{
  if (view->substitute && view->path->i - 1 == child)
    return gh_substituted_view(
      ARG(view->term, child), view->context, view->path->next,
      view->beta, view->beta_context);
  return gh_applied_view(ARG(view->term, child), view->context);
}

static BOOL gh_views_ident(struct gh_term_view first,
                           struct gh_term_view second)
{
  int i;
  gh_normalize_view(&first);
  gh_normalize_view(&second);
  if (VARIABLE(first.term) || VARIABLE(second.term))
    return VARIABLE(first.term) && VARIABLE(second.term) &&
           gh_view_variable_number(&first) ==
             gh_view_variable_number(&second);
  if (SYMNUM(first.term) != SYMNUM(second.term) ||
      ARITY(first.term) != ARITY(second.term))
    return FALSE;
  for (i = 0; i < ARITY(first.term); i++)
    if (!gh_views_ident(gh_view_child(&first, i),
                        gh_view_child(&second, i)))
      return FALSE;
  return TRUE;
}

static BOOL gh_virtual_unit_reflexive(
  Literals from_lit, int from_side, Context from_subst,
  Literals into_lit, Ilist atom_path, Context into_subst)
{
  Term atom = into_lit->atom;
  Term beta = ARG(from_lit->atom, from_side == 0 ? 1 : 0);
  struct gh_term_view sides[2];
  int i;
  for (i = 0; i < 2; i++) {
    if (atom_path->i - 1 == i)
      sides[i] = gh_substituted_view(
        ARG(atom, i), into_subst, atom_path->next, beta, from_subst);
    else
      sides[i] = gh_applied_view(ARG(atom, i), into_subst);
  }
  return gh_views_ident(sides[0], sides[1]);
}

BOOL hint_generalization_hash_lookup_unit_paramod(
  Hint_generalization_hash table,
  Literals from_lit, int from_side, Context from_subst,
  Literals into_lit, Ilist into_pos, Context into_subst,
  unsigned *normal_id, unsigned *flipped_id,
  unsigned *term_nodes, BOOL *reflexive,
  unsigned long long *probes)
{
  struct gh_key normal_key, flipped_key;
  unsigned found_normal, found_flipped;
  unsigned normal_nodes = 0, flipped_nodes = 0;
  unsigned long long normal_probes = 0, flipped_probes = 0;
  if (normal_id != NULL)
    *normal_id = 0;
  if (flipped_id != NULL)
    *flipped_id = 0;
  if (term_nodes != NULL)
    *term_nodes = 0;
  if (reflexive != NULL)
    *reflexive = FALSE;
  if (probes != NULL)
    *probes = 0;
  if (table == NULL || !table->finalized || from_lit == NULL ||
      into_lit == NULL || into_pos == NULL || into_pos->next == NULL ||
      !unit_clause(((Topform) from_lit->atom->container)->literals) ||
      !unit_clause(((Topform) into_lit->atom->container)->literals) ||
      !pos_eq(from_lit) || !into_lit->sign || !eq_term(into_lit->atom) ||
      ARITY(into_lit->atom) != 2)
    return FALSE;
  if (into_pos->next->i < 1 || into_pos->next->i > 2)
    fatal_error("invalid virtual paramodulation atom position");
  normal_key = gh_virtual_unit_key(
    from_lit, from_side, from_subst, into_lit, into_pos->next, into_subst,
    FALSE, &normal_nodes);
  flipped_key = gh_virtual_unit_key(
    from_lit, from_side, from_subst, into_lit, into_pos->next, into_subst,
    TRUE, &flipped_nodes);
  if (normal_nodes != flipped_nodes)
    fatal_error("virtual generalized-hash node count mismatch");
  found_normal = gh_lookup_key(table, normal_key, &normal_probes);
  if (gh_key_compare(normal_key, flipped_key) == 0) {
    found_flipped = found_normal;
  }
  else
    found_flipped = gh_lookup_key(table, flipped_key, &flipped_probes);
  if (normal_id != NULL)
    *normal_id = found_normal;
  if (flipped_id != NULL)
    *flipped_id = found_flipped;
  if (term_nodes != NULL)
    *term_nodes = normal_nodes;
  if (reflexive != NULL)
    *reflexive = gh_virtual_unit_reflexive(
      from_lit, from_side, from_subst, into_lit, into_pos->next,
      into_subst);
  if (probes != NULL)
    *probes = normal_probes + flipped_probes;
  return TRUE;
}

BOOL hint_generalization_hash_preview_unit_equality(
  Hint_generalization_hash table, Literals literal,
  unsigned *normal_id, unsigned *flipped_id,
  unsigned long long *probes)
{
  struct gh_key normal_key, flipped_key;
  unsigned found_normal, found_flipped;
  unsigned long long normal_probes = 0, flipped_probes = 0;
  if (normal_id != NULL)
    *normal_id = 0;
  if (flipped_id != NULL)
    *flipped_id = 0;
  if (probes != NULL)
    *probes = 0;
  if (table == NULL || !table->finalized || literal == NULL ||
      !unit_clause(((Topform) literal->atom->container)->literals) ||
      !literal->sign || !eq_term(literal->atom) ||
      ARITY(literal->atom) != 2)
    return FALSE;
  normal_key = gh_materialized_unit_key(literal, FALSE);
  flipped_key = gh_materialized_unit_key(literal, TRUE);
  found_normal = gh_lookup_key(table, normal_key, &normal_probes);
  if (gh_key_compare(normal_key, flipped_key) == 0)
    found_flipped = found_normal;
  else
    found_flipped = gh_lookup_key(table, flipped_key, &flipped_probes);
  if (normal_id != NULL)
    *normal_id = found_normal;
  if (flipped_id != NULL)
    *flipped_id = found_flipped;
  if (probes != NULL)
    *probes = normal_probes + flipped_probes;
  return TRUE;
}

void hint_generalization_hash_get_stats(
  Hint_generalization_hash table, struct hint_generalization_hash_stats *stats)
{
  memset(stats, 0, sizeof(*stats));
  if (table == NULL)
    return;
  stats->entries = table->count;
  stats->capacity = table->capacity;
  stats->bytes = (unsigned long long) table->capacity *
                   sizeof(*table->slots) +
                 (unsigned long long) table->hint_capacity *
                   sizeof(*table->hint_nodes) +
                 (unsigned long long) table->partial_capacity *
                   sizeof(*table->partial) +
                 gh_target_allocated_bytes(table) + sizeof(*table);
  stats->exact_hints = table->exact_hints;
  stats->exact_new_entries = table->exact_new_entries;
  stats->complete_hints = table->complete_hints;
  stats->partial_hints = table->partial_hints;
  stats->generated_attempts = table->generated_attempts;
  stats->generated_new_entries = table->generated_new_entries;
  stats->duplicates = table->duplicates;
  stats->partial_cap_skips = table->partial_cap_skips;
  stats->rehashes = table->rehashes;
  stats->queries = table->queries;
  stats->hits = table->hits;
  stats->probes = table->probes;
  stats->maximum_probe = table->maximum_probe;
  stats->complete_nodes = table->complete_nodes;
  stats->partial_per_hint = table->partial_per_hint;
  stats->maximum_entries = table->maximum_entries;
  stats->finalized = table->finalized;
  stats->target_recipes = table->target_recipe_count;
  stats->target_exact_recipes = table->target_exact_recipes;
  stats->target_partial_recipes = table->target_partial_recipes;
  stats->target_complete_recipes = table->target_complete_recipes;
  stats->target_position_records = table->target_position_records;
  stats->target_variable_positions = table->target_variable_positions;
  stats->target_rigid_positions = table->target_rigid_positions;
  stats->target_recipe_bytes =
    (unsigned long long) table->target_recipe_capacity *
      sizeof(*table->target_recipes) +
    (unsigned long long) table->complete_target_capacity *
      sizeof(*table->complete_targets);
  stats->target_complete_token_bytes =
    (unsigned long long) table->complete_target_token_capacity *
      sizeof(*table->complete_target_tokens);
  stats->target_max_nodes = table->target_max_nodes;
  stats->target_max_depth = table->target_max_depth;
  stats->target_budget_bytes = table->target_budget_bytes;
}
