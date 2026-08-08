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

Topform paramodulate(Literals from_lit, int from_side, Context from_subst,
		     Topform into_clause, Ilist into_pos, Context into_subst);

void para_from_into(Topform from, Context cf,
		    Topform into, Context ci,
		    BOOL check_top,
		    void (*proc_proc) (Topform));

void para_iterator_init(Para_iterator *it);

void para_iterator_reset(Para_iterator *it);

void para_iterator_zap(Para_iterator *it);

BOOL para_iterator_at_start(const Para_iterator *it);

BOOL para_from_into_bounded(Topform from, Topform into, BOOL check_top,
			    Para_iterator *it,
			    unsigned long long raw_budget,
			    unsigned long long yield_budget,
			    void (*proc_proc) (Topform),
			    unsigned long long *raw_steps,
			    unsigned long long *yielded);

Topform para_pos(Topform from_clause, Ilist from_pos,
		 Topform into_clause, Ilist into_pos);

Topform para_pos2(Topform from, Ilist from_pos, Topform into, Ilist into_pos);

#endif  /* conditional compilation of whole file */
