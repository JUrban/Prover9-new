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
      sizeof(*guide->partner_workspace);
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
  double started = user_seconds();
  if (mode == PROOF_PARENT_GUIDE_OFF)
    return NULL;
  guide = safe_calloc(1, sizeof(*guide));
  guide->mode = mode;
  guide->node_count = count_list(clauses);
  if (guide->node_count == 0)
    fatal_error("proof_parent_guidance requires a proof_parent_guide list");
  if (node_attribute < 0 || para_attribute < 0 || hyper_attribute < 0 ||
      rewrite_attribute < 0)
    fatal_error("proof-parent guide attributes were not registered");
  guide->nodes = safe_calloc(guide->node_count, sizeof(*guide->nodes));
  guide->body_bucket_count = next_power_of_two(
    (unsigned long long) guide->node_count * 2);
  guide->body_buckets = safe_calloc(
    guide->body_bucket_count, sizeof(*guide->body_buckets));
  guide->source_capacity = next_power_of_two(
    (unsigned long long) guide->node_count * 2);
  guide->source_slots = safe_calloc(
    guide->source_capacity, sizeof(*guide->source_slots));
  guide->runtime_capacity = next_power_of_two(
    (unsigned long long) guide->node_count * 2);
  guide->runtime_buckets = safe_calloc(
    guide->runtime_capacity, sizeof(*guide->runtime_buckets));

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
    source_insert(guide, guide->nodes[at].source_id, at);
    renumber_variables(clause, MAX_VARS);
    guide->stats.guide_body_bytes += clause_body_storage_bytes(clause);
    hash = clause_hash(clause);
    group = find_body_group(guide, clause, hash);
    if (group == UINT_MAX)
      group = add_body_group(guide, clause, hash);
    guide->nodes[at].body_group = group;
    append_unsigned(at, &guide->groups[group].nodes,
                    &guide->groups[group].node_count,
                    &guide->groups[group].node_capacity);
  }

  at = 0;
  for (p = clauses; p != NULL; p = p->next, at++) {
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

  for (i = 0; i < guide->node_count; i++) {
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
         "authoritative";
  fprintf(fp, "\nProof-parent guidance:\n");
  fprintf(fp, "  mode=%s, nodes=%u, unique_bodies=%u, build_seconds=%.2f\n",
          mode, s->nodes, s->body_groups, s->build_seconds);
  fprintf(fp, "  guide_body_bytes=%llu, index_bytes=%llu, "
              "index_peak_bytes=%llu\n",
          s->guide_body_bytes, s->resident_bytes, s->resident_peak_bytes);
  fprintf(fp, "  para_steps=%llu, hyper_steps=%llu, rewrite_refs=%llu\n",
          s->paramod_steps, s->hyper_steps, s->rewrite_references);
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
  safe_free(guide->runtime_buckets);
  safe_free(guide->source_slots);
  safe_free(guide->body_buckets);
  safe_free(guide->groups);
  safe_free(guide->nodes);
  safe_free(guide);
}
