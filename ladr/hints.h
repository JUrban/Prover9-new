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

#ifndef TP_HINTS_H
#define TP_HINTS_H

#include "subsume.h"
#include "clist.h"
#include "backdemod.h"
#include "resolve.h"
#include "hint_generalization_hash.h"

/* INTRODUCTION
*/

/* Public definitions */

/* End of public definitions */

/* Public function prototypes from hints.c */

void init_hints(Uniftype utype,
		int bsub_wt_attr,
		BOOL collect_labels,
		BOOL back_demod_hints,
		int fpa_depth,
		BOOL packed_index,
		BOOL better_packed_index,
		BOOL fast_packed_index,
		unsigned fast_cache_kb,
		unsigned conjunction_budget_kb,
		unsigned expected_hints,
		unsigned rebuild_scan_ratio,
		void (*demod_proc) (Topform, int, int, BOOL, BOOL));

/* Admit a packed-fast result only after at least this much posting work. */
void set_hint_cache_min_candidates(unsigned minimum);

/* Enable the static, deliberately incomplete hash-only matcher.  Every
   active hint receives an exact key; short hints are exhaustively
   generalized and long hints receive bounded one-subterm abstractions. */
void set_hint_generalization_hash(BOOL on, unsigned complete_nodes,
                                  unsigned partial_per_hint,
                                  unsigned long long maximum_entries,
                                  unsigned expected_hints);

BOOL preview_generalized_hash_unit_paramod(
  Literals from_lit, int from_side, Context from_subst,
  Literals into_lit, Ilist into_pos, Context into_subst,
  unsigned *normal_id, unsigned *flipped_id,
  unsigned *term_nodes, BOOL *reflexive,
  unsigned long long *probes);

BOOL preview_generalized_hash_unit_equality(
  Literals literal, unsigned *normal_id, unsigned *flipped_id,
  unsigned long long *probes);

void done_with_hints(void);

/* Complete the population-wide packed-fast conjunction plan after all input
   hints have been indexed and before candidate clauses are processed. */
void finalize_hint_conjunction_index(void);

int redundant_hints(void);

BOOL hint_is_redundant(Topform c);

BOOL hint_is_active(Topform c);

void index_hint_as_redundant(Topform c);

void index_hint(Topform c);

void unindex_hint(Topform c);

/* Terminal packed searches retain the hint Topforms for proof annotations,
   but no longer need any matching/back-demodulation index.  Discard the
   complete index in one pass, without replaying ordinary per-hint removal
   and its posting-maintenance policy. */
void discard_packed_hint_indexes(void);

void adjust_weight_with_hints(Topform c,
			      BOOL degrade,
			      BOOL breadth_first_hints);

/* Read-only counterpart of adjust_weight_with_hints().  The candidate is
   queried against the currently selected exact hint index, including the
   unoriented-unit flip fallback, but neither the candidate nor persistent
   hint state is changed.  raw_weight is the candidate's already-computed
   clause weight. */
Topform preview_weight_with_hints(Topform c,
				  double raw_weight,
				  BOOL degrade,
				  BOOL breadth_first_hints,
				  double *adjusted_weight,
				  BOOL *flipped);

void keep_hint_matcher(Topform c);

void back_demod_hints(Topform demod, int type, BOOL lex_order_vars);

void set_hints_given_count(unsigned long long n);

void set_hint_match_stats(BOOL on);

/* Enable the diagnostic-only Phase-0 census for a future compiled hint
   matcher.  Disabled production matching retains its existing hot path. */
void set_hint_compiled_census(BOOL on);

/* Construct the immutable/delta unit-hint term table while packed_fast
   remains authoritative. */
void set_hint_compiled_term_table(BOOL on);

/* Delay canonical term construction until a broad compiled prefilter first
   needs a retained unit hint.  Valid only for non-authoritative filtering. */
void set_hint_compiled_lazy(BOOL on);

/* Apply the compiled SAME program while packed candidates are emitted.
   Experimental; the compressed matcher remains final authority. */
void set_hint_compiled_fused(BOOL on);

void set_hint_compiled_paths(BOOL on);

void set_hint_compiled_min_candidates(unsigned minimum);

void set_hint_compiled_cache_build_factor(unsigned factor);

void set_hint_compiled_cache_kb(unsigned kb);

void set_hint_compiled_authoritative(BOOL on);

void set_hint_compiled_shadow(BOOL on);

void set_hint_compiled_filter(BOOL on);

void set_hint_match_once(BOOL on);

unsigned long long hint_state_epoch(void);

void set_hint_state_epoch(unsigned long long epoch);

int expire_old_hints(unsigned long long current_given,
		     unsigned long long expiry_distance,
		     int min_matches,
		     Clist hint_list);

int active_hints(void);

int matched_hints(Clist hint_list);

BOOL packed_hints_enabled(void);

/* Return the stable owner for a packed hint ID, including an expired hint
   that is no longer active in matching indexes.  NULL means packed mode is
   inactive or the ID has no packed owner. */
Topform packed_hint_by_id(unsigned long long id);

void packed_hint_index_stats(unsigned long long *node_bytes,
			     unsigned long long *reference_bytes,
			     unsigned long long *table_bytes,
			     unsigned long long *candidate_checks);

void fprint_packed_hint_operation_stats(FILE *fp);

void print_hint_match_stats(FILE *fp, Clist hint_list);

#endif  /* conditional compilation of whole file */
