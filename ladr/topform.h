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

#ifndef TP_CLAUSE_H
#define TP_CLAUSE_H

#include "literals.h"
#include "attrib.h"
#include "formula.h"
#include "maximal.h"

/* INTRODUCTION
A Topform can be used to store a formula or a clause.
The field is_formula says which it is.

<p>
In earlier versions of LADR, this data structure was called Clause.
When we decided to put non-clausal formulas in proofs, they
needed to have IDs, attributes, and justifications, so we elevated
the data structure to include non-clausal formulas and changed
the name to Topform (top formula).

<p>
In many cases, when we say "clause", we mean a list of Literals.
For example, most of the functions that tell the properties
of clauses (positive_clause, number_of_literals, etc.) take
a list of Literals, not a Topform.

<p>
If C had data structures with inheritance, this would
be a good place to use it.
*/

/* Public definitions */

typedef struct topform * Topform;

struct topform {

  /* for both clauses and formulas */

  unsigned long long id;        /* clause identifier (0 = unassigned), 64-bit */
  struct clist_pos *containers;     /* Clists that contain the Topform */
  Attribute        attributes;
  struct just      *justification;
  double           weight;
  char             *compressed;     /* if nonNULL, a compressed form */
  Topform          matching_hint;   /* hint that matches clause, if any */
  unsigned long long last_matched_given;  /* given count at last hint match */

  /* A topform is either a clause or a formula, never both.  Keeping the
     alternatives in a union saves one pointer in every passive and hint. */
  union {
    Literals       literals;        /* NULL can mean the empty clause */
    Formula        formula;
  };

  unsigned         compressed_size; /* bytes in compressed payload */
  unsigned         uncompressed_body_bytes; /* logical body estimate */
  int              proof_tree_weight_cache;  /* memoized; -1 = unset */

  int   semantics;        /* evaluation in interpretations */
  unsigned simplifier_epoch; /* active state seen by a DISCOUNT passive */
  unsigned rewrite_epoch;    /* rewrite bank seen by a DISCOUNT passive */

  /* These flags used to occupy fourteen bytes. */
  unsigned is_formula          : 1; /* is this really a formula? */
  unsigned normal_vars         : 1; /* variables have been renumbered */
  unsigned used                : 1; /* used to infer a clause that was kept */
  unsigned official_id         : 1; /* Topform is in the ID table */
  unsigned initial             : 1; /* existed at the start of the search */
  unsigned neg_compressed      : 1; /* negative and compressed */
  unsigned subsumer            : 1; /* has this clause back subsumed anything? */
  unsigned was_given           : 1; /* was this clause selected as given? */
  unsigned goal_derived        : 1; /* descended from a denied goal */
  unsigned disabled            : 1; /* in the compact disabled-clause store */
  unsigned archive_materialized: 1; /* decoded from an ancestor record */
  unsigned cac_candidate       : 1; /* pending insertion in CAC trigger set */
  unsigned delayed_demodulator : 1; /* activate as demodulator when selected */
  unsigned rewrite_rule_dirty  : 1; /* targeted compact interreduction debt */
  unsigned packed_justification: 1; /* cold payload also owns justification */
  unsigned collective_history : 1; /* body is referenced by collective history */
  unsigned hint_indexed       : 1; /* live member of the authoritative hint index */

};

/* End of public definitions */

/* Public function prototypes from topform.c */

Topform get_topform(void);

void fprint_topform_mem(FILE *fp, int heading);

void p_topform_mem();

void zap_topform(Topform tf);

void fprint_clause(FILE *fp, Topform c);

void p_clause(Topform c);

Topform term_to_clause(Term t);

Topform term_to_topform(Term t, BOOL is_formula);

Term topform_to_term(Topform tf);

Term topform_to_term_without_attributes(Topform tf);

void clause_set_variables(Topform c, int max_vars);

void renumber_variables(Topform c, int max_vars);

void term_renumber_variables(Term t, int max_vars);

Plist renum_vars_map(Topform c);

void upward_clause_links(Topform c);

BOOL check_upward_clause_links(Topform c);

Topform copy_clause(Topform c);

Topform copy_clause_with_flags(Topform c);

Topform copy_clause_with_flag(Topform c, int flag);

void inherit_attributes(Topform par1, Context s1,
			Topform par2, Context s2,
			Topform child);

void gather_symbols_in_topform(Topform c, int *rcounts, int *fcounts);

void gather_symbols_in_topforms(Plist lst, int *rcounts, int *fcounts);

Ilist fsym_set_in_topforms(Plist lst);

Ilist rsym_set_in_topforms(Plist lst);

BOOL min_depth(Literals lit);

BOOL initial_clause(Topform c);

BOOL negative_clause_possibly_compressed(Topform c);

Term topform_properties(Topform c);

void append_label_attribute(Topform tf, char *s);

Ordertype cl_id_compare(Topform c1, Topform c2);

Ordertype cl_wt_id_compare(Topform c1, Topform c2);

Ordertype cl_hint_id_compare(Topform c1, Topform c2);

#endif  /* conditional compilation of whole file */
