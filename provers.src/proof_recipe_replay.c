#include "proof_recipe_replay.h"

#include "../ladr/clause_misc.h"
#include "../ladr/clausify.h"
#include "../ladr/demod.h"
#include "../ladr/just.h"
#include "../ladr/paramod.h"
#include "../ladr/parautil.h"
#include "../ladr/resolve.h"
#include "../ladr/unify.h"

#include <limits.h>
#include <string.h>

static void set_diagnostic(struct proof_recipe_replay_diagnostic *diagnostic,
                           unsigned node,
                           enum proof_recipe_replay_stage stage,
                           unsigned secondary, const char *message)
{
  if (diagnostic != NULL) {
    diagnostic->node = node;
    diagnostic->stage = stage;
    diagnostic->secondary = secondary;
    diagnostic->message = message;
  }
}

static Ilist position_to_ilist(const struct proof_recipe_position *position)
{
  Ilist result = NULL;
  unsigned i;
  for (i = 0; i < position->count; i++)
    result = ilist_append(result, position->items[i]);
  return result;
}

/* Safe strict counterpart of para_pos().  The public helper intentionally
   calls fatal_error() for malformed coordinates; an auxiliary recipe must
   instead fail closed with a node-level diagnostic. */
static Topform replay_paramodulation(
  Topform from, const struct proof_recipe_position *from_position,
  Topform into, const struct proof_recipe_position *into_position)
{
  Context from_subst = NULL, into_subst = NULL;
  Trail trail = NULL;
  Literals from_lit, into_lit;
  Ilist from_pos = NULL, into_pos = NULL;
  Term alpha, into_term;
  int from_side;
  Topform result = NULL;

  if (from == NULL || into == NULL || from_position->count < 2 ||
      into_position->count < 2)
    return NULL;
  from_lit = ith_literal(from->literals, from_position->items[0]);
  into_lit = ith_literal(into->literals, into_position->items[0]);
  from_side = from_position->items[1];
  if (from_lit == NULL || into_lit == NULL || !pos_eq(from_lit) ||
      (from_side != 1 && from_side != 2))
    return NULL;

  from_pos = position_to_ilist(from_position);
  into_pos = position_to_ilist(into_position);
  alpha = ARG(from_lit->atom, from_side - 1);
  into_term = term_at_pos(into_lit->atom, into_pos->next);
  if (into_term == NULL)
    goto done;
  from_subst = get_context();
  into_subst = get_context();
  if (!unify(alpha, from_subst, into_term, into_subst, &trail))
    goto done;
  result = paramodulate(from_lit, from_side - 1, from_subst,
                        into, into_pos, into_subst);
  result->justification = para_just(
    PARA_JUST, from, copy_ilist(from_pos), into, copy_ilist(into_pos));
  renumber_variables(result, MAX_VARS);

done:
  if (trail != NULL)
    undo_subst(trail);
  if (from_subst != NULL)
    free_context(from_subst);
  if (into_subst != NULL)
    free_context(into_subst);
  zap_ilist(from_pos);
  zap_ilist(into_pos);
  return result;
}

static Topform replay_resolution(const struct proof_recipe_decoded *recipe,
                                 Topform const *verified)
{
  Topform current = verified[recipe->nucleus_node - 1];
  BOOL current_owned = FALSE;
  unsigned i;
  for (i = 0; i < recipe->clash_count; i++) {
    const struct proof_recipe_clash *clash = &recipe->clashes[i];
    Topform satellite = verified[clash->satellite_node - 1];
    Topform flipped = NULL;
    Topform next;
    int nucleus_literal = clash->nucleus_literal - (int) i;
    int satellite_literal = clash->satellite_literal;
    if (current == NULL || satellite == NULL || nucleus_literal <= 0)
      goto failed;
    if (satellite_literal < 0) {
      Literals literal;
      satellite_literal = -satellite_literal;
      literal = ith_literal(satellite->literals, satellite_literal);
      if (literal == NULL || !eq_term(literal->atom))
        goto failed;
      flipped = copy_inference(satellite);
      literal = ith_literal(flipped->literals, satellite_literal);
      flip_eq(literal->atom, satellite_literal);
      satellite = flipped;
    }
    next = resolve2(current, nucleus_literal,
                    satellite, satellite_literal, TRUE);
    if (flipped != NULL)
      delete_clause(flipped);
    if (next == NULL)
      goto failed;
    if (current_owned)
      delete_clause(current);
    current = next;
    current_owned = TRUE;
  }
  return current_owned ? current : NULL;

failed:
  if (current_owned)
    delete_clause(current);
  return NULL;
}

static Term replay_nth_nonvariable_term(Term term, unsigned target,
                                        unsigned *sequence)
{
  unsigned i;
  Term found;
  if (VARIABLE(term))
    return NULL;
  for (i = 0; i < (unsigned) ARITY(term); i++) {
    found = replay_nth_nonvariable_term(ARG(term, i), target, sequence);
    if (found != NULL)
      return found;
  }
  (*sequence)++;
  return *sequence == target ? term : NULL;
}

static BOOL replay_rewrite_is_applicable(Topform clause, Topform demodulator,
                                         unsigned target, unsigned direction)
{
  Literals literal;
  unsigned sequence = 0;
  Term selected = NULL, pattern;
  Context context;
  Trail trail = NULL;
  BOOL applicable;
  if (clause == NULL || demodulator == NULL || target == 0 ||
      (direction != 1 && direction != 2) ||
      demodulator->literals == NULL ||
      !pos_eq(demodulator->literals))
    return FALSE;
  for (literal = clause->literals; literal != NULL && selected == NULL;
       literal = literal->next)
    selected = replay_nth_nonvariable_term(
      literal->atom, target, &sequence);
  if (selected == NULL)
    return FALSE;
  pattern = ARG(demodulator->literals->atom, direction == 1 ? 0 : 1);
  context = get_context();
  applicable = match(pattern, context, selected, &trail);
  if (trail != NULL)
    undo_subst(trail);
  free_context(context);
  return applicable;
}

static Just build_primary_justification(
  const struct proof_recipe_decoded *recipe, Topform const *verified)
{
  switch (recipe->rule) {
  case PROOF_RECIPE_COPY:
    return copy_just(verified[recipe->unary_parent - 1]);
  case PROOF_RECIPE_BACK_REWRITE:
    return back_demod_just(verified[recipe->unary_parent - 1]);
  case PROOF_RECIPE_PARAMOD:
    return para_just(
      PARA_JUST,
      verified[recipe->paramod[0].node - 1],
      position_to_ilist(&recipe->paramod[0].position),
      verified[recipe->paramod[1].node - 1],
      position_to_ilist(&recipe->paramod[1].position));
  case PROOF_RECIPE_HYPER:
  case PROOF_RECIPE_RESOLVE:
    {
      Ilist items = NULL;
      unsigned i;
      items = ilist_append(
        items, (int) verified[recipe->nucleus_node - 1]->id);
      for (i = 0; i < recipe->clash_count; i++) {
        const struct proof_recipe_clash *clash = &recipe->clashes[i];
        items = ilist_append(items, clash->nucleus_literal);
        items = ilist_append(
          items, (int) verified[clash->satellite_node - 1]->id);
        items = ilist_append(items, clash->satellite_literal);
      }
      return resolve_just(
        items, recipe->rule == PROOF_RECIPE_HYPER ?
                 HYPER_RES_JUST : BINARY_RES_JUST);
    }
  default:
    return NULL;
  }
}

static Just build_secondary_justification(
  const struct proof_recipe_decoded *recipe, Topform const *verified)
{
  Just result = NULL;
  I3list rewrites = NULL;
  unsigned i;
  for (i = 0; i < recipe->secondary_count; i++) {
    const struct proof_recipe_secondary *step = &recipe->secondary[i];
    if (step->rule == PROOF_RECIPE_REWRITE)
      rewrites = i3list_append(
        rewrites, (int) verified[step->parent_node - 1]->id,
        (int) step->target, (int) step->direction);
    else {
      if (rewrites != NULL) {
        result = append_just(result, demod_just(rewrites));
        rewrites = NULL;
      }
      result = append_just(result, flip_just(step->literal));
    }
  }
  if (rewrites != NULL)
    result = append_just(result, demod_just(rewrites));
  return result;
}

static BOOL parents_available(const struct proof_recipe_decoded *recipe,
                              Topform const *verified)
{
  unsigned i;
  if (recipe->rule == PROOF_RECIPE_DENY ||
      recipe->rule == PROOF_RECIPE_COPY ||
      recipe->rule == PROOF_RECIPE_BACK_REWRITE)
    if (verified[recipe->unary_parent - 1] == NULL)
      return FALSE;
  if (recipe->rule == PROOF_RECIPE_PARAMOD)
    for (i = 0; i < 2; i++)
      if (verified[recipe->paramod[i].node - 1] == NULL)
        return FALSE;
  if (recipe->rule == PROOF_RECIPE_HYPER ||
      recipe->rule == PROOF_RECIPE_RESOLVE) {
    if (verified[recipe->nucleus_node - 1] == NULL)
      return FALSE;
    for (i = 0; i < recipe->clash_count; i++)
      if (verified[recipe->clashes[i].satellite_node - 1] == NULL)
        return FALSE;
  }
  for (i = 0; i < recipe->secondary_count; i++)
    if (recipe->secondary[i].rule == PROOF_RECIPE_REWRITE &&
        verified[recipe->secondary[i].parent_node - 1] == NULL)
      return FALSE;
  return TRUE;
}

enum proof_recipe_replay_result proof_recipe_replay_compute(
  Proof_parent_guide guide, unsigned node, Topform const *verified,
  Topform *result, struct proof_recipe_replay_diagnostic *diagnostic)
{
  struct proof_recipe_decoded recipe;
  const char *decode_error = NULL;
  Topform current = NULL;
  unsigned i;
  Just primary, secondary;
  if (result != NULL)
    *result = NULL;
  set_diagnostic(diagnostic, node, PROOF_REPLAY_STAGE_NONE, 0, NULL);
  if (result == NULL || verified == NULL ||
      !proof_parent_guide_decode_recipe(
        guide, node, &recipe, &decode_error)) {
    set_diagnostic(diagnostic, node, PROOF_REPLAY_STAGE_DECODE, 0,
                   decode_error == NULL ? "cannot decode recipe" :
                                          decode_error);
    return PROOF_REPLAY_FAILED;
  }
  if (recipe.rule == PROOF_RECIPE_ASSUMPTION) {
    proof_recipe_decoded_destroy(&recipe);
    return PROOF_REPLAY_NEEDS_ASSUMPTION;
  }
  if (recipe.rule == PROOF_RECIPE_GOAL) {
    proof_recipe_decoded_destroy(&recipe);
    return PROOF_REPLAY_NEEDS_GOAL;
  }
  if (recipe.rule == PROOF_RECIPE_DENY) {
    if (!parents_available(&recipe, verified)) {
      set_diagnostic(diagnostic, node, PROOF_REPLAY_STAGE_PARENT, 0,
                     "denial goal parent is not anchored");
      proof_recipe_decoded_destroy(&recipe);
      return PROOF_REPLAY_FAILED;
    }
    proof_recipe_decoded_destroy(&recipe);
    return PROOF_REPLAY_NEEDS_DENIAL;
  }
  if (!parents_available(&recipe, verified)) {
    set_diagnostic(diagnostic, node, PROOF_REPLAY_STAGE_PARENT, 0,
                   "a verified recipe parent is missing");
    proof_recipe_decoded_destroy(&recipe);
    return PROOF_REPLAY_FAILED;
  }

  if (recipe.rule == PROOF_RECIPE_COPY ||
      recipe.rule == PROOF_RECIPE_BACK_REWRITE)
    current = copy_inference(verified[recipe.unary_parent - 1]);
  else if (recipe.rule == PROOF_RECIPE_PARAMOD)
    current = replay_paramodulation(
      verified[recipe.paramod[0].node - 1], &recipe.paramod[0].position,
      verified[recipe.paramod[1].node - 1], &recipe.paramod[1].position);
  else if (recipe.rule == PROOF_RECIPE_HYPER ||
           recipe.rule == PROOF_RECIPE_RESOLVE)
    current = replay_resolution(&recipe, verified);
  if (current == NULL) {
    set_diagnostic(diagnostic, node, PROOF_REPLAY_STAGE_PRIMARY, 0,
                   "recorded primary inference does not replay");
    proof_recipe_decoded_destroy(&recipe);
    return PROOF_REPLAY_FAILED;
  }

  for (i = 0; i < recipe.secondary_count; i++) {
    const struct proof_recipe_secondary *step = &recipe.secondary[i];
    Topform work = copy_clause(current);
    if (step->rule == PROOF_RECIPE_REWRITE) {
      Ilist from_position = NULL, into_position = NULL;
      BOOL rewritten = replay_rewrite_is_applicable(
        work, verified[step->parent_node - 1], step->target,
        step->direction) && particular_demod(
          work, verified[step->parent_node - 1], (int) step->target,
          (int) step->direction, &from_position, &into_position);
      zap_ilist(from_position);
      zap_ilist(into_position);
      if (!rewritten) {
        delete_clause(work);
        delete_clause(current);
        set_diagnostic(diagnostic, node, PROOF_REPLAY_STAGE_REWRITE, i + 1,
                       "recorded rewrite does not apply");
        proof_recipe_decoded_destroy(&recipe);
        return PROOF_REPLAY_FAILED;
      }
    }
    else {
      Literals literal = ith_literal(work->literals, step->literal);
      if (literal == NULL || !eq_term(literal->atom)) {
        delete_clause(work);
        delete_clause(current);
        set_diagnostic(diagnostic, node, PROOF_REPLAY_STAGE_FLIP, i + 1,
                       "recorded equality flip does not apply");
        proof_recipe_decoded_destroy(&recipe);
        return PROOF_REPLAY_FAILED;
      }
      flip_eq(literal->atom, step->literal);
    }
    delete_clause(current);
    current = work;
    renumber_variables(current, MAX_VARS);
  }

  renumber_variables(current, MAX_VARS);
  if (!clause_ident(
        proof_parent_guide_node_body(guide, node)->literals,
        current->literals)) {
    set_diagnostic(diagnostic, node, PROOF_REPLAY_STAGE_BODY, 0,
                   "computed clause differs from recorded body");
    delete_clause(current);
    proof_recipe_decoded_destroy(&recipe);
    return PROOF_REPLAY_FAILED;
  }

  zap_just(current->justification);
  primary = build_primary_justification(&recipe, verified);
  secondary = build_secondary_justification(&recipe, verified);
  current->justification = append_just(primary, secondary);
  *result = current;
  proof_recipe_decoded_destroy(&recipe);
  return PROOF_REPLAY_VERIFIED;
}

BOOL proof_recipe_replay_anchor_matches(Proof_parent_guide guide,
                                        unsigned node, Topform actual)
{
  Topform expected = proof_parent_guide_node_body(guide, node);
  Topform copy;
  BOOL matches;
  if (expected == NULL || actual == NULL || actual->literals == NULL)
    return FALSE;
  copy = copy_clause(actual);
  renumber_variables(copy, MAX_VARS);
  matches = clause_ident(expected->literals, copy->literals);
  delete_clause(copy);
  return matches;
}

BOOL proof_recipe_replay_goal_anchor_matches(Proof_parent_guide guide,
                                             unsigned node, Topform goal)
{
  Topform clause;
  BOOL matches;
  if (goal == NULL || !goal->is_formula || goal->formula == NULL ||
      !clausal_formula(goal->formula))
    return FALSE;
  clause = get_topform();
  clause->literals = formula_to_literals(goal->formula);
  upward_clause_links(clause);
  clause_set_variables(clause, MAX_VARS);
  matches = proof_recipe_replay_anchor_matches(guide, node, clause);
  delete_clause(clause);
  return matches;
}
