#include "compact_rewrite.h"

#include <limits.h>
#include <stdint.h>
#include <string.h>

#define CR_NONE 0U
#define CR_TOMBSTONE UINT64_MAX
#define CR_OCCURRENCE_BLOCK_PAYLOAD 248

struct cr_node {
  uint32_t token_offset;
  uint32_t token_length;
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

struct cr_occurrence_block {
  uint32_t next;
  uint16_t used;
  uint16_t count;
  unsigned char data[CR_OCCURRENCE_BLOCK_PAYLOAD];
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
  struct cr_occurrence_block *occurrence_blocks;
  size_t occurrence_block_count;
  size_t occurrence_block_capacity;
  size_t occurrence_count;
  size_t occurrence_stream_used;
  uint32_t *occurrence_heads;
  uint32_t *occurrence_tails;
  uint32_t *occurrence_last_rules;
  size_t occurrence_symbol_capacity;
  struct cr_rule *rules;
  size_t rule_count;
  size_t rule_capacity;
  Compact_term_pool term_pool;
  const int32_t *tokens;
  BOOL owns_term_pool;
  unsigned long long *hash_keys;
  uint32_t *hash_values;
  size_t hash_capacity;
  size_t hash_count;
  size_t hash_tombstones;
  struct cr_query_term *query;
  size_t query_capacity;
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
  struct compact_term_pool_stats terms;
  if (bank == NULL)
    return 0;
  compact_term_pool_get_stats(bank->term_pool, &terms);
  return sizeof(*bank) +
    bank->node_capacity * sizeof(*bank->nodes) +
    bank->posting_capacity * sizeof(*bank->postings) +
    bank->occurrence_block_capacity * sizeof(*bank->occurrence_blocks) +
    bank->occurrence_symbol_capacity *
      (sizeof(*bank->occurrence_heads) + sizeof(*bank->occurrence_tails) +
       sizeof(*bank->occurrence_last_rules)) +
    bank->rule_capacity * sizeof(*bank->rules) +
    (bank->owns_term_pool ? terms.total_bytes : 0) +
    bank->hash_capacity *
      (sizeof(*bank->hash_keys) + sizeof(*bank->hash_values)) +
    bank->query_capacity * sizeof(*bank->query);
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

static uint32_t new_occurrence_block(Compact_rewrite_bank bank)
{
  uint32_t block;
  if (bank->occurrence_block_count == bank->occurrence_block_capacity) {
    bank->occurrence_block_capacity = grow_capacity(
      bank->occurrence_block_capacity, sizeof(*bank->occurrence_blocks),
      "compact_rewrite: occurrence block overflow");
    bank->occurrence_blocks = safe_realloc(
      bank->occurrence_blocks,
      bank->occurrence_block_capacity * sizeof(*bank->occurrence_blocks));
  }
  if (bank->occurrence_block_count > UINT32_MAX)
    fatal_error("compact_rewrite: occurrence block offsets exceed 32 bits");
  block = (uint32_t) bank->occurrence_block_count++;
  memset(&bank->occurrence_blocks[block], 0,
         sizeof(bank->occurrence_blocks[block]));
  return block;
}

static void ensure_occurrence_symbol(Compact_rewrite_bank bank,
                                     unsigned symbol)
{
  size_t old = bank->occurrence_symbol_capacity;
  size_t capacity = old == 0 ? 64 : old;
  while (symbol >= capacity) {
    if (capacity > SIZE_MAX / 2)
      fatal_error("compact_rewrite: occurrence symbol overflow");
    capacity *= 2;
  }
  if (capacity != old) {
    bank->occurrence_heads = safe_realloc(
      bank->occurrence_heads, capacity * sizeof(*bank->occurrence_heads));
    bank->occurrence_tails = safe_realloc(
      bank->occurrence_tails, capacity * sizeof(*bank->occurrence_tails));
    bank->occurrence_last_rules = safe_realloc(
      bank->occurrence_last_rules,
      capacity * sizeof(*bank->occurrence_last_rules));
    memset(bank->occurrence_heads + old, 0,
           (capacity - old) * sizeof(*bank->occurrence_heads));
    memset(bank->occurrence_tails + old, 0,
           (capacity - old) * sizeof(*bank->occurrence_tails));
    memset(bank->occurrence_last_rules + old, 0,
           (capacity - old) * sizeof(*bank->occurrence_last_rules));
    bank->occurrence_symbol_capacity = capacity;
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

static uint32_t append_term_tokens(Compact_rewrite_bank bank, Topform clause,
                                   Term term,
                                   uint32_t *length)
{
  return compact_term_pool_intern(bank->term_pool, clause->id,
                                  clause->literals, term, length);
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

static uint32_t new_node(Compact_rewrite_bank bank,
                         uint32_t token_offset, uint32_t token_length)
{
  uint32_t node;
  ensure_nodes(bank);
  if (bank->node_count > UINT32_MAX)
    fatal_error("compact_rewrite: node offsets exceed 32 bits");
  node = (uint32_t) bank->node_count++;
  memset(&bank->nodes[node], 0, sizeof(bank->nodes[node]));
  bank->nodes[node].token_offset = token_offset;
  bank->nodes[node].token_length = token_length;
  return node;
}

static int32_t first_code(Compact_rewrite_bank bank, uint32_t node)
{
  struct cr_node *n = &bank->nodes[node];
  if (n->token_length == 0)
    fatal_error("compact_rewrite: empty nonroot radix edge");
  return bank->tokens[n->token_offset];
}

/* Insert an immutable prefix-token slice as a radix path.  Siblings remain
   ordered by their first token exactly as in the former token-per-node trie.
   Splitting only changes the representation: terminal posting order is still
   the order in which rewrite sides were admitted. */
static uint32_t insert_token_path(Compact_rewrite_bank bank, uint32_t root,
                                  uint32_t offset, uint32_t length)
{
  uint32_t parent = root;
  uint32_t position = 0;
  while (position < length) {
    uint32_t current = bank->nodes[parent].first_child;
    uint32_t previous = CR_NONE;
    int32_t wanted = bank->tokens[offset + position];
    while (current != CR_NONE &&
           code_compare(first_code(bank, current), wanted) < 0) {
      previous = current;
      current = bank->nodes[current].next_sibling;
    }
    if (current == CR_NONE || first_code(bank, current) != wanted) {
      uint32_t added = new_node(bank, offset + position,
                                length - position);
      if (previous == CR_NONE) {
        bank->nodes[added].next_sibling =
          bank->nodes[parent].first_child;
        bank->nodes[parent].first_child = added;
      }
      else {
        bank->nodes[added].next_sibling =
          bank->nodes[previous].next_sibling;
        bank->nodes[previous].next_sibling = added;
      }
      return added;
    }
    else {
      uint32_t old_offset = bank->nodes[current].token_offset;
      uint32_t old_length = bank->nodes[current].token_length;
      uint32_t common = 0;
      while (common < old_length && position + common < length &&
             bank->tokens[old_offset + common] ==
             bank->tokens[offset + position + common])
        common++;
      if (common == old_length) {
        position += common;
        parent = current;
      }
      else {
        uint32_t old_next = bank->nodes[current].next_sibling;
        uint32_t split = new_node(bank, old_offset, common);
        uint32_t added;
        if (common == 0)
          fatal_error("compact_rewrite: invalid zero-length radix split");
        bank->nodes[split].next_sibling = old_next;
        if (previous == CR_NONE)
          bank->nodes[parent].first_child = split;
        else
          bank->nodes[previous].next_sibling = split;
        bank->nodes[current].token_offset += common;
        bank->nodes[current].token_length -= common;
        bank->nodes[current].next_sibling = CR_NONE;
        bank->nodes[split].first_child = current;
        position += common;
        if (position == length)
          return split;
        added = new_node(bank, offset + position, length - position);
        if (code_compare(first_code(bank, added),
                         first_code(bank, current)) < 0) {
          bank->nodes[added].next_sibling = current;
          bank->nodes[split].first_child = added;
        }
        else
          bank->nodes[current].next_sibling = added;
        return added;
      }
    }
  }
  return parent;
}

static void index_side(Compact_rewrite_bank bank, uint32_t rule,
                       uint32_t offset, uint32_t length, int direction)
{
  uint32_t node = insert_token_path(bank, 0, offset, length);
  uint32_t posting;
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

static void collect_occurrence_symbols(Compact_rewrite_bank bank,
                                       uint32_t offset, uint32_t length,
                                       int32_t *symbols, size_t *count)
{
  uint32_t i;
  for (i = 0; i < length; i++) {
    int32_t symbol = bank->tokens[offset + i];
    size_t j;
    if (symbol < 0)
      continue;
    for (j = 0; j < *count && symbols[j] != symbol; j++)
      ;
    if (j == *count)
      symbols[(*count)++] = symbol;
  }
}

static size_t encode_rule_delta(unsigned char *destination, uint32_t value)
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

static void append_rule_occurrence(Compact_rewrite_bank bank,
                                   unsigned symbol, uint32_t rule)
{
  unsigned char encoded[5];
  size_t length;
  uint32_t block;
  struct cr_occurrence_block *tail;
  ensure_occurrence_symbol(bank, symbol);
  if (rule <= bank->occurrence_last_rules[symbol])
    fatal_error("compact_rewrite: nonmonotone occurrence rule");
  length = encode_rule_delta(
    encoded, rule - bank->occurrence_last_rules[symbol]);
  block = bank->occurrence_tails[symbol];
  if (block == CR_NONE ||
      bank->occurrence_blocks[block].used + length >
        CR_OCCURRENCE_BLOCK_PAYLOAD) {
    uint32_t added = new_occurrence_block(bank);
    if (block == CR_NONE)
      bank->occurrence_heads[symbol] = added;
    else
      bank->occurrence_blocks[block].next = added;
    bank->occurrence_tails[symbol] = added;
    block = added;
  }
  tail = &bank->occurrence_blocks[block];
  memcpy(tail->data + tail->used, encoded, length);
  tail->used += (uint16_t) length;
  tail->count++;
  bank->occurrence_last_rules[symbol] = rule;
  bank->occurrence_count++;
  bank->occurrence_stream_used += length;
}

/* Index each function symbol at most once per rule.  The postings answer the
   reverse question needed by interreduction: which old source sides might
   contain a redex for a new rule with this root symbol? */
static void index_rule_occurrences(Compact_rewrite_bank bank, uint32_t index)
{
  struct cr_rule *rule = &bank->rules[index];
  size_t capacity = (size_t) rule->left_length + rule->right_length;
  int32_t *symbols = capacity == 0 ? NULL :
    safe_malloc(capacity * sizeof(*symbols));
  size_t count = 0, i;
  if (rule->type == ORIENTED || rule->type == LEX_DEP_LR ||
      rule->type == LEX_DEP_BOTH)
    collect_occurrence_symbols(bank, rule->left_offset, rule->left_length,
                               symbols, &count);
  if (rule->type == LEX_DEP_RL || rule->type == LEX_DEP_BOTH)
    collect_occurrence_symbols(bank, rule->right_offset, rule->right_length,
                               symbols, &count);
  for (i = 0; i < count; i++) {
    unsigned symbol = (unsigned) symbols[i];
    append_rule_occurrence(bank, symbol, index);
  }
  safe_free(symbols);
}

Compact_rewrite_bank compact_rewrite_init_with_pool(Compact_term_pool pool)
{
  Compact_rewrite_bank bank = safe_calloc(1, sizeof(*bank));
  if (pool == NULL)
    fatal_error("compact_rewrite_init_with_pool: null term pool");
  bank->term_pool = pool;
  bank->tokens = compact_term_pool_tokens(pool);
  (void) new_node(bank, 0, 0);
  ensure_postings(bank);
  memset(&bank->postings[0], 0, sizeof(bank->postings[0]));
  bank->posting_count = 1;
  if (new_occurrence_block(bank) != CR_NONE)
    fatal_error("compact_rewrite: invalid occurrence block sentinel");
  ensure_rules(bank);
  memset(&bank->rules[0], 0, sizeof(bank->rules[0]));
  bank->rule_count = 1;
  update_peak(bank);
  return bank;
}

Compact_rewrite_bank compact_rewrite_init(void)
{
  Compact_term_pool pool = compact_term_pool_init();
  Compact_rewrite_bank bank = compact_rewrite_init_with_pool(pool);
  bank->owns_term_pool = TRUE;
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
  rule->left_offset = append_term_tokens(bank, clause, ARG(atom, 0),
                                          &rule->left_length);
  rule->right_offset = append_term_tokens(bank, clause, ARG(atom, 1),
                                           &rule->right_length);
  bank->tokens = compact_term_pool_tokens(bank->term_pool);
  if (type == ORIENTED || type == LEX_DEP_LR || type == LEX_DEP_BOTH)
    index_side(bank, index, rule->left_offset, rule->left_length, 1);
  if (type == LEX_DEP_RL || type == LEX_DEP_BOTH)
    index_side(bank, index, rule->right_offset, rule->right_length, 2);
  index_rule_occurrences(bank, index);
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
  ensure_rules(destination);
  if (destination->rule_count > UINT32_MAX)
    fatal_error("compact_rewrite: compacted rule offsets exceed 32 bits");
  index = (uint32_t) destination->rule_count++;
  rule = &destination->rules[index];
  memset(rule, 0, sizeof(*rule));
  rule->proof_id = old->proof_id;
  rule->type = old->type;
  rule->active = TRUE;
  rule->left_length = old->left_length;
  rule->right_length = old->right_length;
  if (destination->term_pool == source->term_pool) {
    rule->left_offset = old->left_offset;
    rule->right_offset = old->right_offset;
  }
  else {
    rule->left_offset = compact_term_pool_append(
      destination->term_pool, source->tokens + old->left_offset,
      old->left_length);
    rule->right_offset = compact_term_pool_append(
      destination->term_pool, source->tokens + old->right_offset,
      old->right_length);
  }
  destination->tokens = compact_term_pool_tokens(destination->term_pool);
  if (rule->type == ORIENTED || rule->type == LEX_DEP_LR ||
      rule->type == LEX_DEP_BOTH)
    index_side(destination, index, rule->left_offset, rule->left_length, 1);
  if (rule->type == LEX_DEP_RL || rule->type == LEX_DEP_BOTH)
    index_side(destination, index, rule->right_offset, rule->right_length, 2);
  index_rule_occurrences(destination, index);
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
  bank->tokens = compact_term_pool_tokens(bank->term_pool);
  replacement = bank->owns_term_pool ? compact_rewrite_init() :
    compact_rewrite_init_with_pool(bank->term_pool);
  for (i = 1; i < bank->rule_count; i++)
    if (bank->rules[i].active)
      copy_live_rule(replacement, bank, &bank->rules[i]);

  old = *bank;
  *bank = *replacement;
  safe_free(replacement);
  safe_free(old.nodes);
  safe_free(old.postings);
  safe_free(old.occurrence_blocks);
  safe_free(old.occurrence_heads);
  safe_free(old.occurrence_tails);
  safe_free(old.occurrence_last_rules);
  safe_free(old.rules);
  if (old.owns_term_pool)
    compact_term_pool_free(old.term_pool);
  safe_free(old.hash_keys);
  safe_free(old.hash_values);
  safe_free(old.query);
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

static uint32_t token_term_end(Compact_rewrite_bank bank, uint32_t position,
                               uint32_t end)
{
  int32_t code;
  int i, arity;
  if (position >= end)
    fatal_error("compact_rewrite: truncated occurrence term");
  code = bank->tokens[position++];
  if (code < 0)
    return position;
  arity = sn_to_arity(code);
  for (i = 0; i < arity; i++)
    position = token_term_end(bank, position, end);
  return position;
}

static BOOL token_match_rec(Compact_rewrite_bank bank,
                            uint32_t *pattern_position,
                            uint32_t pattern_end,
                            uint32_t *subject_position,
                            uint32_t subject_end,
                            uint32_t *binding_starts,
                            uint32_t *binding_ends)
{
  int32_t pattern_code, subject_code;
  int i, arity;
  if (*pattern_position >= pattern_end || *subject_position >= subject_end)
    return FALSE;
  pattern_code = bank->tokens[(*pattern_position)++];
  if (pattern_code < 0) {
    unsigned variable = (unsigned) (-pattern_code - 1);
    uint32_t start = *subject_position;
    uint32_t finish = token_term_end(bank, start, subject_end);
    if (variable >= MAX_VARS)
      fatal_error("compact_rewrite: occurrence variable exceeds MAX_VARS");
    if (binding_starts[variable] == UINT32_MAX) {
      binding_starts[variable] = start;
      binding_ends[variable] = finish;
    }
    else {
      size_t old_length = binding_ends[variable] - binding_starts[variable];
      size_t new_length = finish - start;
      if (old_length != new_length ||
          memcmp(bank->tokens + binding_starts[variable],
                 bank->tokens + start,
                 new_length * sizeof(*bank->tokens)) != 0)
        return FALSE;
    }
    *subject_position = finish;
    return TRUE;
  }
  subject_code = bank->tokens[(*subject_position)++];
  if (subject_code != pattern_code)
    return FALSE;
  arity = sn_to_arity(pattern_code);
  for (i = 0; i < arity; i++)
    if (!token_match_rec(bank, pattern_position, pattern_end,
                         subject_position, subject_end,
                         binding_starts, binding_ends))
      return FALSE;
  return TRUE;
}

static BOOL source_contains_pattern(Compact_rewrite_bank bank,
                                    uint32_t source_offset,
                                    uint32_t source_length,
                                    uint32_t pattern_offset,
                                    uint32_t pattern_length)
{
  uint32_t source_end = source_offset + source_length;
  uint32_t at;
  int32_t root = bank->tokens[pattern_offset];
  for (at = source_offset; at < source_end; at++) {
    uint32_t pattern_position, subject_position;
    uint32_t binding_starts[MAX_VARS], binding_ends[MAX_VARS];
    unsigned i;
    if (bank->tokens[at] != root)
      continue;
    for (i = 0; i < MAX_VARS; i++)
      binding_starts[i] = UINT32_MAX;
    pattern_position = pattern_offset;
    subject_position = at;
    if (token_match_rec(bank, &pattern_position,
                        pattern_offset + pattern_length,
                        &subject_position, source_end,
                        binding_starts, binding_ends) &&
        pattern_position == pattern_offset + pattern_length)
      return TRUE;
  }
  return FALSE;
}

static BOOL rule_contains_pattern(Compact_rewrite_bank bank,
                                  struct cr_rule *candidate,
                                  uint32_t pattern_offset,
                                  uint32_t pattern_length)
{
  if ((candidate->type == ORIENTED ||
       candidate->type == LEX_DEP_LR ||
       candidate->type == LEX_DEP_BOTH) &&
      source_contains_pattern(bank, candidate->left_offset,
                              candidate->left_length,
                              pattern_offset, pattern_length))
    return TRUE;
  return (candidate->type == LEX_DEP_RL ||
          candidate->type == LEX_DEP_BOTH) &&
    source_contains_pattern(bank, candidate->right_offset,
                            candidate->right_length,
                            pattern_offset, pattern_length);
}

static uint32_t decode_rule_delta(const struct cr_occurrence_block *block,
                                  uint16_t *position)
{
  uint32_t value = 0;
  unsigned shift = 0;
  while (*position < block->used) {
    unsigned char byte = block->data[(*position)++];
    if (shift == 28 && (byte & 0xf0U) != 0)
      fatal_error("compact_rewrite: corrupt occurrence delta");
    value |= (uint32_t) (byte & 0x7fU) << shift;
    if ((byte & 0x80U) == 0)
      return value;
    shift += 7;
    if (shift > 28)
      fatal_error("compact_rewrite: corrupt occurrence delta");
  }
  fatal_error("compact_rewrite: truncated occurrence delta");
  return 0;
}

void compact_rewrite_visit_overlaps(Compact_rewrite_bank bank,
                                    unsigned long long new_proof_id,
                                    Compact_rewrite_overlap_fn visit,
                                    void *context)
{
  uint32_t new_index = lookup_rule(bank, new_proof_id);
  struct cr_rule *rule;
  uint32_t pattern_offsets[2], pattern_lengths[2];
  unsigned pattern_count = 0, i;
  if (new_index == CR_NONE || visit == NULL)
    return;
  bank->tokens = compact_term_pool_tokens(bank->term_pool);
  rule = &bank->rules[new_index];
  if (rule->type == ORIENTED || rule->type == LEX_DEP_LR ||
      rule->type == LEX_DEP_BOTH) {
    pattern_offsets[pattern_count] = rule->left_offset;
    pattern_lengths[pattern_count++] = rule->left_length;
  }
  if (rule->type == LEX_DEP_RL || rule->type == LEX_DEP_BOTH) {
    pattern_offsets[pattern_count] = rule->right_offset;
    pattern_lengths[pattern_count++] = rule->right_length;
  }
  for (i = 0; i < pattern_count; i++) {
    uint32_t block;
    uint32_t rule_index = 0;
    unsigned symbol;
    int32_t root = bank->tokens[pattern_offsets[i]];
    if (root < 0)
      continue;
    symbol = (unsigned) root;
    if (symbol >= bank->occurrence_symbol_capacity)
      continue;
    for (block = bank->occurrence_heads[symbol]; block != CR_NONE;
         block = bank->occurrence_blocks[block].next) {
      const struct cr_occurrence_block *current;
      uint16_t position = 0;
      uint16_t entries = 0;
      if (block >= bank->occurrence_block_count)
        fatal_error("compact_rewrite: corrupt occurrence block");
      current = &bank->occurrence_blocks[block];
      while (position < current->used) {
        uint32_t delta = decode_rule_delta(current, &position);
        struct cr_rule *candidate;
        if (delta > UINT32_MAX - rule_index)
          fatal_error("compact_rewrite: occurrence rule overflow");
        rule_index += delta;
        if (rule_index == CR_NONE || rule_index >= bank->rule_count)
          fatal_error("compact_rewrite: corrupt occurrence rule");
        candidate = &bank->rules[rule_index];
        if (candidate->active && candidate->proof_id != new_proof_id &&
            rule_contains_pattern(bank, candidate,
                                  pattern_offsets[i], pattern_lengths[i]))
          visit(candidate->proof_id, context);
        entries++;
      }
      if (position != current->used || entries != current->count)
        fatal_error("compact_rewrite: corrupt occurrence block contents");
    }
  }
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

static void flatten_query_rec(Compact_rewrite_bank bank, Term term,
                              size_t *count)
{
  size_t at;
  int i;
  if (*count == bank->query_capacity) {
    bank->query_capacity = grow_capacity(
      bank->query_capacity, sizeof(*bank->query),
      "compact_rewrite: query overflow");
    bank->query = safe_realloc(
      bank->query, bank->query_capacity * sizeof(*bank->query));
  }
  at = (*count)++;
  bank->query[at].term = term;
  for (i = 0; i < ARITY(term); i++)
    flatten_query_rec(bank, ARG(term, i), count);
  if (*count > UINT32_MAX)
    fatal_error("compact_rewrite: query offsets exceed 32 bits");
  bank->query[at].end = (uint32_t) *count;
}

/* The legacy flatterm demodulator marks substituted subject fragments as
   already reduced.  Carry the same information on compact contracta so a
   rewritten RHS does not query the index again for every node copied from a
   normalized binding. */
static Term copy_reduced_binding(Term source, int reduced_flag)
{
  Term copy;
  int i;
  if (VARIABLE(source))
    copy = get_variable_term(VARNUM(source));
  else {
    copy = get_rigid_term_like(source);
    for (i = 0; i < ARITY(source); i++)
      ARG(copy, i) = copy_reduced_binding(ARG(source, i), reduced_flag);
  }
  term_flag_set(copy, reduced_flag);
  return copy;
}

static int nonvariable_term_count(Term term)
{
  int count = VARIABLE(term) ? 0 : 1;
  int i;
  for (i = 0; i < ARITY(term); i++)
    count += nonvariable_term_count(ARG(term, i));
  return count;
}

static Term build_contractum(Compact_rewrite_bank bank, uint32_t *position,
                             uint32_t end, Term *bindings,
                             int reduced_flag)
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
    return copy_reduced_binding(bindings[variable], reduced_flag);
  }
  arity = sn_to_arity(code);
  result = get_rigid_term_dangerously(code, arity);
  for (i = 0; i < arity; i++)
    ARG(result, i) = build_contractum(bank, position, end, bindings,
                                     reduced_flag);
  return result;
}

static BOOL try_leaf(Compact_rewrite_bank bank, uint32_t node, Term target,
                     Term *bindings, BOOL lex_order_vars,
                     int reduced_flag,
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
    contractum = build_contractum(bank, &position, end, bindings,
                                  reduced_flag);
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

static BOOL match_rewrite_edge(
  Compact_rewrite_bank bank, uint32_t node,
  struct cr_query_term *query, uint32_t position, uint32_t end,
  Term *bindings, unsigned *binding_trail, unsigned *trail_count,
  uint32_t *next_position)
{
  struct cr_node *edge = &bank->nodes[node];
  uint32_t i;
  for (i = 0; i < edge->token_length; i++) {
    int32_t code = bank->tokens[edge->token_offset + i];
    Term query_term;
    if (position >= end)
      return FALSE;
    query_term = query[position].term;
    if (code < 0) {
      unsigned variable = (unsigned) (-code - 1);
      if (variable >= MAX_VARS)
        fatal_error("compact_rewrite: variable exceeds MAX_VARS");
      if (bindings[variable] == NULL) {
        bindings[variable] = query_term;
        binding_trail[(*trail_count)++] = variable;
      }
      else if (!term_ident(bindings[variable], query_term))
        return FALSE;
      position = query[position].end;
    }
    else {
      if (VARIABLE(query_term) || SYMNUM(query_term) != code)
        return FALSE;
      position++;
    }
  }
  *next_position = position;
  return TRUE;
}

static void undo_rewrite_bindings(Term *bindings,
                                  const unsigned *binding_trail,
                                  unsigned *trail_count,
                                  unsigned trail_mark)
{
  while (*trail_count != trail_mark)
    bindings[binding_trail[--(*trail_count)]] = NULL;
}

static BOOL retrieve_rec(Compact_rewrite_bank bank, uint32_t node,
                         struct cr_query_term *query, uint32_t position,
                         uint32_t end, Term *bindings,
                         unsigned *binding_trail, unsigned *trail_count,
                         Term target,
                         BOOL lex_order_vars,
                         int reduced_flag,
                         struct cr_match_result *result)
{
  uint32_t child;
  if (position == end)
    return try_leaf(bank, node, target, bindings, lex_order_vars,
                    reduced_flag, result);
  for (child = bank->nodes[node].first_child; child != CR_NONE;
       child = bank->nodes[child].next_sibling) {
    unsigned trail_mark = *trail_count;
    uint32_t next_position = position;
    BOOL found = FALSE;
    if (match_rewrite_edge(bank, child, query, position, end, bindings,
                           binding_trail, trail_count, &next_position))
      found = retrieve_rec(bank, child, query, next_position, end,
                           bindings, binding_trail, trail_count, target,
                           lex_order_vars, reduced_flag, result);
    undo_rewrite_bindings(bindings, binding_trail, trail_count, trail_mark);
    if (found)
      return TRUE;
  }
  return FALSE;
}

static struct cr_match_result find_rewrite(Compact_rewrite_bank bank,
                                           Term target,
                                           BOOL lex_order_vars,
                                           int reduced_flag)
{
  struct cr_match_result result;
  size_t count = 0;
  Term bindings[MAX_VARS];
  unsigned binding_trail[MAX_VARS];
  unsigned trail_count = 0;
  memset(&result, 0, sizeof(result));
  memset(bindings, 0, sizeof(bindings));
  flatten_query_rec(bank, target, &count);
  if (count > 0)
    retrieve_rec(bank, 0, bank->query, 0, (uint32_t) count, bindings,
                 binding_trail, &trail_count, target, lex_order_vars,
                 reduced_flag, &result);
  return result;
}

static Term normalize_term(Compact_rewrite_bank bank, Term term,
                           int *step_limit, int size_limit,
                           int *current_size, int *sequence,
                           I3list *steps, BOOL lex_order_vars,
                           BOOL count_stats, int reduced_flag)
{
  int sequence_save;
  int i;
  struct cr_match_result match;
  if (*step_limit == 0 || *current_size > size_limit || VARIABLE(term))
    return term;
  if (term_flag(term, reduced_flag)) {
    *sequence += nonvariable_term_count(term);
    return term;
  }
  sequence_save = *sequence;
  for (i = 0; i < ARITY(term); i++)
    ARG(term, i) = normalize_term(bank, ARG(term, i), step_limit, size_limit,
                                  current_size, sequence, steps,
                                  lex_order_vars, count_stats, reduced_flag);
  if (*step_limit == 0 || *current_size > size_limit)
    return term;
  if (count_stats)
    bank->attempts++;
  (*sequence)++;
  match = find_rewrite(bank, term, lex_order_vars, reduced_flag);
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
                        count_stats, reduced_flag);
}

void compact_rewrite_clause(Compact_rewrite_bank bank, Topform clause,
                            int step_limit, int increase_limit,
                            BOOL lex_order_vars, BOOL count_stats)
{
  Literals literal;
  I3list steps = NULL;
  int sequence = 0;
  int reduced_flag;
  if (bank == NULL || bank->active_rules == 0 || clause == NULL)
    return;
  bank->tokens = compact_term_pool_tokens(bank->term_pool);
  reduced_flag = claim_term_flag();
  step_limit = step_limit == -1 ? INT_MAX : step_limit;
  increase_limit = increase_limit == -1 ? INT_MAX : increase_limit;
  for (literal = clause->literals; literal != NULL; literal = literal->next) {
    int current_size = symbol_count(literal->atom);
    int size_limit = increase_limit == INT_MAX ? INT_MAX :
                     current_size + increase_limit;
    literal->atom = normalize_term(bank, literal->atom, &step_limit,
                                   size_limit, &current_size, &sequence,
                                   &steps, lex_order_vars, count_stats,
                                   reduced_flag);
    term_flag_clear_recursively(literal->atom, reduced_flag);
    if (current_size > size_limit)
      increase_limit = -1;
  }
  upward_clause_links(clause);
  if (steps != NULL) {
    steps = reverse_i3list(steps);
    clause->justification = append_just(clause->justification,
                                        demod_just(steps));
  }
  release_term_flag(reduced_flag);
}

void compact_rewrite_get_stats(Compact_rewrite_bank bank,
                               struct compact_rewrite_stats *stats)
{
  struct compact_term_pool_stats terms;
  if (stats == NULL)
    return;
  memset(stats, 0, sizeof(*stats));
  if (bank == NULL)
    return;
  compact_term_pool_get_stats(bank->term_pool, &terms);
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
  stats->occurrence_bytes =
    bank->occurrence_block_capacity * sizeof(*bank->occurrence_blocks) +
    bank->occurrence_symbol_capacity *
      (sizeof(*bank->occurrence_heads) + sizeof(*bank->occurrence_tails) +
       sizeof(*bank->occurrence_last_rules));
  stats->occurrence_stream_used = bank->occurrence_stream_used;
  stats->occurrence_stream_bytes =
    bank->occurrence_block_capacity * sizeof(*bank->occurrence_blocks);
  stats->rule_bytes = bank->rule_capacity * sizeof(*bank->rules);
  stats->term_bytes = bank->owns_term_pool ? terms.token_bytes : 0;
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
  safe_free(bank->occurrence_blocks);
  safe_free(bank->occurrence_heads);
  safe_free(bank->occurrence_tails);
  safe_free(bank->occurrence_last_rules);
  safe_free(bank->rules);
  if (bank->owns_term_pool)
    compact_term_pool_free(bank->term_pool);
  safe_free(bank->hash_keys);
  safe_free(bank->hash_values);
  safe_free(bank->query);
  safe_free(bank);
}
