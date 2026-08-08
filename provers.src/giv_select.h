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

#ifndef TP_GIV_SELECT_H
#define TP_GIV_SELECT_H

#include "search-structures.h"

/* INTRODUCTION
*/

/* Public definitions */

/* End of public definitions */

/* Public function prototypes from giv_select.c */

typedef size_t (*Dense_passive_archive_fn)(Topform c);
typedef Topform (*Dense_passive_activate_fn)(size_t store_position,
                                             unsigned long long id,
                                             unsigned long long hint_id);

/* Read-only metadata for one active dense passive.  Dense passives are
   visited in insertion (and therefore clause-ID) order. */
struct dense_passive_view {
  unsigned long long id;
  unsigned long long hint_id;
  size_t store_position;
  double weight;
  unsigned simplifier_epoch;
  int semantics;
  BOOL delayed_demodulator;
};

typedef void (*Dense_passive_visit_fn)(
  const struct dense_passive_view *view, void *context);

typedef size_t (*Dense_passive_relocate_fn)(size_t old_position,
                                            void *context);

void configure_dense_passive(BOOL enabled,
                             Dense_passive_archive_fn archive_fn,
                             Dense_passive_activate_fn activate_fn);

BOOL dense_passive_enabled(void);

int dense_passive_size(void);

BOOL dense_passive_contains_id(unsigned long long id);

void dense_passive_foreach(Dense_passive_visit_fn visit, void *context);

void dense_passive_memory(unsigned long long *record_bytes,
                          unsigned long long *heap_bytes,
                          unsigned long long *records);

unsigned long long dense_passive_delayed_demodulators(void);

BOOL dense_passive_compaction_needed(void);

void dense_passive_compact(Dense_passive_relocate_fn relocate,
                           void *context);

void dense_passive_compaction_stats(unsigned long long *compactions,
                                    unsigned long long *records_reclaimed);

void init_giv_select(Plist rules);

void insert_into_sos2(Topform c, Clist sos);

void remove_from_sos2(Topform c, Clist sos);

BOOL givens_available(void);

Topform get_given_clause2(Clist sos, int num_given,
			 Prover_options opt, char **type);

BOOL sos_keep2(Topform c, Clist sos, Prover_options opt);

void sos_displace2(void (*disable_proc) (Topform), BOOL quiet);

void reset_selector_indexes(void);

void zap_given_selectors(void);

void selector_report(void);

void get_low_selector_state(const char **name, int *count);

void set_low_selector_state(const char *name, int count);

void get_high_selector_state(const char **name, int *count);

void set_high_selector_state(const char *name, int count);

/* Read-only selector classification for a scratch, normalized candidate.
   The bit layout is identical to dense passive selector masks.  Priority is
   0 for a high-priority match, 1 for a low-priority match, and 2 when no
   selector matches. */
void given_selection_preview(Topform c,
			     unsigned long long *selector_mask,
			     unsigned *priority);

Term selector_rule_term(char *name, char *priority,
			char *order, char *rule, int part);

Plist selector_rules_from_options(Prover_options opt);

void bulk_insert_into_sos2(Clist sos);

#endif  /* conditional compilation of whole file */
