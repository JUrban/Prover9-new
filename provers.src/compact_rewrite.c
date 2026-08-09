#include "compact_rewrite.h"

#include <limits.h>
#include <stdint.h>
#include <string.h>

#define CR_NONE 0U
#define CR_TOMBSTONE UINT64_MAX

struct cr_node {
  int32_t code;
  uint32_t first_child;
  uint32_t next_sibling;
  uint32_t first_posting;
  uint32_t last_posting;
};

struct cr_posting {
  uint32_t rule;
  uint32_t next;
  unsigned char direction;
};

struct cr_rule {
  unsigned long long proof_id;
  uint32_t left_offset;
  uint32_t left_length;
  uint32_t right_offset;
  uint32_t right_length;
  unsigned char type;
  unsigned char active;
};

struct compact_rewrite_bank {
  struct cr_node *nodes;
  size_t node_count;
  size_t node_capacity;
  struct cr_posting *postings;
  size_t posting_count;
  size_t posting_capacity;
  struct cr_rule *rules;
  size_t rule_count;
  size_t rule_capacity;
  int32_t *tokens;
  size_t token_count;
  size_t token_capacity;
  unsigned long long *hash_keys;
  uint32_t *hash_values;
  size_t hash_capacity;
  size_t hash_count;
  size_t hash_tombstones;
  unsigned long long active_rules;
  unsigned long long peak_rules;
  unsigned long long retired_rules;
  unsigned long long attempts;
  unsigned long long rewrites;
  unsigned long long peak_bytes;
  unsigned long long compactions;
  unsigned long long bytes_reclaimed;
};

struct cr_query_term {
  Term term;
  uint32_t end;
};

struct cr_match_result {
  BOOL found;
  Term contractum;
  unsigned long long proof_id;
  int direction;
};

static size_t grow_capacity(size_t current, size_t item_size,
                            char *message)
{
  size_t next = current == 0 ? 64 : current * 2;
  if (next < current || next > SIZE_MAX / item_size)
    fatal_error(message);
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

static unsigned long long bank_bytes(Compact_rewrite_bank bank)
{
  if (bank == NULL)
    return 0;
  return sizeof(*bank) +
    bank->node_capacity * sizeof(*bank->nodes) +
    bank->posting_capacity * sizeof(*bank->postings) +
    bank->rule_capacity * sizeof(*bank->rules) +
    bank->token_capacity * sizeof(*bank->tokens) +
    bank->hash_capacity *
      (sizeof(*bank->hash_keys) + sizeof(*bank->hash_values));
}

static void update_peak(Compact_rewrite_bank bank)
{
  unsigned long long bytes = bank_bytes(bank);
  if (bytes > bank->peak_bytes)
    bank->peak_bytes = bytes;
  if (bank->active_rules > bank->peak_rules)
    bank->peak_rules = bank->active_rules;
}

static void ensure_nodes(Compact_rewrite_bank bank)
{
  if (bank->node_count == bank->node_capacity) {
    bank->node_capacity = grow_capacity(bank->node_capacity,
                                         sizeof(*bank->nodes),
                                         "compact_rewrite: node overflow");
    bank->nodes = safe_realloc(bank->nodes,
                                bank->node_capacity * sizeof(*bank->nodes));
  }
}

static void ensure_postings(Compact_rewrite_bank bank)
{
  if (bank->posting_count == bank->posting_capacity) {
    bank->posting_capacity = grow_capacity(bank->posting_capacity,
                                            sizeof(*bank->postings),
                                            "compact_rewrite: posting overflow");
    bank->postings = safe_realloc(
      bank->postings, bank->posting_capacity * sizeof(*bank->postings));
  }
}

static void ensure_rules(Compact_rewrite_bank bank)
{
  if (bank->rule_count == bank->rule_capacity) {
    bank->rule_capacity = grow_capacity(bank->rule_capacity,
                                         sizeof(*bank->rules),
                                         "compact_rewrite: rule overflow");
    bank->rules = safe_realloc(bank->rules,
                                bank->rule_capacity * sizeof(*bank->rules));
  }
}

static void ensure_tokens(Compact_rewrite_bank bank, size_t extra)
{
  size_t needed;
  if (extra > SIZE_MAX - bank->token_count)
    fatal_error("compact_rewrite: token overflow");
  needed = bank->token_count + extra;
  if (needed > UINT32_MAX)
    fatal_error("compact_rewrite: token offsets exceed 32 bits");
  while (needed > bank->token_capacity) {
    bank->token_capacity = grow_capacity(bank->token_capacity,
                                          sizeof(*bank->tokens),
                                          "compact_rewrite: token capacity overflow");
    bank->tokens = safe_realloc(
      bank->tokens, bank->token_capacity * sizeof(*bank->tokens));
  }
}

static size_t hash_slot(Compact_rewrite_bank bank, uint64_t id,
                        BOOL inserting)
{
  size_t mask = bank->hash_capacity - 1;
  size_t at = (size_t) hash_id(id) & mask;
  size_t tombstone = SIZE_MAX;
  for (;;) {
    uint64_t key = bank->hash_keys[at];
    if (key == 0)
      return inserting && tombstone != SIZE_MAX ? tombstone : at;
    if (key == id)
      return at;
    if (inserting && key == CR_TOMBSTONE && tombstone == SIZE_MAX)
      tombstone = at;
    at = (at + 1) & mask;
  }
}

static void rehash(Compact_rewrite_bank bank, size_t capacity)
{
  unsigned long long *old_keys = bank->hash_keys;
  uint32_t *old_values = bank->hash_values;
  size_t old_capacity = bank->hash_capacity;
  size_t i;
  bank->hash_keys = safe_calloc(capacity, sizeof(*bank->hash_keys));
  bank->hash_values = safe_calloc(capacity, sizeof(*bank->hash_values));
  bank->hash_capacity = capacity;
  bank->hash_tombstones = 0;
  for (i = 0; i < old_capacity; i++)
    if (old_keys[i] != 0 && old_keys[i] != CR_TOMBSTONE) {
      size_t at = hash_slot(bank, old_keys[i], TRUE);
      bank->hash_keys[at] = old_keys[i];
      bank->hash_values[at] = old_values[i];
    }
  safe_free(old_keys);
  safe_free(old_values);
}

static void ensure_hash(Compact_rewrite_bank bank)
{
  if (bank->hash_capacity == 0)
    rehash(bank, 128);
  else if ((bank->hash_count + bank->hash_tombstones + 1) * 10 >=
           bank->hash_capacity * 7) {
    if (bank->hash_capacity > SIZE_MAX / 2)
      fatal_error("compact_rewrite: hash overflow");
    rehash(bank, bank->hash_capacity * 2);
  }
}

static uint32_t lookup_rule(Compact_rewrite_bank bank,
                            unsigned long long proof_id)
{
  size_t at;
  if (bank == NULL || proof_id == 0 || bank->hash_capacity == 0)
    return CR_NONE;
  at = hash_slot(bank, proof_id, FALSE);
  return bank->hash_keys[at] == proof_id ? bank->hash_values[at] : CR_NONE;
}

static uint32_t append_term_tokens(Compact_rewrite_bank bank, Term term,
                                   uint32_t *length)
{
  size_t offset = bank->token_count;
  size_t cap = 128, top = 0;
  Term fixed[128];
  Term *stack = fixed;
  stack[top++] = term;
  while (top != 0) {
    Term current = stack[--top];
    int i;
    ensure_tokens(bank, 1);
    bank->tokens[bank->token_count++] = VARIABLE(current) ?
      -(int32_t) VARNUM(current) - 1 : (int32_t) SYMNUM(current);
    for (i = ARITY(current) - 1; i >= 0; i--) {
      if (top == cap) {
        cap *= 2;
        if (stack == fixed) {
          stack = safe_malloc(cap * sizeof(*stack));
          memcpy(stack, fixed, top * sizeof(*stack));
        }
        else
          stack = safe_realloc(stack, cap * sizeof(*stack));
      }
      stack[top++] = ARG(current, i);
    }
  }
  if (stack != fixed)
    safe_free(stack);
  *length = (uint32_t) (bank->token_count - offset);
  return (uint32_t) offset;
}

static int code_compare(int32_t a, int32_t b)
{
  BOOL av = a < 0, bv = b < 0;
  if (av != bv)
    return av ? -1 : 1;
  if (av) {
    int32_t avar = -a - 1, bvar = -b - 1;
    return avar < bvar ? -1 : avar > bvar ? 1 : 0;
  }
  return a < b ? -1 : a > b ? 1 : 0;
}

static uint32_t trie_child(Compact_rewrite_bank bank, uint32_t parent,
                           int32_t code)
{
  uint32_t current = bank->nodes[parent].first_child;
  uint32_t previous = CR_NONE;
  while (current != CR_NONE &&
         code_compare(bank->nodes[current].code, code) < 0) {
    previous = current;
    current = bank->nodes[current].next_sibling;
  }
  if (current != CR_NONE && bank->nodes[current].code == code)
    return current;
  ensure_nodes(bank);
  if (bank->node_count > UINT32_MAX)
    fatal_error("compact_rewrite: node offsets exceed 32 bits");
  current = (uint32_t) bank->node_count++;
  memset(&bank->nodes[current], 0, sizeof(bank->nodes[current]));
  bank->nodes[current].code = code;
  if (previous == CR_NONE) {
    bank->nodes[current].next_sibling = bank->nodes[parent].first_child;
    bank->nodes[parent].first_child = current;
  }
  else {
    bank->nodes[current].next_sibling = bank->nodes[previous].next_sibling;
    bank->nodes[previous].next_sibling = current;
  }
  return current;
}

static void index_side(Compact_rewrite_bank bank, uint32_t rule,
                       uint32_t offset, uint32_t length, int direction)
{
  uint32_t node = 0;
  uint32_t posting;
  uint32_t i;
  for (i = 0; i < length; i++)
    node = trie_child(bank, node, bank->tokens[offset + i]);
  ensure_postings(bank);
  if (bank->posting_count > UINT32_MAX)
    fatal_error("compact_rewrite: posting offsets exceed 32 bits");
  posting = (uint32_t) bank->posting_count++;
  memset(&bank->postings[posting], 0, sizeof(bank->postings[posting]));
  bank->postings[posting].rule = rule;
  bank->postings[posting].direction = (unsigned char) direction;
  if (bank->nodes[node].first_posting == CR_NONE)
    bank->nodes[node].first_posting = posting;
  else
    bank->postings[bank->nodes[node].last_posting].next = posting;
  bank->nodes[node].last_posting = posting;
}

Compact_rewrite_bank compact_rewrite_init(void)
{
  Compact_rewrite_bank bank = safe_calloc(1, sizeof(*bank));
  ensure_nodes(bank);
  memset(&bank->nodes[0], 0, sizeof(bank->nodes[0]));
  bank->node_count = 1;
  ensure_postings(bank);
  memset(&bank->postings[0], 0, sizeof(bank->postings[0]));
  bank->posting_count = 1;
  ensure_rules(bank);
  memset(&bank->rules[0], 0, sizeof(bank->rules[0]));
  bank->rule_count = 1;
  update_peak(bank);
  return bank;
}

BOOL compact_rewrite_add(Compact_rewrite_bank bank, Topform clause, int type)
{
  struct cr_rule *rule;
  uint32_t index;
  Term atom;
  size_t at;
  if (bank == NULL || clause == NULL || clause->id == 0 ||
      type == NOT_DEMODULATOR || lookup_rule(bank, clause->id) != CR_NONE)
    return FALSE;
  ensure_rules(bank);
  if (bank->rule_count > UINT32_MAX)
    fatal_error("compact_rewrite: rule offsets exceed 32 bits");
  index = (uint32_t) bank->rule_count++;
  rule = &bank->rules[index];
  memset(rule, 0, sizeof(*rule));
  rule->proof_id = clause->id;
  rule->type = (unsigned char) type;
  rule->active = TRUE;
  atom = clause->literals->atom;
  rule->left_offset = append_term_tokens(bank, ARG(atom, 0),
                                          &rule->left_length);
  rule->right_offset = append_term_tokens(bank, ARG(atom, 1),
                                           &rule->right_length);
  if (type == ORIENTED || type == LEX_DEP_LR || type == LEX_DEP_BOTH)
    index_side(bank, index, rule->left_offset, rule->left_length, 1);
  if (type == LEX_DEP_RL || type == LEX_DEP_BOTH)
    index_side(bank, index, rule->right_offset, rule->right_length, 2);
  ensure_hash(bank);
  at = hash_slot(bank, clause->id, TRUE);
  if (bank->hash_keys[at] == CR_TOMBSTONE)
    bank->hash_tombstones--;
  bank->hash_keys[at] = clause->id;
  bank->hash_values[at] = index;
  bank->hash_count++;
  bank->active_rules++;
  update_peak(bank);
  return TRUE;
}

static void copy_live_rule(Compact_rewrite_bank destination,
                           Compact_rewrite_bank source,
                           const struct cr_rule *old)
{
  struct cr_rule *rule;
  uint32_t index;
  size_t at;
  size_t token_total = (size_t) old->left_length + old->right_length;
  ensure_rules(destination);
  if (destination->rule_count > UINT32_MAX)
    fatal_error("compact_rewrite: compacted rule offsets exceed 32 bits");
  index = (uint32_t) destination->rule_count++;
  rule = &destination->rules[index];
  memset(rule, 0, sizeof(*rule));
  rule->proof_id = old->proof_id;
  rule->type = old->type;
  rule->active = TRUE;
  ensure_tokens(destination, token_total);
  rule->left_offset = (uint32_t) destination->token_count;
  rule->left_length = old->left_length;
  memcpy(destination->tokens + destination->token_count,
         source->tokens + old->left_offset,
         (size_t) old->left_length * sizeof(*destination->tokens));
  destination->token_count += old->left_length;
  rule->right_offset = (uint32_t) destination->token_count;
  rule->right_length = old->right_length;
  memcpy(destination->tokens + destination->token_count,
         source->tokens + old->right_offset,
         (size_t) old->right_length * sizeof(*destination->tokens));
  destination->token_count += old->right_length;
  if (rule->type == ORIENTED || rule->type == LEX_DEP_LR ||
      rule->type == LEX_DEP_BOTH)
    index_side(destination, index, rule->left_offset, rule->left_length, 1);
  if (rule->type == LEX_DEP_RL || rule->type == LEX_DEP_BOTH)
    index_side(destination, index, rule->right_offset, rule->right_length, 2);
  ensure_hash(destination);
  at = hash_slot(destination, rule->proof_id, TRUE);
  destination->hash_keys[at] = rule->proof_id;
  destination->hash_values[at] = index;
  destination->hash_count++;
  destination->active_rules++;
  update_peak(destination);
}

BOOL compact_rewrite_compaction_needed(Compact_rewrite_bank bank)
{
  unsigned long long physical, stale, threshold;
  if (bank == NULL || bank->rule_count <= 1)
    return FALSE;
  physical = bank->rule_count - 1;
  stale = physical - bank->active_rules;
  threshold = bank->active_rules / 4;
  if (threshold < 1024)
    threshold = 1024;
  return stale >= threshold;
}

void compact_rewrite_compact(Compact_rewrite_bank bank)
{
  Compact_rewrite_bank replacement;
  struct compact_rewrite_bank old;
  unsigned long long old_bytes, old_peak, old_peak_rules;
  unsigned long long attempts, rewrites, retired, compactions, reclaimed;
  size_t i;
  if (bank == NULL || !compact_rewrite_compaction_needed(bank))
    return;
  old_bytes = bank_bytes(bank);
  old_peak = bank->peak_bytes;
  old_peak_rules = bank->peak_rules;
  attempts = bank->attempts;
  rewrites = bank->rewrites;
  retired = bank->retired_rules;
  compactions = bank->compactions;
  reclaimed = bank->bytes_reclaimed;
  replacement = compact_rewrite_init();
  for (i = 1; i < bank->rule_count; i++)
    if (bank->rules[i].active)
      copy_live_rule(replacement, bank, &bank->rules[i]);

  old = *bank;
  *bank = *replacement;
  safe_free(replacement);
  safe_free(old.nodes);
  safe_free(old.postings);
  safe_free(old.rules);
  safe_free(old.tokens);
  safe_free(old.hash_keys);
  safe_free(old.hash_values);
  bank->attempts = attempts;
  bank->rewrites = rewrites;
  bank->retired_rules = retired;
  bank->compactions = compactions + 1;
  bank->bytes_reclaimed = reclaimed +
    (old_bytes > bank_bytes(bank) ? old_bytes - bank_bytes(bank) : 0);
  if (old_peak > bank->peak_bytes)
    bank->peak_bytes = old_peak;
  if (old_peak_rules > bank->peak_rules)
    bank->peak_rules = old_peak_rules;
}

static BOOL remove_rule(Compact_rewrite_bank bank,
                        unsigned long long proof_id, BOOL retirement)
{
  uint32_t index = lookup_rule(bank, proof_id);
  size_t at;
  if (index == CR_NONE || !bank->rules[index].active)
    return FALSE;
  bank->rules[index].active = FALSE;
  at = hash_slot(bank, proof_id, FALSE);
  bank->hash_keys[at] = CR_TOMBSTONE;
  bank->hash_values[at] = 0;
  bank->hash_count--;
  bank->hash_tombstones++;
  bank->active_rules--;
  if (retirement)
    bank->retired_rules++;
  return TRUE;
}

BOOL compact_rewrite_remove(Compact_rewrite_bank bank,
                            unsigned long long proof_id)
{
  return remove_rule(bank, proof_id, TRUE);
}

BOOL compact_rewrite_suspend(Compact_rewrite_bank bank,
                             unsigned long long proof_id)
{
  return remove_rule(bank, proof_id, FALSE);
}

void compact_rewrite_note_suspended_retirement(Compact_rewrite_bank bank)
{
  if (bank != NULL)
    bank->retired_rules++;
}

BOOL compact_rewrite_contains(Compact_rewrite_bank bank,
                              unsigned long long proof_id)
{
  return lookup_rule(bank, proof_id) != CR_NONE;
}

unsigned long long compact_rewrite_identity_hash(Compact_rewrite_bank bank)
{
  uint64_t hash = 0;
  size_t i;
  if (bank == NULL)
    return 0;
  /* Commutative across pool growth and tombstone placement.  Checkpoint
     identity is the live set of stable proof IDs and rule directions. */
  for (i = 1; i < bank->rule_count; i++)
    if (bank->rules[i].active)
      hash ^= hash_id(bank->rules[i].proof_id ^
                      ((uint64_t) bank->rules[i].type << 56));
  return hash;
}

void compact_rewrite_restore_counters(Compact_rewrite_bank bank,
                                      unsigned long long rules_peak,
                                      unsigned long long rules_retired,
                                      unsigned long long attempts,
                                      unsigned long long rewrites,
                                      unsigned long long compactions,
                                      unsigned long long bytes_reclaimed)
{
  if (bank == NULL)
    return;
  if (rules_peak > bank->peak_rules)
    bank->peak_rules = rules_peak;
  bank->retired_rules = rules_retired;
  bank->attempts = attempts;
  bank->rewrites = rewrites;
  bank->compactions = compactions;
  bank->bytes_reclaimed = bytes_reclaimed;
}

static void flatten_query_rec(Term term, struct cr_query_term **items,
                              size_t *count, size_t *capacity)
{
  size_t at;
  int i;
  if (*count == *capacity) {
    *capacity = grow_capacity(*capacity, sizeof(**items),
                              "compact_rewrite: query overflow");
    *items = safe_realloc(*items, *capacity * sizeof(**items));
  }
  at = (*count)++;
  (*items)[at].term = term;
  for (i = 0; i < ARITY(term); i++)
    flatten_query_rec(ARG(term, i), items, count, capacity);
  if (*count > UINT32_MAX)
    fatal_error("compact_rewrite: query offsets exceed 32 bits");
  (*items)[at].end = (uint32_t) *count;
}

static Term build_contractum(Compact_rewrite_bank bank, uint32_t *position,
                             uint32_t end, Term *bindings)
{
  int32_t code;
  Term result;
  int i, arity;
  if (*position >= end)
    fatal_error("compact_rewrite: corrupt RHS token stream");
  code = bank->tokens[(*position)++];
  if (code < 0) {
    int variable = -code - 1;
    if (variable >= MAX_VARS || bindings[variable] == NULL)
      fatal_error("compact_rewrite: unbound RHS variable");
    return copy_term(bindings[variable]);
  }
  arity = sn_to_arity(code);
  result = get_rigid_term_dangerously(code, arity);
  for (i = 0; i < arity; i++)
    ARG(result, i) = build_contractum(bank, position, end, bindings);
  return result;
}

static BOOL try_leaf(Compact_rewrite_bank bank, uint32_t node, Term target,
                     Term *bindings, BOOL lex_order_vars,
                     struct cr_match_result *result)
{
  uint32_t posting;
  for (posting = bank->nodes[node].first_posting;
       posting != CR_NONE; posting = bank->postings[posting].next) {
    struct cr_posting *p = &bank->postings[posting];
    struct cr_rule *rule = &bank->rules[p->rule];
    uint32_t position, end;
    Term contractum;
    if (!rule->active)
      continue;
    if (p->direction == 1) {
      position = rule->right_offset;
      end = position + rule->right_length;
    }
    else {
      position = rule->left_offset;
      end = position + rule->left_length;
    }
    contractum = build_contractum(bank, &position, end, bindings);
    if (position != end)
      fatal_error("compact_rewrite: incomplete RHS consumption");
    if (rule->type == ORIENTED ||
        term_greater(target, contractum, lex_order_vars)) {
      result->found = TRUE;
      result->contractum = contractum;
      result->proof_id = rule->proof_id;
      result->direction = p->direction;
      return TRUE;
    }
    zap_term(contractum);
  }
  return FALSE;
}

static BOOL retrieve_rec(Compact_rewrite_bank bank, uint32_t node,
                         struct cr_query_term *query, uint32_t position,
                         uint32_t end, Term *bindings, Term target,
                         BOOL lex_order_vars,
                         struct cr_match_result *result)
{
  uint32_t child;
  if (position == end)
    return try_leaf(bank, node, target, bindings, lex_order_vars, result);
  for (child = bank->nodes[node].first_child; child != CR_NONE;
       child = bank->nodes[child].next_sibling) {
    int32_t code = bank->nodes[child].code;
    Term query_term = query[position].term;
    if (code < 0) {
      int variable = -code - 1;
      BOOL newly_bound = FALSE;
      if (variable >= MAX_VARS)
        fatal_error("compact_rewrite: variable exceeds MAX_VARS");
      if (bindings[variable] == NULL) {
        bindings[variable] = query_term;
        newly_bound = TRUE;
      }
      if ((newly_bound || term_ident(bindings[variable], query_term)) &&
          retrieve_rec(bank, child, query, query[position].end, end,
                       bindings, target, lex_order_vars, result)) {
        if (newly_bound)
          bindings[variable] = NULL;
        return TRUE;
      }
      if (newly_bound)
        bindings[variable] = NULL;
    }
    else if (!VARIABLE(query_term) && SYMNUM(query_term) == code &&
             retrieve_rec(bank, child, query, position + 1, end,
                          bindings, target, lex_order_vars, result))
      return TRUE;
  }
  return FALSE;
}

static struct cr_match_result find_rewrite(Compact_rewrite_bank bank,
                                           Term target,
                                           BOOL lex_order_vars)
{
  struct cr_match_result result;
  struct cr_query_term *query = NULL;
  size_t count = 0, capacity = 0;
  Term bindings[MAX_VARS];
  memset(&result, 0, sizeof(result));
  memset(bindings, 0, sizeof(bindings));
  flatten_query_rec(target, &query, &count, &capacity);
  if (count > 0)
    retrieve_rec(bank, 0, query, 0, (uint32_t) count, bindings, target,
                 lex_order_vars, &result);
  safe_free(query);
  return result;
}

static Term normalize_term(Compact_rewrite_bank bank, Term term,
                           int *step_limit, int size_limit,
                           int *current_size, int *sequence,
                           I3list *steps, BOOL lex_order_vars,
                           BOOL count_stats)
{
  int sequence_save;
  int i;
  struct cr_match_result match;
  if (*step_limit == 0 || *current_size > size_limit || VARIABLE(term))
    return term;
  sequence_save = *sequence;
  for (i = 0; i < ARITY(term); i++)
    ARG(term, i) = normalize_term(bank, ARG(term, i), step_limit, size_limit,
                                  current_size, sequence, steps,
                                  lex_order_vars, count_stats);
  if (*step_limit == 0 || *current_size > size_limit)
    return term;
  if (count_stats)
    bank->attempts++;
  (*sequence)++;
  match = find_rewrite(bank, term, lex_order_vars);
  if (!match.found)
    return term;
  *current_size += symbol_count(match.contractum) - symbol_count(term);
  (*step_limit)--;
  if (count_stats)
    bank->rewrites++;
  *steps = i3list_prepend(*steps, match.proof_id, *sequence,
                          match.direction);
  zap_term(term);
  *sequence = sequence_save;
  return normalize_term(bank, match.contractum, step_limit, size_limit,
                        current_size, sequence, steps, lex_order_vars,
                        count_stats);
}

void compact_rewrite_clause(Compact_rewrite_bank bank, Topform clause,
                            int step_limit, int increase_limit,
                            BOOL lex_order_vars, BOOL count_stats)
{
  Literals literal;
  I3list steps = NULL;
  int sequence = 0;
  if (bank == NULL || bank->active_rules == 0 || clause == NULL)
    return;
  step_limit = step_limit == -1 ? INT_MAX : step_limit;
  increase_limit = increase_limit == -1 ? INT_MAX : increase_limit;
  for (literal = clause->literals; literal != NULL; literal = literal->next) {
    int current_size = symbol_count(literal->atom);
    int size_limit = increase_limit == INT_MAX ? INT_MAX :
                     current_size + increase_limit;
    literal->atom = normalize_term(bank, literal->atom, &step_limit,
                                   size_limit, &current_size, &sequence,
                                   &steps, lex_order_vars, count_stats);
    if (current_size > size_limit)
      increase_limit = -1;
  }
  upward_clause_links(clause);
  if (steps != NULL) {
    steps = reverse_i3list(steps);
    clause->justification = append_just(clause->justification,
                                        demod_just(steps));
  }
}

void compact_rewrite_get_stats(Compact_rewrite_bank bank,
                               struct compact_rewrite_stats *stats)
{
  if (stats == NULL)
    return;
  memset(stats, 0, sizeof(*stats));
  if (bank == NULL)
    return;
  stats->rules_current = bank->active_rules;
  stats->rules_peak = bank->peak_rules;
  stats->rules_retired = bank->retired_rules;
  stats->rules_physical = bank->rule_count - 1;
  stats->compactions = bank->compactions;
  stats->bytes_reclaimed = bank->bytes_reclaimed;
  stats->attempts = bank->attempts;
  stats->rewrites = bank->rewrites;
  stats->node_bytes = bank->node_capacity * sizeof(*bank->nodes);
  stats->posting_bytes = bank->posting_capacity * sizeof(*bank->postings);
  stats->rule_bytes = bank->rule_capacity * sizeof(*bank->rules);
  stats->term_bytes = bank->token_capacity * sizeof(*bank->tokens);
  stats->hash_bytes = bank->hash_capacity *
    (sizeof(*bank->hash_keys) + sizeof(*bank->hash_values));
  stats->total_bytes = bank_bytes(bank);
  stats->peak_bytes = bank->peak_bytes;
}

void compact_rewrite_free(Compact_rewrite_bank bank)
{
  if (bank == NULL)
    return;
  safe_free(bank->nodes);
  safe_free(bank->postings);
  safe_free(bank->rules);
  safe_free(bank->tokens);
  safe_free(bank->hash_keys);
  safe_free(bank->hash_values);
  safe_free(bank);
}
