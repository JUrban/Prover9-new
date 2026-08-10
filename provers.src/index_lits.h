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

#ifndef TP_INDEX_LITS_H
#define TP_INDEX_LITS_H

#include "../ladr/clock.h"
#include "../ladr/subsume.h"
#include "../ladr/di_tree.h"
#include "compact_term_pool.h"

/* INTRODUCTION
*/

/* Public definitions */

/* End of public definitions */

/* Public function prototypes from index_lits.c */

typedef Topform (*Compact_clause_resolver)(unsigned long long id,
                                           void *context);
typedef void (*Compact_clause_releaser)(Topform clause, void *context);

void configure_compact_clause_access(Compact_clause_resolver resolver,
                                     Compact_clause_releaser releaser,
                                     void *context);

void release_compact_index_clause(Topform clause);

unsigned long long compact_unit_active_count(void);

unsigned long long compact_unit_physical_count(void);

void compact_unit_compact_all_stale(void);

void compact_unit_copy_term_clauses(Compact_term_pool destination,
                                    Compact_term_rebase_map map);

void compact_unit_retain_term_clauses(Compact_term_rebase_map map);

void compact_unit_rebase_term_pool(Compact_term_pool pool,
                                   Compact_term_rebase_map map);

void init_literals_index(int depth);

void destroy_literals_index(void);

void index_literals(Topform c, Indexop op, Clock clock, BOOL no_fapl);

void index_denial(Topform c, Indexop op, Clock clock);

void unit_conflict(Topform c, void (*empty_proc) (Topform));

void unit_deletion(Topform c);

Plist back_unit_deletable(Topform c);

Topform forward_subsumption(Topform d);

Topform forward_subsumption_filter(Topform d,
                                   BOOL (*accept_cb)(Topform subsumer,
                                                     Topform new_clause,
                                                     void *arg),
                                   void *cb_arg);

Plist back_subsumption(Topform c);

void lits_idx_report(void);

void index_literals_fpa_only(Topform c, Indexop op, Clock clock, BOOL no_fapl);

void index_literals_features_only(Topform c, Indexop op, Clock clock);

void write_unit_discrim_index(const char *dir);

void restore_unit_discrim_index(const char *dir);

void write_fpa_lits_index(const char *dir);

BOOL restore_fpa_lits_index(const char *dir);

void configure_compact_unit_index(BOOL audit, BOOL authoritative);

void configure_compact_unit_term_pool(Compact_term_pool pool);

void configure_compact_unit_stale_pct(unsigned percentage);

void configure_compact_nonunit_index(BOOL audit, BOOL authoritative);

unsigned long long compact_unit_subsumption_audit_failures(void);

void fprint_compact_unit_index(FILE *fp);

void fprint_compact_nonunit_index(FILE *fp);

#endif  /* conditional compilation of whole file */
