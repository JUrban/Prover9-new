#include "hash_target_inference.h"
#include "compact_unit_index.h"

#include "../ladr/clause_misc.h"
#include "../ladr/clock.h"
#include "../ladr/memory.h"
#include "../ladr/paramod.h"

#include <limits.h>
#include <stdint.h>

struct target_requirement {
  unsigned long long id;
  Topform from;
  struct target_requirement *next_from;
};

struct hash_target_inference {
  Hash_target_index targets;
  Mindex active_units;
  Clist ordinary_partners;
  Compact_unit_index requirements;
  struct target_requirement **requirements_by_clause;
  unsigned requirement_clause_capacity;
  struct target_requirement **requirement_by_id;
  unsigned requirement_id_capacity;
  unsigned long long next_requirement_id;
  unsigned long long *free_requirement_ids;
  unsigned free_requirement_count;
  unsigned free_requirement_capacity;
  unsigned long long *retired_requirement_ids;
  unsigned retired_requirement_count;
  unsigned retired_requirement_capacity;
  unsigned *from_marks;
  unsigned *into_marks;
  unsigned mark_capacity;
  unsigned mark_serial;
  Topform *from_partners;
  unsigned from_partner_count;
  unsigned from_partner_capacity;
  Topform *into_partners;
  unsigned into_partner_count;
  unsigned into_partner_capacity;
  struct hash_target_inference_stats stats;
};

static BOOL target_active_unit(Topform c)
{
  return c != NULL && unit_clause(c->literals) && pos_eq(c->literals);
}

static void reserve_partner_marks(Hash_target_inference inference,
                                  unsigned long long id)
{
  unsigned capacity;
  if (id > UINT_MAX)
    fatal_error("target-directed active clause ID exceeds compact marks");
  if (id < inference->mark_capacity)
    return;
  capacity = inference->mark_capacity == 0 ? 1024 : inference->mark_capacity;
  while (capacity <= (unsigned) id) {
    unsigned next = capacity + (capacity + 1) / 2;
    if (next <= capacity)
      fatal_error("target-directed partner marks overflow");
    capacity = next;
  }
  inference->from_marks = safe_realloc(
    inference->from_marks, (size_t) capacity * sizeof(*inference->from_marks));
  inference->into_marks = safe_realloc(
    inference->into_marks, (size_t) capacity * sizeof(*inference->into_marks));
  memset(inference->from_marks + inference->mark_capacity, 0,
         (size_t) (capacity - inference->mark_capacity) *
           sizeof(*inference->from_marks));
  memset(inference->into_marks + inference->mark_capacity, 0,
         (size_t) (capacity - inference->mark_capacity) *
           sizeof(*inference->into_marks));
  inference->mark_capacity = capacity;
}

static void reserve_partners(Topform **items, unsigned *capacity_ptr,
                             unsigned needed)
{
  unsigned capacity;
  if (needed <= *capacity_ptr)
    return;
  capacity = *capacity_ptr == 0 ? 256 : *capacity_ptr;
  while (capacity < needed) {
    unsigned next = capacity + (capacity + 1) / 2;
    if (next <= capacity)
      fatal_error("target-directed partner workspace overflow");
    capacity = next;
  }
  *items = safe_realloc(*items, (size_t) capacity * sizeof(**items));
  *capacity_ptr = capacity;
}

static void refresh_workspace_stats(Hash_target_inference inference)
{
  unsigned long long bytes =
    (unsigned long long) inference->mark_capacity * sizeof(*inference->from_marks) +
    (unsigned long long) inference->mark_capacity * sizeof(*inference->into_marks) +
    (unsigned long long) inference->from_partner_capacity *
      sizeof(*inference->from_partners) +
    (unsigned long long) inference->into_partner_capacity *
      sizeof(*inference->into_partners) +
    (unsigned long long) inference->requirement_clause_capacity *
      sizeof(*inference->requirements_by_clause) +
    (unsigned long long) inference->requirement_id_capacity *
      sizeof(*inference->requirement_by_id) +
    (unsigned long long) inference->free_requirement_capacity *
      sizeof(*inference->free_requirement_ids) +
    (unsigned long long) inference->retired_requirement_capacity *
      sizeof(*inference->retired_requirement_ids);
  inference->stats.workspace_bytes = bytes;
  if (bytes > inference->stats.workspace_peak_bytes)
    inference->stats.workspace_peak_bytes = bytes;
}

static void add_partner(Hash_target_inference inference, Topform partner,
                        BOOL given_is_from)
{
  unsigned id;
  unsigned *marks;
  Topform **partners;
  unsigned *count, *capacity;
  if (!target_active_unit(partner))
    return;
  reserve_partner_marks(inference, partner->id);
  id = (unsigned) partner->id;
  marks = given_is_from ? inference->from_marks : inference->into_marks;
  partners = given_is_from ? &inference->from_partners :
                             &inference->into_partners;
  count = given_is_from ? &inference->from_partner_count :
                          &inference->into_partner_count;
  capacity = given_is_from ? &inference->from_partner_capacity :
                             &inference->into_partner_capacity;
  if (marks[id] == inference->mark_serial) {
    inference->stats.duplicate_partners++;
    return;
  }
  marks[id] = inference->mark_serial;
  reserve_partners(partners, capacity, *count + 1);
  (*partners)[(*count)++] = partner;
  inference->stats.unique_partners++;
  refresh_workspace_stats(inference);
}

static int compare_partner_id(const void *a, const void *b)
{
  Topform x = *(const Topform *) a;
  Topform y = *(const Topform *) b;
  return x->id < y->id ? -1 : x->id > y->id ? 1 : 0;
}

static void query_required_atom(Hash_target_inference inference, Term atom)
{
  Context query_context = get_context();
  Context found_context = get_context();
  Mindex_pos position = NULL;
  Term found;
  term_renumber_variables(atom, MAX_VARS);
  inference->stats.required_queries++;
  found = mindex_retrieve_first(atom, inference->active_units, UNIFY,
                                query_context, found_context, FALSE,
                                &position);
  while (found != NULL) {
    inference->stats.required_query_answers++;
    add_partner(inference, (Topform) found->container, TRUE);
    found = mindex_retrieve_next(position);
  }
  free_context(query_context);
  free_context(found_context);
}

static void reserve_requirement_clauses(Hash_target_inference inference,
                                        unsigned long long id)
{
  unsigned capacity;
  if (id > UINT_MAX)
    fatal_error("target-directed requirement clause ID overflow");
  if (id < inference->requirement_clause_capacity)
    return;
  capacity = inference->requirement_clause_capacity == 0 ? 1024 :
             inference->requirement_clause_capacity;
  while (capacity <= (unsigned) id) {
    unsigned next = capacity + (capacity + 1) / 2;
    if (next <= capacity)
      fatal_error("target-directed requirement directory overflow");
    capacity = next;
  }
  inference->requirements_by_clause = safe_realloc(
    inference->requirements_by_clause,
    (size_t) capacity * sizeof(*inference->requirements_by_clause));
  memset(inference->requirements_by_clause +
           inference->requirement_clause_capacity, 0,
         (size_t) (capacity - inference->requirement_clause_capacity) *
           sizeof(*inference->requirements_by_clause));
  inference->requirement_clause_capacity = capacity;
  refresh_workspace_stats(inference);
}

static void reserve_requirement_ids(Hash_target_inference inference,
                                    unsigned long long id)
{
  unsigned capacity;
  if (id > UINT_MAX)
    fatal_error("target-directed requirement ID overflow");
  if (id < inference->requirement_id_capacity)
    return;
  capacity = inference->requirement_id_capacity == 0 ? 1024 :
             inference->requirement_id_capacity;
  while (capacity <= (unsigned) id) {
    unsigned next = capacity + (capacity + 1) / 2;
    if (next <= capacity)
      fatal_error("target-directed requirement ID directory overflow");
    capacity = next;
  }
  inference->requirement_by_id = safe_realloc(
    inference->requirement_by_id,
    (size_t) capacity * sizeof(*inference->requirement_by_id));
  memset(inference->requirement_by_id + inference->requirement_id_capacity, 0,
         (size_t) (capacity - inference->requirement_id_capacity) *
           sizeof(*inference->requirement_by_id));
  inference->requirement_id_capacity = capacity;
  refresh_workspace_stats(inference);
}

static void append_requirement_id(unsigned long long **ids,
                                  unsigned *count, unsigned *capacity,
                                  unsigned long long id)
{
  if (*count == *capacity) {
    unsigned next = *capacity == 0 ? 1024 :
      *capacity + (*capacity + 1) / 2;
    if (next <= *capacity)
      fatal_error("target-directed retired-ID workspace overflow");
    *ids = safe_realloc(*ids, (size_t) next * sizeof(**ids));
    *capacity = next;
  }
  (*ids)[(*count)++] = id;
}

static unsigned long long new_requirement_id(Hash_target_inference inference)
{
  unsigned long long id;
  if (inference->free_requirement_count != 0)
    id = inference->free_requirement_ids[--inference->free_requirement_count];
  else {
    if (inference->next_requirement_id == UINT_MAX)
      fatal_error("target-directed requirement ID overflow");
    id = ++inference->next_requirement_id;
  }
  if (id == 0 || id > UINT_MAX)
    fatal_error("target-directed requirement ID is not compact");
  return id;
}

static void release_reclaimed_requirement_ids(
  Hash_target_inference inference)
{
  unsigned i;
  for (i = 0; i < inference->retired_requirement_count; i++)
    append_requirement_id(&inference->free_requirement_ids,
                          &inference->free_requirement_count,
                          &inference->free_requirement_capacity,
                          inference->retired_requirement_ids[i]);
  inference->retired_requirement_count = 0;
  refresh_workspace_stats(inference);
}

static void refresh_requirement_bytes(Hash_target_inference inference)
{
  struct compact_unit_index_stats compact;
  unsigned long long bytes;
  compact_unit_index_get_stats(inference->requirements, &compact);
  bytes = compact.total_bytes +
    inference->stats.requirements * sizeof(struct target_requirement) +
    (unsigned long long) inference->requirement_id_capacity *
      sizeof(*inference->requirement_by_id) +
    (unsigned long long) inference->requirement_clause_capacity *
      sizeof(*inference->requirements_by_clause);
  inference->stats.requirement_physical = compact.physical;
  inference->stats.requirement_compactions = compact.compactions;
  inference->stats.requirement_bytes_reclaimed = compact.bytes_reclaimed;
  inference->stats.requirement_code_nodes = compact.node_items;
  inference->stats.requirement_code_postings = compact.posting_items;
  inference->stats.requirement_code_queries = compact.code_tree_queries;
  inference->stats.requirement_code_nodes_examined =
    compact.code_tree_nodes_examined;
  inference->stats.requirement_code_postings_examined =
    compact.code_tree_postings_examined;
  inference->stats.requirement_exact_tests = compact.unifier_exact_tests;
  inference->stats.requirement_token_bytes = compact.token_bytes;
  inference->stats.requirement_bytes = bytes;
  if (bytes > inference->stats.requirement_peak_bytes)
    inference->stats.requirement_peak_bytes = bytes;
  if (inference->stats.requirement_budget_bytes != 0 &&
      bytes > inference->stats.requirement_budget_bytes)
    fatal_error("target-directed requirements exceed hash_target_index_kb");
}

static void store_requirement(Hash_target_inference inference, Term atom,
                              Topform from, int from_side)
{
  struct target_requirement *requirement;
  Topform packed = get_topform();
  unsigned long long id = new_requirement_id(inference);
  (void) from_side;
  reserve_requirement_clauses(inference, from->id);
  reserve_requirement_ids(inference, id);
  packed->id = id;
  packed->literals = new_literal(TRUE, atom);
  upward_clause_links(packed);
  if (!compact_unit_index_add(inference->requirements, packed)) {
    delete_clause(packed);
    fatal_error("cannot add target-directed compact requirement");
  }
  requirement = safe_malloc(sizeof(*requirement));
  requirement->id = id;
  requirement->from = from;
  requirement->next_from =
    inference->requirements_by_clause[(unsigned) from->id];
  inference->requirements_by_clause[(unsigned) from->id] = requirement;
  inference->requirement_by_id[(unsigned) id] = requirement;
  delete_clause(packed);
  inference->stats.requirements++;
  if (inference->stats.requirements > inference->stats.requirement_peak)
    inference->stats.requirement_peak = inference->stats.requirements;
  refresh_requirement_bytes(inference);
}

static void query_required_orientations(Hash_target_inference inference,
                                        Term atom, Topform from,
                                        int from_side)
{
  Term tmp;
  query_required_atom(inference, atom);
  store_requirement(inference, copy_term(atom), from, from_side);
  tmp = ARG(atom, 0);
  ARG(atom, 0) = ARG(atom, 1);
  ARG(atom, 1) = tmp;
  query_required_atom(inference, atom);
}

static void plan_target_term(Hash_target_inference inference,
                             Term target_atom, Term target_term,
                             Term alpha, Term beta,
                             Topform from, int from_side,
                             Context from_context, Context target_context,
                             struct ilist *path, unsigned depth)
{
  int i;
  Trail trail = NULL;
  inference->stats.target_positions++;
  if ((VARIABLE(beta) ||
       (!VARIABLE(target_term) &&
        SYMNUM(beta) == SYMNUM(target_term))) &&
      unify(beta, from_context, target_term, target_context, &trail)) {
    Term required;
    inference->stats.compatible_positions++;
    path[depth].next = NULL;
    required = apply_substitute2(target_atom, alpha, from_context,
                                 path, target_context);
    query_required_orientations(inference, required, from, from_side);
    zap_term(required);
    undo_subst(trail);
  }
  for (i = 0; i < ARITY(target_term); i++) {
    if (depth + 1 >= 1000)
      fatal_error("target-directed term path exceeds 1000");
    path[depth].next = path + depth + 1;
    path[depth + 1].i = i + 1;
    plan_target_term(inference, target_atom, ARG(target_term, i),
                     alpha, beta, from, from_side,
                     from_context, target_context,
                     path, depth + 1);
  }
  path[depth].next = NULL;
}

static void plan_from_side(Hash_target_inference inference, Topform given,
                           int side)
{
  Literals literal = given->literals;
  Term alpha = ARG(literal->atom, side);
  Term beta = ARG(literal->atom, side == 0 ? 1 : 0);
  Hash_target_query query;
  unsigned recipe;
  Context from_context = get_context();
  Context target_context = get_context();
  struct ilist path[1000];
  inference->stats.from_sides++;
  if (VARIABLE(beta)) {
    inference->stats.variable_from_sides++;
    free_context(from_context);
    free_context(target_context);
    return;
  }
  hash_target_query_init(inference->targets, beta, &query);
  begin_generalized_hash_target_scan();
  while (hash_target_query_next(&query, &recipe)) {
    Topform target = reconstruct_generalized_hash_target(recipe);
    Term atom = target->literals->atom;
    int argument;
    inference->stats.target_recipes++;
    for (argument = 0; argument < 2; argument++) {
      path[0].i = argument + 1;
      path[0].next = NULL;
      plan_target_term(inference, atom, ARG(atom, argument), alpha, beta,
                       given, side, from_context, target_context, path, 0);
    }
    delete_clause(target);
  }
  end_generalized_hash_target_scan();
  free_context(from_context);
  free_context(target_context);
}

Hash_target_inference hash_target_inference_init(Hash_target_index targets,
                                                 int fpa_depth,
                                                 unsigned long long
                                                   requirement_budget_bytes)
{
  Hash_target_inference inference = safe_calloc(1, sizeof(*inference));
  inference->targets = targets;
  inference->active_units = mindex_init(FPA, ORDINARY_UNIF, fpa_depth);
  inference->ordinary_partners = clist_init("hash_target_ordinary_partners");
  inference->requirements =
    compact_unit_index_init_strategy(COMPACT_UNIT_CODE_TREE);
  inference->stats.requirement_budget_bytes = requirement_budget_bytes;
  inference->mark_serial = 1;
  refresh_requirement_bytes(inference);
  return inference;
}

void hash_target_inference_destroy(Hash_target_inference inference)
{
  if (inference == NULL)
    return;
  if (!mindex_empty(inference->active_units))
    fatal_error("target-directed active-unit index is not empty");
  if (!clist_empty(inference->ordinary_partners))
    fatal_error("target-directed ordinary-partner list is not empty");
  if (inference->stats.requirements != 0 ||
      compact_unit_index_active_records(inference->requirements) != 0)
    fatal_error("target-directed requirement index is not empty");
  mindex_destroy(inference->active_units);
  clist_free(inference->ordinary_partners);
  compact_unit_index_free(inference->requirements);
  safe_free(inference->requirements_by_clause);
  safe_free(inference->requirement_by_id);
  safe_free(inference->free_requirement_ids);
  safe_free(inference->retired_requirement_ids);
  safe_free(inference->from_marks);
  safe_free(inference->into_marks);
  safe_free(inference->from_partners);
  safe_free(inference->into_partners);
  safe_free(inference);
}

void hash_target_inference_update(Hash_target_inference inference,
                                  Topform clause, Indexop op)
{
  if (inference == NULL)
    return;
  if (!target_active_unit(clause)) {
    if (op == INSERT) {
      clist_append(clause, inference->ordinary_partners);
      inference->stats.active_ordinary_partners++;
      if (inference->stats.active_ordinary_partners >
          inference->stats.active_ordinary_partner_peak)
        inference->stats.active_ordinary_partner_peak =
          inference->stats.active_ordinary_partners;
    }
    else {
      if (!clist_member(clause, inference->ordinary_partners) ||
          inference->stats.active_ordinary_partners == 0)
        fatal_error("target-directed ordinary-partner lifecycle mismatch");
      clist_remove(clause, inference->ordinary_partners);
      inference->stats.active_ordinary_partners--;
    }
    return;
  }
  mindex_update(inference->active_units, clause->literals->atom, op);
  if (op == INSERT) {
    inference->stats.active_units++;
    if (inference->stats.active_units > inference->stats.active_unit_peak)
      inference->stats.active_unit_peak = inference->stats.active_units;
  }
  else {
    struct target_requirement *requirement, *next;
    BOOL removed_requirement = FALSE;
    if (clause->id < inference->requirement_clause_capacity) {
      requirement =
        inference->requirements_by_clause[(unsigned) clause->id];
      inference->requirements_by_clause[(unsigned) clause->id] = NULL;
      while (requirement != NULL) {
        next = requirement->next_from;
        if (!compact_unit_index_remove(inference->requirements,
                                       requirement->id))
          fatal_error("cannot remove target-directed compact requirement");
        if (requirement->id >= inference->requirement_id_capacity ||
            inference->requirement_by_id[(unsigned) requirement->id] !=
              requirement)
          fatal_error("target-directed requirement directory mismatch");
        inference->requirement_by_id[(unsigned) requirement->id] = NULL;
        append_requirement_id(&inference->retired_requirement_ids,
                              &inference->retired_requirement_count,
                              &inference->retired_requirement_capacity,
                              requirement->id);
        if (inference->stats.requirements == 0)
          fatal_error("target-directed requirement count underflow");
        inference->stats.requirements--;
        removed_requirement = TRUE;
        safe_free(requirement);
        requirement = next;
      }
      if (removed_requirement && inference->stats.requirements == 0) {
        compact_unit_index_free(inference->requirements);
        inference->requirements =
          compact_unit_index_init_strategy(COMPACT_UNIT_CODE_TREE);
        inference->stats.requirement_resets++;
        release_reclaimed_requirement_ids(inference);
      }
      else if (removed_requirement &&
               compact_unit_index_compaction_needed(inference->requirements) &&
               compact_unit_index_reclaim_owning_pool(
                 inference->requirements))
        release_reclaimed_requirement_ids(inference);
      refresh_requirement_bytes(inference);
    }
    if (inference->stats.active_units == 0)
      fatal_error("target-directed active-unit count underflow");
    inference->stats.active_units--;
  }
}

static void plan_into_from_requirements(Hash_target_inference inference,
                                        Topform given)
{
  Term query_atom;
  int orientation;
  if (!target_active_unit(given))
    return;
  query_atom = copy_term(given->literals->atom);
  for (orientation = 0; orientation < 2; orientation++) {
    unsigned long long *ids;
    size_t count, i;
    if (orientation == 1) {
      Term tmp = ARG(query_atom, 0);
      ARG(query_atom, 0) = ARG(query_atom, 1);
      ARG(query_atom, 1) = tmp;
    }
    inference->stats.requirement_queries++;
    ids = compact_unit_unifier_ids(inference->requirements, query_atom,
                                   TRUE, 0, &count);
    inference->stats.requirement_answers += count;
    for (i = 0; i < count; i++) {
      struct target_requirement *requirement;
      if (ids[i] >= inference->requirement_id_capacity ||
          (requirement =
             inference->requirement_by_id[(unsigned) ids[i]]) == NULL)
        fatal_error("target-directed compact result has no owner");
      add_partner(inference, requirement->from, FALSE);
    }
    safe_free(ids);
  }
  zap_term(query_atom);
  refresh_requirement_bytes(inference);
}

void hash_target_inference_plan_from(Hash_target_inference inference,
                                     Topform given)
{
  double started;
  int side;
  if (inference == NULL)
    return;
  inference->from_partner_count = 0;
  inference->into_partner_count = 0;
  if (++inference->mark_serial == 0) {
    memset(inference->from_marks, 0,
           (size_t) inference->mark_capacity * sizeof(*inference->from_marks));
    memset(inference->into_marks, 0,
           (size_t) inference->mark_capacity * sizeof(*inference->into_marks));
    inference->mark_serial = 1;
  }
  inference->stats.plans++;
  started = user_seconds();
  plan_into_from_requirements(inference, given);
  if (target_active_unit(given))
    for (side = 0; side < 2; side++)
      if (para_unit_from_side_eligible(given, side))
        plan_from_side(inference, given, side);
  if (inference->from_partner_count > 1)
    qsort(inference->from_partners, inference->from_partner_count,
          sizeof(*inference->from_partners), compare_partner_id);
  if (inference->into_partner_count > 1)
    qsort(inference->into_partners, inference->into_partner_count,
          sizeof(*inference->into_partners), compare_partner_id);
  if (inference->stats.active_units > ULLONG_MAX / 2 ||
      inference->stats.potential_unit_pair_directions >
        ULLONG_MAX - inference->stats.active_units * 2)
    fatal_error("target-directed pair-direction statistics overflow");
  inference->stats.potential_unit_pair_directions +=
    inference->stats.active_units * 2;
  if (inference->stats.planned_unit_pair_directions >
      ULLONG_MAX - inference->from_partner_count -
        inference->into_partner_count)
    fatal_error("target-directed planned-direction statistics overflow");
  inference->stats.planned_unit_pair_directions +=
    inference->from_partner_count + inference->into_partner_count;
  inference->stats.avoided_unit_pair_directions =
    inference->stats.potential_unit_pair_directions -
    inference->stats.planned_unit_pair_directions;
  inference->stats.planning_seconds += user_seconds() - started;
}

unsigned hash_target_inference_partner_count(Hash_target_inference inference,
                                             BOOL given_is_from)
{
  return inference == NULL ? 0 :
    (given_is_from ? inference->from_partner_count :
                     inference->into_partner_count);
}

Topform hash_target_inference_partner(Hash_target_inference inference,
                                      BOOL given_is_from, unsigned index)
{
  unsigned count = hash_target_inference_partner_count(
    inference, given_is_from);
  if (inference == NULL || index >= count)
    return NULL;
  return given_is_from ? inference->from_partners[index] :
                         inference->into_partners[index];
}

Clist hash_target_inference_ordinary_partners(
  Hash_target_inference inference)
{
  return inference == NULL ? NULL : inference->ordinary_partners;
}

BOOL hash_target_inference_partner_planned(Hash_target_inference inference,
                                           Topform clause,
                                           BOOL given_is_from)
{
  unsigned id;
  if (inference == NULL || clause == NULL || clause->id > UINT_MAX)
    return FALSE;
  id = (unsigned) clause->id;
  return id < inference->mark_capacity &&
         (given_is_from ? inference->from_marks[id] :
                          inference->into_marks[id]) == inference->mark_serial;
}

void hash_target_inference_note_hash_hit(Hash_target_inference inference,
                                         BOOL covered)
{
  if (inference == NULL)
    return;
  inference->stats.ordinary_hash_hits++;
  if (covered)
    inference->stats.covered_hash_hits++;
  else
    inference->stats.missed_hash_hits++;
}

void hash_target_inference_get_stats(Hash_target_inference inference,
                                     struct hash_target_inference_stats *stats)
{
  if (inference == NULL)
    memset(stats, 0, sizeof(*stats));
  else {
    refresh_workspace_stats(inference);
    refresh_requirement_bytes(inference);
    *stats = inference->stats;
  }
}

void fprint_hash_target_inference_stats(FILE *fp,
                                        Hash_target_inference inference)
{
  struct hash_target_inference_stats s;
  hash_target_inference_get_stats(inference, &s);
  fprintf(fp,
          "Hash_target_inference: active_units=%llu, peak=%llu, "
          "ordinary_partners=%llu, ordinary_partner_peak=%llu, plans=%llu, "
          "from_sides=%llu, variable_from_sides=%llu, recipes=%llu, "
          "positions=%llu, compatible_positions=%llu, required_queries=%llu, "
          "required_answers=%llu, unique_partners=%llu, duplicates=%llu, "
          "potential_unit_directions=%llu, planned_unit_directions=%llu, "
          "avoided_unit_directions=%llu, "
          "ordinary_hash_hits=%llu, covered_hash_hits=%llu, "
          "missed_hash_hits=%llu, planning_seconds=%.3f, "
          "requirements=%llu, requirement_peak=%llu, "
          "requirement_physical=%llu, requirement_bytes=%llu, "
          "requirement_peak_bytes=%llu, requirement_token_bytes=%llu, "
          "requirement_compactions=%llu, requirement_resets=%llu, "
          "requirement_bytes_reclaimed=%llu, requirement_code_nodes=%llu, "
          "requirement_code_postings=%llu, requirement_code_queries=%llu, "
          "requirement_code_nodes_examined=%llu, "
          "requirement_code_postings_examined=%llu, "
          "requirement_exact_tests=%llu, "
          "requirement_budget_bytes=%llu, requirement_queries=%llu, "
          "requirement_answers=%llu, requirement_duplicates=%llu, "
          "workspace_bytes=%llu, "
          "workspace_peak_bytes=%llu.\n",
          s.active_units, s.active_unit_peak, s.active_ordinary_partners,
          s.active_ordinary_partner_peak, s.plans, s.from_sides,
          s.variable_from_sides, s.target_recipes, s.target_positions,
          s.compatible_positions, s.required_queries,
          s.required_query_answers, s.unique_partners,
          s.duplicate_partners, s.potential_unit_pair_directions,
          s.planned_unit_pair_directions,
          s.avoided_unit_pair_directions, s.ordinary_hash_hits,
          s.covered_hash_hits, s.missed_hash_hits, s.planning_seconds,
          s.requirements, s.requirement_peak, s.requirement_physical,
          s.requirement_bytes, s.requirement_peak_bytes,
          s.requirement_token_bytes, s.requirement_compactions,
          s.requirement_resets, s.requirement_bytes_reclaimed,
          s.requirement_code_nodes, s.requirement_code_postings,
          s.requirement_code_queries, s.requirement_code_nodes_examined,
          s.requirement_code_postings_examined,
          s.requirement_exact_tests, s.requirement_budget_bytes,
          s.requirement_queries, s.requirement_answers,
          s.requirement_duplicates,
          s.workspace_bytes, s.workspace_peak_bytes);
}
