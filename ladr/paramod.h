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

#ifndef TP_PARAMOD_H
#define TP_PARAMOD_H

#include "resolve.h"
#include "basic.h"

/* INTRODUCTION
This package has a paramodulation inference rule.
*/

/* Public definitions */

/* where to paramodulate into */

typedef enum { PARA_ALL,
	       PARA_ALL_EXCEPT_TOP,
	       PARA_TOP_ONLY } Para_loc;

/* Stable, pointer-free coordinates for bounded non-indexed
   paramodulation.  path contains zero-based child numbers below the current
   top-level atom argument.  The allocation is transient process state; only
   the scalar fields and path values are serialized by callers. */
typedef struct para_iterator {
  unsigned from_literal;
  unsigned into_literal;
  unsigned from_side;
  unsigned into_argument;
  unsigned path_depth;
  unsigned path_capacity;
  unsigned *path;
  BOOL positioned;
  BOOL complete;
} Para_iterator;

/* A successful paramodulation candidate immediately before its conclusion
   is materialized.  The terms and position lists remain owned by the
   inference traversal and are valid only for the duration of the callback. */
typedef struct para_candidate {
  Literals from_lit;
  int from_side;
  Context from_subst;
  Topform into_clause;
  Literals into_lit;
  Ilist into_pos;
  Context into_subst;
  BOOL timing_sample;
} Para_candidate;

typedef enum {
  PARA_CANDIDATE_MATERIALIZE,
  PARA_CANDIDATE_SKIP,
  PARA_CANDIDATE_CANCEL
} Para_candidate_decision;

typedef Para_candidate_decision
  (*Para_candidate_proc)(const Para_candidate *candidate);

typedef void (*Para_materialized_proc)(Topform conclusion);

struct para_candidate_stats {
  unsigned long long candidates;
  unsigned long long materialized;
  unsigned long long skipped;
  unsigned long long cancelled;
  unsigned long long timing_samples;
  unsigned long long timing_materialized_samples;
  double precheck_seconds;
  double construction_seconds;
  double consumer_seconds;
  unsigned long long construction_allocation_calls;
  unsigned long long construction_allocation_bytes;
  unsigned long long consumer_allocation_calls;
  unsigned long long consumer_allocation_bytes;
};

/* End of public definitions */

/* Public function prototypes from paramod.c */


void paramodulation_options(BOOL ordered_inference,
			    BOOL check_instances,
			    BOOL positive_inference,
			    BOOL basic_paramodulation,
			    BOOL para_from_vars,
			    BOOL para_into_vars,
			    BOOL para_from_small);

unsigned long long para_instance_prunes();

unsigned long long basic_paramodulation_prunes(void);

/* Install an optional pre-materialization callback.  A zero sample rate
   disables timing; otherwise every Nth successful candidate is timed. */
void set_paramodulation_candidate_proc(Para_candidate_proc proc,
                                       unsigned sample_rate);

void set_paramodulation_materialized_proc(Para_materialized_proc proc);

void reset_paramodulation_candidate_stats(void);

void get_paramodulation_candidate_stats(struct para_candidate_stats *stats);

Topform paramodulate(Literals from_lit, int from_side, Context from_subst,
		     Topform into_clause, Ilist into_pos, Context into_subst);

BOOL para_from_into(Topform from, Context cf,
                    Topform into, Context ci,
                    BOOL check_top,
                    Topform_proc proc_proc);

/* Exact public form of the unit/from-side eligibility checks used by
   para_from_into().  This lets target planners avoid inventing a second
   interpretation of ordering and para-from flags. */
BOOL para_unit_from_side_eligible(Topform from, int side);

void para_iterator_init(Para_iterator *it);

void para_iterator_reset(Para_iterator *it);

void para_iterator_zap(Para_iterator *it);

BOOL para_iterator_at_start(const Para_iterator *it);

BOOL para_from_into_bounded(Topform from, Topform into, BOOL check_top,
                            Para_iterator *it,
                            unsigned long long raw_budget,
                            unsigned long long yield_budget,
                            Topform_proc proc_proc,
                            unsigned long long *raw_steps,
                            unsigned long long *yielded);

Topform para_pos(Topform from_clause, Ilist from_pos,
		 Topform into_clause, Ilist into_pos);

Topform para_pos2(Topform from, Ilist from_pos, Topform into, Ilist into_pos);

#endif  /* conditional compilation of whole file */
