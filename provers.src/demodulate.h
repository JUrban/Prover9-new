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

#ifndef TP_DEMODULATE_H
#define TP_DEMODULATE_H

#include "../ladr/ladr.h"
#include "compact_term_pool.h"

/* INTRODUCTION
*/

/* Public definitions */

/* End of public definitions */

/* Public function prototypes from demodulate.c */

void init_demodulator_index(Mindextype mtype, Uniftype utype, int fpa_depth);

void configure_compact_back_demod(BOOL audit, BOOL authoritative);

void configure_compact_back_demod_term_pool(Compact_term_pool pool);

void configure_compact_back_demod_stale_pct(unsigned percentage);

unsigned long long compact_back_demod_active_count(void);

unsigned long long compact_back_demod_physical_count(void);

void compact_back_demod_compact_all_stale_records(void);

void compact_back_demod_copy_term_clauses(Compact_term_pool destination,
                                          Compact_term_rebase_map map);

void compact_back_demod_retain_term_clauses(Compact_term_rebase_map map);

void compact_back_demod_rebase_shared_term_pool(
  Compact_term_pool pool, Compact_term_rebase_map map);

typedef Topform (*Compact_back_demod_resolver)(unsigned long long id,
                                               void *context);
typedef void (*Compact_back_demod_releaser)(Topform clause, void *context);
typedef void (*Compact_back_demod_batch_adviser)(
  const unsigned long long *ids, size_t count, void *context);

void configure_compact_back_demod_access(
  Compact_back_demod_resolver resolver,
  Compact_back_demod_releaser releaser,
  Compact_back_demod_batch_adviser adviser,
  void *context);

void init_back_demod_index(Mindextype mtype, Uniftype utype, int fpa_depth);

void index_demodulator(Topform c, int type, Indexop operation, Clock clock);

void index_back_demod(Topform c, Indexop operation, Clock clock, BOOL enabled);

void write_demod_index(const char *dir);

typedef Topform (*Demodulator_resolver)(unsigned long long id, void *context);

void restore_demod_index(const char *dir, Clock clock,
                         Demodulator_resolver resolver, void *context);

void destroy_demodulation_index(void);

void destroy_back_demod_index(void);

void demodulate_clause(Topform c, int step_limit, int increase_limit,
		       BOOL print, BOOL lex_order_vars);

/* Same rewrite result as demodulate_clause(), without diagnostics or global
   forward-demodulation attempt/rewrite accounting. */
void demodulate_clause_preview(Topform c, int step_limit, int increase_limit,
			       BOOL lex_order_vars);

Plist back_demodulatable(Topform demod, int type, BOOL lex_order_vars);

void back_demod_idx_report(void);

void fprint_compact_back_demod(FILE *fp);

void write_fpa_back_demod_index(const char *dir);

BOOL restore_fpa_back_demod_index(const char *dir);

#endif  /* conditional compilation of whole file */
