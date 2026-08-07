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

#include "hints.h"
#include "compress.h"

/* Private definitions and types */

static Lindex Hints_idx = NULL;       /* FPA index for hints */
static Clist Redundant_hints = NULL;  /* list of hints not indexed */
static Mindex Back_demod_idx;        /* to index hints for back demodulation */
static int Bsub_wt_attr;
static BOOL Back_demod_hints;
static BOOL Collect_labels;

/* The packed hint bank keeps sound path/symbol feature bitsets and 32-bit
   hint IDs, never pointers into hint term trees.  Exact subsumption is still
   the final test, so hash collisions only cost time.  Active hint bodies can
   therefore remain compressed and are materialized only for candidates. */

static BOOL Packed_index = FALSE;
static unsigned Packed_feature_counts[2][64];
static unsigned long long *Packed_feature_bitsets = NULL;
static unsigned Packed_feature_words = 0;
static Topform *Packed_hint_by_id = NULL;
static unsigned char *Packed_hint_active = NULL;
static unsigned char *Packed_hint_anyconst = NULL;
static unsigned long long *Packed_hint_rewrite_symbols = NULL;
static unsigned long long *Packed_hint_pos_features = NULL;
static unsigned long long *Packed_hint_neg_features = NULL;
static unsigned *Packed_candidate_mark = NULL;
static unsigned Packed_hint_capacity = 0;
static unsigned Packed_candidate_serial = 1;
static unsigned *Packed_candidates = NULL;
static unsigned Packed_candidates_count = 0, Packed_candidates_capacity = 0;
static unsigned long long Packed_candidate_checks = 0;

static void packed_reserve_hints(unsigned id)
{
  if (id >= Packed_hint_capacity) {
    unsigned old = Packed_hint_capacity;
    unsigned cap = old == 0 ? 1024 : old;
    unsigned old_words = Packed_feature_words;
    unsigned new_words;
    unsigned row;
    while (cap <= id)
      cap *= 2;
    new_words = (cap + 63) / 64;
    {
      unsigned long long *bits = safe_calloc(
        (size_t) 128 * new_words, sizeof(unsigned long long));
      if (Packed_feature_bitsets != NULL) {
        for (row = 0; row < 128; row++)
          memcpy(bits + (size_t) row * new_words,
                 Packed_feature_bitsets + (size_t) row * old_words,
                 (size_t) old_words * sizeof(unsigned long long));
        safe_free(Packed_feature_bitsets);
      }
      Packed_feature_bitsets = bits;
      Packed_feature_words = new_words;
    }
    Packed_hint_by_id = safe_realloc(Packed_hint_by_id,
                                     (size_t) cap * sizeof(Topform));
    Packed_hint_active = safe_realloc(Packed_hint_active, cap);
    Packed_hint_anyconst = safe_realloc(Packed_hint_anyconst, cap);
    Packed_hint_rewrite_symbols = safe_realloc(
      Packed_hint_rewrite_symbols,
      (size_t) cap * sizeof(unsigned long long));
    Packed_hint_pos_features = safe_realloc(
      Packed_hint_pos_features, (size_t) cap * sizeof(unsigned long long));
    Packed_hint_neg_features = safe_realloc(
      Packed_hint_neg_features, (size_t) cap * sizeof(unsigned long long));
    Packed_candidate_mark = safe_realloc(Packed_candidate_mark,
                                         (size_t) cap * sizeof(unsigned));
    memset(Packed_hint_by_id + old, 0,
           (size_t) (cap - old) * sizeof(Topform));
    memset(Packed_hint_active + old, 0, cap - old);
    memset(Packed_hint_anyconst + old, 0, cap - old);
    memset(Packed_hint_rewrite_symbols + old, 0,
           (size_t) (cap - old) * sizeof(unsigned long long));
    memset(Packed_hint_pos_features + old, 0,
           (size_t) (cap - old) * sizeof(unsigned long long));
    memset(Packed_hint_neg_features + old, 0,
           (size_t) (cap - old) * sizeof(unsigned long long));
    memset(Packed_candidate_mark + old, 0,
           (size_t) (cap - old) * sizeof(unsigned));
    Packed_hint_capacity = cap;
  }
}

static BOOL packed_term_has_theory_symbol(Term t);

static unsigned long long packed_rewrite_symbol_bits(Term t)
{
  int i;
  unsigned long long bits;
  if (VARIABLE(t))
    return 0;
  bits = 1ULL << (((unsigned) SYMNUM(t) * 2654435761U) >> 26);
  for (i = 0; i < ARITY(t); i++)
    bits |= packed_rewrite_symbol_bits(ARG(t,i));
  return bits;
}

static unsigned packed_feature_bit(int sn, unsigned path)
{
  unsigned x = (unsigned) sn * 2654435761U;
  x ^= path * 2246822519U;
  x ^= x >> 16;
  return x >> 26;
}

static unsigned long long packed_term_feature_mask_rec(Term t, unsigned path)
{
  unsigned long long mask;
  int i;
  if (VARIABLE(t))
    return 0;
  mask = 1ULL << packed_feature_bit(SYMNUM(t), path);
  for (i = 0; i < ARITY(t); i++)
    mask |= packed_term_feature_mask_rec(ARG(t,i), path * 33U + (unsigned)i + 1);
  return mask;
}

static unsigned long long packed_term_feature_mask(Term t, BOOL query)
{
  if (query && packed_term_has_theory_symbol(t))
    return VARIABLE(t) ? 0 :
      1ULL << packed_feature_bit(SYMNUM(t), 1);
  return packed_term_feature_mask_rec(t, 1);
}

static void packed_add_feature_ref(int sign, unsigned bit, unsigned id)
{
  unsigned row = (unsigned) sign * 64 + bit;
  unsigned long long *word = Packed_feature_bitsets +
    (size_t) row * Packed_feature_words + id / 64;
  unsigned long long flag = 1ULL << (id % 64);
  if ((*word & flag) == 0) {
    *word |= flag;
    Packed_feature_counts[sign][bit]++;
  }
}

static void packed_index_hint_terms(Topform h, BOOL anyconst)
{
  Literals lit;
  unsigned id = (unsigned) h->id;
  unsigned bit;
  packed_reserve_hints(id);
  Packed_hint_by_id[id] = h;
  Packed_hint_active[id] = 1;
  if (anyconst)
    Packed_hint_anyconst[id] = 1;
  for (lit = h->literals; lit != NULL; lit = lit->next) {
    int i;
    unsigned long long mask = packed_term_feature_mask(lit->atom, FALSE);
    if (lit->sign)
      Packed_hint_pos_features[id] |= mask;
    else
      Packed_hint_neg_features[id] |= mask;
    if (Back_demod_hints && !anyconst) {
      for (i = 0; i < ARITY(lit->atom); i++)
        Packed_hint_rewrite_symbols[id] |=
          packed_rewrite_symbol_bits(ARG(lit->atom,i));
    }
  }
  for (bit = 0; bit < 64; bit++) {
    unsigned long long b = 1ULL << bit;
    if (Packed_hint_pos_features[id] & b)
      packed_add_feature_ref(1, bit, id);
    if (Packed_hint_neg_features[id] & b)
      packed_add_feature_ref(0, bit, id);
  }
}

static void packed_add_candidate(unsigned id)
{
  if (id == 0 || id >= Packed_hint_capacity ||
      !Packed_hint_active[id] || Packed_candidate_mark[id] == Packed_candidate_serial)
    return;
  Packed_candidate_mark[id] = Packed_candidate_serial;
  if (Packed_candidates_count == Packed_candidates_capacity) {
    unsigned cap = Packed_candidates_capacity == 0 ? 1024 :
                                                   Packed_candidates_capacity * 2;
    Packed_candidates = safe_realloc(Packed_candidates,
                                     (size_t) cap * sizeof(unsigned));
    Packed_candidates_capacity = cap;
  }
  Packed_candidates[Packed_candidates_count++] = id;
}

static BOOL packed_term_has_theory_symbol(Term t)
{
  int i;
  if (!VARIABLE(t) && (is_assoc_comm(SYMNUM(t)) || is_commutative(SYMNUM(t))))
    return TRUE;
  for (i = 0; i < ARITY(t); i++) {
    if (packed_term_has_theory_symbol(ARG(t,i)))
      return TRUE;
  }
  return FALSE;
}

static void packed_begin_candidates(void)
{
  Packed_candidates_count = 0;
  Packed_candidate_serial++;
  if (Packed_candidate_serial == 0) {
    memset(Packed_candidate_mark, 0,
           (size_t) Packed_hint_capacity * sizeof(unsigned));
    Packed_candidate_serial = 1;
  }
}

static void packed_collect_term_candidates(Term t, int sign)
{
  unsigned long long mask = packed_term_feature_mask(t, TRUE);
  unsigned bit, best = 0;
  unsigned best_count = UINT_MAX;
  unsigned word;
  if (mask == 0) {
    unsigned id;
    for (id = 1; id < Packed_hint_capacity; id++)
      packed_add_candidate(id);
    return;
  }
  for (bit = 0; bit < 64; bit++) {
    if ((mask & (1ULL << bit)) &&
        Packed_feature_counts[sign][bit] < best_count) {
      best = bit;
      best_count = Packed_feature_counts[sign][bit];
    }
  }
  for (word = 0; word < Packed_feature_words; word++) {
    unsigned long long bits = Packed_feature_bitsets[
      ((size_t) sign * 64 + best) * Packed_feature_words + word];
    while (bits != 0) {
      unsigned offset = (unsigned) __builtin_ctzll(bits);
      unsigned id = word * 64 + offset;
      unsigned long long stored = sign ? Packed_hint_pos_features[id] :
                                         Packed_hint_neg_features[id];
      if (id < Packed_hint_capacity && (stored & mask) == mask)
        packed_add_candidate(id);
      bits &= bits - 1;
    }
  }
}

static int packed_id_decreasing(const void *a, const void *b)
{
  unsigned x = *(const unsigned *) a;
  unsigned y = *(const unsigned *) b;
  return x < y ? 1 : x > y ? -1 : 0;
}

static void packed_finish_candidates(BOOL include_anyconst)
{
  unsigned id;
  if (include_anyconst) {
    for (id = 1; id < Packed_hint_capacity; id++) {
      if (Packed_hint_anyconst[id])
        packed_add_candidate(id);
    }
  }
  qsort(Packed_candidates, Packed_candidates_count, sizeof(unsigned),
        packed_id_decreasing);
}

/* pointer to procedure for demodulating hints (when back demod hints) */

static void (*Demod_proc) (Topform, int, int, BOOL, BOOL);

/* stats */

static int Hint_id_count = 0;
static int Active_hints_count = 0;
static int Redundant_hints_count = 0;

/* given-clause counter for hint expiry */

static unsigned long long Current_given_for_hints = 0;
static unsigned long long Hint_state_epoch = 1;

static
void advance_hint_epoch(void)
{
  if (Hint_state_epoch != ULLONG_MAX)
    Hint_state_epoch++;
}

static void discard_packed_hint_proof(Topform c)
{
  /* Hints influence search only through their body, attributes, ID and match
     counters.  They are not proof ancestors.  Back-demodulation can start
     from a NULL list (append_just handles it), and re-indexing drops the
     transient demodulation proof again. */
  if (Packed_index && c->justification != NULL) {
    zap_just(c->justification);
    c->justification = NULL;
  }
}

/* Hint match stats: optional delta histogram + end-of-search summary.
   Controlled by set_hint_match_stats(TRUE). */

static BOOL Hint_match_stats = FALSE;
static BOOL Hint_match_once = FALSE;

/* Re-match delta histogram: delta = current_given - last_matched_given.
   Only recorded on 2nd+ match (weight > 0 before increment). */

#define DELTA_BUCKETS 16
static int Delta_bucket[DELTA_BUCKETS];  /* initialized to 0 */
static int Delta_total = 0;
static unsigned long long Delta_min = 0;
static unsigned long long Delta_max = 0;
static double Delta_sum = 0;

/*************
 *
 *   init_hints()
 *
 *************/

/* DOCUMENTATION
*/

/* PUBLIC */
void init_hints(Uniftype utype,
		int bsub_wt_attr,
		BOOL collect_labels,
		BOOL back_demod_hints,
		int fpa_depth,
		BOOL packed_index,
		void (*demod_proc) (Topform, int, int, BOOL, BOOL))
{
  Bsub_wt_attr = bsub_wt_attr;
  Collect_labels = collect_labels;
  Back_demod_hints = back_demod_hints;
  Packed_index = packed_index;
  Demod_proc = demod_proc;
  /* Keep an empty Lindex in packed mode so the established lifecycle and
     checkpoint code can use the same ownership boundary. */
  Hints_idx = lindex_init(FPA, utype, packed_index ? 1 : fpa_depth,
                          FPA, utype, packed_index ? 1 : fpa_depth);
  if (Back_demod_hints && !Packed_index)
    Back_demod_idx = mindex_init(FPA, utype, fpa_depth);
  Redundant_hints = clist_init("redundant_hints");
}  /* init_hints */

/*************
 *
 *   done_with_hints()
 *
 *************/

/* DOCUMENTATION
*/

/* PUBLIC */
void done_with_hints(void)
{
  if (!lindex_empty(Hints_idx) ||
      !clist_empty(Redundant_hints))
    printf("ERROR: Hints index not empty!\n");
  lindex_destroy(Hints_idx);
  if (Back_demod_hints && !Packed_index)
    mindex_destroy(Back_demod_idx);
  Hints_idx = NULL;
  clist_free(Redundant_hints);
  Redundant_hints = NULL;
  if (Packed_hint_by_id) safe_free(Packed_hint_by_id);
  if (Packed_hint_active) safe_free(Packed_hint_active);
  if (Packed_hint_anyconst) safe_free(Packed_hint_anyconst);
  if (Packed_hint_rewrite_symbols) safe_free(Packed_hint_rewrite_symbols);
  if (Packed_hint_pos_features) safe_free(Packed_hint_pos_features);
  if (Packed_hint_neg_features) safe_free(Packed_hint_neg_features);
  if (Packed_feature_bitsets) safe_free(Packed_feature_bitsets);
  if (Packed_candidate_mark) safe_free(Packed_candidate_mark);
  if (Packed_candidates) safe_free(Packed_candidates);
  Packed_hint_by_id = NULL; Packed_hint_active = NULL;
  Packed_hint_anyconst = NULL; Packed_candidate_mark = NULL;
  Packed_hint_rewrite_symbols = NULL;
  Packed_hint_pos_features = Packed_hint_neg_features = NULL;
  Packed_feature_bitsets = NULL;
  Packed_feature_words = 0;
  Packed_candidates = NULL;
  Packed_hint_capacity = 0;
  Packed_candidates_count = Packed_candidates_capacity = 0;
  memset(Packed_feature_counts, 0, sizeof(Packed_feature_counts));
  Packed_candidate_checks = 0;
  Packed_index = FALSE;
}  /* done_with_hints */

/*************
 *
 *   redundant_hints()
 *
 *************/

/* DOCUMENTATION
*/

/* PUBLIC */
int redundant_hints(void)
{
  return clist_length(Redundant_hints);
}  /* redundant_hints */

/*************
 *
 *   hint_is_redundant()
 *
 *************/

/* DOCUMENTATION
Check if a hint is in the redundant hints list.
*/

/* PUBLIC */
BOOL hint_is_redundant(Topform c)
{
  return clist_member(c, Redundant_hints);
}  /* hint_is_redundant */

/*************
 *
 *   index_hint_as_redundant()
 *
 *************/

/* DOCUMENTATION
Mark a hint as redundant without checking subsumption.
Used during checkpoint resume to preserve the original
redundant/active partition.
*/

/* PUBLIC */
void index_hint_as_redundant(Topform c)
{
  c->weight = 0;
  clist_append(c, Redundant_hints);
  Redundant_hints_count++;
  advance_hint_epoch();
  if (compress_clause(c) == CLAUSE_COMPRESS_INVALID)
    fatal_error("index_hint_as_redundant: cannot compact hint");
  discard_packed_hint_proof(c);
}  /* index_hint_as_redundant */

/*************
 *
 *   find_equivalent_hint()
 *
 *************/

static
Topform find_equivalent_hint(Topform c, Lindex idx)
{
  Topform equiv_hint = NULL;
  Plist subsumees = back_subsume(c, idx);
  Plist p;
  for (p = subsumees; p && equiv_hint == NULL; p = p->next) {
    if (subsumes(p->v, c))
      equiv_hint = p->v;
  }
  zap_plist(subsumees);
  return equiv_hint;
}  /* find_equivalent_hint */

/*************
 *
 *   find_matching_hint()
 *
 *   Return the first equivalent hint;  if none, return the last
 *   subsumed hint.
 *
 *   "First" and "last" refer to the order returned by the index,
 *   which is not necessarily the order in which the hints were
 *   inserted into the index.  In fact, it is likely that the
 *   clauses are returned in the reverse order.
 *
 *************/

static
Topform find_matching_hint(Topform c, Lindex idx)
{
  Topform hint = NULL;
  Plist subsumees = back_subsume(c, idx);
  Plist p;
  BOOL equivalent = FALSE;
  for (p = subsumees; p && !equivalent; p = p->next) {
    /* printf("subsumee: "); f_clause(p->v); */
    hint = p->v;
    if (subsumes(p->v, c))
      equivalent = TRUE;
  }
  zap_plist(subsumees);
  return hint;
}  /* find_matching_hint */

static void packed_collect_clause_candidates(Topform c)
{
  Literals first = c->literals;
  packed_begin_candidates();
  if (first != NULL)
    packed_collect_term_candidates(first->atom, first->sign ? 1 : 0);
  packed_finish_candidates(MATCH_HINTS_ANYCONST);
}

static Topform packed_find_equivalent_hint(Topform c)
{
  unsigned i;
  int nc = number_of_literals(c->literals);
  packed_collect_clause_candidates(c);
  for (i = 0; i < Packed_candidates_count; i++) {
    Topform h = Packed_hint_by_id[Packed_candidates[i]];
    BOOL was_compressed;
    BOOL c_sub_h, h_sub_c;
    if (h == NULL || h == c)
      continue;
    was_compressed = h->compressed != NULL;
    if (was_compressed && !materialize_clause(h))
      fatal_error("packed_find_equivalent_hint: invalid packed hint");
    Packed_candidate_checks++;
    c_sub_h = nc <= number_of_literals(h->literals) && subsumes(c, h);
    h_sub_c = c_sub_h && subsumes(h, c);
    if (was_compressed && !recompress_clause(h))
      fatal_error("packed_find_equivalent_hint: cannot recompress hint");
    if (h_sub_c)
      return h;
  }
  return NULL;
}

static Topform packed_find_matching_hint(Topform c)
{
  unsigned i;
  int nc = number_of_literals(c->literals);
  Topform match_hint = NULL;
  packed_collect_clause_candidates(c);
  /* Legacy back_subsume() returns hints in decreasing clause-ID order.
     It chooses the first equivalent hint, or the last proper subsumee.
     Preserve that tie-breaking exactly, independent of trie traversal. */
  for (i = 0; i < Packed_candidates_count; i++) {
    Topform h = Packed_hint_by_id[Packed_candidates[i]];
    BOOL was_compressed;
    BOOL c_sub_h, equivalent;
    if (h == NULL || h == c)
      continue;
    was_compressed = h->compressed != NULL;
    if (was_compressed && !materialize_clause(h))
      fatal_error("packed_find_matching_hint: invalid packed hint");
    Packed_candidate_checks++;
    c_sub_h = nc <= number_of_literals(h->literals) && subsumes(c, h);
    equivalent = c_sub_h && subsumes(h, c);
    if (c_sub_h)
      match_hint = h;
    if (was_compressed && !recompress_clause(h))
      fatal_error("packed_find_matching_hint: cannot recompress hint");
    if (equivalent)
      break;
  }
  return match_hint;
}

/*************
 *
 *   index_hint()
 *
 *************/

/* DOCUMENTATION
Index a clause C as a hint (make sure to call init_hints first).
If the clause is equivalent to a previously indexed hint H, any
labels on C are copied to H, and C is not indexed.
*/

/* PUBLIC */
/*************
 *
 *   hint_contains_anyconst() -- check if a hint has generic _AnyConst
 *
 *************/

static BOOL hint_contains_anyconst(Topform c)
{
  Literals lit;
  int sn = any_const_sn(0);  /* symnum for _AnyConst */
  for (lit = c->literals; lit; lit = lit->next) {
    if (lit->atom != NULL && symbol_in_term(sn, lit->atom))
      return TRUE;
  }
  return FALSE;
}  /* hint_contains_anyconst */

void index_hint(Topform c)
{
  Topform h;

  /* Disable _AnyConst matching during redundancy check so that
     hints with _AnyConst are not marked redundant vs concrete hints. */
  AnyConstsEnabled = FALSE;
  h = Packed_index ? packed_find_equivalent_hint(c) :
                     find_equivalent_hint(c, Hints_idx);
  AnyConstsEnabled = TRUE;

  c->weight = 0;  /* this is used in hints degradation to count matches */
  if (h != NULL) {
    /* copy any bsub_hint_wt attrs from rundundant hint to the indexed hint */
    h->attributes = copy_int_attribute(c->attributes, h->attributes,
				       Bsub_wt_attr);
    if (Collect_labels) {
      /* copy any labels from rundundant hint to the indexed hint */
      h->attributes = copy_string_attribute(c->attributes, h->attributes,
					    label_att());
    }
    clist_append(c, Redundant_hints);
    Redundant_hints_count++;
    advance_hint_epoch();
    if (compress_clause(c) == CLAUSE_COMPRESS_INVALID)
      fatal_error("index_hint: cannot compact redundant hint");
    /*
    printf("redundant hint: "); f_clause(c);
    printf("      original: "); f_clause(h);
    */
  }
  else {
    Active_hints_count++;
    advance_hint_epoch();
    Hint_id_count++;
    /* Keep the original id on re-index (back-demod).  Reassigning the
       id would change the hint_age key used by AVL trees in the
       hint_age given selection rule, making avl_delete unable to find
       clauses that were inserted under the old id.  (BV 2016-jun-17) */
    if (c->id == 0)
      c->id = Hint_id_count;
    if (Packed_index) {
      BOOL anyconst = MATCH_HINTS_ANYCONST && hint_contains_anyconst(c);
      if (c->id > UINT_MAX)
        fatal_error("index_hint: packed hint ID overflow");
      packed_index_hint_terms(c, anyconst);
      if (compress_clause(c) == CLAUSE_COMPRESS_INVALID)
        fatal_error("index_hint: cannot compact active packed hint");
    }
    else
      lindex_update(Hints_idx, c, INSERT);
    if (Back_demod_hints && !Packed_index) {
      /* Do not index hints containing generic _AnyConst for back-demod.
         Back-demodulating _AnyConst would be unsound. */
      if (MATCH_HINTS_ANYCONST && hint_contains_anyconst(c))
        ;  /* skip back-demod indexing */
      else
        index_clause_back_demod(c, Back_demod_idx, INSERT);
    }
  }
  discard_packed_hint_proof(c);
}  /* index_hint */

/*************
 *
 *   unindex_hint()
 *
 *************/

/* DOCUMENTATION
*/

/* PUBLIC */
void unindex_hint(Topform c)
{
  if (clist_member(c, Redundant_hints)) {
    clist_remove(c, Redundant_hints);
    Redundant_hints_count--;
  }
  else {
    if (Packed_index) {
      if (c->id < Packed_hint_capacity)
        Packed_hint_active[c->id] = 0;
    }
    else
      lindex_update(Hints_idx, c, DELETE);
    if (Back_demod_hints && !Packed_index) {
      if (!(MATCH_HINTS_ANYCONST && hint_contains_anyconst(c)))
        index_clause_back_demod(c, Back_demod_idx, DELETE);
    }
    Active_hints_count--;
  }
  advance_hint_epoch();
}  /* unindex_hint */

/*************
 *
 *   adjust_weight_with_hints()
 *
 *************/

/* DOCUMENTATION
*/

/* PUBLIC */
void adjust_weight_with_hints(Topform c,
			      BOOL degrade,
			      BOOL breadth_first_hints)
{
  Topform hint = Packed_index ? packed_find_matching_hint(c) :
                                find_matching_hint(c, Hints_idx);

  if (hint == NULL &&
      unit_clause(c->literals) &&
      eq_term(c->literals->atom) &&
      !oriented_eq(c->literals->atom)) {

    /* Try to find a hint that matches the flipped equality. */

    Term save_atom = c->literals->atom;
    c->literals->atom = top_flip(save_atom);
    hint = Packed_index ? packed_find_matching_hint(c) :
                          find_matching_hint(c, Hints_idx);
    zap_top_flip(c->literals->atom);
    c->literals->atom = save_atom;
    if (hint != NULL)
      c->attributes = set_string_attribute(c->attributes, label_att(),
					   "flip_matches_hint");
  }

  if (hint != NULL) {

    int bsub_wt = get_int_attribute(hint->attributes, Bsub_wt_attr, 1);

    if (bsub_wt != INT_MAX)
      c->weight = bsub_wt;
    else if (breadth_first_hints)
      c->weight = 0;

    /* If the hint has label attributes, copy them to the clause. */
    
    {
      int i = 0;
      char *s = get_string_attribute(hint->attributes, label_att(), ++i);
      while (s) {
	if (!string_attribute_member(c->attributes, label_att(), s))
	  c->attributes = set_string_attribute(c->attributes, label_att(), s);
	s = get_string_attribute(hint->attributes, label_att(), ++i);
      }
    }

    /* Veroff's hint degradation strategy. */

    if (degrade) {
      /* add 1000 for each previous match */
      c->weight = c->weight + hint->weight * 1000;
    }
    c->matching_hint = hint;
    /* If/when c is eventually kept, the hint will have its weight
       field incremented in case hint degradation is being used. */
  }
}  /* adjust_weight_with_hints */

/*************
 *
 *   keep_hint_matcher()
 *
 *************/

/* DOCUMENTATION
*/

/* PUBLIC */
void keep_hint_matcher(Topform c)
{
  Topform hint = c->matching_hint;

  /* Record re-match delta (only when hint was previously matched) */
  if (Hint_match_stats && hint->weight > 0 && hint->last_matched_given > 0) {
    unsigned long long delta = Current_given_for_hints - hint->last_matched_given;
    int bucket;

    if (delta <= 500)       bucket = 0;
    else if (delta <= 1000) bucket = 1;
    else if (delta <= 1500) bucket = 2;
    else if (delta <= 2000) bucket = 3;
    else if (delta <= 2500) bucket = 4;
    else if (delta <= 3000) bucket = 5;
    else if (delta <= 3500) bucket = 6;
    else if (delta <= 4000) bucket = 7;
    else if (delta <= 4500) bucket = 8;
    else if (delta <= 5000) bucket = 9;
    else if (delta <= 5500) bucket = 10;
    else if (delta <= 6000) bucket = 11;
    else if (delta <= 6500) bucket = 12;
    else if (delta <= 7000) bucket = 13;
    else if (delta <= 7500) bucket = 14;
    else                    bucket = 15;

    Delta_bucket[bucket]++;
    Delta_total++;
    Delta_sum += (double) delta;
    if (Delta_total == 1 || delta < Delta_min) Delta_min = delta;
    if (delta > Delta_max) Delta_max = delta;
  }

  hint->weight++;
  hint->last_matched_given = Current_given_for_hints;

  if (Hint_match_once) {
    /* Remove from index immediately so it can't match again.
       The hint struct stays alive (kept clauses hold matching_hint
       pointers).  It remains in the hints clist for stats. */
    if (Packed_index) {
      if (hint->id < Packed_hint_capacity)
        Packed_hint_active[hint->id] = 0;
    }
    else
      lindex_update(Hints_idx, hint, DELETE);
    if (Back_demod_hints && !Packed_index) {
      if (!(MATCH_HINTS_ANYCONST && hint_contains_anyconst(hint)))
        index_clause_back_demod(hint, Back_demod_idx, DELETE);
    }
    Active_hints_count--;
    advance_hint_epoch();
    if (compress_clause(hint) == CLAUSE_COMPRESS_INVALID)
      fatal_error("keep_hint_matcher: cannot compact retired hint");
  }
}  /* keep_hint_matcher */

/*************
 *
 *   back_demod_hints()
 *
 *************/

/* DOCUMENTATION
*/

/* PUBLIC */
void back_demod_hints(Topform demod, int type, BOOL lex_order_vars)
{
  if (Back_demod_hints && Packed_index) {
    Term atom = demod->literals->atom;
    Term alpha = ARG(atom,0);
    Term beta = ARG(atom,1);
    unsigned i, candidate_count;
    unsigned *candidate_ids;
    unsigned long long wanted = 0;
    packed_begin_candidates();
    if (type == ORIENTED || type == LEX_DEP_LR || type == LEX_DEP_BOTH) {
      if (VARIABLE(alpha))
        wanted = ULLONG_MAX;
      else
        wanted |= 1ULL << (((unsigned) SYMNUM(alpha) * 2654435761U) >> 26);
    }
    if (type == LEX_DEP_RL || type == LEX_DEP_BOTH) {
      if (VARIABLE(beta))
        wanted = ULLONG_MAX;
      else
        wanted |= 1ULL << (((unsigned) SYMNUM(beta) * 2654435761U) >> 26);
    }
    for (i = 1; i < Packed_hint_capacity; i++) {
      if (Packed_hint_active[i] &&
          (wanted == ULLONG_MAX ||
           (Packed_hint_rewrite_symbols[i] & wanted) != 0))
        packed_add_candidate(i);
    }
    packed_finish_candidates(FALSE);
    candidate_count = Packed_candidates_count;
    candidate_ids = candidate_count == 0 ? NULL :
      safe_malloc((size_t) candidate_count * sizeof(unsigned));
    if (candidate_count != 0)
      memcpy(candidate_ids, Packed_candidates,
             (size_t) candidate_count * sizeof(unsigned));
    for (i = 0; i < candidate_count; i++) {
      unsigned id = candidate_ids[i];
      Topform hint = Packed_hint_by_id[id];
      Topform before;
      BOOL changed;
      if (hint == NULL || !Packed_hint_active[id] ||
          (MATCH_HINTS_ANYCONST && Packed_hint_anyconst[id]))
        continue;
      if (hint->compressed != NULL && !materialize_clause(hint))
        fatal_error("back_demod_hints: invalid packed hint");
      if (!rewritable_clause_type(demod, hint, type, lex_order_vars)) {
        if (compress_clause(hint) == CLAUSE_COMPRESS_INVALID)
          fatal_error("back_demod_hints: cannot recompress filtered hint");
        continue;
      }
      before = copy_clause(hint);
      (*Demod_proc)(hint, 1000, 1000, FALSE, lex_order_vars);
      changed = !clause_ident(before->literals, hint->literals);
      zap_topform(before);
      if (changed) {
        unindex_hint(hint);
        orient_equalities(hint, TRUE);
        simplify_literals2(hint);
        merge_literals(hint);
        renumber_variables(hint, MAX_VARS);
        index_hint(hint);
        hint->weight = 0;
      }
      else if (compress_clause(hint) == CLAUSE_COMPRESS_INVALID)
        fatal_error("back_demod_hints: cannot recompress packed hint");
      discard_packed_hint_proof(hint);
    }
    if (candidate_ids != NULL)
      safe_free(candidate_ids);
  }
  else if (Back_demod_hints) {
    Plist rewritables = back_demod_indexed(demod, type, Back_demod_idx,
					   lex_order_vars);
    Plist p, prev;
    for (prev = NULL, p = rewritables; p; p = p->next) {
      Topform hint = p->v;
      if (prev) free_plist(prev);
      /* printf("\nBEFORE: "); f_clause(hint); */
      unindex_hint(hint);
      (*Demod_proc)(hint, 1000, 1000, FALSE, lex_order_vars);

      orient_equalities(hint, TRUE);
      simplify_literals2(hint);
      merge_literals(hint);
      renumber_variables(hint, MAX_VARS);

      /* printf("AFTER : "); f_clause(hint); */
      index_hint(hint);
      hint->weight = 0;  /* reset count of number of matches */
      prev = p;
    }
    if (prev) free_plist(prev);
  }
}  /* back_demod_hints */

/*************
 *
 *   set_hints_given_count()
 *
 *************/

/* PUBLIC */
void set_hints_given_count(unsigned long long n)
{
  Current_given_for_hints = n;
}  /* set_hints_given_count */

/*************
 *
 *   set_hint_match_stats()
 *
 *************/

/* PUBLIC */
void set_hint_match_stats(BOOL on)
{
  Hint_match_stats = on;
}  /* set_hint_match_stats */

/*************
 *
 *   set_hint_match_once()
 *
 *************/

/* PUBLIC */
void set_hint_match_once(BOOL on)
{
  Hint_match_once = on;
}  /* set_hint_match_once */

/* PUBLIC */
unsigned long long hint_state_epoch(void)
{
  return Hint_state_epoch;
}

/* PUBLIC */
void set_hint_state_epoch(unsigned long long epoch)
{
  Hint_state_epoch = epoch == 0 ? 1 : epoch;
}

/*************
 *
 *   expire_old_hints()
 *
 *   Remove hints that were matched but not recently.
 *   Never-matched hints (weight == 0) are kept.
 *   Returns the number of expired hints.
 *
 *************/

/* PUBLIC */
int expire_old_hints(unsigned long long current_given,
		     unsigned long long expiry_distance,
		     int min_matches,
		     Clist hint_list)
{
  Plist to_expire = NULL;
  Clist_pos p;
  int expired_count = 0;
  Plist q;

  /* Pass 1: collect expired hints */
  for (p = hint_list->first; p; p = p->next) {
    Topform c = p->c;
    if (c->weight >= min_matches &&
	current_given - c->last_matched_given > expiry_distance)
      to_expire = plist_prepend(to_expire, c);
  }

  /* Pass 2: unindex and remove from clist.
     Do NOT zap the topform -- kept clauses hold matching_hint pointers
     to these hints, and the hint_age AVL tree uses matching_hint->id
     as a comparison key.  Freeing the hint would create dangling pointers. */
  for (q = to_expire; q; q = q->next) {
    Topform c = q->v;
    unindex_hint(c);
    clist_remove(c, hint_list);
    if (compress_clause(c) == CLAUSE_COMPRESS_INVALID)
      fatal_error("expire_old_hints: cannot compact expired hint");
    expired_count++;
  }
  zap_plist(to_expire);
  return expired_count;
}  /* expire_old_hints */

/*************
 *
 *   active_hints()
 *
 *************/

/* PUBLIC */
int active_hints(void)
{
  return Active_hints_count;
}  /* active_hints */

/* PUBLIC */
BOOL packed_hints_enabled(void)
{
  return Packed_index;
}

/* PUBLIC */
void packed_hint_index_stats(unsigned long long *node_bytes,
                             unsigned long long *reference_bytes,
                             unsigned long long *table_bytes,
                             unsigned long long *candidate_checks)
{
  *node_bytes = 0;
  *reference_bytes = Packed_index ?
    (unsigned long long) 128 * Packed_feature_words *
      sizeof(unsigned long long) : 0;
  *table_bytes = Packed_index ?
    (unsigned long long) Packed_hint_capacity *
      (sizeof(Topform) + 2 * sizeof(unsigned char) + sizeof(unsigned) +
       3 * sizeof(unsigned long long)) +
    (unsigned long long) Packed_candidates_capacity * sizeof(unsigned) : 0;
  *candidate_checks = Packed_candidate_checks;
}

/*************
 *
 *   compare_doubles() -- qsort comparator
 *
 *************/

static int compare_doubles(const void *a, const void *b)
{
  double da = *(const double *)a;
  double db = *(const double *)b;
  if (da < db) return -1;
  if (da > db) return 1;
  return 0;
}  /* compare_doubles */

/*************
 *
 *   print_hint_match_stats()
 *
 *   Print min/mean/median/max of match counts for hints
 *   that were matched at least once (weight > 0).
 *
 *************/

/* PUBLIC */
void print_hint_match_stats(FILE *fp, Clist hint_list)
{
  int total, n = 0, i;
  double *counts;
  double min_v, max_v, sum, mean, median;
  Clist_pos p;

  if (hint_list == NULL)
    return;

  /* Count hints with at least one match */
  total = 0;
  for (p = hint_list->first; p; p = p->next) {
    if (p->c->weight > 0)
      total++;
  }

  if (total == 0) {
    fprintf(fp,
      "\nHint match stats: no hints were matched.\n"
      "  total=%d, redundant=%d, active=%d\n",
      hint_list->length, Redundant_hints_count, active_hints());
    return;
  }

  /* Collect match counts */
  counts = safe_malloc(total * sizeof(double));
  i = 0;
  for (p = hint_list->first; p; p = p->next) {
    if (p->c->weight > 0)
      counts[i++] = p->c->weight;
  }

  qsort(counts, total, sizeof(double), compare_doubles);

  min_v = counts[0];
  max_v = counts[total - 1];
  sum = 0;
  for (i = 0; i < total; i++)
    sum += counts[i];
  mean = sum / total;

  if (total % 2 == 1)
    median = counts[total / 2];
  else
    median = (counts[total / 2 - 1] + counts[total / 2]) / 2.0;

  /* Count never-matched */
  n = 0;
  for (p = hint_list->first; p; p = p->next) {
    if (p->c->weight == 0)
      n++;
  }

  fprintf(fp,
    "\nHint match stats:\n"
    "  total=%d, redundant=%d, active=%d, matched=%d\n"
    "  match counts: min=%.0f, mean=%.1f, median=%.0f, max=%.0f\n",
    total + n, Redundant_hints_count, active_hints(), total,
    min_v, mean, median, max_v);

  /* Re-match delta histogram (only when hint_match_stats enabled) */
  if (Hint_match_stats && Delta_total > 0) {
    static const char *labels[DELTA_BUCKETS] = {
      "0-500", "501-1000", "1001-1500", "1501-2000", "2001-2500",
      "2501-3000", "3001-3500", "3501-4000", "4001-4500", "4501-5000",
      "5001-5500", "5501-6000", "6001-6500", "6501-7000", "7001-7500",
      "7501+"
    };
    fprintf(fp,
      "  re-match deltas: n=%d, min=%llu, mean=%.1f, max=%llu\n"
      "  delta histogram:\n",
      Delta_total, Delta_min, Delta_sum / Delta_total, Delta_max);
    for (i = 0; i < DELTA_BUCKETS; i++) {
      if (Delta_bucket[i] > 0)
        fprintf(fp, "      %10s: %d\n", labels[i], Delta_bucket[i]);
    }
  }

  safe_free(counts);
}  /* print_hint_match_stats */
