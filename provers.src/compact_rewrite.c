#include "compact_rewrite.h"
#include "compact_id_map.h"

#include <limits.h>
#include <stdint.h>
#include <string.h>

static unsigned Compaction_stale_pct = 25;
static unsigned long long Deep_child_cache_budget_bytes = 0;

#define CR_NONE 0U
#define CR_OCCURRENCE_BLOCK_PAYLOAD 248
#define CR_BINDING_WORDS ((MAX_VARS + 63U) / 64U)
#define CR_RULE_PROOF_ID_MASK UINT64_C(0x0fffffffffffffff)
#define CR_RULE_TYPE_SHIFT 60
#define CR_RULE_TYPE_MASK UINT64_C(0x7000000000000000)
#define CR_RULE_ACTIVE UINT64_C(0x8000000000000000)
#define CR_DEEP_CHILD_CACHE_MAX_BYTES (UINT64_C(8) * 1024 * 1024)
#define CR_DEEP_CHILD_CACHE_MIN_SCAN 32
#define CR_DEEP_CHILD_CACHE_EMPTY UINT32_MAX
#define CR_SUBJECT_BLOCK_NODES 1024
#define CR_GROWTH_STALE_FLOOR 16384ULL

struct cr_node {
  Compact_term_slice tokens;
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

/* A bounded positive/negative cache for rigid children below the radix root.
   It is never an answer authority: collision and eviction are ordinary cache
   misses followed by the exact ordered sibling traversal.  CR_NONE is a
   cached negative result; parent==CR_DEEP_CHILD_CACHE_EMPTY is an unused
   slot. */
struct cr_deep_child_cache_entry {
  uint32_t parent;
  int32_t code;
  uint32_t child;
};

/* Transient rewrite subjects need stable links while subterms are replaced.
   Recycle these nodes inside the bank instead of sending every atom node
   through LADR's general allocator. */
struct cr_subject_block {
  struct cr_subject_block *next;
  struct flatterm nodes[CR_SUBJECT_BLOCK_NODES];
};

struct cr_rule {
  unsigned long long proof_id_flags;
  Compact_term_slice left;
  Compact_term_slice right;
};

typedef char compact_rewrite_node_must_remain_24_bytes[
  sizeof(struct cr_node) == 24 ? 1 : -1];
typedef char compact_rewrite_rule_must_remain_24_bytes[
  sizeof(struct cr_rule) == 24 ? 1 : -1];

struct compact_rewrite_bank {
  struct cr_node *nodes;
  size_t node_count;
  size_t node_capacity;
  int32_t *node_first_codes;
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
  BOOL owns_term_pool;
  Compact_id_map id_map;
  const int32_t *query_token_base;
  unsigned long long query_logical_base;
  struct cr_subject_block *subject_blocks;
  Flatterm subject_free_nodes;
  size_t subject_block_count;
  size_t subject_live_nodes;
  size_t subject_peak_live_nodes;
  uint32_t *child_cache;
  size_t child_cache_capacity;
  struct cr_deep_child_cache_entry *deep_child_cache;
  size_t deep_child_cache_capacity;
  uint64_t *deep_child_cache_parents;
  size_t deep_child_cache_parent_words;
  size_t deep_child_cache_parent_count;
  BOOL deep_child_cache_growth_blocked;
  unsigned long long deep_child_cache_budget_bytes;
  unsigned long long variable_sibling_checks;
  unsigned long long rigid_sibling_checks;
  unsigned long long deep_child_cache_lookups;
  unsigned long long deep_child_cache_hits;
  unsigned long long deep_child_cache_misses;
  unsigned long long deep_child_cache_replacements;
  unsigned long long deep_child_cache_growth_denials;
  unsigned long long active_rules;
  unsigned long long peak_rules;
  unsigned long long retired_rules;
  unsigned long long attempts;
  unsigned long long rewrites;
  unsigned long long subject_atoms;
  unsigned long long subject_initial_nodes;
  unsigned long long subject_target_nodes;
  unsigned long long peak_bytes;
  unsigned long long compactions;
  unsigned long long bytes_reclaimed;
};

struct cr_match_result {
  BOOL found;
  Flatterm contractum;
  unsigned long long proof_id;
  int direction;
};

static unsigned long long rule_proof_id(const struct cr_rule *rule)
{
  return rule->proof_id_flags & CR_RULE_PROOF_ID_MASK;
}

static int rule_type(const struct cr_rule *rule)
{
  return (int) ((rule->proof_id_flags & CR_RULE_TYPE_MASK) >>
                CR_RULE_TYPE_SHIFT);
}

static BOOL rule_active(const struct cr_rule *rule)
{
  return (rule->proof_id_flags & CR_RULE_ACTIVE) != 0;
}

static void set_rule_metadata(struct cr_rule *rule,
                              unsigned long long proof_id,
                              int type, BOOL active)
{
  if (proof_id == 0 || proof_id > CR_RULE_PROOF_ID_MASK ||
      type < 0 || type > 7)
    fatal_error("compact_rewrite: rule metadata overflow");
  rule->proof_id_flags = proof_id |
    ((unsigned long long) type << CR_RULE_TYPE_SHIFT) |
    (active ? CR_RULE_ACTIVE : 0);
}

static void set_rule_active(struct cr_rule *rule, BOOL active)
{
  if (active)
    rule->proof_id_flags |= CR_RULE_ACTIVE;
  else
    rule->proof_id_flags &= ~CR_RULE_ACTIVE;
}

static size_t grow_capacity(size_t current, size_t item_size,
                            char *message)
{
  size_t next = current == 0 ? 64 : current * 2;
  if (next < current || next > SIZE_MAX / item_size)
    fatal_error(message);
  return next;
}

static size_t grow_record_capacity(size_t current, size_t item_size,
                                   char *message)
{
  size_t increment;
  size_t next;
  if (current == 0)
    return 64;
  increment = current / 4;
  if (increment < 64)
    increment = 64;
  if (increment > SIZE_MAX - current)
    fatal_error(message);
  next = current + increment;
  if (next > SIZE_MAX / item_size)
    fatal_error(message);
  return next;
}

static size_t grow_node_capacity(size_t current, size_t item_size,
                                 char *message)
{
  size_t increment;
  size_t next;
  if (current == 0)
    return 64;
  increment = current / 4;
  if (increment < 16)
    increment = 16;
  if (increment > SIZE_MAX - current)
    fatal_error(message);
  next = current + increment;
  if (next > SIZE_MAX / item_size)
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
    bank->node_capacity * sizeof(*bank->node_first_codes) +
    bank->posting_capacity * sizeof(*bank->postings) +
    bank->occurrence_block_capacity * sizeof(*bank->occurrence_blocks) +
    bank->occurrence_symbol_capacity *
      (sizeof(*bank->occurrence_heads) + sizeof(*bank->occurrence_tails) +
       sizeof(*bank->occurrence_last_rules)) +
    bank->rule_capacity * sizeof(*bank->rules) +
    (bank->owns_term_pool ? terms.total_bytes : 0) +
    compact_id_map_bytes(bank->id_map) +
    bank->subject_block_count * sizeof(*bank->subject_blocks) +
    bank->child_cache_capacity * sizeof(*bank->child_cache) +
    bank->deep_child_cache_capacity * sizeof(*bank->deep_child_cache) +
    bank->deep_child_cache_parent_words *
      sizeof(*bank->deep_child_cache_parents);
}

static void update_peak(Compact_rewrite_bank bank)
{
  unsigned long long bytes = bank_bytes(bank);
  if (bytes > bank->peak_bytes)
    bank->peak_bytes = bytes;
  if (bank->active_rules > bank->peak_rules)
    bank->peak_rules = bank->active_rules;
}

static Flatterm subject_get_node(Compact_rewrite_bank bank)
{
  Flatterm node;
  if (bank->subject_free_nodes == NULL) {
    struct cr_subject_block *block = safe_malloc(sizeof(*block));
    int i;
    block->next = bank->subject_blocks;
    bank->subject_blocks = block;
    bank->subject_block_count++;
    for (i = 0; i < CR_SUBJECT_BLOCK_NODES; i++) {
      block->nodes[i].next = bank->subject_free_nodes;
      bank->subject_free_nodes = &block->nodes[i];
    }
    update_peak(bank);
  }
  node = bank->subject_free_nodes;
  bank->subject_free_nodes = node->next;
  node->prev = NULL;
  node->next = NULL;
  node->varnum_bound_to = -1;
  node->alternative = NULL;
  node->reduced_flag = FALSE;
  node->size = 0;
  bank->subject_live_nodes++;
  if (bank->subject_live_nodes > bank->subject_peak_live_nodes)
    bank->subject_peak_live_nodes = bank->subject_live_nodes;
  return node;
}

static void subject_free_node(Compact_rewrite_bank bank, Flatterm node)
{
  if (bank->subject_live_nodes == 0)
    fatal_error("compact_rewrite: subject arena underflow");
  node->next = bank->subject_free_nodes;
  bank->subject_free_nodes = node;
  bank->subject_live_nodes--;
}

static void subject_zap(Compact_rewrite_bank bank, Flatterm subject)
{
  Flatterm current = subject;
  Flatterm after = subject->end->next;
  while (current != after) {
    Flatterm next = current->next;
    subject_free_node(bank, current);
    current = next;
  }
}

static void subject_free_blocks(Compact_rewrite_bank bank)
{
  struct cr_subject_block *block = bank->subject_blocks;
  if (bank->subject_live_nodes != 0)
    fatal_error("compact_rewrite: live subject nodes at bank destruction");
  while (block != NULL) {
    struct cr_subject_block *next = block->next;
    safe_free(block);
    block = next;
  }
  bank->subject_blocks = NULL;
  bank->subject_free_nodes = NULL;
  bank->subject_block_count = 0;
}

static Flatterm subject_from_term(Compact_rewrite_bank bank, Term term)
{
  Flatterm result = subject_get_node(bank);
  Flatterm tail = result;
  int i;
  result->private_symbol = term->private_symbol;
  ARITY(result) = ARITY(term);
  result->size = 1;
  for (i = 0; i < ARITY(term); i++) {
    Flatterm child = subject_from_term(bank, ARG(term, i));
    result->size += child->size;
    tail->next = child;
    child->prev = tail;
    tail = child->end;
  }
  result->end = tail;
  return result;
}

static Flatterm subject_copy(Compact_rewrite_bank bank, Flatterm source)
{
  Flatterm result = subject_get_node(bank);
  Flatterm tail = result;
  Flatterm child = source->next;
  int i;
  result->private_symbol = source->private_symbol;
  ARITY(result) = ARITY(source);
  result->size = 1;
  for (i = 0; i < ARITY(source); i++) {
    Flatterm copy = subject_copy(bank, child);
    result->size += copy->size;
    tail->next = copy;
    copy->prev = tail;
    tail = copy->end;
    child = child->end->next;
  }
  result->end = tail;
  return result;
}

static void ensure_nodes(Compact_rewrite_bank bank)
{
  if (bank->node_count == bank->node_capacity) {
    size_t old_capacity = bank->node_capacity;
    bank->node_capacity = grow_node_capacity(
      bank->node_capacity, sizeof(*bank->nodes),
      "compact_rewrite: node overflow");
    bank->nodes = safe_realloc(bank->nodes,
                                bank->node_capacity * sizeof(*bank->nodes));
    bank->node_first_codes = safe_realloc(
      bank->node_first_codes,
      bank->node_capacity * sizeof(*bank->node_first_codes));
    memset(bank->node_first_codes + old_capacity, 0,
           (bank->node_capacity - old_capacity) *
             sizeof(*bank->node_first_codes));
  }
}

static void ensure_postings(Compact_rewrite_bank bank)
{
  if (bank->posting_count == bank->posting_capacity) {
    bank->posting_capacity = grow_record_capacity(
      bank->posting_capacity, sizeof(*bank->postings),
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
    bank->rule_capacity = grow_record_capacity(
      bank->rule_capacity, sizeof(*bank->rules),
      "compact_rewrite: rule overflow");
    bank->rules = safe_realloc(bank->rules,
                                bank->rule_capacity * sizeof(*bank->rules));
  }
}

static uint32_t lookup_rule(Compact_rewrite_bank bank,
                            unsigned long long proof_id)
{
  uint32_t value = CR_NONE;
  return bank != NULL &&
    compact_id_map_get(bank->id_map, proof_id, &value) ? value : CR_NONE;
}

static Compact_term_slice append_term_tokens(Compact_rewrite_bank bank,
                                             Topform clause, Term term)
{
  return compact_term_pool_intern_slice(bank->term_pool, clause->id,
                                        clause->literals, term);
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
                         Compact_term_slice tokens)
{
  uint32_t node;
  ensure_nodes(bank);
  if (bank->node_count > UINT32_MAX)
    fatal_error("compact_rewrite: node offsets exceed 32 bits");
  node = (uint32_t) bank->node_count++;
  memset(&bank->nodes[node], 0, sizeof(bank->nodes[node]));
  bank->nodes[node].tokens = tokens;
  if (compact_term_slice_length(tokens) != 0)
    bank->node_first_codes[node] =
      compact_term_pool_slice_tokens(bank->term_pool, tokens)[0];
  return node;
}

static int32_t first_code(Compact_rewrite_bank bank, uint32_t node)
{
  /* Every nonroot node is created from a nonempty slice.  Rechecking the
     packed length here put a function call in the innermost retrieval loop
     (about 189 million calls by given 600 on CHAT).  Construction, radix
     splitting, compaction, and checkpoint rebuilding are the authorities for
     this invariant; retrieval only needs the cached first token. */
  return bank->node_first_codes[node];
}

static const int32_t *hot_slice_tokens(Compact_rewrite_bank bank,
                                       Compact_term_slice slice)
{
  unsigned long long offset =
    slice & COMPACT_TERM_SLICE_OFFSET_MAX;
  unsigned long long local;
  if (offset < bank->query_logical_base)
    fatal_error("compact_rewrite: hot slice precedes logical base");
  local = offset - bank->query_logical_base;
  if (local > SIZE_MAX)
    fatal_error("compact_rewrite: hot slice exceeds address space");
  return bank->query_token_base + (size_t) local;
}

static uint32_t hot_slice_length(Compact_term_slice slice)
{
  return (uint32_t) (slice >> COMPACT_TERM_SLICE_OFFSET_BITS);
}

static void ensure_child_cache(Compact_rewrite_bank bank, int32_t code)
{
  size_t old = bank->child_cache_capacity;
  size_t capacity = old == 0 ? 64 : old;
  while ((size_t) code >= capacity) {
    if (capacity > SIZE_MAX / 2)
      fatal_error("compact_rewrite: root child cache overflow");
    capacity *= 2;
  }
  if (capacity != old) {
    bank->child_cache = safe_realloc(
      bank->child_cache, capacity * sizeof(*bank->child_cache));
    memset(bank->child_cache + old, 0,
           (capacity - old) * sizeof(*bank->child_cache));
    bank->child_cache_capacity = capacity;
  }
}

static void child_cache_put(Compact_rewrite_bank bank, uint32_t parent,
                            int32_t code, uint32_t child)
{
  if (parent != 0 || code < 0 || child == CR_NONE)
    return;
  ensure_child_cache(bank, code);
  bank->child_cache[code] = child;
}

static BOOL child_cache_get(Compact_rewrite_bank bank, uint32_t parent,
                            int32_t code, uint32_t *child)
{
  uint32_t cached;
  cached = parent == 0 && code >= 0 &&
    (size_t) code < bank->child_cache_capacity ? bank->child_cache[code] : 0;
  if (cached != CR_NONE && first_code(bank, cached) == code) {
    *child = cached;
    return TRUE;
  }
  return FALSE;
}

static size_t deep_child_cache_slot(uint32_t parent, int32_t code,
                                    size_t capacity)
{
  uint64_t mixed =
    (uint64_t) parent * UINT64_C(0x9e3779b97f4a7c15) ^
    (uint64_t) (uint32_t) code * UINT64_C(0xbf58476d1ce4e5b9);
  mixed ^= mixed >> 32;
  return (size_t) mixed & (capacity - 1);
}

static BOOL deep_child_cache_parent_enabled(Compact_rewrite_bank bank,
                                            uint32_t parent)
{
  size_t word = (size_t) parent / 64;
  return word < bank->deep_child_cache_parent_words &&
    (bank->deep_child_cache_parents[word] &
     (UINT64_C(1) << (parent % 64))) != 0;
}

static BOOL deep_child_cache_enable_parent(Compact_rewrite_bank bank,
                                           uint32_t parent)
{
  size_t word = (size_t) parent / 64;
  if (parent == 0 || bank->deep_child_cache_budget_bytes == 0)
    return FALSE;
  if (word >= bank->deep_child_cache_parent_words) {
    size_t old_words = bank->deep_child_cache_parent_words;
    size_t new_words = (bank->node_count + 63) / 64;
    unsigned long long entry_bytes =
      bank->deep_child_cache_capacity * sizeof(*bank->deep_child_cache);
    unsigned long long new_parent_bytes;
    if (new_words <= word)
      new_words = word + 1;
    if (new_words > SIZE_MAX / sizeof(*bank->deep_child_cache_parents))
      fatal_error("compact_rewrite: deep child parent map overflow");
    new_parent_bytes = (unsigned long long) new_words *
      sizeof(*bank->deep_child_cache_parents);
    if (new_parent_bytes > bank->deep_child_cache_budget_bytes ||
        entry_bytes > bank->deep_child_cache_budget_bytes -
          new_parent_bytes) {
      bank->deep_child_cache_growth_denials++;
      return FALSE;
    }
    bank->deep_child_cache_parents = safe_realloc(
      bank->deep_child_cache_parents,
      new_words * sizeof(*bank->deep_child_cache_parents));
    memset(bank->deep_child_cache_parents + old_words, 0,
           (new_words - old_words) *
             sizeof(*bank->deep_child_cache_parents));
    bank->deep_child_cache_parent_words = new_words;
  }
  if (!deep_child_cache_parent_enabled(bank, parent)) {
    bank->deep_child_cache_parents[word] |=
      UINT64_C(1) << (parent % 64);
    bank->deep_child_cache_parent_count++;
  }
  return TRUE;
}

static void deep_child_cache_put(Compact_rewrite_bank bank,
                                 uint32_t parent, int32_t code,
                                 uint32_t child, BOOL count_replacement)
{
  struct cr_deep_child_cache_entry *entry;
  size_t slot;
  if (parent == 0 || code < 0 ||
      bank->deep_child_cache_capacity == 0 ||
      !deep_child_cache_parent_enabled(bank, parent))
    return;
  slot = deep_child_cache_slot(
    parent, code, bank->deep_child_cache_capacity);
  entry = &bank->deep_child_cache[slot];
  if (count_replacement &&
      entry->parent != CR_DEEP_CHILD_CACHE_EMPTY &&
      (entry->parent != parent || entry->code != code))
    bank->deep_child_cache_replacements++;
  entry->parent = parent;
  entry->code = code;
  entry->child = child;
}

static void maybe_grow_deep_child_cache(Compact_rewrite_bank bank)
{
  struct cr_deep_child_cache_entry *old, *replacement;
  size_t old_capacity = bank->deep_child_cache_capacity;
  size_t desired = 64, target, max_entries, i;
  unsigned long long parent_bytes =
    bank->deep_child_cache_parent_words *
      sizeof(*bank->deep_child_cache_parents);
  if (bank->deep_child_cache_growth_blocked)
    return;
  if (parent_bytes >= bank->deep_child_cache_budget_bytes) {
    bank->deep_child_cache_growth_blocked = TRUE;
    bank->deep_child_cache_growth_denials++;
    return;
  }
  max_entries = (size_t) ((bank->deep_child_cache_budget_bytes -
    parent_bytes) /
    sizeof(*bank->deep_child_cache));
  target = bank->deep_child_cache_parent_count > SIZE_MAX / 16 ?
    SIZE_MAX : bank->deep_child_cache_parent_count * 16;
  while (desired < target && desired <= SIZE_MAX / 2)
    desired *= 2;
  while (desired > max_entries && desired > 1)
    desired /= 2;
  if (desired <= old_capacity)
    return;
  if (desired < 64) {
    bank->deep_child_cache_growth_blocked = TRUE;
    bank->deep_child_cache_growth_denials++;
    return;
  }
  replacement = safe_malloc(desired * sizeof(*replacement));
  for (i = 0; i < desired; i++) {
    replacement[i].parent = CR_DEEP_CHILD_CACHE_EMPTY;
    replacement[i].code = 0;
    replacement[i].child = CR_NONE;
  }
  old = bank->deep_child_cache;
  bank->deep_child_cache = replacement;
  bank->deep_child_cache_capacity = desired;
  for (i = 0; i < old_capacity; i++)
    if (old[i].parent != CR_DEEP_CHILD_CACHE_EMPTY)
      deep_child_cache_put(bank, old[i].parent, old[i].code,
                           old[i].child, FALSE);
  safe_free(old);
  update_peak(bank);
}

static BOOL deep_child_cache_get(Compact_rewrite_bank bank,
                                 uint32_t parent, int32_t code,
                                 uint32_t *child)
{
  struct cr_deep_child_cache_entry *entry;
  size_t slot;
  bank->deep_child_cache_lookups++;
  if (bank->deep_child_cache_capacity == 0) {
    bank->deep_child_cache_misses++;
    return FALSE;
  }
  slot = deep_child_cache_slot(
    parent, code, bank->deep_child_cache_capacity);
  entry = &bank->deep_child_cache[slot];
  if (entry->parent == parent && entry->code == code) {
    bank->deep_child_cache_hits++;
    *child = entry->child;
    return TRUE;
  }
  bank->deep_child_cache_misses++;
  return FALSE;
}

static void cached_child_put(Compact_rewrite_bank bank, uint32_t parent,
                             int32_t code, uint32_t child)
{
  if (parent == 0)
    child_cache_put(bank, parent, code, child);
  else
    deep_child_cache_put(bank, parent, code, child, TRUE);
}

/* Insert an immutable prefix-token slice as a radix path.  Siblings remain
   ordered by their first token exactly as in the former token-per-node trie.
   Splitting only changes the representation: terminal posting order is still
   the order in which rewrite sides were admitted. */
static uint32_t insert_token_path(Compact_rewrite_bank bank, uint32_t root,
                                  Compact_term_slice slice)
{
  uint32_t parent = root;
  uint32_t position = 0;
  uint32_t length = compact_term_slice_length(slice);
  const int32_t *tokens = compact_term_pool_slice_tokens(
    bank->term_pool, slice);
  while (position < length) {
    uint32_t current = bank->nodes[parent].first_child;
    uint32_t previous = CR_NONE;
    int32_t wanted = tokens[position];
    while (current != CR_NONE &&
           code_compare(first_code(bank, current), wanted) < 0) {
      previous = current;
      current = bank->nodes[current].next_sibling;
    }
    if (current == CR_NONE || first_code(bank, current) != wanted) {
      Compact_term_slice suffix;
      uint32_t added;
      if (!compact_term_slice_subslice(
            slice, position, length - position, &suffix))
        fatal_error("compact_rewrite: invalid radix suffix");
      added = new_node(bank, suffix);
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
      cached_child_put(bank, parent, wanted, added);
      return added;
    }
    else {
      Compact_term_slice old_slice = bank->nodes[current].tokens;
      const int32_t *old_tokens = compact_term_pool_slice_tokens(
        bank->term_pool, old_slice);
      uint32_t old_length = compact_term_slice_length(old_slice);
      uint32_t common = 0;
      while (common < old_length && position + common < length &&
             old_tokens[common] == tokens[position + common])
        common++;
      if (common == old_length) {
        cached_child_put(bank, parent, wanted, current);
        position += common;
        parent = current;
      }
      else {
        uint32_t old_next = bank->nodes[current].next_sibling;
        Compact_term_slice prefix, old_suffix;
        uint32_t split;
        uint32_t added;
        if (common == 0)
          fatal_error("compact_rewrite: invalid zero-length radix split");
        if (!compact_term_slice_subslice(old_slice, 0, common, &prefix) ||
            !compact_term_slice_subslice(
              old_slice, common, old_length - common, &old_suffix))
          fatal_error("compact_rewrite: invalid radix split slices");
        split = new_node(bank, prefix);
        bank->nodes[split].next_sibling = old_next;
        if (previous == CR_NONE)
          bank->nodes[parent].first_child = split;
        else
          bank->nodes[previous].next_sibling = split;
        bank->nodes[current].tokens = old_suffix;
        bank->node_first_codes[current] =
          compact_term_pool_slice_tokens(bank->term_pool, old_suffix)[0];
        bank->nodes[current].next_sibling = CR_NONE;
        bank->nodes[split].first_child = current;
        cached_child_put(bank, parent, wanted, split);
        cached_child_put(bank, split, first_code(bank, current), current);
        position += common;
        if (position == length)
          return split;
        {
          Compact_term_slice suffix;
          if (!compact_term_slice_subslice(
                slice, position, length - position, &suffix))
            fatal_error("compact_rewrite: invalid added radix suffix");
          added = new_node(bank, suffix);
        }
        if (code_compare(first_code(bank, added),
                         first_code(bank, current)) < 0) {
          bank->nodes[added].next_sibling = current;
          bank->nodes[split].first_child = added;
        }
        else
          bank->nodes[current].next_sibling = added;
        cached_child_put(bank, split, first_code(bank, added), added);
        return added;
      }
    }
  }
  return parent;
}

static void index_side(Compact_rewrite_bank bank, uint32_t rule,
                       Compact_term_slice slice, int direction)
{
  uint32_t node = insert_token_path(bank, 0, slice);
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
                                       Compact_term_slice slice,
                                       int32_t *symbols, size_t *count)
{
  const int32_t *tokens = compact_term_pool_slice_tokens(
    bank->term_pool, slice);
  uint32_t length = compact_term_slice_length(slice);
  uint32_t i;
  for (i = 0; i < length; i++) {
    int32_t symbol = tokens[i];
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
  int type = rule_type(rule);
  size_t capacity = (size_t) compact_term_slice_length(rule->left) +
    compact_term_slice_length(rule->right);
  int32_t *symbols = capacity == 0 ? NULL :
    safe_malloc(capacity * sizeof(*symbols));
  size_t count = 0, i;
  if (type == ORIENTED || type == LEX_DEP_LR || type == LEX_DEP_BOTH)
    collect_occurrence_symbols(bank, rule->left, symbols, &count);
  if (type == LEX_DEP_RL || type == LEX_DEP_BOTH)
    collect_occurrence_symbols(bank, rule->right, symbols, &count);
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
  bank->deep_child_cache_budget_bytes = Deep_child_cache_budget_bytes;
  bank->id_map = compact_id_map_init(1);
  (void) new_node(bank, 0);
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
  if (bank == NULL || clause == NULL || clause->id == 0 ||
      type == NOT_DEMODULATOR || lookup_rule(bank, clause->id) != CR_NONE)
    return FALSE;
  ensure_rules(bank);
  if (bank->rule_count > UINT32_MAX)
    fatal_error("compact_rewrite: rule offsets exceed 32 bits");
  index = (uint32_t) bank->rule_count++;
  rule = &bank->rules[index];
  memset(rule, 0, sizeof(*rule));
  atom = clause->literals->atom;
  rule->left = append_term_tokens(bank, clause, ARG(atom, 0));
  rule->right = append_term_tokens(bank, clause, ARG(atom, 1));
  set_rule_metadata(rule, clause->id, type, TRUE);
  if (type == ORIENTED || type == LEX_DEP_LR || type == LEX_DEP_BOTH)
    index_side(bank, index, rule->left, 1);
  if (type == LEX_DEP_RL || type == LEX_DEP_BOTH)
    index_side(bank, index, rule->right, 2);
  index_rule_occurrences(bank, index);
  if (!compact_id_map_put(bank->id_map, clause->id, &index))
    fatal_error("compact_rewrite: duplicate proof ID");
  bank->active_rules++;
  update_peak(bank);
  return TRUE;
}

static void copy_live_rule(Compact_rewrite_bank destination,
                           Compact_rewrite_bank source,
                           const struct cr_rule *old)
{
  struct cr_rule saved = *old;
  struct cr_rule *rule;
  uint32_t index;
  int type = rule_type(&saved);
  ensure_rules(destination);
  if (destination->rule_count > UINT32_MAX)
    fatal_error("compact_rewrite: compacted rule offsets exceed 32 bits");
  index = (uint32_t) destination->rule_count++;
  rule = &destination->rules[index];
  memset(rule, 0, sizeof(*rule));
  set_rule_metadata(rule, rule_proof_id(&saved), type, TRUE);
  if (destination->term_pool == source->term_pool) {
    rule->left = saved.left;
    rule->right = saved.right;
  }
  else {
    rule->left = compact_term_pool_append_slice(
      destination->term_pool,
      compact_term_pool_slice_tokens(source->term_pool, saved.left),
      compact_term_slice_length(saved.left));
    rule->right = compact_term_pool_append_slice(
      destination->term_pool,
      compact_term_pool_slice_tokens(source->term_pool, saved.right),
      compact_term_slice_length(saved.right));
  }
  if (type == ORIENTED || type == LEX_DEP_LR || type == LEX_DEP_BOTH)
    index_side(destination, index, rule->left, 1);
  if (type == LEX_DEP_RL || type == LEX_DEP_BOTH)
    index_side(destination, index, rule->right, 2);
  index_rule_occurrences(destination, index);
  if (!compact_id_map_put(destination->id_map, rule_proof_id(rule), &index))
    fatal_error("compact_rewrite: duplicate compacted proof ID");
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
  threshold = (bank->active_rules / 100) * Compaction_stale_pct +
    ((bank->active_rules % 100) * Compaction_stale_pct + 99) / 100;
  /* While an index is still growing, rebuilding every 1,024 retirements can
     copy the same expanding live population repeatedly.  Require stale
     records to approach the live population, capped at 16K; beyond that
     point the configured percentage regains control. */
  if (threshold < bank->active_rules &&
      threshold < CR_GROWTH_STALE_FLOOR)
    threshold = bank->active_rules < CR_GROWTH_STALE_FLOOR ?
      bank->active_rules : CR_GROWTH_STALE_FLOOR;
  if (threshold < 1024)
    threshold = 1024;
  return stale >= threshold;
}

void compact_rewrite_set_compaction_stale_pct(unsigned percentage)
{
  if (percentage == 0 || percentage > 1000)
    fatal_error("compact_rewrite: invalid stale percentage");
  Compaction_stale_pct = percentage;
}

void compact_rewrite_set_deep_child_cache_kb(unsigned kilobytes)
{
  if ((unsigned long long) kilobytes * 1024 >
      CR_DEEP_CHILD_CACHE_MAX_BYTES)
    fatal_error("compact_rewrite: deep child cache exceeds 8 MiB");
  Deep_child_cache_budget_bytes =
    (unsigned long long) kilobytes * 1024;
}

static void compact_rewrite_compact_internal(Compact_rewrite_bank bank,
                                             BOOL force)
{
  Compact_rewrite_bank replacement;
  struct compact_rewrite_bank old;
  unsigned long long old_bytes, old_peak, old_peak_rules;
  unsigned long long attempts, rewrites, retired, compactions, reclaimed;
  unsigned long long subject_atoms, subject_initial_nodes;
  unsigned long long subject_target_nodes;
  unsigned long long variable_sibling_checks, rigid_sibling_checks;
  unsigned long long deep_lookups, deep_hits, deep_misses;
  unsigned long long deep_replacements, deep_growth_denials;
  size_t i, packed;
  if (bank == NULL ||
      (!force && !compact_rewrite_compaction_needed(bank)) ||
      (force && bank->rule_count - 1 == bank->active_rules))
    return;
  old_bytes = bank_bytes(bank);
  old_peak = bank->peak_bytes;
  old_peak_rules = bank->peak_rules;
  attempts = bank->attempts;
  rewrites = bank->rewrites;
  subject_atoms = bank->subject_atoms;
  subject_initial_nodes = bank->subject_initial_nodes;
  subject_target_nodes = bank->subject_target_nodes;
  retired = bank->retired_rules;
  compactions = bank->compactions;
  reclaimed = bank->bytes_reclaimed;
  variable_sibling_checks = bank->variable_sibling_checks;
  rigid_sibling_checks = bank->rigid_sibling_checks;
  deep_lookups = bank->deep_child_cache_lookups;
  deep_hits = bank->deep_child_cache_hits;
  deep_misses = bank->deep_child_cache_misses;
  deep_replacements = bank->deep_child_cache_replacements;
  deep_growth_denials = bank->deep_child_cache_growth_denials;
  /* A rule record plus the shared token pool is a complete rebuild recipe.
     Pack live records in predecessor order, release all search structures,
     shrink and reuse that record array as the replacement, and rebuild its
     secondary indexes in place.  A standalone owning bank keeps its source
     pool until token copying ends. */
  packed = 1;
  for (i = 1; i < bank->rule_count; i++)
    if (rule_active(&bank->rules[i])) {
      if (packed != i)
        bank->rules[packed] = bank->rules[i];
      packed++;
    }

  old = *bank;
  safe_free(old.nodes);
  safe_free(old.node_first_codes);
  safe_free(old.postings);
  safe_free(old.occurrence_blocks);
  safe_free(old.occurrence_heads);
  safe_free(old.occurrence_tails);
  safe_free(old.occurrence_last_rules);
  compact_id_map_free(old.id_map);
  subject_free_blocks(&old);
  safe_free(old.child_cache);
  safe_free(old.deep_child_cache);
  safe_free(old.deep_child_cache_parents);
  old.rules = safe_realloc(old.rules, packed * sizeof(*old.rules));
  old.rule_capacity = packed;
  old.rule_count = packed;
  replacement = old.owns_term_pool ? compact_rewrite_init() :
    compact_rewrite_init_with_pool(old.term_pool);
  safe_free(replacement->rules);
  replacement->rules = old.rules;
  replacement->rule_capacity = packed;
  replacement->rule_count = 1;
  for (i = 1; i < packed; i++)
    copy_live_rule(replacement, &old, &old.rules[i]);
  *bank = *replacement;
  safe_free(replacement);
  if (old.owns_term_pool)
    compact_term_pool_free(old.term_pool);
  bank->attempts = attempts;
  bank->rewrites = rewrites;
  bank->subject_atoms = subject_atoms;
  bank->subject_initial_nodes = subject_initial_nodes;
  bank->subject_target_nodes = subject_target_nodes;
  bank->variable_sibling_checks = variable_sibling_checks;
  bank->rigid_sibling_checks = rigid_sibling_checks;
  bank->deep_child_cache_lookups = deep_lookups;
  bank->deep_child_cache_hits = deep_hits;
  bank->deep_child_cache_misses = deep_misses;
  bank->deep_child_cache_replacements = deep_replacements;
  bank->deep_child_cache_growth_denials = deep_growth_denials;
  bank->retired_rules = retired;
  bank->compactions = compactions + 1;
  bank->bytes_reclaimed = reclaimed +
    (old_bytes > bank_bytes(bank) ? old_bytes - bank_bytes(bank) : 0);
  if (old_peak > bank->peak_bytes)
    bank->peak_bytes = old_peak;
  if (old_peak_rules > bank->peak_rules)
    bank->peak_rules = old_peak_rules;
}

void compact_rewrite_compact(Compact_rewrite_bank bank)
{
  compact_rewrite_compact_internal(bank, FALSE);
}

void compact_rewrite_compact_all_stale(Compact_rewrite_bank bank)
{
  compact_rewrite_compact_internal(bank, TRUE);
}

static BOOL remove_rule(Compact_rewrite_bank bank,
                        unsigned long long proof_id, BOOL retirement)
{
  uint32_t index = lookup_rule(bank, proof_id);
  if (index == CR_NONE || !rule_active(&bank->rules[index]))
    return FALSE;
  set_rule_active(&bank->rules[index], FALSE);
  if (!compact_id_map_remove(bank->id_map, proof_id))
    fatal_error("compact_rewrite: missing proof ID on removal");
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

void compact_rewrite_copy_live_clauses(Compact_rewrite_bank bank,
                                       Compact_term_pool destination,
                                       Compact_term_rebase_map map)
{
  size_t i;
  if (bank == NULL)
    return;
  for (i = 1; i < bank->rule_count; i++)
    if (!compact_term_pool_copy_clause(
          destination, bank->term_pool, map,
          rule_proof_id(&bank->rules[i])))
      fatal_error("compact_rewrite: cannot copy live clause to compacted pool");
}

void compact_rewrite_retain_live_clauses(Compact_rewrite_bank bank,
                                         Compact_term_rebase_map map)
{
  size_t i;
  if (bank == NULL)
    return;
  for (i = 1; i < bank->rule_count; i++)
    if (!compact_term_rebase_map_retain_clause(
          map, bank->term_pool, rule_proof_id(&bank->rules[i])))
      fatal_error("compact_rewrite: cannot retain live term-pool clause");
}

void compact_rewrite_rebase_term_pool(Compact_rewrite_bank bank,
                                      Compact_term_pool pool,
                                      Compact_term_rebase_map map)
{
  size_t i;
  if (bank == NULL)
    return;
  for (i = 1; i < bank->rule_count; i++) {
    bank->rules[i].left = compact_term_rebase_slice(
      map, bank->rules[i].left);
    bank->rules[i].right = compact_term_rebase_slice(
      map, bank->rules[i].right);
  }
  for (i = 1; i < bank->node_count; i++)
    if (compact_term_slice_length(bank->nodes[i].tokens) != 0)
      bank->nodes[i].tokens = compact_term_rebase_slice(
        map, bank->nodes[i].tokens);
  bank->term_pool = pool;
  update_peak(bank);
}

static uint32_t token_term_end(const int32_t *tokens, uint32_t position,
                               uint32_t end)
{
  int32_t code;
  int i, arity;
  if (position >= end)
    fatal_error("compact_rewrite: truncated occurrence term");
  code = tokens[position++];
  if (code < 0)
    return position;
  arity = sn_to_arity(code);
  for (i = 0; i < arity; i++)
    position = token_term_end(tokens, position, end);
  return position;
}

static BOOL token_match_rec(const int32_t *pattern_tokens,
                            uint32_t *pattern_position,
                            uint32_t pattern_end,
                            const int32_t *subject_tokens,
                            uint32_t *subject_position,
                            uint32_t subject_end,
                            uint32_t *binding_starts,
                            uint32_t *binding_ends)
{
  int32_t pattern_code, subject_code;
  int i, arity;
  if (*pattern_position >= pattern_end || *subject_position >= subject_end)
    return FALSE;
  pattern_code = pattern_tokens[(*pattern_position)++];
  if (pattern_code < 0) {
    unsigned variable = (unsigned) (-pattern_code - 1);
    uint32_t start = *subject_position;
    uint32_t finish = token_term_end(subject_tokens, start, subject_end);
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
          memcmp(subject_tokens + binding_starts[variable],
                 subject_tokens + start,
                 new_length * sizeof(*subject_tokens)) != 0)
        return FALSE;
    }
    *subject_position = finish;
    return TRUE;
  }
  subject_code = subject_tokens[(*subject_position)++];
  if (subject_code != pattern_code)
    return FALSE;
  arity = sn_to_arity(pattern_code);
  for (i = 0; i < arity; i++)
    if (!token_match_rec(pattern_tokens, pattern_position, pattern_end,
                         subject_tokens, subject_position, subject_end,
                         binding_starts, binding_ends))
      return FALSE;
  return TRUE;
}

static BOOL source_contains_pattern(Compact_rewrite_bank bank,
                                    Compact_term_slice source,
                                    Compact_term_slice pattern)
{
  const int32_t *source_tokens = compact_term_pool_slice_tokens(
    bank->term_pool, source);
  const int32_t *pattern_tokens = compact_term_pool_slice_tokens(
    bank->term_pool, pattern);
  uint32_t source_end = compact_term_slice_length(source);
  uint32_t pattern_length = compact_term_slice_length(pattern);
  uint32_t at;
  int32_t root = pattern_tokens[0];
  for (at = 0; at < source_end; at++) {
    uint32_t pattern_position, subject_position;
    uint32_t binding_starts[MAX_VARS], binding_ends[MAX_VARS];
    unsigned i;
    if (source_tokens[at] != root)
      continue;
    for (i = 0; i < MAX_VARS; i++)
      binding_starts[i] = UINT32_MAX;
    pattern_position = 0;
    subject_position = at;
    if (token_match_rec(pattern_tokens, &pattern_position, pattern_length,
                        source_tokens, &subject_position, source_end,
                        binding_starts, binding_ends) &&
        pattern_position == pattern_length)
      return TRUE;
  }
  return FALSE;
}

static BOOL rule_contains_pattern(Compact_rewrite_bank bank,
                                  struct cr_rule *candidate,
                                  Compact_term_slice pattern)
{
  int type = rule_type(candidate);
  if ((type == ORIENTED || type == LEX_DEP_LR ||
       type == LEX_DEP_BOTH) &&
      source_contains_pattern(bank, candidate->left, pattern))
    return TRUE;
  return (type == LEX_DEP_RL || type == LEX_DEP_BOTH) &&
    source_contains_pattern(bank, candidate->right, pattern);
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
  Compact_term_slice patterns[2];
  unsigned pattern_count = 0, i;
  if (new_index == CR_NONE || visit == NULL)
    return;
  rule = &bank->rules[new_index];
  if (rule_type(rule) == ORIENTED || rule_type(rule) == LEX_DEP_LR ||
      rule_type(rule) == LEX_DEP_BOTH) {
    patterns[pattern_count++] = rule->left;
  }
  if (rule_type(rule) == LEX_DEP_RL || rule_type(rule) == LEX_DEP_BOTH) {
    patterns[pattern_count++] = rule->right;
  }
  for (i = 0; i < pattern_count; i++) {
    uint32_t block;
    uint32_t rule_index = 0;
    unsigned symbol;
    int32_t root = compact_term_pool_slice_tokens(
      bank->term_pool, patterns[i])[0];
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
        if (rule_active(candidate) &&
            rule_proof_id(candidate) != new_proof_id &&
            rule_contains_pattern(bank, candidate, patterns[i]))
          visit(rule_proof_id(candidate), context);
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
    if (rule_active(&bank->rules[i]))
      hash ^= hash_id(rule_proof_id(&bank->rules[i]) ^
                      ((uint64_t) rule_type(&bank->rules[i]) << 56));
  return hash;
}

void compact_rewrite_restore_counters(Compact_rewrite_bank bank,
                                      unsigned long long rules_peak,
                                      unsigned long long rules_retired,
                                      unsigned long long attempts,
                                      unsigned long long rewrites,
                                      unsigned long long subject_atoms,
                                      unsigned long long subject_initial_nodes,
                                      unsigned long long subject_target_nodes,
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
  bank->subject_atoms = subject_atoms;
  bank->subject_initial_nodes = subject_initial_nodes;
  bank->subject_target_nodes = subject_target_nodes;
  bank->compactions = compactions;
  bank->bytes_reclaimed = bytes_reclaimed;
}

static BOOL rewrite_binding_is_set(const uint64_t *bound, unsigned variable)
{
  return (bound[variable / 64U] &
          (UINT64_C(1) << (variable % 64U))) != 0;
}

static void set_rewrite_binding(uint64_t *bound, unsigned variable)
{
  bound[variable / 64U] |= UINT64_C(1) << (variable % 64U);
}

static void clear_rewrite_binding(uint64_t *bound, unsigned variable)
{
  bound[variable / 64U] &= ~(UINT64_C(1) << (variable % 64U));
}

/* Build a transient flatterm directly from the compact RHS.  A substituted
   binding is already in normal form because matching is bottom-up; mark the
   copied root exactly as LADR's fapply_demod does. */
static Flatterm build_flat_contractum(Compact_rewrite_bank bank,
                                      const int32_t *tokens,
                                      uint32_t *position, uint32_t end,
                                      Flatterm *bindings,
                                      const uint64_t *bound)
{
  int32_t code;
  Flatterm result, tail;
  int i, arity;
  if (*position >= end)
    fatal_error("compact_rewrite: corrupt RHS token stream");
  code = tokens[(*position)++];
  if (code < 0) {
    int variable = -code - 1;
    if (variable >= MAX_VARS ||
        !rewrite_binding_is_set(bound, (unsigned) variable))
      fatal_error("compact_rewrite: unbound RHS variable");
    result = subject_copy(bank, bindings[variable]);
    result->reduced_flag = TRUE;
    return result;
  }
  arity = sn_to_arity(code);
  result = subject_get_node(bank);
  result->private_symbol = -code;
  ARITY(result) = arity;
  result->size = 1;
  tail = result;
  for (i = 0; i < arity; i++) {
    Flatterm child = build_flat_contractum(bank, tokens, position, end,
                                           bindings, bound);
    result->size += child->size;
    tail->next = child;
    child->prev = tail;
    tail = child->end;
  }
  result->end = tail;
  return result;
}

static BOOL try_leaf(Compact_rewrite_bank bank, uint32_t node,
                     Flatterm target, Flatterm *bindings,
                     const uint64_t *bound,
                     BOOL lex_order_vars,
                     struct cr_match_result *result)
{
  uint32_t posting;
  for (posting = bank->nodes[node].first_posting;
       posting != CR_NONE; posting = bank->postings[posting].next) {
    struct cr_posting *p = &bank->postings[posting];
    struct cr_rule *rule = &bank->rules[p->rule];
    Compact_term_slice contractum_slice;
    const int32_t *tokens;
    uint32_t position = 0, end;
    Flatterm contractum;
    if (!rule_active(rule))
      continue;
    if (p->direction == 1) {
      contractum_slice = rule->right;
    }
    else {
      contractum_slice = rule->left;
    }
    tokens = hot_slice_tokens(bank, contractum_slice);
    end = hot_slice_length(contractum_slice);
    contractum = build_flat_contractum(bank, tokens, &position, end,
                                       bindings, bound);
    if (position != end)
      fatal_error("compact_rewrite: incomplete RHS consumption");
    if (rule_type(rule) == ORIENTED ||
        flat_greater(target, contractum, lex_order_vars)) {
      result->found = TRUE;
      result->contractum = contractum;
      result->proof_id = rule_proof_id(rule);
      result->direction = p->direction;
      return TRUE;
    }
    subject_zap(bank, contractum);
  }
  return FALSE;
}

static inline __attribute__((always_inline))
BOOL match_rewrite_edge(
  Compact_rewrite_bank bank, uint32_t node,
  Flatterm position, Flatterm end,
  Flatterm *bindings, uint64_t *bound, unsigned *binding_trail,
  unsigned *trail_count,
  Flatterm *next_position)
{
  struct cr_node *edge = &bank->nodes[node];
  const int32_t *tokens = hot_slice_tokens(bank, edge->tokens);
  uint32_t length = hot_slice_length(edge->tokens);
  uint32_t i;
  for (i = 0; i < length; i++) {
    int32_t code = tokens[i];
    Flatterm query_term;
    if (position == end)
      return FALSE;
    query_term = position;
    if (code < 0) {
      unsigned variable = (unsigned) (-code - 1);
      if (variable >= MAX_VARS)
        fatal_error("compact_rewrite: variable exceeds MAX_VARS");
      if (!rewrite_binding_is_set(bound, variable)) {
        bindings[variable] = query_term;
        set_rewrite_binding(bound, variable);
        binding_trail[(*trail_count)++] = variable;
      }
      else if (!flatterm_ident(bindings[variable], query_term))
        return FALSE;
      position = query_term->end->next;
    }
    else {
      if (VARIABLE(query_term) || SYMNUM(query_term) != code)
        return FALSE;
      position = position->next;
    }
  }
  *next_position = position;
  return TRUE;
}

static void undo_rewrite_bindings(uint64_t *bound,
                                  const unsigned *binding_trail,
                                  unsigned *trail_count,
                                  unsigned trail_mark)
{
  while (*trail_count != trail_mark)
    clear_rewrite_binding(bound, binding_trail[--(*trail_count)]);
}

static BOOL retrieve_rec(Compact_rewrite_bank bank, uint32_t node,
                         Flatterm position, Flatterm end,
                         Flatterm *bindings, uint64_t *bound,
                         unsigned *binding_trail, unsigned *trail_count,
                         Flatterm target,
                         BOOL lex_order_vars,
                         struct cr_match_result *result);

/* Keep edge matching in the caller's traversal frame.  This helper used to
   be the other half of a mutually recursive pair and accounted for roughly
   59 million calls in a 600-given CHAT profile.  It is not recursively
   self-calling: after matching one radix edge it enters retrieve_rec(), so
   forcing this small wrapper inline removes a complete call/return and lets
   the compiler keep the binding trail in registers. */
static inline __attribute__((always_inline))
BOOL retrieve_child(Compact_rewrite_bank bank, uint32_t child,
                    Flatterm position, Flatterm end,
                    Flatterm *bindings, uint64_t *bound,
                    unsigned *binding_trail, unsigned *trail_count,
                    Flatterm target, BOOL lex_order_vars,
                    struct cr_match_result *result)
{
  unsigned trail_mark = *trail_count;
  Flatterm next_position = position;
  BOOL found = FALSE;
  if (match_rewrite_edge(bank, child, position, end, bindings, bound,
                         binding_trail, trail_count, &next_position))
    found = retrieve_rec(bank, child, next_position, end, bindings,
                         bound, binding_trail, trail_count, target,
                         lex_order_vars, result);
  undo_rewrite_bindings(bound, binding_trail, trail_count, trail_mark);
  return found;
}

static BOOL retrieve_rec(Compact_rewrite_bank bank, uint32_t node,
                         Flatterm position, Flatterm end,
                         Flatterm *bindings, uint64_t *bound,
                         unsigned *binding_trail, unsigned *trail_count,
                         Flatterm target,
                         BOOL lex_order_vars,
                         struct cr_match_result *result)
{
  uint32_t child, rigid_start;
  unsigned rigid_scanned = 0;
  BOOL deep_cache_enabled = bank->deep_child_cache_budget_bytes != 0;
  Flatterm query_term;
  int32_t query_code;
  if (position == end)
    return try_leaf(bank, node, target, bindings, bound, lex_order_vars,
                    result);
  query_term = position;
  query_code = VARIABLE(query_term) ? INT32_MIN : SYMNUM(query_term);
  /* Variable-led alternatives retain their exact predecessor order and must
     be tried before a rigid edge. */
  for (child = bank->nodes[node].first_child; child != CR_NONE &&
       first_code(bank, child) < 0;
       child = bank->nodes[child].next_sibling) {
    if (deep_cache_enabled)
      bank->variable_sibling_checks++;
    if (retrieve_child(bank, child, position, end, bindings, bound,
                       binding_trail, trail_count, target, lex_order_vars,
                       result))
      return TRUE;
  }
  if (query_code == INT32_MIN)
    return FALSE;

  rigid_start = child;
  if (node == 0 && child_cache_get(bank, node, query_code, &child))
    return retrieve_child(bank, child, position, end, bindings, bound,
                          binding_trail, trail_count, target, lex_order_vars,
                          result);
  if (deep_cache_enabled && node != 0 &&
      deep_child_cache_parent_enabled(bank, node) &&
      deep_child_cache_get(bank, node, query_code, &child)) {
    if (child == CR_NONE)
      return FALSE;
    return retrieve_child(bank, child, position, end, bindings, bound,
                          binding_trail, trail_count, target, lex_order_vars,
                          result);
  }

  /* A cache miss is semantically uninteresting: search the same ordered
     sibling chain as the original implementation and refresh the slot. */
  for (child = rigid_start; child != CR_NONE;
       child = bank->nodes[child].next_sibling) {
    int32_t edge_code = first_code(bank, child);
    if (deep_cache_enabled) {
      rigid_scanned++;
      bank->rigid_sibling_checks++;
    }
    if (edge_code > query_code)
      break;
    if (edge_code < query_code)
      continue;
    if (node == 0)
      child_cache_put(bank, node, query_code, child);
    else if (deep_cache_enabled &&
             (deep_child_cache_parent_enabled(bank, node) ||
              rigid_scanned >= CR_DEEP_CHILD_CACHE_MIN_SCAN)) {
      if (deep_child_cache_enable_parent(bank, node)) {
        maybe_grow_deep_child_cache(bank);
        deep_child_cache_put(bank, node, query_code, child, TRUE);
      }
    }
    return retrieve_child(bank, child, position, end, bindings, bound,
                          binding_trail, trail_count, target, lex_order_vars,
                          result);
  }
  if (deep_cache_enabled && node != 0 &&
      (deep_child_cache_parent_enabled(bank, node) ||
       rigid_scanned >= CR_DEEP_CHILD_CACHE_MIN_SCAN) &&
      deep_child_cache_enable_parent(bank, node)) {
    maybe_grow_deep_child_cache(bank);
    deep_child_cache_put(bank, node, query_code, CR_NONE, TRUE);
  }
  return FALSE;
}

static struct cr_match_result find_rewrite(Compact_rewrite_bank bank,
                                           Flatterm target,
                                           BOOL lex_order_vars)
{
  struct cr_match_result result;
  Flatterm bindings[MAX_VARS];
  uint64_t bound[CR_BINDING_WORDS];
  unsigned binding_trail[MAX_VARS];
  unsigned trail_count = 0;
  memset(&result, 0, sizeof(result));
  memset(bound, 0, sizeof(bound));
  bank->query_token_base = compact_term_pool_tokens(bank->term_pool);
  bank->query_logical_base =
    compact_term_pool_logical_base(bank->term_pool);
  {
    uint32_t first = bank->nodes[0].first_child;
    int32_t code = VARIABLE(target) ? INT32_MIN : SYMNUM(target);
    /* Demodulator sides normally have rigid roots, and index construction
       maintains an exact direct child for every such root symbol.  Avoid the
       generic variable-first sibling traversal for this overwhelmingly hot
       case.  The unusual variable-root case retains the original DFS. */
    if (code != INT32_MIN &&
        (first == CR_NONE || bank->node_first_codes[first] >= 0)) {
      uint32_t child = code >= 0 &&
        (size_t) code < bank->child_cache_capacity ?
        bank->child_cache[code] : CR_NONE;
      if (child != CR_NONE)
        retrieve_child(bank, child, target, target->end->next,
                       bindings, bound, binding_trail, &trail_count,
                       target, lex_order_vars, &result);
    }
    else
      retrieve_rec(bank, 0, target, target->end->next, bindings, bound,
                   binding_trail, &trail_count, target, lex_order_vars,
                   &result);
  }
  return result;
}

static Flatterm normalize_flat_term(Compact_rewrite_bank bank, Flatterm term,
                                   int *step_limit, int size_limit,
                                   int *current_size, int *sequence,
                                   I3list *steps, BOOL lex_order_vars,
                                   BOOL count_stats)
{
  int sequence_save;
  struct cr_match_result match;
  if (*step_limit == 0 || *current_size > size_limit || VARIABLE(term))
    return term;
  if (term->reduced_flag) {
    *sequence += flatterm_count_without_vars(term);
    return term;
  }
  sequence_save = *sequence;
  {
    Flatterm argument = term->next;
    Flatterm tail = term;
    int size = 1;
    int i;
    for (i = 0; i < ARITY(term); i++) {
      Flatterm next = argument->end->next;
      Flatterm normalized = normalize_flat_term(
        bank, argument, step_limit, size_limit, current_size, sequence,
        steps, lex_order_vars, count_stats);
      size += normalized->size;
      tail->next = normalized;
      normalized->prev = tail;
      tail = normalized->end;
      argument = next;
    }
    term->size = size;
    term->end = tail;
    term->prev = NULL;
    tail->next = NULL;
  }
  if (*step_limit == 0 || *current_size > size_limit)
    return term;
  if (count_stats) {
    bank->attempts++;
    bank->subject_target_nodes += (unsigned long long) term->size;
  }
  (*sequence)++;
  match = find_rewrite(bank, term, lex_order_vars);
  if (!match.found) {
    term->reduced_flag = TRUE;
    return term;
  }
  *current_size += match.contractum->size - term->size;
  (*step_limit)--;
  if (count_stats)
    bank->rewrites++;
  *steps = i3list_prepend(*steps, match.proof_id, *sequence,
                          match.direction);
  subject_zap(bank, term);
  *sequence = sequence_save;
  return normalize_flat_term(bank, match.contractum, step_limit, size_limit,
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
    Flatterm flat, normalized;
    I3list steps_before;
    Term original;
    int current_size, size_limit;
    if (step_limit == 0 || increase_limit == -1)
      continue;
    original = literal->atom;
    flat = subject_from_term(bank, original);
    if (count_stats) {
      bank->subject_atoms++;
      bank->subject_initial_nodes += (unsigned long long) flat->size;
    }
    current_size = flat->size;
    size_limit = increase_limit == INT_MAX ? INT_MAX :
                 current_size + increase_limit;
    steps_before = steps;
    normalized = normalize_flat_term(bank, flat, &step_limit, size_limit,
                                     &current_size, &sequence, &steps,
                                     lex_order_vars, count_stats);
    if (steps != steps_before) {
      literal->atom = flatterm_to_term(normalized);
      zap_term(original);
    }
    subject_zap(bank, normalized);
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
  stats->subject_atoms = bank->subject_atoms;
  stats->subject_initial_nodes = bank->subject_initial_nodes;
  stats->subject_target_nodes = bank->subject_target_nodes;
  stats->node_items = bank->node_count;
  stats->posting_items = bank->posting_count;
  stats->node_bytes = bank->node_capacity * sizeof(*bank->nodes);
  stats->node_bytes +=
    bank->node_capacity * sizeof(*bank->node_first_codes);
  stats->posting_bytes = bank->posting_capacity * sizeof(*bank->postings);
  stats->child_cache_bytes =
    bank->child_cache_capacity * sizeof(*bank->child_cache);
  stats->child_cache_capacity = bank->child_cache_capacity;
  stats->deep_child_cache_bytes =
    bank->deep_child_cache_capacity * sizeof(*bank->deep_child_cache) +
    bank->deep_child_cache_parent_words *
      sizeof(*bank->deep_child_cache_parents);
  stats->deep_child_cache_budget_bytes =
    bank->deep_child_cache_budget_bytes;
  stats->deep_child_cache_capacity = bank->deep_child_cache_capacity;
  stats->deep_child_cache_parents = bank->deep_child_cache_parent_count;
  stats->deep_child_cache_lookups = bank->deep_child_cache_lookups;
  stats->deep_child_cache_hits = bank->deep_child_cache_hits;
  stats->deep_child_cache_misses = bank->deep_child_cache_misses;
  stats->deep_child_cache_replacements =
    bank->deep_child_cache_replacements;
  stats->deep_child_cache_growth_denials =
    bank->deep_child_cache_growth_denials;
  stats->variable_sibling_checks = bank->variable_sibling_checks;
  stats->rigid_sibling_checks = bank->rigid_sibling_checks;
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
  stats->hash_bytes = compact_id_map_bytes(bank->id_map);
  stats->total_bytes = bank_bytes(bank);
  stats->peak_bytes = bank->peak_bytes;
}

void compact_rewrite_free(Compact_rewrite_bank bank)
{
  if (bank == NULL)
    return;
  safe_free(bank->nodes);
  safe_free(bank->node_first_codes);
  safe_free(bank->postings);
  safe_free(bank->occurrence_blocks);
  safe_free(bank->occurrence_heads);
  safe_free(bank->occurrence_tails);
  safe_free(bank->occurrence_last_rules);
  safe_free(bank->rules);
  if (bank->owns_term_pool)
    compact_term_pool_free(bank->term_pool);
  compact_id_map_free(bank->id_map);
  subject_free_blocks(bank);
  safe_free(bank->child_cache);
  safe_free(bank->deep_child_cache);
  safe_free(bank->deep_child_cache_parents);
  safe_free(bank);
}
