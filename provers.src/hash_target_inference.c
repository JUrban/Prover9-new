#include "hash_target_inference.h"

#include "../ladr/clause_misc.h"
#include "../ladr/clock.h"
#include "../ladr/memory.h"
#include "../ladr/paramod.h"

#include <stdint.h>

struct target_requirement {
  Term atom;
  Topform from;
  int from_side;
  unsigned long long charged_bytes;
  struct target_requirement *next_from;
};

struct hash_target_inference {
  Hash_target_index targets;
  Mindex active_units;
  Mindex requirements;
  struct target_requirement **requirements_by_clause;
  unsigned requirement_clause_capacity;
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
      sizeof(*inference->requirements_by_clause);
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

static unsigned long long requirement_term_bytes(Term t)
{
  unsigned long long bytes = 0;
  int i;
  if (!VARIABLE(t))
    bytes += (unsigned long long)
      (PTRS(sizeof(struct term)) + ARITY(t)) * BYTES_POINTER;
  for (i = 0; i < ARITY(t); i++)
    bytes += requirement_term_bytes(ARG(t, i));
  return bytes;
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

static void store_requirement(Hash_target_inference inference, Term atom,
                              Topform from, int from_side)
{
  struct target_requirement *requirement;
  unsigned long long bytes = sizeof(*requirement) +
                             requirement_term_bytes(atom);
  if (inference->stats.requirement_budget_bytes != 0 &&
      inference->stats.requirement_bytes + bytes >
        inference->stats.requirement_budget_bytes) {
    zap_term(atom);
    fatal_error("target-directed requirements exceed hash_target_index_kb");
  }
  reserve_requirement_clauses(inference, from->id);
  requirement = safe_malloc(sizeof(*requirement));
  requirement->atom = atom;
  requirement->from = from;
  requirement->from_side = from_side;
  requirement->charged_bytes = bytes;
  requirement->next_from =
    inference->requirements_by_clause[(unsigned) from->id];
  inference->requirements_by_clause[(unsigned) from->id] = requirement;
  atom->container = requirement;
  mindex_update(inference->requirements, atom, INSERT);
  inference->stats.requirements++;
  inference->stats.requirement_bytes += bytes;
  if (inference->stats.requirements > inference->stats.requirement_peak)
    inference->stats.requirement_peak = inference->stats.requirements;
  if (inference->stats.requirement_bytes >
      inference->stats.requirement_peak_bytes)
    inference->stats.requirement_peak_bytes =
      inference->stats.requirement_bytes;
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
  store_requirement(inference, copy_term(atom), from, from_side);
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
  inference->requirements = mindex_init(FPA, ORDINARY_UNIF, fpa_depth);
  inference->stats.requirement_budget_bytes = requirement_budget_bytes;
  inference->mark_serial = 1;
  return inference;
}

void hash_target_inference_destroy(Hash_target_inference inference)
{
  if (inference == NULL)
    return;
  if (!mindex_empty(inference->active_units))
    fatal_error("target-directed active-unit index is not empty");
  if (!mindex_empty(inference->requirements))
    fatal_error("target-directed requirement index is not empty");
  mindex_destroy(inference->active_units);
  mindex_destroy(inference->requirements);
  safe_free(inference->requirements_by_clause);
  safe_free(inference->from_marks);
  safe_free(inference->into_marks);
  safe_free(inference->from_partners);
  safe_free(inference->into_partners);
  safe_free(inference);
}

void hash_target_inference_update(Hash_target_inference inference,
                                  Topform clause, Indexop op)
{
  if (inference == NULL || !target_active_unit(clause))
    return;
  mindex_update(inference->active_units, clause->literals->atom, op);
  if (op == INSERT) {
    inference->stats.active_units++;
    if (inference->stats.active_units > inference->stats.active_unit_peak)
      inference->stats.active_unit_peak = inference->stats.active_units;
  }
  else {
    struct target_requirement *requirement, *next;
    if (clause->id < inference->requirement_clause_capacity) {
      requirement =
        inference->requirements_by_clause[(unsigned) clause->id];
      inference->requirements_by_clause[(unsigned) clause->id] = NULL;
      while (requirement != NULL) {
        next = requirement->next_from;
        mindex_update(inference->requirements, requirement->atom, DELETE);
        zap_term(requirement->atom);
        if (inference->stats.requirements == 0 ||
            inference->stats.requirement_bytes < requirement->charged_bytes)
          fatal_error("target-directed requirement accounting underflow");
        inference->stats.requirements--;
        inference->stats.requirement_bytes -= requirement->charged_bytes;
        safe_free(requirement);
        requirement = next;
      }
    }
    if (inference->stats.active_units == 0)
      fatal_error("target-directed active-unit count underflow");
    inference->stats.active_units--;
  }
}

static void plan_into_from_requirements(Hash_target_inference inference,
                                        Topform given)
{
  Context query_context, found_context;
  Mindex_pos position = NULL;
  Term found;
  if (!target_active_unit(given))
    return;
  query_context = get_context();
  found_context = get_context();
  inference->stats.requirement_queries++;
  found = mindex_retrieve_first(given->literals->atom,
                                inference->requirements, UNIFY,
                                query_context, found_context, FALSE,
                                &position);
  while (found != NULL) {
    struct target_requirement *requirement = found->container;
    inference->stats.requirement_answers++;
    add_partner(inference, requirement->from, FALSE);
    found = mindex_retrieve_next(position);
  }
  free_context(query_context);
  free_context(found_context);
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
  else
    *stats = inference->stats;
}

void fprint_hash_target_inference_stats(FILE *fp,
                                        Hash_target_inference inference)
{
  struct hash_target_inference_stats s;
  hash_target_inference_get_stats(inference, &s);
  fprintf(fp,
          "Hash_target_inference: active_units=%llu, peak=%llu, plans=%llu, "
          "from_sides=%llu, variable_from_sides=%llu, recipes=%llu, "
          "positions=%llu, compatible_positions=%llu, required_queries=%llu, "
          "required_answers=%llu, unique_partners=%llu, duplicates=%llu, "
          "ordinary_hash_hits=%llu, covered_hash_hits=%llu, "
          "missed_hash_hits=%llu, planning_seconds=%.3f, "
          "requirements=%llu, requirement_peak=%llu, "
          "requirement_bytes=%llu, requirement_peak_bytes=%llu, "
          "requirement_budget_bytes=%llu, requirement_queries=%llu, "
          "requirement_answers=%llu, workspace_bytes=%llu, "
          "workspace_peak_bytes=%llu.\n",
          s.active_units, s.active_unit_peak, s.plans, s.from_sides,
          s.variable_from_sides, s.target_recipes, s.target_positions,
          s.compatible_positions, s.required_queries,
          s.required_query_answers, s.unique_partners,
          s.duplicate_partners, s.ordinary_hash_hits,
          s.covered_hash_hits, s.missed_hash_hits, s.planning_seconds,
          s.requirements, s.requirement_peak, s.requirement_bytes,
          s.requirement_peak_bytes, s.requirement_budget_bytes,
          s.requirement_queries, s.requirement_answers,
          s.workspace_bytes, s.workspace_peak_bytes);
}
