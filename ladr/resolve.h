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

#ifndef TP_RESOLVE_H
#define TP_RESOLVE_H

#include "clash.h"
#include "lindex.h"

/* INTRODUCTION
*/

/* Public definitions */

enum {  /* literal selection */
  LIT_SELECTION_NONE,
  LIT_SELECTION_MAXIMAL,
  LIT_SELECTION_ALL
};

enum {  /* types of resolution (binary, hyper, UR) */
  POS_RES,  /* positive */
  NEG_RES,  /* negative */
  ANY_RES   /* unrestricted by sign */
};

/* End of public definitions */

/* Public function prototypes from resolve.c */


void resolution_options(BOOL ordered,
			BOOL check_instances,
			BOOL initial_nuclei,
			int ur_nucleus_limit,
			BOOL production_mode);

unsigned long long res_instance_prunes();

BOOL hyper_resolution(Topform c, int pos_or_neg, Lindex idx,
                      Topform_proc proc_proc);

BOOL hyper_resolution_with_clause_test(Topform c, int pos_or_neg, Lindex idx,
                                       Clash_clause_test clause_test,
                                       void *clause_test_data,
                                       Topform_proc proc_proc);

typedef unsigned long long (*Hyper_parent_count_proc)(void *);
typedef Topform (*Hyper_parent_clause_proc)(unsigned long long, void *);
typedef BOOL (*Hyper_parent_test_proc)(Topform, void *);

typedef struct hyper_parent_source {
  Hyper_parent_count_proc count;
  Hyper_parent_clause_proc clause;
  Hyper_parent_test_proc test;
  void *data;
} Hyper_parent_source;

typedef struct hyper_iterator_choice {
  unsigned long long parent_position;
  unsigned long long resume_parent;
  unsigned literal;
  unsigned phase;
  unsigned resume_literal;
  unsigned resume_phase;
} Hyper_iterator_choice;

typedef struct hyper_iterator {
  unsigned initialized;
  unsigned satellite_mode;
  unsigned complete;
  unsigned given_literal;
  unsigned given_phase;
  unsigned outer_literal;
  unsigned long long outer_parent;
  unsigned nucleus_selected;
  unsigned nucleus_literal;
  unsigned long long nucleus_parent;
  unsigned depth;
  unsigned choice_capacity;
  Hyper_iterator_choice *choices;
  unsigned mate_phase;
  unsigned mate_literal;
  unsigned long long mate_parent;
} Hyper_iterator;

void hyper_iterator_init(Hyper_iterator *it);

void hyper_iterator_reset(Hyper_iterator *it);

void hyper_iterator_zap(Hyper_iterator *it);

BOOL hyper_iterator_at_start(const Hyper_iterator *it);

BOOL hyper_resolution_bounded(
  Topform given, int pos_or_neg, const Hyper_parent_source *source,
  Hyper_iterator *it, unsigned long long raw_budget,
  unsigned long long yield_budget, Topform_proc proc_proc,
  unsigned long long *raw_steps, unsigned long long *yielded);

BOOL ur_resolution(Topform c, int target_constraint, Lindex idx,
                   Topform_proc proc_proc);

Topform instantiate_clause(Topform c, Context subst);

BOOL binary_resolution(Topform c,
                       int res_type,  /* POS_RES, NEG_RES, ANY_RES */
                       Lindex idx,
                       Topform_proc proc_proc);

BOOL binary_factors(Topform c, Topform_proc proc_proc);

void merge_literals(Topform c);

Topform copy_inference(Topform c);

Topform resolve2(Topform c1, int n1, Topform c2, int n2, BOOL renumber_vars);

Topform resolve3(Topform c1, Literals l1, Topform c2, Literals l2, BOOL renumber_vars);

Topform xx_resolve2(Topform c, int n, BOOL renumber_vars);

#endif  /* conditional compilation of whole file */
