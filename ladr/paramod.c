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

#include "paramod.h"
#include "clock.h"
#include "memory.h"

/* Private definitions and types */

static BOOL  Ordered_inference = FALSE;
static BOOL  Positive_inference = FALSE;
static BOOL  Para_from_vars = TRUE;
static BOOL  Para_into_vars = FALSE;
static BOOL  Para_from_small = FALSE;
static BOOL  Check_instances   = FALSE;  /* non-oriented from lits */

static unsigned long long Para_instance_prunes = 0;     /* counter */
static unsigned long long Basic_prunes = 0;             /* counter */

static Para_candidate_proc Candidate_proc = NULL;
static Para_materialized_proc Materialized_proc = NULL;
static unsigned Candidate_sample_rate = 0;
static struct para_candidate_stats Candidate_stats;

/* PUBLIC */
void set_paramodulation_candidate_proc(Para_candidate_proc proc,
                                       unsigned sample_rate)
{
  Candidate_proc = proc;
  Candidate_sample_rate = sample_rate;
}

/* PUBLIC */
void set_paramodulation_materialized_proc(Para_materialized_proc proc)
{
  Materialized_proc = proc;
}

/* PUBLIC */
void reset_paramodulation_candidate_stats(void)
{
  memset(&Candidate_stats, 0, sizeof(Candidate_stats));
}

/* PUBLIC */
void get_paramodulation_candidate_stats(struct para_candidate_stats *stats)
{
  *stats = Candidate_stats;
}

static Para_candidate_decision inspect_candidate(Para_candidate *candidate)
{
  Para_candidate_decision decision;
  double started = 0;
  Candidate_stats.candidates++;
  candidate->timing_sample =
    Candidate_sample_rate != 0 &&
    Candidate_stats.candidates % Candidate_sample_rate == 0;
  if (candidate->timing_sample) {
    Candidate_stats.timing_samples++;
    started = user_seconds();
  }
  decision = Candidate_proc == NULL ? PARA_CANDIDATE_MATERIALIZE :
                                      (*Candidate_proc)(candidate);
  if (candidate->timing_sample)
    Candidate_stats.precheck_seconds += user_seconds() - started;
  switch (decision) {
  case PARA_CANDIDATE_MATERIALIZE:
    Candidate_stats.materialized++;
    if (candidate->timing_sample)
      Candidate_stats.timing_materialized_samples++;
    break;
  case PARA_CANDIDATE_SKIP:
    Candidate_stats.skipped++;
    break;
  case PARA_CANDIDATE_CANCEL:
    Candidate_stats.cancelled++;
    break;
  default:
    fatal_error("invalid paramodulation candidate decision");
  }
  return decision;
}

/*************
 *
 *   paramodulation_options()
 *
 *************/

/* DOCUMENTATION
*/

/* PUBLIC */

void paramodulation_options(BOOL ordered_inference,
			    BOOL check_instances,
			    BOOL positive_inference,
			    BOOL basic_paramodulation,
			    BOOL para_from_vars,
			    BOOL para_into_vars,
			    BOOL para_from_small)
{
  Ordered_inference = ordered_inference;
  Para_from_vars = para_from_vars;
  Para_into_vars = para_into_vars;
  Para_from_small = para_from_small;
  Check_instances = check_instances;
  Positive_inference = positive_inference;
  set_basic_paramod(basic_paramodulation);
  Para_instance_prunes = 0;
  Basic_prunes = 0;
}  /* paramodulation_options */

/*************
 *
 *   para_instance_prunes()
 *
 *************/

/* DOCUMENTATION
*/

/* PUBLIC */
unsigned long long para_instance_prunes()
{
  return Para_instance_prunes;
}  /* para_instance_prunes */

/*************
 *
 *   basic_paramodulation_prunes()
 *
 *************/

/* DOCUMENTATION
How many paramodulants were killed because they failed the "basic" test.
*/

/* PUBLIC */
unsigned long long basic_paramodulation_prunes(void)
{
  return Basic_prunes;
}  /* basic_paramodulation_prunes */

/*************
 *
 *   basic_check()
 *
 *************/

static
BOOL basic_check(Term into_term)
{
  if (basic_paramod() && nonbasic_term(into_term)) {
    Basic_prunes++;
    return FALSE;
  }
  else
    return TRUE;
}  /* basic_check */

/*************
 *
 *   apply_lit_para()
 *
 *************/

static
Literals apply_lit_para(Literals lit, Context c)
{
  if (basic_paramod())
    return new_literal(lit->sign, apply_basic(lit->atom, c));
  else
    return new_literal(lit->sign, apply(lit->atom, c));
}  /* apply_lit_para */

/*************
 *
 *   apply_substitute_para()
 *
 *************/

#if 0
static
Term apply_substitute_para(Term t, Term beta, Context from_subst,
			   Term into, Context into_subst)
{
  if (basic_paramod())
    return apply_basic_substitute(t, beta, from_subst, into, into_subst);
  else
    return apply_substitute(t, beta, from_subst, into, into_subst);
}  /* apply_substitute_para */
#endif

/*************
 *
 *   paramodulate()
 *
 *   Construct a paramodulant.
 *
 *************/

/* DOCUMENTATION
*/

/* PUBLIC */
Topform paramodulate(Literals from_lit, int from_side, Context from_subst,
		     Topform into_clause, Ilist into_pos, Context into_subst)
{
  Topform from_clause = from_lit->atom->container;
  Topform p = get_topform();
  Term beta = ARG(from_lit->atom, from_side == 0 ? 1 : 0);
  Literals into_lit = ith_literal(into_clause->literals, into_pos->i);
  Literals lit;
  for (lit = from_clause->literals; lit != NULL; lit = lit->next) {
    if (lit != from_lit)
      p->literals = append_literal(p->literals,
				   apply_lit_para(lit, from_subst));
  }
  for (lit = into_clause->literals; lit != NULL; lit = lit->next) {
    if (lit != into_lit)
      p->literals = append_literal(p->literals,
				   apply_lit_para(lit, into_subst));
    else
      p->literals = 
	append_literal(p->literals,
		       new_literal(lit->sign,
				   apply_substitute2(lit->atom,
						     beta,
						     from_subst,
						     into_pos->next,
						     into_subst)));
  }
  inherit_attributes(from_clause, from_subst, into_clause, into_subst, p);
  upward_clause_links(p);
  return p;
}  /* paramodulate */

/*************
 *
 *   para_from_right()
 *
 *   Should we paramoulate from the right side?
 *   If it is oriented, then NO;
 *   else if it is a renamable_flip unit, then NO;
 *   else YES.
 *
 *************/

static
BOOL para_from_right(Term atom)
{
  /* Assume atom is an eq_atom. */
  if (Para_from_small)
    return TRUE;
  if (oriented_eq(atom))
    return FALSE;
  else if (renamable_flip_eq(atom) &&
	   unit_clause(((Topform) atom->container)->literals))
    return FALSE;
  else
    return TRUE;
}  /* para_from_right */

/*************
 *
 *   from_parent_test()
 *
 *************/

static
BOOL from_parent_test(Literals from_lit, int check)
{
  Topform from_parent = from_lit->atom->container;
  if (Positive_inference)
    return
      pos_eq(from_lit) &&
      positive_clause(from_parent->literals) &&
      (!Ordered_inference ||
       maximal_literal(from_parent->literals, from_lit, check));
  else
    return
      pos_eq(from_lit) &&
      !exists_selected_literal(from_parent->literals) &&
      (!Ordered_inference ||
       maximal_literal(from_parent->literals, from_lit, check));
}  /* from_parent_test */

/*************
 *
 *   into_parent_test()
 *
 *************/

static
BOOL into_parent_test(Literals into_lit, int check)
{
  Topform into_parent = into_lit->atom->container;
  if (into_lit->sign) {
    /* into positive literal */
    if (Positive_inference)
      return
	positive_clause(into_parent->literals) &&
	(!Ordered_inference ||
	 maximal_literal(into_parent->literals, into_lit, check));
    else
      return
	!exists_selected_literal(into_parent->literals) &&
	(!Ordered_inference ||
	 maximal_literal(into_parent->literals, into_lit, check));
  }
  else {
    /* into negative literal */
    if (Positive_inference) {
      if (exists_selected_literal(into_parent->literals))
	return selected_literal(into_lit);
      else
	return (!Ordered_inference ||
		maximal_signed_literal(into_parent->literals,into_lit,check));
    }
    else {
      if (exists_selected_literal(into_parent->literals))
	return selected_literal(into_lit);
      else
	return (!Ordered_inference ||
		maximal_literal(into_parent->literals, into_lit, check));
    }
  }
}  /* into_parent_test */

/*************
 *
 *   check_instance()
 *
 *************/

static
BOOL check_instance(Literals lit, Context subst, BOOL is_from_parent)
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

    if (is_from_parent)
      ok = from_parent_test(a, FULL_CHECK);
    else
      ok = into_parent_test(a, FULL_CHECK);
    zap_topform(d);
    if (!ok)
      Para_instance_prunes++;
    return ok;
  }
}  /* check_instance */

/*************
 *
 *   check_instances()
 *
 *************/

static
BOOL check_instances(Literals from_lit, int from_side, Context cf,
		     Literals into_lit, Term into, Context ci)
{
  if (!Check_instances)
    return TRUE;
  else {
    return
      check_instance(from_lit, cf, TRUE) &&
      check_instance(into_lit, ci, FALSE);
  }
}  /* check_instances */

/*************
 *
 *   para_into()
 *
 *   Paramodulate from a given side of a given literal into a given
 *   term and/or its subterms.
 *
 *************/

static
BOOL para_into(Literals from_lit, int from_side, Context cf, Ilist from_pos,
	       Topform into_clause, Literals into_lit, Term into, Context ci,
	       Ilist into_pos,
	       BOOL skip_top,
	       Topform_proc proc_proc)
{
  /* Iterative subterm traversal for paramodulation-into.  The two leading
     coordinates in into_pos are owned by para_into_lit(); deeper coordinates
     live in this depth-indexed stack array.  Every descended frame has a
     complex parent, so path_nodes[top-1] is its exact predecessor.
     paramodulate() copies the live coordinate list into each justification
     before the consumer is called; no stack address escapes this routine. */
  struct { Term node; int child; BOOL skip; } stack[1000];
  struct ilist path_nodes[1000];
  Ilist path_base, path_tail;
  int top;

  if (!(((!VARIABLE(into)) | Para_into_vars) && basic_check(into)))
    return TRUE;

  top = 0;
  stack[0].node = into;
  stack[0].child = -1;  /* -1 means haven't started children yet */
  stack[0].skip = skip_top;
  path_base = ilist_last(into_pos);
  path_tail = path_base;

  while (top >= 0) {
    Term cur = stack[top].node;

    if (stack[top].child == -1) {
      /* First visit to this node.  Set up position list and start children. */
      if (COMPLEX(cur)) {
        Ilist new_pos = &path_nodes[top];
        new_pos->i = 0;
        new_pos->next = NULL;
        path_tail->next = new_pos;
        path_tail = new_pos;
        stack[top].child = 0;
      }
      else
        stack[top].child = ARITY(cur);  /* skip to "try unify at top" */
    }

    /* Process children. */
    if (stack[top].child >= 0 && stack[top].child < ARITY(cur)) {
      int ci_idx = stack[top].child;
      Term ch = ARG(cur, ci_idx);
      stack[top].child++;
      path_nodes[top].i++;

      if (((!VARIABLE(ch)) | Para_into_vars) && basic_check(ch)) {
        top++;
        stack[top].node = ch;
        stack[top].child = -1;
        stack[top].skip = FALSE;
      }
      continue;
    }

    /* All children done. Clean up position node if any. */
    if (COMPLEX(cur)) {
      path_tail = top == 0 ? path_base : &path_nodes[top - 1];
      path_tail->next = NULL;
    }

    /* Try unifying at this node (unless skip_top). */
    if (!stack[top].skip) {
      Trail tr = NULL;
      Term alpha = ARG(from_lit->atom, from_side);
      if (unify(alpha, cf, cur, ci, &tr)) {
        if (check_instances(from_lit, from_side, cf, into_lit, cur, ci)) {
          Para_candidate candidate;
          Para_candidate_decision decision;
          Topform p;
          double started = 0;
          unsigned long long allocation_calls = 0;
          unsigned long long allocation_bytes = 0;
          struct memory_stats memory_before;
          candidate.from_lit = from_lit;
          candidate.from_side = from_side;
          candidate.from_subst = cf;
          candidate.into_clause = into_clause;
          candidate.into_lit = into_lit;
          candidate.into_pos = into_pos;
          candidate.into_subst = ci;
          decision = Candidate_proc == NULL ? PARA_CANDIDATE_MATERIALIZE :
                                               inspect_candidate(&candidate);
          if (Candidate_proc == NULL)
            candidate.timing_sample = FALSE;
          if (decision == PARA_CANDIDATE_CANCEL) {
            path_base->next = NULL;
            undo_subst(tr);
            return FALSE;
          }
          if (decision == PARA_CANDIDATE_SKIP) {
            undo_subst(tr);
            top--;
            continue;
          }
          if (candidate.timing_sample) {
            started = user_seconds();
            allocation_calls = memory_allocation_calls();
            memory_get_stats(&memory_before);
            allocation_bytes = memory_before.cumulative_bytes;
          }
          p = paramodulate(from_lit, from_side, cf,
                           into_clause, into_pos, ci);
          p->justification = para_just(PARA_JUST,
                                        from_lit->atom->container,
                                        copy_ilist(from_pos),
                                        into_clause,
                                        copy_ilist(into_pos));
          if (Materialized_proc != NULL)
            (*Materialized_proc)(p);
          if (candidate.timing_sample) {
            struct memory_stats memory_after;
            Candidate_stats.construction_seconds += user_seconds() - started;
            memory_get_stats(&memory_after);
            Candidate_stats.construction_allocation_calls +=
              memory_allocation_calls() - allocation_calls;
            Candidate_stats.construction_allocation_bytes +=
              memory_after.cumulative_bytes - allocation_bytes;
            started = user_seconds();
            allocation_calls = memory_allocation_calls();
            allocation_bytes = memory_after.cumulative_bytes;
          }
          if (!(*proc_proc)(p)) {
            if (candidate.timing_sample) {
              struct memory_stats memory_after;
              Candidate_stats.consumer_seconds += user_seconds() - started;
              memory_get_stats(&memory_after);
              Candidate_stats.consumer_allocation_calls +=
                memory_allocation_calls() - allocation_calls;
              Candidate_stats.consumer_allocation_bytes +=
                memory_after.cumulative_bytes - allocation_bytes;
            }
            /* The dynamic suffix consists of stack-owned traversal nodes.
               Detach it before returning to para_into_lit(). */
            path_base->next = NULL;
            undo_subst(tr);
            return FALSE;
          }
          if (candidate.timing_sample) {
            struct memory_stats memory_after;
            Candidate_stats.consumer_seconds += user_seconds() - started;
            memory_get_stats(&memory_after);
            Candidate_stats.consumer_allocation_calls +=
              memory_allocation_calls() - allocation_calls;
            Candidate_stats.consumer_allocation_bytes +=
              memory_after.cumulative_bytes - allocation_bytes;
          }
        }
        undo_subst(tr);
      }
    }

    top--;
  }
  return TRUE;
}  /* para_into */

/*************
 *
 *   para_into_lit()
 *
 *************/

static
BOOL para_into_lit(Literals from_lit, int from_side, Context cf,
		   Literals into_lit, Context ci,
		   BOOL check_top,
		   Topform_proc proc_proc)
{
  Term alpha = ARG(from_lit->atom, from_side);
  if (!VARIABLE(alpha) || Para_from_vars) {
    /* Position vectors are constructed FORWARD.  They are copied into every
       retained justification before this call returns, so the fixed
       literal/argument prefix can share the traversal's stack lifetime. */
    struct ilist from_storage[2] = {{0, &from_storage[1]}, {0, NULL}};
    struct ilist into_storage[2] = {{0, &into_storage[1]}, {0, NULL}};
    Ilist from_pos = from_storage;
    Ilist into_pos = into_storage;
    Term into_atom = into_lit->atom;
    Term from_atom = from_lit->atom;
    int i;
    Topform from_clause = from_atom->container;
    Topform into_clause = into_atom->container;
    BOOL positive_equality = pos_eq(into_lit);

    from_pos->i = literal_number(from_clause->literals, from_lit);
    from_pos->next->i = from_side+1;  /* arg of from_lit, counts from 1 */
    into_pos->i = literal_number(into_clause->literals, into_lit);
    for (i = 0; i < ARITY(into_atom); i++) {
      BOOL skip_top = (check_top &&
		       positive_equality &&
		       (i == 0 ||
			(i == 1 && para_from_right(into_lit->atom))));

      into_pos->next->i += 1;  /* increment arg number */
      if (!para_into(from_lit, from_side, cf, from_pos,
		     into_clause, into_lit, ARG(into_atom,i), ci, into_pos,
		     skip_top, proc_proc)) {
        return FALSE;
      }
    }
  }
  return TRUE;
}  /* para_into_lit */

/*************
 *
 *   para_from_into()
 *
 *************/

/* DOCUMENTATION
Paramodulate from one clause into another (non-backtrack unification version).
<P>
For oriented equality atoms, we go from left sides only
and into both sides.
For nonoriented equality atoms, we go from and into both sides.
*/

/* PUBLIC */
BOOL para_from_into(Topform from, Context cf,
		    Topform into, Context ci,
		    BOOL check_top,
		    Topform_proc proc_proc)
{
  if (exists_selected_literal(from->literals))
    return TRUE;  /* cannot para from clause with selected literals */
  else {
    Literals from_lit;
    for (from_lit = from->literals; from_lit; from_lit = from_lit->next) {
      if (from_parent_test(from_lit, FLAG_CHECK)) {
	Literals into_lit;
	for (into_lit = into->literals; into_lit; into_lit = into_lit->next) {
	  if (into_parent_test(into_lit, FLAG_CHECK)) {
	    if (!para_into_lit(from_lit,0,cf,into_lit,ci,check_top,proc_proc))
              return FALSE;  /* from L */
	    if (para_from_right(from_lit->atom))
	      if (!para_into_lit(from_lit,1,cf,into_lit,ci,check_top,proc_proc))
                return FALSE; /* from R */
	  }
	}
      }
    }
  }
  return TRUE;
}  /* para_from_into */

/* PUBLIC */
BOOL para_unit_from_side_eligible(Topform from, int side)
{
  Literals lit;
  if (from == NULL || side < 0 || side > 1 ||
      !unit_clause(from->literals))
    return FALSE;
  lit = from->literals;
  if (!from_parent_test(lit, FLAG_CHECK))
    return FALSE;
  if (side == 1 && !para_from_right(lit->atom))
    return FALSE;
  return !VARIABLE(ARG(lit->atom, side)) || Para_from_vars;
}

static Literals para_iterator_literal(Literals lits, unsigned position)
{
  while (lits != NULL && position != 0) {
    lits = lits->next;
    position--;
  }
  return lits;
}

static BOOL para_iterator_term_eligible(Term t)
{
  return (((!VARIABLE(t)) | Para_into_vars) && basic_check(t));
}

static void para_iterator_reserve_path(Para_iterator *it, unsigned need)
{
  unsigned capacity;
  unsigned *path;
  if (need <= it->path_capacity)
    return;
  capacity = it->path_capacity == 0 ? 8 : it->path_capacity;
  while (capacity < need) {
    if (capacity > UINT_MAX / 2)
      fatal_error("paramodulation iterator path overflow");
    capacity *= 2;
  }
  path = safe_calloc(capacity, sizeof(*path));
  if (it->path != NULL) {
    memcpy(path, it->path, it->path_depth * sizeof(*path));
    safe_free(it->path);
  }
  it->path = path;
  it->path_capacity = capacity;
}

static Term para_iterator_term_at_path(
  Term root, const Para_iterator *it, unsigned depth)
{
  unsigned i;
  Term t = root;
  for (i = 0; i < depth; i++) {
    unsigned child = it->path[i];
    if (!COMPLEX(t) || child >= (unsigned) ARITY(t))
      fatal_error("invalid paramodulation iterator term path");
    t = ARG(t, child);
  }
  return t;
}

/* Move from root to its first postorder eligible descendant. */
static void para_iterator_descend_first(Para_iterator *it, Term root)
{
  Term t = root;
  while (COMPLEX(t)) {
    int i;
    for (i = 0; i < ARITY(t); i++)
      if (para_iterator_term_eligible(ARG(t, i)))
        break;
    if (i == ARITY(t))
      break;
    para_iterator_reserve_path(it, it->path_depth + 1);
    it->path[it->path_depth++] = (unsigned) i;
    t = ARG(t, i);
  }
}

/* Advance one position in the same children-before-parent order used by
   para_into().  FALSE means the top-level atom argument is complete. */
static BOOL para_iterator_next_path(Para_iterator *it, Term root)
{
  Term parent;
  unsigned current, i;
  if (it->path_depth == 0)
    return FALSE;
  parent = para_iterator_term_at_path(root, it, it->path_depth - 1);
  current = it->path[it->path_depth - 1];
  for (i = current + 1; i < (unsigned) ARITY(parent); i++)
    if (para_iterator_term_eligible(ARG(parent, i))) {
      it->path[it->path_depth - 1] = i;
      para_iterator_descend_first(it, ARG(parent, i));
      return TRUE;
    }
  /* All siblings are done; the parent itself is next. */
  it->path_depth--;
  return TRUE;
}

static void para_iterator_next_into_literal(Para_iterator *it)
{
  it->into_literal++;
  it->from_side = 0;
  it->into_argument = 0;
  it->path_depth = 0;
  it->positioned = FALSE;
}

static void para_iterator_next_from_literal(Para_iterator *it)
{
  it->from_literal++;
  it->into_literal = 0;
  it->from_side = 0;
  it->into_argument = 0;
  it->path_depth = 0;
  it->positioned = FALSE;
}

/* Establish the next stable coordinate without enumerating any earlier raw
   inference position.  Parent/literal eligibility is immutable for a
   collective historical pair, so skipped axes never need to be revisited. */
static BOOL para_iterator_prepare(
  Topform from, Topform into, Para_iterator *it)
{
  if (it->complete)
    return FALSE;
  if (exists_selected_literal(from->literals)) {
    it->complete = TRUE;
    return FALSE;
  }
  while (!it->positioned) {
    Literals from_lit =
      para_iterator_literal(from->literals, it->from_literal);
    Literals into_lit;
    Term into_atom, root;
    if (from_lit == NULL) {
      it->complete = TRUE;
      return FALSE;
    }
    if (!from_parent_test(from_lit, FLAG_CHECK)) {
      para_iterator_next_from_literal(it);
      continue;
    }
    into_lit = para_iterator_literal(into->literals, it->into_literal);
    if (into_lit == NULL) {
      para_iterator_next_from_literal(it);
      continue;
    }
    if (!into_parent_test(into_lit, FLAG_CHECK)) {
      para_iterator_next_into_literal(it);
      continue;
    }
    if (it->from_side > 1 ||
        (it->from_side == 1 && !para_from_right(from_lit->atom))) {
      para_iterator_next_into_literal(it);
      continue;
    }
    if (VARIABLE(ARG(from_lit->atom, it->from_side)) && !Para_from_vars) {
      it->from_side++;
      it->into_argument = 0;
      it->path_depth = 0;
      continue;
    }
    into_atom = into_lit->atom;
    if (it->into_argument >= (unsigned) ARITY(into_atom)) {
      it->from_side++;
      it->into_argument = 0;
      it->path_depth = 0;
      continue;
    }
    root = ARG(into_atom, it->into_argument);
    if (!para_iterator_term_eligible(root)) {
      it->into_argument++;
      it->path_depth = 0;
      continue;
    }
    it->path_depth = 0;
    para_iterator_descend_first(it, root);
    it->positioned = TRUE;
  }
  return TRUE;
}

static Ilist para_iterator_from_position(
  const Para_iterator *it)
{
  Ilist pos = NULL;
  pos = ilist_append(pos, (int) it->from_literal + 1);
  pos = ilist_append(pos, (int) it->from_side + 1);
  return pos;
}

static Ilist para_iterator_into_position(
  const Para_iterator *it)
{
  unsigned i;
  Ilist pos = NULL;
  pos = ilist_append(pos, (int) it->into_literal + 1);
  pos = ilist_append(pos, (int) it->into_argument + 1);
  for (i = 0; i < it->path_depth; i++)
    pos = ilist_append(pos, (int) it->path[i] + 1);
  return pos;
}

static void para_iterator_advance(Para_iterator *it, Term root)
{
  if (para_iterator_next_path(it, root))
    it->positioned = TRUE;
  else {
    it->into_argument++;
    it->path_depth = 0;
    it->positioned = FALSE;
  }
}

/* PUBLIC */
void para_iterator_init(Para_iterator *it)
{
  memset(it, 0, sizeof(*it));
}

/* PUBLIC */
void para_iterator_reset(Para_iterator *it)
{
  unsigned *path = it->path;
  unsigned capacity = it->path_capacity;
  memset(it, 0, sizeof(*it));
  it->path = path;
  it->path_capacity = capacity;
}

/* PUBLIC */
void para_iterator_zap(Para_iterator *it)
{
  safe_free(it->path);
  memset(it, 0, sizeof(*it));
}

/* PUBLIC */
BOOL para_iterator_at_start(const Para_iterator *it)
{
  return it->from_literal == 0 && it->into_literal == 0 &&
         it->from_side == 0 && it->into_argument == 0 &&
         it->path_depth == 0 && !it->positioned && !it->complete;
}

/* Enumerate a bounded suffix of para_from_into() in exactly its original
   raw conclusion order.  Every eligible subterm visit consumes one raw
   unit, including a failed unification or a check_top-suppressed root.
   Hence both successful and unsuccessful searches have a hard turn bound. */
/* PUBLIC */
BOOL para_from_into_bounded(Topform from, Topform into, BOOL check_top,
			    Para_iterator *it,
			    unsigned long long raw_budget,
			    unsigned long long yield_budget,
			    Topform_proc proc_proc,
			    unsigned long long *raw_steps,
			    unsigned long long *yielded)
{
  Context cf, ci;
  if (raw_budget == 0 || yield_budget == 0)
    fatal_error("bounded paramodulation requires nonzero budgets");
  *raw_steps = 0;
  *yielded = 0;
  cf = get_context();
  ci = get_context();
  while (*raw_steps < raw_budget && *yielded < yield_budget &&
         para_iterator_prepare(from, into, it)) {
    Literals from_lit =
      para_iterator_literal(from->literals, it->from_literal);
    Literals into_lit =
      para_iterator_literal(into->literals, it->into_literal);
    Term root = ARG(into_lit->atom, it->into_argument);
    Term cur = para_iterator_term_at_path(root, it, it->path_depth);
    BOOL positive_equality = pos_eq(into_lit);
    BOOL skip_top = check_top && positive_equality && it->path_depth == 0 &&
      (it->into_argument == 0 ||
       (it->into_argument == 1 && para_from_right(into_lit->atom)));
    Ilist from_pos = NULL, into_pos = NULL;
    Trail tr = NULL;
    Topform result = NULL;

    (*raw_steps)++;
    if (!skip_top) {
      Term alpha = ARG(from_lit->atom, it->from_side);
      if (unify(alpha, cf, cur, ci, &tr)) {
        if (check_instances(from_lit, (int) it->from_side, cf,
                            into_lit, cur, ci)) {
          Para_candidate candidate;
          Para_candidate_decision decision;
          double started = 0;
          unsigned long long allocation_calls = 0;
          unsigned long long allocation_bytes = 0;
          struct memory_stats memory_before;
          from_pos = para_iterator_from_position(it);
          into_pos = para_iterator_into_position(it);
          candidate.from_lit = from_lit;
          candidate.from_side = (int) it->from_side;
          candidate.from_subst = cf;
          candidate.into_clause = into;
          candidate.into_lit = into_lit;
          candidate.into_pos = into_pos;
          candidate.into_subst = ci;
          decision = Candidate_proc == NULL ? PARA_CANDIDATE_MATERIALIZE :
                                               inspect_candidate(&candidate);
          if (Candidate_proc == NULL)
            candidate.timing_sample = FALSE;
          if (decision == PARA_CANDIDATE_CANCEL) {
            undo_subst(tr);
            zap_ilist(from_pos);
            zap_ilist(into_pos);
            it->complete = TRUE;
            free_context(cf);
            free_context(ci);
            return TRUE;
          }
          if (decision == PARA_CANDIDATE_SKIP) {
            undo_subst(tr);
            para_iterator_advance(it, root);
            zap_ilist(from_pos);
            zap_ilist(into_pos);
            continue;
          }
          if (candidate.timing_sample) {
            started = user_seconds();
            allocation_calls = memory_allocation_calls();
            memory_get_stats(&memory_before);
            allocation_bytes = memory_before.cumulative_bytes;
          }
          result = paramodulate(from_lit, (int) it->from_side, cf,
                                into, into_pos, ci);
          result->justification = para_just(
            PARA_JUST, from_lit->atom->container, copy_ilist(from_pos),
            into, copy_ilist(into_pos));
          if (Materialized_proc != NULL)
            (*Materialized_proc)(result);
          if (candidate.timing_sample) {
            struct memory_stats memory_after;
            Candidate_stats.construction_seconds += user_seconds() - started;
            memory_get_stats(&memory_after);
            Candidate_stats.construction_allocation_calls +=
              memory_allocation_calls() - allocation_calls;
            Candidate_stats.construction_allocation_bytes +=
              memory_after.cumulative_bytes - allocation_bytes;
          }
        }
        undo_subst(tr);
      }
    }
    para_iterator_advance(it, root);
    zap_ilist(from_pos);
    zap_ilist(into_pos);
    if (result != NULL) {
      (*yielded)++;
      {
        BOOL sampled = Candidate_sample_rate != 0 &&
          Candidate_stats.candidates % Candidate_sample_rate == 0;
        double started = sampled ? user_seconds() : 0;
        unsigned long long allocation_calls = sampled ?
          memory_allocation_calls() : 0;
        unsigned long long allocation_bytes = 0;
        struct memory_stats memory_before;
        if (sampled) {
          memory_get_stats(&memory_before);
          allocation_bytes = memory_before.cumulative_bytes;
        }
      if (!(*proc_proc)(result)) {
        if (sampled) {
          struct memory_stats memory_after;
          Candidate_stats.consumer_seconds += user_seconds() - started;
          memory_get_stats(&memory_after);
          Candidate_stats.consumer_allocation_calls +=
            memory_allocation_calls() - allocation_calls;
          Candidate_stats.consumer_allocation_bytes +=
            memory_after.cumulative_bytes - allocation_bytes;
        }
        it->complete = TRUE;
        free_context(cf);
        free_context(ci);
        return TRUE;
      }
        if (sampled) {
          struct memory_stats memory_after;
          Candidate_stats.consumer_seconds += user_seconds() - started;
          memory_get_stats(&memory_after);
          Candidate_stats.consumer_allocation_calls +=
            memory_allocation_calls() - allocation_calls;
          Candidate_stats.consumer_allocation_bytes +=
            memory_after.cumulative_bytes - allocation_bytes;
        }
      }
    }
  }
  if (!it->complete)
    (void) para_iterator_prepare(from, into, it);
  free_context(cf);
  free_context(ci);
  return it->complete;
}  /* para_from_into_bounded */

/*************
 *
 *   para_pos()
 *
 *************/

/* DOCUMENTATION
Construct a paramodulant from the given data.  A fatal error
occurs if it does not exist.  In building the justification,
the position vectors are copied.
*/

/* PUBLIC */
Topform para_pos(Topform from_clause, Ilist from_pos,
		 Topform into_clause, Ilist into_pos)
{
  Context cf = get_context();
  Context ci = get_context();
  Trail tr = NULL;
  Topform paramodulant;
  BOOL ok;

  Literals from_lit = ith_literal(from_clause->literals, from_pos->i);
  Literals into_lit = ith_literal(into_clause->literals, into_pos->i);
  int from_side = (from_pos->next->i == 1 ? 0 : 1);
  Term alpha = ARG(from_lit->atom, from_side);
  Term into_term = term_at_pos(into_lit->atom, into_pos->next);
  if (into_term == NULL)
    fatal_error("paramod2_instances, term does not exist");

  ok = unify(alpha, cf, into_term, ci, &tr);
  if (!ok)
    fatal_error("para_pos, terms do not unify");

  paramodulant = paramodulate(from_lit, from_side, cf,
			      into_clause, into_pos, ci);

  paramodulant->justification = para_just(PARA_JUST,
					  from_clause, copy_ilist(from_pos),
					  into_clause, copy_ilist(into_pos));
  renumber_variables(paramodulant, MAX_VARS);
  undo_subst(tr);
  free_context(cf);
  free_context(ci);
  return paramodulant;
}  /* para_pos */

/*************
 *
 *   para_pos2()
 *
 *************/

/* DOCUMENTATION
Construct a paramodulant from the given data.  A fatal error
occurs if it does not exist.  In building the justification,
the position vectors are copied.

This is similar to para_pos(), except that it allows the
into_term to be a variable.
*/

/* PUBLIC */
Topform para_pos2(Topform from, Ilist from_pos, Topform into, Ilist into_pos)
{
  Context from_subst = get_context();
  Context into_subst = get_context();
  Trail tr = NULL;
  BOOL ok;
  int from_side;
  Term alpha, into_term;
  Topform p;
  Term beta;
  Literals lit;

  Literals from_lit = ith_literal(from->literals, from_pos->i);
  Literals into_lit = ith_literal(into->literals, into_pos->i);
  from_side = (from_pos->next->i == 1 ? 0 : 1);
  alpha = ARG(from_lit->atom, from_side);
  into_term = term_at_pos(into_lit->atom, into_pos->next);
  if (into_term == NULL)
    fatal_error("paramod2_instances, term does not exist");

  ok = unify(alpha, from_subst, into_term, into_subst, &tr);
  if (!ok)
    fatal_error("para_pos2, terms do not unify");

  p = get_topform();
  beta = ARG(from_lit->atom, from_side == 0 ? 1 : 0);
  for (lit = from->literals; lit; lit = lit->next) {
    if (lit != from_lit)
      p->literals = append_literal(p->literals,
				   apply_lit_para(lit, from_subst));
  }
  for (lit = into->literals; lit; lit = lit->next) {
    if (lit != into_lit) {
      p->literals = append_literal(p->literals,
				   apply_lit_para(lit, into_subst));
    }
    else {
      p->literals = 
	append_literal(p->literals,
		       new_literal(lit->sign,
				   apply_substitute2(lit->atom,
						     beta,
						     from_subst,
						     into_pos->next,
						     into_subst)));
    }
  }
  inherit_attributes(from, from_subst, into, into_subst, p);
  upward_clause_links(p);

  p->justification = para_just(PARA_JUST,
			       from, copy_ilist(from_pos),
			       into, copy_ilist(into_pos));
  renumber_variables(p, MAX_VARS);
  undo_subst(tr);
  free_context(from_subst);
  free_context(into_subst);
  return p;
}  /* para_pos2 */
