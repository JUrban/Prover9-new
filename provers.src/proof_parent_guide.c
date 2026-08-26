#include "proof_parent_guide.h"

#include "../ladr/clause_misc.h"
#include "../ladr/clock.h"
#include "../ladr/memory.h"

#include <limits.h>
#include <stdint.h>
#include <stdlib.h>
#include <string.h>

struct guide_node {
  uint64_t source_id;
  unsigned body_group;
  unsigned recipe_offset;
  unsigned recipe_size;
  enum proof_recipe_rule recipe_rule;
  BOOL source_given;
  BOOL has_recipe;
  unsigned rewrite_count;
  unsigned flip_count;
  unsigned *paramod_neighbors;
  unsigned paramod_count;
  unsigned paramod_capacity;
  unsigned *hyper_neighbors;
  unsigned hyper_count;
  unsigned hyper_capacity;
};

struct runtime_clause;

struct body_group {
  uint64_t hash;
  Topform pattern;
  unsigned *nodes;
  unsigned node_count;
  unsigned node_capacity;
  unsigned bucket_next;  /* group index + 1; zero is the end sentinel */
  struct runtime_clause *active;
};

struct source_slot {
  uint64_t id;
  unsigned node_plus_one;
};

struct runtime_clause {
  uint64_t id;
  Topform clause;
  unsigned group;
  BOOL active;
  struct runtime_clause *next_bucket;
  struct runtime_clause *next_group;
  struct runtime_clause *next_all;
};

struct proof_parent_guide_stats {
  unsigned nodes;
  unsigned body_groups;
  unsigned long long paramod_steps;
  unsigned long long hyper_steps;
  unsigned long long rewrite_references;
  unsigned long long recipe_nodes;
  unsigned long long recipe_bytes;
  unsigned long long guide_body_bytes;
  unsigned long long paramod_relations;
  unsigned long long hyper_relations;
  unsigned long long activations;
  unsigned long long mapped_activations;
  unsigned long long unmapped_activations;
  unsigned long long deactivations;
  unsigned long long active_mapped;
  unsigned long long active_mapped_peak;
  unsigned long long mapped_givens;
  unsigned long long unmapped_givens;
  unsigned long long paramod_pair_tests;
  unsigned long long paramod_pair_accepts;
  unsigned long long paramod_pair_rejects;
  unsigned long long hyper_pair_tests;
  unsigned long long hyper_pair_accepts;
  unsigned long long hyper_pair_rejects;
  unsigned long long authoritative_queries;
  unsigned long long authoritative_raw_partners;
  unsigned long long authoritative_unique_partners;
  unsigned long long authoritative_duplicate_partners;
  unsigned long long authoritative_max_partners;
  unsigned long long resident_bytes;
  unsigned long long resident_peak_bytes;
  double build_seconds;
};

struct proof_parent_guide {
  enum proof_parent_guide_mode mode;
  struct guide_node *nodes;
  unsigned node_count;
  struct body_group *groups;
  unsigned group_count;
  unsigned group_capacity;
  unsigned *body_buckets;
  unsigned body_bucket_count;
  struct source_slot *source_slots;
  unsigned source_capacity;
  struct runtime_clause **runtime_buckets;
  unsigned runtime_capacity;
  struct runtime_clause *runtime_all;
  Topform *partner_workspace;
  unsigned partner_capacity;
  unsigned char *recipe_arena;
  unsigned recipe_arena_size;
  unsigned recipe_arena_capacity;
  struct proof_parent_guide_stats stats;
};

static uint64_t mix64(uint64_t value)
{
  value ^= value >> 30;
  value *= UINT64_C(0xbf58476d1ce4e5b9);
  value ^= value >> 27;
  value *= UINT64_C(0x94d049bb133111eb);
  value ^= value >> 31;
  return value;
}

static uint64_t hash_mix_u32(uint64_t hash, uint32_t value)
{
  int i;
  for (i = 0; i < 4; i++) {
    hash ^= (value >> (8 * i)) & 0xff;
    hash *= UINT64_C(1099511628211);
  }
  return hash;
}

static uint64_t term_hash(Term term, uint64_t hash)
{
  int i;
  hash = hash_mix_u32(hash, VARIABLE(term) ? 0U : 1U);
  hash = hash_mix_u32(hash, VARIABLE(term) ? (uint32_t) VARNUM(term) :
                                           (uint32_t) SYMNUM(term));
  hash = hash_mix_u32(hash, (uint32_t) ARITY(term));
  for (i = 0; i < ARITY(term); i++)
    hash = term_hash(ARG(term, i), hash);
  return hash;
}

static uint64_t clause_hash(Topform clause)
{
  uint64_t hash = UINT64_C(1469598103934665603);
  Literals literal;
  for (literal = clause->literals; literal != NULL; literal = literal->next) {
    hash = hash_mix_u32(hash, literal->sign ? 1U : 0U);
    hash = term_hash(literal->atom, hash);
  }
  return hash_mix_u32(hash, UINT32_MAX);
}

static unsigned next_power_of_two(unsigned long long needed)
{
  unsigned capacity = 16;
  while ((unsigned long long) capacity < needed) {
    if (capacity > UINT_MAX / 2)
      fatal_error("proof-parent guide directory overflow");
    capacity *= 2;
  }
  return capacity;
}

struct recipe_reader {
  const unsigned char *data;
  unsigned size;
  unsigned at;
  const char *error;
};

static BOOL recipe_read_byte(struct recipe_reader *reader, unsigned *value)
{
  if (reader->at >= reader->size) {
    reader->error = "truncated recipe";
    return FALSE;
  }
  *value = reader->data[reader->at++];
  return TRUE;
}

static BOOL recipe_read_uvarint(struct recipe_reader *reader, unsigned *value)
{
  unsigned result = 0, shift = 0, byte;
  while (shift < 35) {
    if (!recipe_read_byte(reader, &byte))
      return FALSE;
    if (shift == 28 && (byte & 0xf0U) != 0) {
      reader->error = "recipe integer overflow";
      return FALSE;
    }
    result |= (byte & 0x7fU) << shift;
    if ((byte & 0x80U) == 0) {
      *value = result;
      return TRUE;
    }
    shift += 7;
  }
  reader->error = "recipe varint is too long";
  return FALSE;
}

static BOOL recipe_read_svarint(struct recipe_reader *reader, int *value)
{
  unsigned encoded, magnitude;
  if (!recipe_read_uvarint(reader, &encoded))
    return FALSE;
  magnitude = encoded >> 1;
  if ((encoded & 1U) == 0) {
    if (magnitude > INT_MAX) {
      reader->error = "positive recipe integer overflow";
      return FALSE;
    }
    *value = (int) magnitude;
  }
  else {
    if (magnitude >= (unsigned) INT_MAX) {
      reader->error = "negative recipe integer overflow";
      return FALSE;
    }
    *value = -(int) magnitude - 1;
  }
  return TRUE;
}

void proof_recipe_decoded_destroy(struct proof_recipe_decoded *recipe)
{
  unsigned i;
  if (recipe == NULL)
    return;
  for (i = 0; i < 2; i++)
    safe_free(recipe->paramod[i].position.items);
  safe_free(recipe->clashes);
  safe_free(recipe->secondary);
  memset(recipe, 0, sizeof(*recipe));
}

static BOOL recipe_read_position(struct recipe_reader *reader,
                                 struct proof_recipe_position *position)
{
  unsigned count, i;
  if (!recipe_read_uvarint(reader, &count))
    return FALSE;
  if (count < 2 || count > reader->size - reader->at) {
    reader->error = "invalid recipe position length";
    return FALSE;
  }
  position->items = safe_calloc(count, sizeof(*position->items));
  position->count = count;
  for (i = 0; i < count; i++) {
    if (!recipe_read_svarint(reader, &position->items[i]))
      return FALSE;
    if (position->items[i] <= 0) {
      reader->error = "recipe position contains a nonpositive component";
      return FALSE;
    }
  }
  return TRUE;
}

static BOOL decode_recipe_data(const unsigned char *data, unsigned size,
                               struct proof_recipe_decoded *recipe,
                               const char **error)
{
  struct recipe_reader reader = {data, size, 0, NULL};
  unsigned version = 0, rule = 0, source_given = 0, secondary_count, i;
  memset(recipe, 0, sizeof(*recipe));
  if (!recipe_read_byte(&reader, &version))
    goto failed;
  if (version != 1) {
    reader.error = "unsupported recipe version";
    goto failed;
  }
  if (!recipe_read_byte(&reader, &rule) ||
      rule < PROOF_RECIPE_ASSUMPTION || rule > PROOF_RECIPE_RESOLVE) {
    reader.error = "unknown recipe primary rule";
    goto failed;
  }
  if (!recipe_read_byte(&reader, &source_given) || source_given > 1) {
    reader.error = "invalid recipe source-given flag";
    goto failed;
  }
  recipe->rule = (enum proof_recipe_rule) rule;
  recipe->source_given = source_given != 0;

  switch (recipe->rule) {
  case PROOF_RECIPE_ASSUMPTION:
  case PROOF_RECIPE_GOAL:
    break;
  case PROOF_RECIPE_DENY:
  case PROOF_RECIPE_COPY:
  case PROOF_RECIPE_BACK_REWRITE:
    if (!recipe_read_uvarint(&reader, &recipe->unary_parent))
      goto failed;
    break;
  case PROOF_RECIPE_PARAMOD:
    for (i = 0; i < 2; i++)
      if (!recipe_read_uvarint(&reader, &recipe->paramod[i].node) ||
          !recipe_read_position(&reader, &recipe->paramod[i].position))
        goto failed;
    break;
  case PROOF_RECIPE_HYPER:
  case PROOF_RECIPE_RESOLVE:
    if (!recipe_read_uvarint(&reader, &recipe->nucleus_node) ||
        !recipe_read_uvarint(&reader, &recipe->clash_count))
      goto failed;
    if (recipe->clash_count == 0 ||
        recipe->clash_count > (reader.size - reader.at) / 3) {
      reader.error = "invalid recipe resolution arity";
      goto failed;
    }
    recipe->clashes = safe_calloc(
      recipe->clash_count, sizeof(*recipe->clashes));
    for (i = 0; i < recipe->clash_count; i++) {
      if (!recipe_read_svarint(
            &reader, &recipe->clashes[i].nucleus_literal) ||
          !recipe_read_uvarint(
            &reader, &recipe->clashes[i].satellite_node) ||
          !recipe_read_svarint(
            &reader, &recipe->clashes[i].satellite_literal))
        goto failed;
      if (recipe->clashes[i].nucleus_literal <= 0 ||
          recipe->clashes[i].satellite_literal == 0) {
        reader.error = "invalid recipe resolution literal";
        goto failed;
      }
    }
    break;
  default:
    reader.error = "unknown recipe rule";
    goto failed;
  }

  if (!recipe_read_uvarint(&reader, &secondary_count))
    goto failed;
  if (secondary_count > reader.size - reader.at) {
    reader.error = "invalid recipe secondary count";
    goto failed;
  }
  recipe->secondary_count = secondary_count;
  if (secondary_count != 0)
    recipe->secondary = safe_calloc(
      secondary_count, sizeof(*recipe->secondary));
  for (i = 0; i < secondary_count; i++) {
    unsigned secondary_rule;
    if (!recipe_read_uvarint(&reader, &secondary_rule))
      goto failed;
    if (secondary_rule == PROOF_RECIPE_REWRITE) {
      struct proof_recipe_secondary *step = &recipe->secondary[i];
      step->rule = PROOF_RECIPE_REWRITE;
      if (!recipe_read_uvarint(&reader, &step->parent_node) ||
          !recipe_read_uvarint(&reader, &step->target) ||
          !recipe_read_uvarint(&reader, &step->direction))
        goto failed;
      if (step->target == 0 ||
          (step->direction != 1 && step->direction != 2)) {
        reader.error = "invalid recipe rewrite target or direction";
        goto failed;
      }
    }
    else if (secondary_rule == PROOF_RECIPE_FLIP) {
      struct proof_recipe_secondary *step = &recipe->secondary[i];
      step->rule = PROOF_RECIPE_FLIP;
      if (!recipe_read_svarint(&reader, &step->literal))
        goto failed;
      if (step->literal <= 0) {
        reader.error = "invalid recipe flip literal";
        goto failed;
      }
    }
    else {
      reader.error = "unknown recipe secondary rule";
      goto failed;
    }
  }
  if (reader.at != reader.size) {
    reader.error = "trailing bytes in recipe";
    goto failed;
  }
  if (error != NULL)
    *error = NULL;
  return TRUE;

failed:
  if (error != NULL)
    *error = reader.error == NULL ? "malformed recipe" : reader.error;
  proof_recipe_decoded_destroy(recipe);
  return FALSE;
}

static int recipe_base64_value(char c)
{
  if (c >= 'A' && c <= 'Z') return c - 'A';
  if (c >= 'a' && c <= 'z') return c - 'a' + 26;
  if (c >= '0' && c <= '9') return c - '0' + 52;
  if (c == '-') return 62;
  if (c == '_') return 63;
  return -1;
}

static BOOL decode_recipe_base64(const char *text, unsigned char **data,
                                 unsigned *size, const char **error)
{
  size_t length = strlen(text), i;
  unsigned capacity, out = 0, bits = 0, accumulator = 0;
  unsigned char *decoded;
  if (length == 0 || length > UINT_MAX) {
    *error = "empty or oversized recipe string";
    return FALSE;
  }
  capacity = (unsigned) ((length * 6 + 7) / 8);
  decoded = safe_malloc(capacity);
  for (i = 0; i < length; i++) {
    int value = recipe_base64_value(text[i]);
    if (value < 0) {
      safe_free(decoded);
      *error = "invalid URL-safe base64 recipe character";
      return FALSE;
    }
    accumulator = (accumulator << 6) | (unsigned) value;
    bits += 6;
    if (bits >= 8) {
      bits -= 8;
      decoded[out++] = (unsigned char) ((accumulator >> bits) & 0xffU);
    }
  }
  if (bits != 0 && (accumulator & ((1U << bits) - 1U)) != 0) {
    safe_free(decoded);
    *error = "nonzero recipe base64 padding bits";
    return FALSE;
  }
  *data = decoded;
  *size = out;
  *error = NULL;
  return TRUE;
}

static void reserve_recipe_arena(Proof_parent_guide guide, unsigned needed)
{
  unsigned capacity;
  if (needed <= guide->recipe_arena_capacity)
    return;
  capacity = guide->recipe_arena_capacity == 0 ? 4096 :
             guide->recipe_arena_capacity;
  while (capacity < needed) {
    unsigned next = capacity + (capacity + 1) / 2;
    if (next <= capacity)
      fatal_error("proof recipe arena overflow");
    capacity = next;
  }
  guide->recipe_arena = safe_realloc(guide->recipe_arena, capacity);
  guide->recipe_arena_capacity = capacity;
}

static unsigned read_parent_attributes(Topform clause, int attribute,
                                       uint64_t **parents)
{
  unsigned count = 0, capacity = 0;
  int value;
  *parents = NULL;
  while ((value = get_int_attribute(
            clause->attributes, attribute, count + 1)) != INT_MAX) {
    if (value <= 0)
      fatal_error("proof-parent guide has a nonpositive parent node");
    if (count == capacity) {
      unsigned next = capacity == 0 ? 4 : capacity * 2;
      if (next <= capacity)
        fatal_error("proof-parent guide parent list overflow");
      *parents = safe_realloc(*parents, (size_t) next * sizeof(**parents));
      capacity = next;
    }
    (*parents)[count++] = (uint64_t) value;
  }
  return count;
}

static void append_unsigned(unsigned value, unsigned **items,
                            unsigned *count, unsigned *capacity)
{
  if (*count == *capacity) {
    unsigned next = *capacity == 0 ? 4 : *capacity * 2;
    if (next <= *capacity)
      fatal_error("proof-parent guide relation overflow");
    *items = safe_realloc(*items, (size_t) next * sizeof(**items));
    *capacity = next;
  }
  (*items)[(*count)++] = value;
}

static int compare_unsigned(const void *left, const void *right)
{
  unsigned a = *(const unsigned *) left;
  unsigned b = *(const unsigned *) right;
  return a < b ? -1 : a > b ? 1 : 0;
}

static unsigned sort_unique_unsigned(unsigned *items, unsigned count)
{
  unsigned in, out = 0;
  if (count > 1)
    qsort(items, count, sizeof(*items), compare_unsigned);
  for (in = 0; in < count; in++)
    if (out == 0 || items[in] != items[out - 1])
      items[out++] = items[in];
  return out;
}

static void source_insert(Proof_parent_guide guide, uint64_t id,
                          unsigned node)
{
  unsigned at = (unsigned) mix64(id) & (guide->source_capacity - 1);
  while (guide->source_slots[at].id != 0) {
    if (guide->source_slots[at].id == id)
      fatal_error("proof-parent guide has duplicate node IDs");
    at = (at + 1) & (guide->source_capacity - 1);
  }
  guide->source_slots[at].id = id;
  guide->source_slots[at].node_plus_one = node + 1;
}

static unsigned source_lookup(Proof_parent_guide guide, uint64_t id)
{
  unsigned at = (unsigned) mix64(id) & (guide->source_capacity - 1);
  while (guide->source_slots[at].id != 0) {
    if (guide->source_slots[at].id == id)
      return guide->source_slots[at].node_plus_one - 1;
    at = (at + 1) & (guide->source_capacity - 1);
  }
  fatal_error("proof-parent guide references a missing parent node");
  return 0;
}

static BOOL recipe_parent_is_prior(unsigned parent, unsigned node)
{
  /* NODE is zero-based and recipe parents are one-based. */
  return parent != 0 && parent <= node;
}

static BOOL validate_recipe_dependencies(const struct proof_recipe_decoded *r,
                                         unsigned node, const char **error)
{
  unsigned i;
  if ((r->rule == PROOF_RECIPE_DENY ||
       r->rule == PROOF_RECIPE_COPY ||
       r->rule == PROOF_RECIPE_BACK_REWRITE) &&
      !recipe_parent_is_prior(r->unary_parent, node)) {
    *error = "recipe has a non-prior unary parent";
    return FALSE;
  }
  if (r->rule == PROOF_RECIPE_PARAMOD)
    for (i = 0; i < 2; i++)
      if (!recipe_parent_is_prior(r->paramod[i].node, node)) {
        *error = "recipe has a non-prior paramodulation parent";
        return FALSE;
      }
  if (r->rule == PROOF_RECIPE_HYPER ||
      r->rule == PROOF_RECIPE_RESOLVE) {
    if (!recipe_parent_is_prior(r->nucleus_node, node)) {
      *error = "recipe has a non-prior resolution nucleus";
      return FALSE;
    }
    for (i = 0; i < r->clash_count; i++)
      if (!recipe_parent_is_prior(r->clashes[i].satellite_node, node)) {
        *error = "recipe has a non-prior resolution satellite";
        return FALSE;
      }
  }
  for (i = 0; i < r->secondary_count; i++)
    if (r->secondary[i].rule == PROOF_RECIPE_REWRITE &&
        !recipe_parent_is_prior(r->secondary[i].parent_node, node)) {
      *error = "recipe has a non-prior rewrite parent";
      return FALSE;
    }
  *error = NULL;
  return TRUE;
}

static void load_node_recipe(Proof_parent_guide guide, unsigned node,
                             Topform clause, int recipe_attribute)
{
  char *encoded = get_string_attribute(
    clause->attributes, recipe_attribute, 1);
  char *unquoted = NULL;
  char *duplicate = get_string_attribute(
    clause->attributes, recipe_attribute, 2);
  unsigned char *decoded = NULL;
  unsigned decoded_size = 0, i;
  struct proof_recipe_decoded recipe;
  const char *error = NULL;
  struct guide_node *guide_node = &guide->nodes[node];
  if (encoded == NULL)
    return;
  if (duplicate != NULL)
    fatal_error("proof-parent guide node has multiple recipe attributes");
  if (guide_node->source_id != (uint64_t) node + 1)
    fatal_error("proof recipe nodes must use dense ordered IDs");
  {
    size_t length = strlen(encoded);
    if (length >= 2 && encoded[0] == '"' && encoded[length - 1] == '"') {
      unquoted = safe_malloc(length - 1);
      memcpy(unquoted, encoded + 1, length - 2);
      unquoted[length - 2] = '\0';
      encoded = unquoted;
    }
  }
  if (!decode_recipe_base64(encoded, &decoded, &decoded_size, &error) ||
      !decode_recipe_data(decoded, decoded_size, &recipe, &error) ||
      !validate_recipe_dependencies(&recipe, node, &error)) {
    fprintf(stderr, "Proof recipe node %u: %s\n", node + 1,
            error == NULL ? "malformed recipe" : error);
    safe_free(unquoted);
    safe_free(decoded);
    fatal_error("invalid proof recipe");
  }
  if (guide->recipe_arena_size > UINT_MAX - decoded_size) {
    proof_recipe_decoded_destroy(&recipe);
    safe_free(decoded);
    fatal_error("proof recipe arena size overflow");
  }
  reserve_recipe_arena(guide, guide->recipe_arena_size + decoded_size);
  guide_node->recipe_offset = guide->recipe_arena_size;
  guide_node->recipe_size = decoded_size;
  guide_node->recipe_rule = recipe.rule;
  guide_node->source_given = recipe.source_given;
  guide_node->has_recipe = TRUE;
  for (i = 0; i < recipe.secondary_count; i++) {
    if (recipe.secondary[i].rule == PROOF_RECIPE_REWRITE)
      guide_node->rewrite_count++;
    else if (recipe.secondary[i].rule == PROOF_RECIPE_FLIP)
      guide_node->flip_count++;
  }
  if (guide->mode == PROOF_PARENT_GUIDE_RECIPE_REPLAY) {
    if (recipe.rule == PROOF_RECIPE_PARAMOD)
      guide->stats.paramod_steps++;
    else if (recipe.rule == PROOF_RECIPE_HYPER)
      guide->stats.hyper_steps++;
    guide->stats.rewrite_references += guide_node->rewrite_count;
  }
  memcpy(guide->recipe_arena + guide->recipe_arena_size,
         decoded, decoded_size);
  guide->recipe_arena_size += decoded_size;
  guide->stats.recipe_nodes++;
  guide->stats.recipe_bytes += decoded_size;
  proof_recipe_decoded_destroy(&recipe);
  safe_free(unquoted);
  safe_free(decoded);
}

static unsigned find_body_group(Proof_parent_guide guide, Topform clause,
                                uint64_t hash)
{
  unsigned link = guide->body_buckets[(unsigned) hash &
                                      (guide->body_bucket_count - 1)];
  while (link != 0) {
    unsigned group = link - 1;
    if (guide->groups[group].hash == hash &&
        clause_ident(guide->groups[group].pattern->literals,
                     clause->literals))
      return group;
    link = guide->groups[group].bucket_next;
  }
  return UINT_MAX;
}

static unsigned add_body_group(Proof_parent_guide guide, Topform clause,
                               uint64_t hash)
{
  unsigned bucket, group;
  if (guide->group_count == guide->group_capacity) {
    unsigned next = guide->group_capacity == 0 ? 16 :
                    guide->group_capacity * 2;
    if (next <= guide->group_capacity)
      fatal_error("proof-parent guide body directory overflow");
    guide->groups = safe_realloc(
      guide->groups, (size_t) next * sizeof(*guide->groups));
    memset(guide->groups + guide->group_capacity, 0,
           (size_t) (next - guide->group_capacity) * sizeof(*guide->groups));
    guide->group_capacity = next;
  }
  group = guide->group_count++;
  guide->groups[group].hash = hash;
  guide->groups[group].pattern = clause;
  bucket = (unsigned) hash & (guide->body_bucket_count - 1);
  guide->groups[group].bucket_next = guide->body_buckets[bucket];
  guide->body_buckets[bucket] = group + 1;
  return group;
}

static void add_relation(Proof_parent_guide guide, unsigned first,
                         unsigned second, enum proof_parent_rule rule)
{
  struct guide_node *node = &guide->nodes[first];
  if (rule == PROOF_PARENT_PARAMOD)
    append_unsigned(second, &node->paramod_neighbors,
                    &node->paramod_count, &node->paramod_capacity);
  else
    append_unsigned(second, &node->hyper_neighbors,
                    &node->hyper_count, &node->hyper_capacity);
}

static void add_parent_clique(Proof_parent_guide guide, Topform clause,
                              int attribute, unsigned result_node,
                              enum proof_parent_rule rule)
{
  uint64_t *ids;
  unsigned *nodes;
  unsigned count = read_parent_attributes(clause, attribute, &ids);
  unsigned i, j;
  if (rule == PROOF_PARENT_PARAMOD && count != 2)
    fatal_error("proof-parent paramodulation label must have two parents");
  if (rule == PROOF_PARENT_HYPER && count < 2)
    fatal_error("proof-parent hyperresolution label needs two parents");
  nodes = safe_malloc((size_t) count * sizeof(*nodes));
  for (i = 0; i < count; i++) {
    nodes[i] = source_lookup(guide, ids[i]);
    if (nodes[i] >= result_node)
      fatal_error("proof-parent inference references a non-prior parent");
  }
  for (i = 0; i < count; i++)
    for (j = i + 1; j < count; j++) {
      add_relation(guide, nodes[i], nodes[j], rule);
      add_relation(guide, nodes[j], nodes[i], rule);
    }
  if (rule == PROOF_PARENT_PARAMOD)
    guide->stats.paramod_steps++;
  else
    guide->stats.hyper_steps++;
  safe_free(nodes);
  safe_free(ids);
}

static unsigned count_list(Plist clauses)
{
  unsigned count = 0;
  Plist p;
  for (p = clauses; p != NULL; p = p->next) {
    if (count == UINT_MAX)
      fatal_error("proof-parent guide has too many nodes");
    count++;
  }
  return count;
}

static void refresh_resident_bytes(Proof_parent_guide guide)
{
  unsigned i;
  unsigned long long bytes = sizeof(*guide) +
    (unsigned long long) guide->node_count * sizeof(*guide->nodes) +
    (unsigned long long) guide->group_capacity * sizeof(*guide->groups) +
    (unsigned long long) guide->body_bucket_count *
      sizeof(*guide->body_buckets) +
    (unsigned long long) guide->source_capacity *
      sizeof(*guide->source_slots) +
    (unsigned long long) guide->runtime_capacity *
      sizeof(*guide->runtime_buckets) +
    (unsigned long long) guide->partner_capacity *
      sizeof(*guide->partner_workspace) +
    (unsigned long long) guide->recipe_arena_capacity;
  struct runtime_clause *runtime;
  for (i = 0; i < guide->node_count; i++)
    bytes += (unsigned long long) guide->nodes[i].paramod_capacity *
               sizeof(*guide->nodes[i].paramod_neighbors) +
             (unsigned long long) guide->nodes[i].hyper_capacity *
               sizeof(*guide->nodes[i].hyper_neighbors);
  for (i = 0; i < guide->group_count; i++)
    bytes += (unsigned long long) guide->groups[i].node_capacity *
             sizeof(*guide->groups[i].nodes);
  for (runtime = guide->runtime_all; runtime != NULL;
       runtime = runtime->next_all)
    bytes += sizeof(*runtime);
  guide->stats.resident_bytes = bytes;
  if (bytes > guide->stats.resident_peak_bytes)
    guide->stats.resident_peak_bytes = bytes;
}

Proof_parent_guide proof_parent_guide_build(
  Plist clauses, enum proof_parent_guide_mode mode)
{
  Proof_parent_guide guide;
  Plist p;
  unsigned at = 0, i;
  int node_attribute = attribute_name_to_id("proof_parent_node");
  int para_attribute = attribute_name_to_id("proof_parent_para");
  int hyper_attribute = attribute_name_to_id("proof_parent_hyper");
  int rewrite_attribute = attribute_name_to_id("proof_parent_rewrite");
  int recipe_attribute = attribute_name_to_id("proof_parent_recipe");
  double started = user_seconds();
  if (mode == PROOF_PARENT_GUIDE_OFF)
    return NULL;
  guide = safe_calloc(1, sizeof(*guide));
  guide->mode = mode;
  guide->node_count = count_list(clauses);
  if (guide->node_count == 0)
    fatal_error("proof_parent_guidance requires a proof_parent_guide list");
  if (node_attribute < 0 || para_attribute < 0 || hyper_attribute < 0 ||
      rewrite_attribute < 0 || recipe_attribute < 0)
    fatal_error("proof-parent guide attributes were not registered");
  guide->nodes = safe_calloc(guide->node_count, sizeof(*guide->nodes));
  guide->body_bucket_count = next_power_of_two(
    (unsigned long long) guide->node_count * 2);
  guide->body_buckets = safe_calloc(
    guide->body_bucket_count, sizeof(*guide->body_buckets));
  guide->source_capacity = mode == PROOF_PARENT_GUIDE_RECIPE_REPLAY ? 0 :
    next_power_of_two((unsigned long long) guide->node_count * 2);
  if (mode != PROOF_PARENT_GUIDE_RECIPE_REPLAY) {
    guide->source_slots = safe_calloc(
      guide->source_capacity, sizeof(*guide->source_slots));
    guide->runtime_capacity = next_power_of_two(
      (unsigned long long) guide->node_count * 2);
    guide->runtime_buckets = safe_calloc(
      guide->runtime_capacity, sizeof(*guide->runtime_buckets));
  }

  for (p = clauses; p != NULL; p = p->next, at++) {
    Topform clause = p->v;
    int node_id = get_int_attribute(
      clause->attributes, node_attribute, 1);
    uint64_t hash;
    unsigned group;
    if (node_id == INT_MAX) {
      fprintf(stderr, "Proof-parent guide clause missing metadata: ");
      fwrite_clause(stderr, clause, CL_FORM_STD);
      fatal_error("proof-parent guide clause has no proof_parent_node attribute");
    }
    if (node_id <= 0 ||
        get_int_attribute(clause->attributes, node_attribute, 2) != INT_MAX)
      fatal_error("proof-parent guide clause must have one positive node ID");
    guide->nodes[at].source_id = (uint64_t) node_id;
    if (mode != PROOF_PARENT_GUIDE_RECIPE_REPLAY)
      source_insert(guide, guide->nodes[at].source_id, at);
    load_node_recipe(guide, at, clause, recipe_attribute);
    renumber_variables(clause, MAX_VARS);
    guide->stats.guide_body_bytes += clause_body_storage_bytes(clause);
    hash = clause_hash(clause);
    group = find_body_group(guide, clause, hash);
    if (group == UINT_MAX)
      group = add_body_group(guide, clause, hash);
    guide->nodes[at].body_group = group;
    if (mode != PROOF_PARENT_GUIDE_RECIPE_REPLAY)
      append_unsigned(at, &guide->groups[group].nodes,
                      &guide->groups[group].node_count,
                      &guide->groups[group].node_capacity);
  }

  at = 0;
  for (p = clauses;
       mode != PROOF_PARENT_GUIDE_RECIPE_REPLAY && p != NULL;
       p = p->next, at++) {
    Topform clause = p->v;
    BOOL para = exists_attribute(clause->attributes, para_attribute);
    BOOL hyper = exists_attribute(clause->attributes, hyper_attribute);
    if (para && hyper)
      fatal_error("proof-parent node has two primary inference labels");
    if (para)
      add_parent_clique(
        guide, clause, para_attribute, at, PROOF_PARENT_PARAMOD);
    if (hyper)
      add_parent_clique(
        guide, clause, hyper_attribute, at, PROOF_PARENT_HYPER);
    if (exists_attribute(clause->attributes, rewrite_attribute)) {
      uint64_t *ids;
      unsigned count = read_parent_attributes(
        clause, rewrite_attribute, &ids);
      for (i = 0; i < count; i++) {
        unsigned parent = source_lookup(guide, ids[i]);
        if (parent >= at)
          fatal_error("proof-parent rewrite references a non-prior parent");
      }
      guide->stats.rewrite_references += count;
      safe_free(ids);
    }
  }

  for (i = 0; mode != PROOF_PARENT_GUIDE_RECIPE_REPLAY &&
              i < guide->node_count; i++) {
    struct guide_node *node = &guide->nodes[i];
    node->paramod_count = sort_unique_unsigned(
      node->paramod_neighbors, node->paramod_count);
    node->hyper_count = sort_unique_unsigned(
      node->hyper_neighbors, node->hyper_count);
    guide->stats.paramod_relations += node->paramod_count;
    guide->stats.hyper_relations += node->hyper_count;
  }
  /* Metadata is needed only while constructing the graph.  Do not retain
     roughly one attribute object per edge throughout a long search. */
  for (p = clauses; p != NULL; p = p->next) {
    Topform clause = p->v;
    clause->attributes = delete_attributes(
      clause->attributes, node_attribute);
    clause->attributes = delete_attributes(
      clause->attributes, para_attribute);
    clause->attributes = delete_attributes(
      clause->attributes, hyper_attribute);
    clause->attributes = delete_attributes(
      clause->attributes, rewrite_attribute);
    clause->attributes = delete_attributes(
      clause->attributes, recipe_attribute);
  }
  guide->stats.nodes = guide->node_count;
  guide->stats.body_groups = guide->group_count;
  guide->stats.build_seconds = user_seconds() - started;
  refresh_resident_bytes(guide);
  return guide;
}

static struct runtime_clause *find_runtime(Proof_parent_guide guide,
                                           uint64_t id)
{
  struct runtime_clause *record =
    guide->runtime_buckets[(unsigned) mix64(id) &
                           (guide->runtime_capacity - 1)];
  while (record != NULL && record->id != id)
    record = record->next_bucket;
  return record;
}

void proof_parent_guide_activate(Proof_parent_guide guide, Topform clause)
{
  uint64_t hash;
  unsigned group, bucket;
  struct runtime_clause *record;
  if (guide == NULL)
    return;
  guide->stats.activations++;
  hash = clause_hash(clause);
  group = find_body_group(guide, clause, hash);
  if (group == UINT_MAX) {
    guide->stats.unmapped_activations++;
    return;
  }
  if (find_runtime(guide, clause->id) != NULL)
    fatal_error("proof-parent guide activated a duplicate runtime clause ID");
  record = safe_calloc(1, sizeof(*record));
  record->id = clause->id;
  record->clause = clause;
  record->group = group;
  record->active = TRUE;
  bucket = (unsigned) mix64(record->id) & (guide->runtime_capacity - 1);
  record->next_bucket = guide->runtime_buckets[bucket];
  guide->runtime_buckets[bucket] = record;
  record->next_group = guide->groups[group].active;
  guide->groups[group].active = record;
  record->next_all = guide->runtime_all;
  guide->runtime_all = record;
  guide->stats.mapped_activations++;
  guide->stats.active_mapped++;
  if (guide->stats.active_mapped > guide->stats.active_mapped_peak)
    guide->stats.active_mapped_peak = guide->stats.active_mapped;
}

void proof_parent_guide_deactivate(Proof_parent_guide guide, Topform clause)
{
  struct runtime_clause *record;
  if (guide == NULL)
    return;
  record = find_runtime(guide, clause->id);
  if (record != NULL && record->active) {
    record->active = FALSE;
    record->clause = NULL;
    guide->stats.deactivations++;
    guide->stats.active_mapped--;
  }
}

void proof_parent_guide_note_given(Proof_parent_guide guide, Topform clause)
{
  struct runtime_clause *record;
  if (guide == NULL)
    return;
  record = find_runtime(guide, clause->id);
  if (record != NULL && record->active)
    guide->stats.mapped_givens++;
  else
    guide->stats.unmapped_givens++;
}

static BOOL sorted_member(const unsigned *items, unsigned count,
                          unsigned value)
{
  unsigned low = 0, high = count;
  while (low < high) {
    unsigned middle = low + (high - low) / 2;
    if (items[middle] < value)
      low = middle + 1;
    else
      high = middle;
  }
  return low < count && items[low] == value;
}

static BOOL groups_related(Proof_parent_guide guide, unsigned first,
                           unsigned second, enum proof_parent_rule rule)
{
  struct body_group *a = &guide->groups[first];
  struct body_group *b = &guide->groups[second];
  unsigned i, j;
  for (i = 0; i < a->node_count; i++) {
    struct guide_node *node = &guide->nodes[a->nodes[i]];
    const unsigned *neighbors = rule == PROOF_PARENT_PARAMOD ?
      node->paramod_neighbors : node->hyper_neighbors;
    unsigned count = rule == PROOF_PARENT_PARAMOD ?
      node->paramod_count : node->hyper_count;
    for (j = 0; j < b->node_count; j++)
      if (sorted_member(neighbors, count, b->nodes[j]))
        return TRUE;
  }
  return FALSE;
}

BOOL proof_parent_guide_pair_test(Proof_parent_guide guide,
                                  Topform first, Topform second,
                                  enum proof_parent_rule rule)
{
  struct runtime_clause *a, *b;
  BOOL accepted;
  if (guide == NULL)
    return TRUE;
  a = find_runtime(guide, first->id);
  b = find_runtime(guide, second->id);
  accepted = a != NULL && b != NULL && a->active && b->active &&
             groups_related(guide, a->group, b->group, rule);
  if (rule == PROOF_PARENT_PARAMOD) {
    guide->stats.paramod_pair_tests++;
    if (accepted)
      guide->stats.paramod_pair_accepts++;
    else
      guide->stats.paramod_pair_rejects++;
  }
  else {
    guide->stats.hyper_pair_tests++;
    if (accepted)
      guide->stats.hyper_pair_accepts++;
    else
      guide->stats.hyper_pair_rejects++;
  }
  return accepted;
}

static void reserve_partner_workspace(Proof_parent_guide guide,
                                      unsigned needed)
{
  unsigned capacity;
  if (needed <= guide->partner_capacity)
    return;
  capacity = guide->partner_capacity == 0 ? 64 : guide->partner_capacity;
  while (capacity < needed) {
    unsigned next = capacity + (capacity + 1) / 2;
    if (next <= capacity)
      fatal_error("proof-parent partner workspace overflow");
    capacity = next;
  }
  guide->partner_workspace = safe_realloc(
    guide->partner_workspace,
    (size_t) capacity * sizeof(*guide->partner_workspace));
  guide->partner_capacity = capacity;
}

static int compare_clause_id(const void *left, const void *right)
{
  Topform a = *(Topform const *) left;
  Topform b = *(Topform const *) right;
  return a->id < b->id ? -1 : a->id > b->id ? 1 : 0;
}

Topform *proof_parent_guide_paramod_partners(Proof_parent_guide guide,
                                             Topform given,
                                             unsigned *count)
{
  struct runtime_clause *record;
  unsigned raw = 0, out = 0, i, j;
  *count = 0;
  if (guide == NULL)
    return NULL;
  guide->stats.authoritative_queries++;
  record = find_runtime(guide, given->id);
  if (record == NULL || !record->active)
    return guide->partner_workspace;
  {
    struct body_group *group = &guide->groups[record->group];
    for (i = 0; i < group->node_count; i++) {
      struct guide_node *node = &guide->nodes[group->nodes[i]];
      for (j = 0; j < node->paramod_count; j++) {
        struct body_group *partner_group =
          &guide->groups[guide->nodes[node->paramod_neighbors[j]].body_group];
        struct runtime_clause *partner;
        for (partner = partner_group->active; partner != NULL;
             partner = partner->next_group)
          if (partner->active) {
            reserve_partner_workspace(guide, raw + 1);
            guide->partner_workspace[raw++] = partner->clause;
          }
      }
    }
  }
  if (raw > 1)
    qsort(guide->partner_workspace, raw,
          sizeof(*guide->partner_workspace), compare_clause_id);
  for (i = 0; i < raw; i++)
    if (out == 0 || guide->partner_workspace[i]->id !=
                    guide->partner_workspace[out - 1]->id)
      guide->partner_workspace[out++] = guide->partner_workspace[i];
  guide->stats.authoritative_raw_partners += raw;
  guide->stats.authoritative_unique_partners += out;
  guide->stats.authoritative_duplicate_partners += raw - out;
  if (out > guide->stats.authoritative_max_partners)
    guide->stats.authoritative_max_partners = out;
  *count = out;
  return guide->partner_workspace;
}

void fprint_proof_parent_guide_stats(FILE *fp, Proof_parent_guide guide)
{
  const char *mode;
  struct proof_parent_guide_stats *s;
  if (guide == NULL)
    return;
  refresh_resident_bytes(guide);
  s = &guide->stats;
  mode = guide->mode == PROOF_PARENT_GUIDE_SHADOW ? "shadow" :
         guide->mode == PROOF_PARENT_GUIDE_RECIPE_REPLAY ?
           "recipe_replay" : "authoritative";
  fprintf(fp, "\nProof-parent guidance:\n");
  fprintf(fp, "  mode=%s, nodes=%u, unique_bodies=%u, build_seconds=%.2f\n",
          mode, s->nodes, s->body_groups, s->build_seconds);
  fprintf(fp, "  guide_body_bytes=%llu, index_bytes=%llu, "
              "index_peak_bytes=%llu\n",
          s->guide_body_bytes, s->resident_bytes, s->resident_peak_bytes);
  fprintf(fp, "  para_steps=%llu, hyper_steps=%llu, rewrite_refs=%llu\n",
          s->paramod_steps, s->hyper_steps, s->rewrite_references);
  fprintf(fp, "  recipe_nodes=%llu, recipe_bytes=%llu, complete=%s\n",
          s->recipe_nodes, s->recipe_bytes,
          s->recipe_nodes == s->nodes ? "yes" : "no");
  fprintf(fp, "  directed_para_relations=%llu, directed_hyper_relations=%llu\n",
          s->paramod_relations, s->hyper_relations);
  fprintf(fp, "  activations=%llu (mapped=%llu, unmapped=%llu), "
              "deactivations=%llu, active=%llu, active_peak=%llu\n",
          s->activations, s->mapped_activations, s->unmapped_activations,
          s->deactivations, s->active_mapped, s->active_mapped_peak);
  fprintf(fp, "  givens: mapped=%llu, unmapped=%llu\n",
          s->mapped_givens, s->unmapped_givens);
  fprintf(fp, "  para_pair_tests=%llu (accepted=%llu, rejected=%llu)\n",
          s->paramod_pair_tests, s->paramod_pair_accepts,
          s->paramod_pair_rejects);
  fprintf(fp, "  hyper_pair_tests=%llu (accepted=%llu, rejected=%llu)\n",
          s->hyper_pair_tests, s->hyper_pair_accepts,
          s->hyper_pair_rejects);
  fprintf(fp, "  sparse_para_queries=%llu, raw_partners=%llu, "
              "unique_partners=%llu, duplicate_partners=%llu, "
              "max_partners=%llu\n",
          s->authoritative_queries, s->authoritative_raw_partners,
          s->authoritative_unique_partners,
          s->authoritative_duplicate_partners,
          s->authoritative_max_partners);
}

unsigned proof_parent_guide_node_count(Proof_parent_guide guide)
{
  return guide == NULL ? 0 : guide->node_count;
}

Topform proof_parent_guide_node_body(Proof_parent_guide guide, unsigned node)
{
  if (guide == NULL || node == 0 || node > guide->node_count)
    return NULL;
  return guide->groups[guide->nodes[node - 1].body_group].pattern;
}

BOOL proof_parent_guide_has_complete_recipes(Proof_parent_guide guide)
{
  return guide != NULL && guide->stats.recipe_nodes == guide->node_count;
}

BOOL proof_parent_guide_decode_recipe(Proof_parent_guide guide,
                                      unsigned node,
                                      struct proof_recipe_decoded *recipe,
                                      const char **error)
{
  struct guide_node *guide_node;
  if (recipe == NULL) {
    if (error != NULL)
      *error = "NULL decoded recipe output";
    return FALSE;
  }
  memset(recipe, 0, sizeof(*recipe));
  if (guide == NULL || node == 0 || node > guide->node_count) {
    if (error != NULL)
      *error = "proof recipe node is out of range";
    return FALSE;
  }
  guide_node = &guide->nodes[node - 1];
  if (!guide_node->has_recipe) {
    if (error != NULL)
      *error = "proof guide node has no recipe";
    return FALSE;
  }
  return decode_recipe_data(
    guide->recipe_arena + guide_node->recipe_offset,
    guide_node->recipe_size, recipe, error);
}

BOOL proof_parent_guide_recipe_info(Proof_parent_guide guide, unsigned node,
                                    enum proof_recipe_rule *rule,
                                    BOOL *source_given,
                                    unsigned *rewrites,
                                    unsigned *flips)
{
  struct guide_node *guide_node;
  if (guide == NULL || node == 0 || node > guide->node_count)
    return FALSE;
  guide_node = &guide->nodes[node - 1];
  if (!guide_node->has_recipe)
    return FALSE;
  if (rule != NULL)
    *rule = guide_node->recipe_rule;
  if (source_given != NULL)
    *source_given = guide_node->source_given;
  if (rewrites != NULL)
    *rewrites = guide_node->rewrite_count;
  if (flips != NULL)
    *flips = guide_node->flip_count;
  return TRUE;
}

void proof_parent_guide_destroy(Proof_parent_guide guide)
{
  struct runtime_clause *runtime;
  unsigned i;
  if (guide == NULL)
    return;
  for (i = 0; i < guide->node_count; i++) {
    safe_free(guide->nodes[i].paramod_neighbors);
    safe_free(guide->nodes[i].hyper_neighbors);
  }
  for (i = 0; i < guide->group_count; i++)
    safe_free(guide->groups[i].nodes);
  runtime = guide->runtime_all;
  while (runtime != NULL) {
    struct runtime_clause *next = runtime->next_all;
    safe_free(runtime);
    runtime = next;
  }
  safe_free(guide->partner_workspace);
  safe_free(guide->recipe_arena);
  safe_free(guide->runtime_buckets);
  safe_free(guide->source_slots);
  safe_free(guide->body_buckets);
  safe_free(guide->groups);
  safe_free(guide->nodes);
  safe_free(guide);
}
