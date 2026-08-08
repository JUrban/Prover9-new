/*  Copyright (C) 2006, 2007 William McCune

    This file is part of the LADR Deduction Library.

    The LADR Deduction Library is free software; you can redistribute it
    and/or modify it under the terms of the GNU General Public License,
    version 2.

    The LADR Deduction Library is distributed in the hope that it will be
    useful, but WITHOUT ANY WARRANTY; without even the implied warranty of
    MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
    GNU General Public License for more details.

    You should have received a copy of the GNU General Public License
    along with the LADR Deduction Library; if not, write to the Free Software
    Foundation, Inc., 51 Franklin St, Fifth Floor, Boston, MA 02110-1301 USA.
*/

#include "resolve.h"
#include "dollar.h"

/* Private definitions and types */

static BOOL  Ordered            = FALSE;
static BOOL  Check_instances    = FALSE;

static int   Ur_nucleus_limit   = INT_MAX;  /* limit num of clashable lits */
static BOOL  Initial_nuclei     = FALSE;    /* nuclei must be input clauses  */
static BOOL  Production_mode    = FALSE;

static unsigned long long Res_instance_prunes = 0;  /* counter */


/*************
 *
 *   resolution_options()
 *
 *************/

/* DOCUMENTATION
*/

/* PUBLIC */

void resolution_options(BOOL ordered,
			BOOL check_instances,
			BOOL initial_nuclei,
			int ur_nucleus_limit,
			BOOL production_mode)
{
  Ordered = ordered;
  Check_instances = check_instances;
  Initial_nuclei = initial_nuclei;
  Ur_nucleus_limit = (ur_nucleus_limit == -1 ? INT_MAX : ur_nucleus_limit);
  Production_mode = production_mode;
  Res_instance_prunes = 0;

}  /* resolution_options */

/*************
 *
 *   res_instance_prunes()
 *
 *************/

/* DOCUMENTATION
*/

/* PUBLIC */
unsigned long long res_instance_prunes()
{
  return Res_instance_prunes;
}  /* res_instance_prunes */

/*************
 *
 *   unit_check()
 *
 *************/

static
BOOL unit_check(Literals lit)
{
  Topform c = lit->atom->container;
  return unit_clause(c->literals);
}  /* unit_check */

/*************
 *
 *   pos_hyper_sat_test()
 *
 *************/

static
BOOL pos_hyper_sat_test(Literals lit)
{
  Topform c = lit->atom->container;
  if (positive_clause(c->literals))
    return Ordered ? maximal_literal(c->literals, lit, FLAG_CHECK) : TRUE;
  else
    return FALSE;
}  /* pos_hyper_sat_test */

/*************
 *
 *   neg_hyper_sat_test()
 *
 *************/

static
BOOL neg_hyper_sat_test(Literals lit)
{
  Topform c = lit->atom->container;
  if (negative_clause(c->literals))
    return Ordered ? maximal_literal(c->literals, lit, FLAG_CHECK) : TRUE;
  else
    return FALSE;
}  /* neg_hyper_sat_test */

/*************
 *
 *   hyper_sat_atom()
 *
 *************/

static
void hyper_sat_atom(BOOL flipped, Literals slit, Term atom, int pos_or_neg,
		    Lindex idx, Clash_clause_test clause_test,
		    void *clause_test_data,
		    void (*proc_proc) (Topform))
{
  BOOL positive = (pos_or_neg == POS_RES);
  Context sat_subst = get_context();
  Context nuc_subst = get_context();
  Term fnd_atom;
  Mindex_pos mate_pos;
  fnd_atom = mindex_retrieve_first(atom,
				   positive ? idx->neg : idx->pos,
				   UNIFY,
				   sat_subst, nuc_subst,
				   FALSE, &mate_pos);
  while (fnd_atom) {  /* loop through found nuclei atoms */
    /* it must be in a nucleus because of the index used above */
    Topform nuc = fnd_atom->container;
    Literals nuc_lit = atom_to_literal(fnd_atom);
    Clash p = NULL;
    Clash first = NULL;
    Literals nlit;
    if (clause_test != NULL &&
        !(*clause_test)(nuc, clause_test_data)) {
      fnd_atom = mindex_retrieve_next(mate_pos);
      continue;
    }
    for (nlit = nuc->literals; nlit; nlit = nlit->next) {
      p = append_clash(p);
      if (first == NULL)
	first = p;
      p->nuc_lit = nlit;
      p->nuc_subst = nuc_subst;
      if (nlit == nuc_lit) {
	p->sat_lit = slit;
	p->sat_subst = sat_subst;
	p->clashable = TRUE;
	p->clashed = TRUE;
	p->flipped = flipped;
      }
      else {
	if (!Production_mode)
	  p->clashable = positive ? !nlit->sign : nlit->sign;
	else
	  p->clashable = (positive &&
			  !nlit->sign &&
			  !evaluable_predicate(SYMNUM(nlit->atom)));
	if (p->clashable) {
	  p->mate_index = positive ? idx->pos : idx->neg;
	  p->sat_subst = get_context();
	}
      }
    }  /* for each literal of nucleus */
    clash_with_clause_test(
      first,
      positive ? pos_hyper_sat_test : neg_hyper_sat_test,
      clause_test, clause_test_data, HYPER_RES_JUST, proc_proc);
    zap_clash(first);
    fnd_atom = mindex_retrieve_next(mate_pos);
  }  /* for each found nucleus atom */
  free_context(sat_subst);
  free_context(nuc_subst);
}  /* hyper_sat_atom */

/*************
 *
 *   hyper_satellite()
 *
 *************/

static
void hyper_satellite(Topform c, int pos_or_neg, Lindex idx,
		     Clash_clause_test clause_test, void *clause_test_data,
		     void (*proc_proc) (Topform))
{
  Literals slit;
  for (slit = c->literals; slit; slit = slit->next) {
    if (!Ordered || maximal_literal(c->literals, slit, FLAG_CHECK)) {
      hyper_sat_atom(FALSE, slit, slit->atom, pos_or_neg, idx,
                     clause_test, clause_test_data, proc_proc);
      if (pos_eq(slit)) {
	Term flip = top_flip(slit->atom);
	hyper_sat_atom(TRUE, slit, flip, pos_or_neg, idx,
                       clause_test, clause_test_data, proc_proc);
	zap_top_flip(flip);
      }
    }  /* if sat is ok */
  }  /* for each literal of satellite */
}  /* hyper_satellite */

/*************
 *
 *   hyper_nucleus()
 *
 *************/

static
void hyper_nucleus(Topform c, int pos_or_neg, Lindex idx,
		   Clash_clause_test clause_test, void *clause_test_data,
		   void (*proc_proc) (Topform))
{
  BOOL positive = (pos_or_neg == POS_RES);
  Clash p = NULL;
  Clash first = NULL;
  Literals lit;
  Context nuc_subst = get_context();
  for (lit = c->literals; lit; lit = lit->next) {
    p = append_clash(p);
    if (first == NULL)
      first = p;
    if (!Production_mode)
      p->clashable = positive ? !lit->sign : lit->sign;
    else
      p->clashable = (positive &&
		      !lit->sign &&
		      !evaluable_predicate(SYMNUM(lit->atom)));
    p->nuc_lit = lit;
    p->nuc_subst = nuc_subst;
    if (p->clashable) {
      p->mate_index = positive ? idx->pos : idx->neg;
      p->sat_subst = get_context();
    }
  }
  clash_with_clause_test(
    first,
    positive ? pos_hyper_sat_test : neg_hyper_sat_test,
    clause_test, clause_test_data, HYPER_RES_JUST, proc_proc);
  free_context(nuc_subst);
  zap_clash(first);  /* This also frees satellite contexts. */
}  /* hyper_nucleus */

/*************
 *
 *   hyper_resolution()
 *
 *************/

/* DOCUMENTATION
Hyperresolution.
*/

/* PUBLIC */
void hyper_resolution(Topform c, int pos_or_neg, Lindex idx,
		      void (*proc_proc) (Topform))
{
  hyper_resolution_with_clause_test(c, pos_or_neg, idx, NULL, NULL,
                                    proc_proc);
}  /* hyper_resolution */

/* PUBLIC */
void hyper_resolution_with_clause_test(Topform c, int pos_or_neg, Lindex idx,
				       Clash_clause_test clause_test,
				       void *clause_test_data,
				       void (*proc_proc) (Topform))
{
  if (pos_or_neg == POS_RES ?
      positive_clause(c->literals) :
      negative_clause(c->literals))
    hyper_satellite(c, pos_or_neg, idx, clause_test, clause_test_data,
                    proc_proc);
  else
    hyper_nucleus(c, pos_or_neg, idx, clause_test, clause_test_data,
                  proc_proc);
}  /* hyper_resolution_with_clause_test */

enum hyper_scan_result {
  HYPER_SCAN_FOUND,
  HYPER_SCAN_BUDGET,
  HYPER_SCAN_EXHAUSTED
};

static Literals hyper_iterator_literal(Literals lits, unsigned position)
{
  while (lits != NULL && position != 0) {
    lits = lits->next;
    position--;
  }
  return lits;
}

static unsigned hyper_iterator_literal_count(Literals lits)
{
  unsigned n = 0;
  while (lits != NULL) {
    n++;
    lits = lits->next;
  }
  return n;
}

static BOOL hyper_iterator_clashable(Literals lit, BOOL positive)
{
  if (!Production_mode)
    return positive ? !lit->sign : lit->sign;
  else
    return positive && !lit->sign &&
           !evaluable_predicate(SYMNUM(lit->atom));
}

static BOOL hyper_iterator_sat_test(Literals lit, BOOL positive)
{
  return positive ? pos_hyper_sat_test(lit) : neg_hyper_sat_test(lit);
}

static void hyper_iterator_reserve_choices(Hyper_iterator *it, unsigned need)
{
  unsigned capacity;
  Hyper_iterator_choice *choices;
  if (need <= it->choice_capacity)
    return;
  capacity = it->choice_capacity == 0 ? 4 : it->choice_capacity;
  while (capacity < need) {
    if (capacity > UINT_MAX / 2)
      fatal_error("hyperresolution iterator choice overflow");
    capacity *= 2;
  }
  choices = safe_calloc(capacity, sizeof(*choices));
  if (it->choices != NULL) {
    memcpy(choices, it->choices, it->depth * sizeof(*choices));
    safe_free(it->choices);
  }
  it->choices = choices;
  it->choice_capacity = capacity;
}

static void hyper_iterator_reset_scan(
  unsigned long long count, unsigned long long *parent, unsigned *literal)
{
  *parent = count;
  *literal = UINT_MAX;
}

/* Search stable parents in reverse activation/literal insertion order.  FPA
   answers are descending insertion IDs, so this is the pointer-free order
   corresponding to the collective append-only history.  Rejected historical
   parents and inspected literals are both charged, ensuring a hard turn
   bound even across a long inactive prefix. */
static enum hyper_scan_result hyper_iterator_scan(
  Term query, Context query_subst, Context found_subst,
  BOOL wanted_sign, BOOL require_satellite,
  const Hyper_parent_source *source,
  unsigned long long *parent_cursor, unsigned *literal_cursor,
  unsigned long long raw_budget, unsigned long long *raw_steps,
  unsigned long long *found_parent, unsigned *found_literal, Trail *found_trail)
{
  while (*parent_cursor != 0) {
    unsigned long long parent_position = *parent_cursor - 1;
    Topform c = source->clause(parent_position, source->data);
    if (*literal_cursor == UINT_MAX) {
      if (*raw_steps >= raw_budget)
        return HYPER_SCAN_BUDGET;
      if (source->test != NULL && !source->test(c, source->data)) {
        (*raw_steps)++;
        (*parent_cursor)--;
        continue;
      }
      *literal_cursor = hyper_iterator_literal_count(c->literals);
    }
    if (*literal_cursor == 0) {
      (*parent_cursor)--;
      *literal_cursor = UINT_MAX;
      continue;
    }
    else {
      unsigned literal_position;
      Literals lit;
      Trail tr = NULL;
      if (*raw_steps >= raw_budget)
        return HYPER_SCAN_BUDGET;
      literal_position = --(*literal_cursor);
      lit = hyper_iterator_literal(c->literals, literal_position);
      (*raw_steps)++;
      if (lit->sign != wanted_sign ||
          (require_satellite &&
           !hyper_iterator_sat_test(lit, wanted_sign)))
        continue;
      if (unify(query, query_subst, lit->atom, found_subst, &tr)) {
        *found_parent = parent_position;
        *found_literal = literal_position;
        *found_trail = tr;
        return HYPER_SCAN_FOUND;
      }
    }
  }
  return HYPER_SCAN_EXHAUSTED;
}

static Clash hyper_iterator_build_clash(
  Topform nucleus, BOOL positive, int preselected_literal,
  Clash **frames_out, unsigned *frame_count_out, Context *nuc_subst_out)
{
  unsigned literal_position = 0, frame_count = 0;
  unsigned literal_count = hyper_iterator_literal_count(nucleus->literals);
  Clash *frames = safe_calloc(literal_count == 0 ? 1 : literal_count,
                              sizeof(*frames));
  Context nuc_subst = get_context();
  Clash first = NULL, last = NULL;
  Literals lit;
  for (lit = nucleus->literals; lit != NULL;
       lit = lit->next, literal_position++) {
    Clash p = append_clash(last);
    if (first == NULL)
      first = p;
    last = p;
    p->clashable = hyper_iterator_clashable(lit, positive);
    p->nuc_lit = lit;
    p->nuc_subst = nuc_subst;
    if (p->clashable) {
      p->sat_subst = get_context();
      if ((int) literal_position != preselected_literal)
        frames[frame_count++] = p;
    }
  }
  *frames_out = frames;
  *frame_count_out = frame_count;
  *nuc_subst_out = nuc_subst;
  return first;
}

static void hyper_iterator_cleanup_clash(
  Clash first, Context nuc_subst, Clash *frames,
  Trail *trails, Term *flips, unsigned depth,
  Trail preselected_trail, Term preselected_flip)
{
  unsigned i;
  Clash p;
  for (i = depth; i != 0; i--) {
    if (trails[i - 1] != NULL)
      undo_subst(trails[i - 1]);
    if (flips[i - 1] != NULL)
      zap_top_flip(flips[i - 1]);
  }
  if (preselected_trail != NULL)
    undo_subst(preselected_trail);
  if (preselected_flip != NULL)
    zap_top_flip(preselected_flip);
  for (p = first; p != NULL; p = p->next)
    p->clashed = FALSE;
  zap_clash(first);
  free_context(nuc_subst);
  safe_free(frames);
  safe_free(trails);
  safe_free(flips);
}

static void hyper_iterator_backtrack(
  Hyper_iterator *it, Clash *frames, Trail *trails, Term *flips)
{
  unsigned at;
  Hyper_iterator_choice *choice;
  if (it->depth == 0)
    fatal_error("hyperresolution iterator backtrack underflow");
  at = --it->depth;
  choice = &it->choices[at];
  if (trails[at] != NULL) {
    undo_subst(trails[at]);
    trails[at] = NULL;
  }
  if (flips[at] != NULL) {
    zap_top_flip(flips[at]);
    flips[at] = NULL;
  }
  frames[at]->clashed = FALSE;
  frames[at]->sat_lit = NULL;
  it->mate_phase = choice->resume_phase;
  it->mate_parent = choice->resume_parent;
  it->mate_literal = choice->resume_literal;
}

static void hyper_iterator_reconstruct_choices(
  Hyper_iterator *it, Clash *frames, unsigned frame_count,
  const Hyper_parent_source *source, Trail *trails, Term *flips)
{
  unsigned i;
  if (it->depth > frame_count)
    fatal_error("invalid hyperresolution iterator depth");
  for (i = 0; i < it->depth; i++) {
    Hyper_iterator_choice *choice = &it->choices[i];
    Clash p = frames[i];
    Trail tr = NULL;
    if (choice->phase <= 1) {
      Topform sat = source->clause(choice->parent_position, source->data);
      Literals lit = hyper_iterator_literal(sat->literals, choice->literal);
      Term query = p->nuc_lit->atom;
      if (lit == NULL)
        fatal_error("invalid hyperresolution iterator satellite literal");
      if (choice->phase == 1) {
        query = top_flip(query);
        flips[i] = query;
      }
      if (!unify(query, p->nuc_subst, lit->atom, p->sat_subst, &tr))
        fatal_error("hyperresolution iterator choice no longer unifies");
      p->sat_lit = lit;
      p->flipped = choice->phase == 1;
    }
    else if (choice->phase == 2) {
      if (!neg_eq(p->nuc_lit) ||
          !unify(ARG(p->nuc_lit->atom, 0), p->nuc_subst,
                 ARG(p->nuc_lit->atom, 1), p->nuc_subst, &tr))
        fatal_error("hyperresolution iterator x=x choice changed");
      p->sat_lit = NULL;
      p->flipped = FALSE;
    }
    else
      fatal_error("invalid hyperresolution iterator choice phase");
    trails[i] = tr;
    p->clashed = TRUE;
  }
}

enum hyper_inner_result {
  HYPER_INNER_COMPLETE,
  HYPER_INNER_BUDGET
};

static enum hyper_inner_result hyper_iterator_run_inner(
  Topform nucleus, BOOL positive, int preselected_literal,
  Literals given_satellite, BOOL given_flipped,
  const Hyper_parent_source *source, Hyper_iterator *it,
  unsigned long long raw_budget, unsigned long long yield_budget,
  void (*proc_proc) (Topform), unsigned long long *raw_steps,
  unsigned long long *yielded)
{
  Clash *frames, first;
  unsigned frame_count, i;
  Context nuc_subst;
  Trail preselected_trail = NULL;
  Term preselected_flip = NULL;
  Trail *trails;
  Term *flips;

  first = hyper_iterator_build_clash(
    nucleus, positive, preselected_literal,
    &frames, &frame_count, &nuc_subst);
  trails = safe_calloc(frame_count == 0 ? 1 : frame_count,
                        sizeof(*trails));
  flips = safe_calloc(frame_count == 0 ? 1 : frame_count,
                       sizeof(*flips));

  if (preselected_literal >= 0) {
    Clash p;
    unsigned position = 0;
    Term query = given_satellite->atom;
    for (p = first; p != NULL && position < (unsigned) preselected_literal;
         p = p->next, position++)
      ;
    if (p == NULL || !p->clashable)
      fatal_error("invalid hyperresolution iterator nucleus literal");
    if (given_flipped) {
      query = top_flip(query);
      preselected_flip = query;
    }
    if (!unify(query, p->sat_subst, p->nuc_lit->atom,
               p->nuc_subst, &preselected_trail))
      fatal_error("hyperresolution iterator nucleus no longer unifies");
    p->sat_lit = given_satellite;
    p->flipped = given_flipped;
    p->clashed = TRUE;
  }

  hyper_iterator_reconstruct_choices(
    it, frames, frame_count, source, trails, flips);

  while (*yielded < yield_budget) {
    Clash p;
    if (it->depth == frame_count) {
      if (frame_count == 0 && it->mate_phase >= 3)
        break;
      (*proc_proc)(clash_resolve(first, HYPER_RES_JUST));
      (*yielded)++;
      if (frame_count == 0)
        it->mate_phase = 3;
      else
        hyper_iterator_backtrack(it, frames, trails, flips);
      continue;
    }

    p = frames[it->depth];
    if (it->mate_phase <= 1) {
      Term query = p->nuc_lit->atom;
      Term flip = NULL;
      unsigned long long found_parent;
      unsigned found_literal;
      Trail tr = NULL;
      enum hyper_scan_result scan;
      if (it->mate_phase == 1) {
        if (!eq_term(query)) {
          it->mate_phase = 2;
          hyper_iterator_reset_scan(
            source->count(source->data),
            &it->mate_parent, &it->mate_literal);
          continue;
        }
        flip = top_flip(query);
        query = flip;
      }
      scan = hyper_iterator_scan(
        query, p->nuc_subst, p->sat_subst, positive, TRUE, source,
        &it->mate_parent, &it->mate_literal,
        raw_budget, raw_steps, &found_parent, &found_literal, &tr);
      if (scan == HYPER_SCAN_BUDGET) {
        if (flip != NULL)
          zap_top_flip(flip);
        hyper_iterator_cleanup_clash(
          first, nuc_subst, frames, trails, flips, it->depth,
          preselected_trail, preselected_flip);
        return HYPER_INNER_BUDGET;
      }
      if (scan == HYPER_SCAN_EXHAUSTED) {
        if (flip != NULL)
          zap_top_flip(flip);
        it->mate_phase++;
        hyper_iterator_reset_scan(
          source->count(source->data),
          &it->mate_parent, &it->mate_literal);
        continue;
      }
      hyper_iterator_reserve_choices(it, it->depth + 1);
      it->choices[it->depth].parent_position = found_parent;
      it->choices[it->depth].literal = found_literal;
      it->choices[it->depth].phase = it->mate_phase;
      it->choices[it->depth].resume_phase = it->mate_phase;
      it->choices[it->depth].resume_parent = it->mate_parent;
      it->choices[it->depth].resume_literal = it->mate_literal;
      p->sat_lit = hyper_iterator_literal(
        source->clause(found_parent, source->data)->literals,
        found_literal);
      p->flipped = it->mate_phase == 1;
      p->clashed = TRUE;
      trails[it->depth] = tr;
      flips[it->depth] = flip;
      it->depth++;
      it->mate_phase = 0;
      hyper_iterator_reset_scan(
        source->count(source->data),
        &it->mate_parent, &it->mate_literal);
      continue;
    }
    else if (it->mate_phase == 2) {
      Trail tr = NULL;
      if (*raw_steps >= raw_budget) {
        hyper_iterator_cleanup_clash(
          first, nuc_subst, frames, trails, flips, it->depth,
          preselected_trail, preselected_flip);
        return HYPER_INNER_BUDGET;
      }
      (*raw_steps)++;
      it->mate_phase = 3;
      if (neg_eq(p->nuc_lit) &&
          unify(ARG(p->nuc_lit->atom, 0), p->nuc_subst,
                ARG(p->nuc_lit->atom, 1), p->nuc_subst, &tr)) {
        hyper_iterator_reserve_choices(it, it->depth + 1);
        it->choices[it->depth].parent_position = 0;
        it->choices[it->depth].literal = 0;
        it->choices[it->depth].phase = 2;
        it->choices[it->depth].resume_phase = 3;
        it->choices[it->depth].resume_parent = it->mate_parent;
        it->choices[it->depth].resume_literal = it->mate_literal;
        p->sat_lit = NULL;
        p->flipped = FALSE;
        p->clashed = TRUE;
        trails[it->depth] = tr;
        it->depth++;
        it->mate_phase = 0;
        hyper_iterator_reset_scan(
          source->count(source->data),
          &it->mate_parent, &it->mate_literal);
      }
      continue;
    }
    else if (it->depth == 0)
      break;
    else
      hyper_iterator_backtrack(it, frames, trails, flips);
  }

  i = it->depth;
  hyper_iterator_cleanup_clash(
    first, nuc_subst, frames, trails, flips, i,
    preselected_trail, preselected_flip);
  return (it->depth == 0 && it->mate_phase >= 3) ?
         HYPER_INNER_COMPLETE : HYPER_INNER_BUDGET;
}

static BOOL hyper_iterator_given_literal_ok(
  Topform given, unsigned position)
{
  Literals lit = hyper_iterator_literal(given->literals, position);
  return lit != NULL &&
         (!Ordered || maximal_literal(given->literals, lit, FLAG_CHECK));
}

static void hyper_iterator_advance_given(
  Topform given, Hyper_iterator *it, unsigned long long parent_count)
{
  Literals lit = hyper_iterator_literal(given->literals, it->given_literal);
  if (it->given_phase == 0 && lit != NULL && pos_eq(lit))
    it->given_phase = 1;
  else {
    it->given_literal++;
    it->given_phase = 0;
  }
  hyper_iterator_reset_scan(
    parent_count, &it->outer_parent, &it->outer_literal);
}

/* PUBLIC */
void hyper_iterator_init(Hyper_iterator *it)
{
  memset(it, 0, sizeof(*it));
}

/* PUBLIC */
void hyper_iterator_reset(Hyper_iterator *it)
{
  Hyper_iterator_choice *choices = it->choices;
  unsigned capacity = it->choice_capacity;
  memset(it, 0, sizeof(*it));
  it->choices = choices;
  it->choice_capacity = capacity;
}

/* PUBLIC */
void hyper_iterator_zap(Hyper_iterator *it)
{
  safe_free(it->choices);
  memset(it, 0, sizeof(*it));
}

/* PUBLIC */
BOOL hyper_iterator_at_start(const Hyper_iterator *it)
{
  return !it->initialized && !it->complete && it->depth == 0 &&
         !it->nucleus_selected;
}

/* Pointer-free, bounded hyperresolution over an append-only stable parent
   source.  The source order must be activation insertion order. */
/* PUBLIC */
BOOL hyper_resolution_bounded(
  Topform given, int pos_or_neg, const Hyper_parent_source *source,
  Hyper_iterator *it, unsigned long long raw_budget,
  unsigned long long yield_budget, void (*proc_proc) (Topform),
  unsigned long long *raw_steps, unsigned long long *yielded)
{
  BOOL positive = pos_or_neg == POS_RES;
  unsigned long long parent_count = source->count(source->data);
  if (raw_budget == 0 || yield_budget == 0)
    fatal_error("bounded hyperresolution requires nonzero budgets");
  *raw_steps = 0;
  *yielded = 0;
  if (it->complete)
    return TRUE;
  if (!it->initialized) {
    it->initialized = TRUE;
    it->satellite_mode = positive ?
      positive_clause(given->literals) : negative_clause(given->literals);
    hyper_iterator_reset_scan(
      parent_count, &it->outer_parent, &it->outer_literal);
    hyper_iterator_reset_scan(
      parent_count, &it->mate_parent, &it->mate_literal);
  }

  if (!it->satellite_mode) {
    enum hyper_inner_result result = hyper_iterator_run_inner(
      given, positive, -1, NULL, FALSE, source, it,
      raw_budget, yield_budget, proc_proc, raw_steps, yielded);
    if (result == HYPER_INNER_COMPLETE) {
      it->complete = TRUE;
      return TRUE;
    }
    return FALSE;
  }

  while (*raw_steps < raw_budget && *yielded < yield_budget) {
    Literals given_lit;
    if (it->nucleus_selected) {
      Topform nucleus = source->clause(it->nucleus_parent, source->data);
      enum hyper_inner_result result = hyper_iterator_run_inner(
        nucleus, positive, (int) it->nucleus_literal,
        hyper_iterator_literal(given->literals, it->given_literal),
        it->given_phase == 1, source, it,
        raw_budget, yield_budget, proc_proc, raw_steps, yielded);
      if (result == HYPER_INNER_BUDGET)
        return FALSE;
      it->nucleus_selected = FALSE;
      it->depth = 0;
      it->mate_phase = 0;
      hyper_iterator_reset_scan(
        parent_count, &it->mate_parent, &it->mate_literal);
      continue;
    }

    given_lit = hyper_iterator_literal(given->literals, it->given_literal);
    if (given_lit == NULL) {
      it->complete = TRUE;
      return TRUE;
    }
    if (!hyper_iterator_given_literal_ok(given, it->given_literal) ||
        (it->given_phase == 1 && !pos_eq(given_lit))) {
      hyper_iterator_advance_given(given, it, parent_count);
      continue;
    }
    else {
      Context sat_subst = get_context();
      Context nuc_subst = get_context();
      Term query = given_lit->atom;
      Term flip = NULL;
      unsigned long long found_parent;
      unsigned found_literal;
      Trail tr = NULL;
      enum hyper_scan_result scan;
      if (it->given_phase == 1) {
        flip = top_flip(query);
        query = flip;
      }
      scan = hyper_iterator_scan(
        query, sat_subst, nuc_subst, !positive, FALSE, source,
        &it->outer_parent, &it->outer_literal,
        raw_budget, raw_steps, &found_parent, &found_literal, &tr);
      if (scan == HYPER_SCAN_FOUND) {
        undo_subst(tr);
        it->nucleus_selected = TRUE;
        it->nucleus_parent = found_parent;
        it->nucleus_literal = found_literal;
        it->depth = 0;
        it->mate_phase = 0;
        hyper_iterator_reset_scan(
          parent_count, &it->mate_parent, &it->mate_literal);
      }
      else if (scan == HYPER_SCAN_EXHAUSTED)
        hyper_iterator_advance_given(given, it, parent_count);
      if (flip != NULL)
        zap_top_flip(flip);
      free_context(sat_subst);
      free_context(nuc_subst);
      if (scan == HYPER_SCAN_BUDGET)
        return FALSE;
    }
  }
  return FALSE;
}  /* hyper_resolution_bounded */

/*************
 *
 *   target_check()
 *
 *************/

static
BOOL target_check(Literals lit, int target_constraint)
{
  if (target_constraint == ANY_RES)
    return TRUE;
  else if (target_constraint == POS_RES)
    return lit->sign;
  else if (target_constraint == NEG_RES)
    return !lit->sign;
  else {
    fatal_error("target_check, constraint out of range");
    return FALSE;  /* to please the compiler */
  }
}  /* target_check */

/*************
 *
 *   ur_sat_atom()
 *
 *************/

static
void ur_sat_atom(BOOL flipped, Topform c, int target_constraint,
		 Term sat_atom, Lindex idx,
		 void (*proc_proc) (Topform))

{
  /* Assume C is a unit. */
  Context sat_subst = get_context();
  Context nuc_subst = get_context();
  Term fnd_atom;
  Mindex_pos mate_pos;
  Literals slit = c->literals;

  fnd_atom = mindex_retrieve_first(sat_atom, (slit->sign?idx->neg:idx->pos),
				   UNIFY, sat_subst, nuc_subst,
				   FALSE, &mate_pos);
  while (fnd_atom) {
    Topform nuc = fnd_atom->container;
    int numlits = number_of_literals(nuc->literals);
    if (numlits > 1 && numlits <= Ur_nucleus_limit &&
	(!Initial_nuclei || nuc->initial)) {
      Literals fnd_lit = atom_to_literal(fnd_atom);
      Literals target;
      for (target=nuc->literals; target; target=target->next) {
	if (target != fnd_lit && target_check(target, target_constraint)) {
	    
	  Clash p = NULL;
	  Clash first = NULL;
	  Literals nlit;
	  for (nlit = nuc->literals; nlit; nlit = nlit->next) {
	    p = append_clash(p);
	    if (first == NULL)
	      first = p;
	    p->nuc_lit = nlit;
	    p->nuc_subst = nuc_subst;
	    if (nlit == fnd_lit) {
	      p->sat_lit = slit;
	      p->sat_subst = sat_subst;
	      p->clashable = TRUE;
	      p->clashed = TRUE;
	      p->flipped = flipped;
	    }
	    else {
	      p->clashable = (nlit != target);
	      if (p->clashable) {
		p->mate_index = (nlit->sign ? idx->neg : idx->pos);
		p->sat_subst = get_context();
	      }
	    }
	  }  /* for each literal of nucleus */
	  clash(first, unit_check, UR_RES_JUST, proc_proc);
	  zap_clash(first);
	}  
      }  /* for each target */
    }  /* if we have a nuc */
    fnd_atom = mindex_retrieve_next(mate_pos);
  }  /* for each mate */
  free_context(sat_subst);
  free_context(nuc_subst);
}  /* ur_sat_atom */

/*************
 *
 *   ur_satellite()
 *
 *************/

static
void ur_satellite(Topform c, int target_constraint, Lindex idx,
		  void (*proc_proc) (Topform))

{
  Term atom = c->literals->atom;
  ur_sat_atom(FALSE, c, target_constraint, atom, idx, proc_proc);
  /* if equality, try with the flip */
  if (eq_term(atom)) {
    Term flip = top_flip(atom);
    ur_sat_atom(TRUE, c, target_constraint, flip, idx, proc_proc);
    zap_top_flip(flip);
  }
}  /* ur_satellite */

/*************
 *
 *   ur_nucleus()
 *
 *************/

static
void ur_nucleus(Topform c, int target_constraint, Lindex idx,
		 void (*proc_proc) (Topform))
{
  if (number_of_literals(c->literals) > Ur_nucleus_limit ||
      (Initial_nuclei && !c->initial))
    return;
  else {
    Literals target;
    for (target = c->literals; target; target = target->next) {
      if (target_check(target, target_constraint)) {
	Clash p = NULL;
	Clash first = NULL;
	Literals lit;
	Context nuc_subst = get_context();
	for (lit = c->literals; lit; lit = lit->next) {
	  p = append_clash(p);
	  if (first == NULL)
	    first = p;
	  p->clashable = (lit != target);
	  p->nuc_lit = lit;
	  p->nuc_subst = nuc_subst;
	  if (p->clashable) {
	    p->mate_index = (lit->sign ? idx->neg : idx->pos);
	    p->sat_subst = get_context();
	  }
	}
	clash(first, unit_check, UR_RES_JUST, proc_proc);
	free_context(nuc_subst);
	zap_clash(first);  /* This also frees satellite contexts. */
      }
    }
  }
}  /* ur_nucleus */

/*************
 *
 *   ur_resolution()
 *
 *************/

/* DOCUMENTATION
Unit-resulting resolution.
*/

/* PUBLIC */
void ur_resolution(Topform c, int target_constraint, Lindex idx,
		   void (*proc_proc) (Topform))
{
  if (unit_clause(c->literals))
    ur_satellite(c, target_constraint, idx, proc_proc);
  else
    ur_nucleus(c, target_constraint, idx, proc_proc);
}  /* ur_resolution */

/*************
 *
 *   xx_res()
 *
 *************/

static
void xx_res(Literals lit, void (*proc_proc) (Topform))
{
  Term alpha = ARG(lit->atom,0);
  Term beta  = ARG(lit->atom,1);
  Context subst = get_context();
  Trail tr = NULL;

  if (unify(alpha, subst, beta, subst, &tr)) {
    Topform parent = lit->atom->container;
    int n = literal_number(parent->literals, lit);
    Topform c = get_topform();
    Literals l;
    c->justification = xxres_just(parent, n);
    for (l = parent->literals; l; l = l->next) {
      if (l != lit)
	c->literals = append_literal(c->literals, apply_lit(l, subst));
    }
    undo_subst(tr);
    upward_clause_links(c);
    c->attributes = inheritable_att_instances(parent->attributes, subst);
    
    (*proc_proc)(c);
  }
  free_context(subst);
}  /* xx_res */

/*************
 *
 *   binary_resolvent() - construct a binary resolvent
 *
 *************/

static
void binary_resolvent(BOOL flipped,
	     Literals l1, Context s1,
	     Literals l2, Context s2,
	     void (*proc_proc) (Topform))
{
  Topform r = get_topform();
  Topform nuc =  l1->atom->container;
  Topform sat =  l2->atom->container;
  Ilist j  = NULL;
  Literals l3;
  int i;
  int n = 0;

  /* Include literals in the nucleus. */
  for (l3 = nuc->literals, i=1; l3; l3 = l3->next, i++) {
    if (l3 == l1)
      n = i;  /* index of resolved literal */
    else
      r->literals = append_literal(r->literals, apply_lit(l3, s1));
  }
  j = ilist_append(j, nuc->id);
  j = ilist_append(j, n);
  
  /* Include literals in the satellite. */
  for (l3 = sat->literals, i=1; l3; l3 = l3->next, i++) {
    if (l3 == l2)
      n = i;  /* index of resolved literal */
    else
      r->literals = append_literal(r->literals, apply_lit(l3, s2));
  }
  
  j = ilist_append(j, sat->id);
  j = ilist_append(j, flipped ? -n : n);
  
  inherit_attributes(nuc, s1, sat, s2, r);

  r->justification = resolve_just(j, BINARY_RES_JUST);
  upward_clause_links(r);
  (*proc_proc)(r);
}  /* binary_resolvent */

/*************
 *
 *   binary_parent_test()
 *
 *   Is a literal eligible for binary resolution?
 *
 *************/

static
BOOL binary_parent_test(Literals lit, int res_type, int check_type)
{
  Topform c = lit->atom->container;

  if (res_type == POS_RES) {  /* positive resolution (one parent positive) */
    if (positive_clause(c->literals))
      return !Ordered || maximal_literal(c->literals, lit, check_type);
    else if (lit->sign)
      return FALSE;  /* cannot resolve on pos literal in nonpos clause */
    else if (exists_selected_literal(c->literals))
      return selected_literal(lit);
    else
      return !Ordered || maximal_signed_literal(c->literals, lit, check_type); /* max neg */
  }

  else if (res_type == NEG_RES) {  /* negative resolution (one parent neg) */
    if (negative_clause(c->literals))
      return !Ordered || maximal_literal(c->literals, lit, check_type);
    else if (!lit->sign)
      return FALSE;  /* cannot resolve on neg literal in nonneg clause */
    else  /* selection ignored for negative resolution */
      return !Ordered || maximal_signed_literal(c->literals, lit, check_type); /* max pos */
  }

  else {  /* ANY_RES (not necessarily positive or negative resolution) */
    if (exists_selected_literal(c->literals)) {
      if (lit->sign)
	return FALSE;  /* if any selected lits, cannot resolve on pos lit */
      else
	return selected_literal(lit);
    }
    else
      /* no selected literals in clause */
      return !Ordered || maximal_literal(c->literals, lit, check_type);
  }
}  /* binary_parent_test */

/*************
 *
 *   instantiate_clause()
 *
 *************/

/* DOCUMENTATION
*/

/* PUBLIC */
Topform instantiate_clause(Topform c, Context subst)
{
  Topform d = get_topform();
  Literals lit;
  for (lit = c->literals; lit; lit = lit->next)
    d->literals = append_literal(d->literals, apply_lit(lit, subst));
  for (lit = d->literals; lit; lit = lit->next)
    lit->atom->container = d;
  return d;
}  /* instantiate_clause */

/*************
 *
 *   check_instance()
 *
 *************/

static
BOOL check_instance(Literals lit, Context subst, int res_type)
{
  Topform c = lit->atom->container;
  if (number_of_maximal_literals(c->literals, FLAG_CHECK) == 1 ||
      variable_substitution(subst))
    return TRUE;
  else {
    Literals a;
    BOOL ok;
    int n = literal_number(c->literals, lit);
    Topform d = instantiate_clause(c, subst);
    copy_selected_literal_marks(c->literals, d->literals);
    a = ith_literal(d->literals, n);

    /* Note that using binary_parent_test repeats several tests
       not having to do with maximality.  These repeated steps
       are not expensive.
    */

    ok = binary_parent_test(a, res_type, FULL_CHECK);
    zap_topform(d);
    if (!ok)
      Res_instance_prunes++;
    return ok;
  }
}  /* check_instance */

/*************
 *
 *   check_instances()
 *
 *************/

static
BOOL check_instances(Literals lit1, Context subst1,
		     Literals lit2, Context subst2,
		     int res_type)
{
  if (!Check_instances)
    return TRUE;
  else
    return
      check_instance(lit1, subst1, res_type) &&
      check_instance(lit2, subst2, res_type);
}  /* check_instances */

/*************
 *
 *   bin_res_lit()
 *
 *************/

static
void bin_res_lit(Topform giv, Literals lit, Term atom,
		 int res_type, Lindex idx, void (*proc_proc) (Topform))
{
  BOOL flipped = (lit->atom != atom);
  Context nuc_subst = get_context();
  Context sat_subst = get_context();
  Term sat_atom;
  Mindex_pos mate_pos;
  sat_atom = mindex_retrieve_first(atom,
				   lit->sign ? idx->neg : idx->pos,
				   UNIFY, nuc_subst, sat_subst,
				   FALSE, &mate_pos);
  while (sat_atom) {
    Literals slit = atom_to_literal(sat_atom);
    if (binary_parent_test(slit, res_type, FLAG_CHECK) &&
	check_instances(lit, nuc_subst, slit, sat_subst, res_type))
      binary_resolvent(flipped, lit, nuc_subst, slit, sat_subst, proc_proc);
    sat_atom = mindex_retrieve_next(mate_pos);
  }
  free_context(nuc_subst);
  free_context(sat_subst);
}  /* bin_res_lit */

/*************
 *
 *   binary_resolution()
 *
 *************/

/* DOCUMENTATION
Binary resolution.
*/

/* PUBLIC */
void binary_resolution(Topform c,
		       int res_type,  /* POS_RES, NEG_RES, ANY_RES */
		       Lindex idx,
		       void (*proc_proc) (Topform))
{
  Literals lit;
  for (lit = c->literals; lit; lit = lit->next) {
    if (binary_parent_test(lit, res_type, FLAG_CHECK)) {
      bin_res_lit(c, lit, lit->atom, res_type, idx, proc_proc);

      /* If equality, try for resolution with the flip. */
      if (eq_term(lit->atom)) {
	Term flip = top_flip(lit->atom);
	bin_res_lit(c, lit, flip, res_type, idx, proc_proc);
	zap_top_flip(flip);
      }

      /* Try for resolution with x=x. */
      if (neg_eq(lit)) {
	xx_res(lit, proc_proc);
      }
    }
  }
}  /* binary_resolution */

/*************
 *
 *   binary_factors()
 *
 *************/

/* DOCUMENTATION
*/

/* PUBLIC */
void binary_factors(Topform c, void (*proc_proc) (Topform))
{
  Literals l1;
  int i = 1;
  Context subst = get_context();
  for (l1 = c->literals; l1; l1 = l1->next, i++) {
    Literals l2;
    int j = i+1;
    for (l2 = l1->next; l2; l2 = l2->next, j++) {

      Trail tr = NULL;
      if (l1->sign == l2->sign &&
	  /* maximal_literal_check ??? */
	  unify(l1->atom,subst,l2->atom,subst,&tr)) {
	Topform f = get_topform();
	Literals l3;
	f->justification = factor_just(c, i, j);
	for (l3 = c->literals; l3; l3 = l3->next) {
	  if (l3 != l2)
	    f->literals = append_literal(f->literals, apply_lit(l3, subst));
	}
	undo_subst(tr);
	upward_clause_links(f);
	f->attributes = cat_att(f->attributes,
				inheritable_att_instances(c->attributes,
							  subst));
	(*proc_proc)(f);
      }
    }
  }
  free_context(subst);
}  /* binary_factors */

/*************
 *
 *   merge_literals()
 *
 *************/

/* DOCUMENTATION
*/

/* PUBLIC */
void merge_literals(Topform c)
{
  Literals l3;
  int n;
  BOOL null_literals = FALSE;
  for (l3 = c->literals, n = 1; l3; l3 = l3->next, n++) {
    Literals l2;
    for (l2 = c->literals; l2 != l3; l2 = l2->next) {
      if (l2->atom && l3->atom) {
	if (l3->sign == l2->sign && term_ident(l3->atom, l2->atom)) {
	  zap_term(l3->atom);
	  c->justification = append_just(c->justification, merge_just(n));
	  l3->atom = NULL;  /* remove it below */
	  null_literals = TRUE;
	}
      }
    }
  }
  if (null_literals)
    c->literals = remove_null_literals(c->literals);
}  /* merge_literals */

/*************
 *
 *   copy_inference()
 *
 *************/

/* DOCUMENTATION
This makes a "copy" inference; that is, a copy of the clause
in which the justification is "copy".
All attributes are copied (not just the inheritible attributes).
An ID is not assigned.
*/

/* PUBLIC */
Topform copy_inference(Topform c)
{
  Topform new = copy_clause(c);
  /* Don't copy the justification; build a "copy" justification. */
  new->justification = copy_just(c);
#if 0
  /* Copy all attributes. */
  new->attributes = copy_attributes(c->attributes);
#else
  /* Copy inheritable attributes only. */
  new->attributes = inheritable_att_instances(c->attributes, NULL);
#endif
  return new;
}  /* copy_inference */

/*************
 *
 *   resolve2()
 *
 *************/

/* DOCUMENTATION
Resolve, if possible, two clauses on the literals (specified
by literals, counting from 1).
Include justification, transfer inheritable
attributes, but do not assign an ID.  Renumbering of variables
is optional.
<P>
if n2 < 0, then the literal is abs(n2), and it should be flipped.
*/

/* PUBLIC */
Topform resolve2(Topform c1, int n1, Topform c2, int n2, BOOL renumber_vars)
{
  Topform res;
  Literals l1 = ith_literal(c1->literals, n1);
  Literals l2 = ith_literal(c2->literals, abs(n2));
  Term a1 = l1->atom;
  Term a2 = l2->atom;
  Context s1 = get_context();
  Context s2 = get_context();
  Trail tr = NULL;
  Term a2x;

  if (n2 < 0)
    a2x = top_flip(a2);
  else
    a2x = a2;

  if (l1->sign != l2->sign && unify(a1, s1, a2x, s2, &tr)) {
    Literals lit;
    res = get_topform();
    for (lit = c1->literals; lit; lit = lit->next)
      if (lit != l1)
	res->literals = append_literal(res->literals, apply_lit(lit,  s1));
    for (lit = c2->literals; lit; lit = lit->next)
      if (lit != l2)
	res->literals = append_literal(res->literals, apply_lit(lit,  s2));

    inherit_attributes(c1, s1, c2, s2, res);
    res->justification = binary_res_just(c1, n1, c2, n2);
    if (c1->goal_derived || c2->goal_derived)
      res->goal_derived = TRUE;
    upward_clause_links(res);
    if (renumber_vars)
      renumber_variables(res, MAX_VARS);
    undo_subst(tr);
  }
  else
    res = NULL;

  if (n2 < 0)
    zap_top_flip(a2x);

  free_context(s1);
  free_context(s2);
  return res;
}  /* resolve2 */

/*************
 *
 *   resolve3()
 *
 *************/

/* DOCUMENTATION
Similar to resolve2(), but literals are given instead of integers.
*/

/* PUBLIC */
Topform resolve3(Topform c1, Literals l1, Topform c2, Literals l2, BOOL renumber_vars)
{
  return resolve2(c1, literal_number(c1->literals, l1),
		  c2, literal_number(c2->literals, l2),
		  renumber_vars);
}  /* resolve3 */

/*************
 *
 *   xx_resolve2()
 *
 *************/

/* DOCUMENTATION
Resolve, if possible, a clause with x=x.
Renumber vars, include justification, transfer inheritable
attributes, but do not assign an ID.
*/

/* PUBLIC */
Topform xx_resolve2(Topform c, int n, BOOL renumber_vars)
{
  Topform res;
  Literals l = ith_literal(c->literals, n);
  Context s = get_context();
  Trail tr = NULL;

  if (neg_eq(l) &&
      unify(ARG(l->atom,0), s,
	    ARG(l->atom,1), s, &tr)) {
    Literals lit;
    res = get_topform();
    for (lit = c->literals; lit; lit = lit->next)
      if (lit != l)
	res->literals = append_literal(res->literals, apply_lit(lit,  s));

    res->attributes = inheritable_att_instances(c->attributes, s);
    res->justification = xxres_just(c, n);
    upward_clause_links(res);
    if (renumber_vars)
      renumber_variables(res, MAX_VARS);
    undo_subst(tr);
  }
  else
    res = NULL;
  free_context(s);
  return res;
}  /* xx_resolve2 */
