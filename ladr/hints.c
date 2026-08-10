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
#include "clock.h"
#include "hint_postings.h"
#include <stdint.h>

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
static BOOL Better_packed_index = FALSE;
static BOOL Fast_packed_index = FALSE;
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

/* Dedicated read-only-preview query workspace.  It is allocated alongside
   the packed bank, never aliases authoritative matcher scratch, and is not
   part of persistent hint semantics or authoritative operation accounting. */
static unsigned *Preview_candidate_mark = NULL;
static unsigned Preview_candidate_serial = 1;
static unsigned *Preview_candidates = NULL;
static unsigned Preview_candidates_count = 0;
static unsigned Preview_candidates_capacity = 0;

/* Experimental stable-ID structural index.  Rewrites leave conservative
   stale IDs; a bounded rebuild materializes one active body at a time. */

static Hint_postings Better_postings = NULL;
static unsigned long long Better_feature_live_count = 0;
static unsigned long long Better_equivalence_live_count = 0;
static unsigned *Better_anyconst_references = NULL;
static unsigned Better_anyconst_reference_count = 0;
static unsigned Better_anyconst_reference_capacity = 0;
static unsigned long long Better_anyconst_live_count = 0;
struct better_equivalence_reference {
  unsigned id;
  unsigned next;
};
static unsigned *Better_equivalence_buckets = NULL;
static unsigned Better_equivalence_bucket_capacity = 0;
static struct better_equivalence_reference *Better_equivalence_references = NULL;
static unsigned Better_equivalence_reference_count = 0;
static unsigned Better_equivalence_reference_capacity = 0;
static unsigned *Better_hint_feature_count = NULL;
static unsigned long long *Better_hint_match_fingerprint = NULL;
static unsigned short *Better_hint_positive_count = NULL;
static unsigned short *Better_hint_negative_count = NULL;
static unsigned *Better_intersection_member = NULL;
static unsigned *Better_intersection_match = NULL;
static unsigned Better_intersection_serial = 1;
static unsigned Better_match_serial = 1;
static unsigned *Better_intersection_ids = NULL;
static unsigned Better_intersection_count = 0;
static unsigned Better_intersection_capacity = 0;
static unsigned long long Better_posting_rebuilds = 0;
static unsigned long long Better_posting_rebuild_refs = 0;
static unsigned long long Better_posting_rebuild_materializations = 0;

static unsigned long long *Better_key_scratch = NULL;
static unsigned Better_key_scratch_count = 0;
static unsigned Better_key_scratch_capacity = 0;
static unsigned *Preview_intersection_member = NULL;
static unsigned *Preview_intersection_match = NULL;
static unsigned Preview_intersection_serial = 1;
static unsigned Preview_match_serial = 1;
static unsigned *Preview_intersection_ids = NULL;
static unsigned Preview_intersection_count = 0;
static unsigned Preview_intersection_capacity = 0;
static unsigned long long *Preview_key_scratch = NULL;
static unsigned Preview_key_scratch_count = 0;
static unsigned Preview_key_scratch_capacity = 0;

/* packed_fast starts with an exact, dependency-scoped cache of the final
   structural candidate vector.  Generated equational clauses repeat shallow
   feature profiles heavily; a hit avoids rebuilding an intersection while
   preserving the exact matcher and decreasing-ID order.  Full profile fields
   are compared, and a rare required posting plus AnyConst/rebuild generations
   invalidate possible additions, so neither hash collisions nor unrelated
   hint mutations can cause an unsafe omission. */

#define FAST_MATCH_CACHE_CAPACITY 32768U
/* CHAT profiling shows that eight exact feature keys cover 98.7% of fast
   queries at 1,000 givens.  Larger profiles simply take the unchanged exact
   posting intersection; they never enter a truncated cache identity. */
#define FAST_MATCH_CACHE_KEYS 8U
#define FAST_MATCH_CACHE_CANDIDATES 8U
#define FAST_DENSE_MIN_POSTING 512U
#define FAST_DENSE_MAX_KEYS 64U

struct fast_match_cache_entry {
  unsigned long long seed_key;
  unsigned long long seed_generation;
  unsigned long long first_mask;
  unsigned long long keys[FAST_MATCH_CACHE_KEYS];
  unsigned long long source_posting_candidates;
  unsigned candidates[FAST_MATCH_CACHE_CANDIDATES];
  unsigned short positive;
  unsigned short negative;
  unsigned char key_count;
  unsigned char candidate_count;
  unsigned char valid;
};

static struct fast_match_cache_entry *Fast_match_cache = NULL;
static unsigned long long Fast_cache_queries = 0;
static unsigned long long Fast_cache_eligible = 0;
static unsigned long long Fast_cache_hits = 0;
static unsigned long long Fast_cache_misses = 0;
static unsigned long long Fast_cache_stores = 0;
static unsigned long long Fast_cache_key_overflow = 0;
static unsigned long long Fast_cache_candidate_overflow = 0;
static unsigned long long Fast_cache_posting_candidates_avoided = 0;
static unsigned long long Fast_cache_dependency_misses = 0;
static unsigned long long Fast_cache_profile_misses = 0;
static unsigned long long Fast_dense_queries = 0;
static unsigned long long Fast_dense_used = 0;
static unsigned long long Fast_sparse_used = 0;
static unsigned long long Fast_sparse_seed_ids = 0;
static unsigned long long Fast_sparse_feature_tests = 0;
static unsigned long long Fast_sparse_rejects = 0;
static unsigned long long Fast_dense_seed_ids_avoided = 0;
static unsigned long long Fast_dense_summary_words = 0;
static unsigned long long Fast_dense_data_words = 0;
static unsigned long long Fast_dense_result_ids = 0;

/* Posting rebuilds and AnyConst additions invalidate every dependency set.
   Clearing validity in one sequential pass is equivalent to carrying two
   64-bit global generations in every direct-mapped entry, and saves 512 KiB
   without changing the 32,768-slot working set or exact profile checks. */
static void fast_cache_invalidate_all(void)
{
  if (Fast_match_cache != NULL)
    memset(Fast_match_cache, 0,
           (size_t) FAST_MATCH_CACHE_CAPACITY * sizeof(*Fast_match_cache));
}

#define BETTER_FEATURE_BACK 1U
#define BETTER_FEATURE_MATCH_POS 2U
#define BETTER_FEATURE_MATCH_NEG 3U
#define BETTER_FEATURE_BACK_CORRELATED 4U
#define BETTER_BACK_FEATURE_DEPTH 2U
#define BETTER_MATCH_FEATURE_DEPTH 2U
#define FAST_MATCH_FEATURE_DEPTH 2U
#define BETTER_REBUILD_STALE_MIN 65536ULL

/* Attribute candidate selection and exact materialization cost to the four
   packed operations.  These counters intentionally live with the hint bank:
   initial hint indexing and back-demodulation are not covered by the search
   loop's ordinary hint-matching clock. */

#define PACKED_HINT_OPERATIONS 4
#define PACKED_HINT_CANDIDATE_BUCKETS 8

enum packed_hint_operation {
  PACKED_HINT_EQUIVALENCE = 0,
  PACKED_HINT_MATCH = 1,
  PACKED_HINT_FLIPPED_MATCH = 2,
  PACKED_HINT_BACK_DEMOD = 3
};

struct packed_hint_operation_stats {
  unsigned long long queries;
  unsigned long long posting_lists;
  unsigned long long posting_candidates;
  unsigned long long unique_candidates;
  unsigned long long materializations;
  unsigned long long exact_positives;
  unsigned long long rewrites;
  unsigned long long reindexes;
  unsigned long long stale_skips;
  unsigned long long fingerprint_rejects;
  unsigned long long direct_attempts;
  unsigned long long direct_handled;
  unsigned long long direct_matches;
  unsigned long long direct_equivalences;
  unsigned long long candidate_max;
  unsigned long long candidate_buckets[PACKED_HINT_CANDIDATE_BUCKETS];
  Clock clock;
};

static struct packed_hint_operation_stats
  Packed_operation_stats[PACKED_HINT_OPERATIONS];

/* Preview queries use the same exact matcher and tie breaking as the
   authoritative path, but operation counters and clocks must describe only
   authoritative search work.  Candidate arrays and serial marks are scratch
   index state and may be reused by a preview. */
static BOOL Hint_preview_active = FALSE;

static const char *Packed_operation_names[PACKED_HINT_OPERATIONS] = {
  "equivalence", "match", "flipped_match", "back_demod"
};

static unsigned packed_candidate_bucket(unsigned n)
{
  if (n == 0) return 0;
  else if (n == 1) return 1;
  else if (n <= 7) return 2;
  else if (n <= 31) return 3;
  else if (n <= 127) return 4;
  else if (n <= 1023) return 5;
  else if (n <= 16383) return 6;
  else return 7;
}

static void packed_operation_begin(enum packed_hint_operation op)
{
  if (!Hint_preview_active) {
    Packed_operation_stats[op].queries++;
    clock_start(Packed_operation_stats[op].clock);
  }
}

static void packed_operation_candidates(enum packed_hint_operation op)
{
  struct packed_hint_operation_stats *s = Packed_operation_stats + op;
  unsigned n = Packed_candidates_count;
  s->unique_candidates += n;
  if (n > s->candidate_max)
    s->candidate_max = n;
  s->candidate_buckets[packed_candidate_bucket(n)]++;
}

static void packed_operation_end(enum packed_hint_operation op)
{
  if (!Hint_preview_active)
    clock_stop(Packed_operation_stats[op].clock);
}

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
    if (!Better_packed_index) {
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
    if (!Better_packed_index)
      Packed_hint_rewrite_symbols = safe_realloc(
        Packed_hint_rewrite_symbols,
        (size_t) cap * sizeof(unsigned long long));
    Packed_hint_pos_features = safe_realloc(
      Packed_hint_pos_features, (size_t) cap * sizeof(unsigned long long));
    Packed_hint_neg_features = safe_realloc(
      Packed_hint_neg_features, (size_t) cap * sizeof(unsigned long long));
    Packed_candidate_mark = safe_realloc(Packed_candidate_mark,
                                         (size_t) cap * sizeof(unsigned));
    if (Preview_candidate_mark != NULL)
      Preview_candidate_mark = safe_realloc(
        Preview_candidate_mark, (size_t) cap * sizeof(unsigned));
    if (Preview_candidates != NULL) {
      Preview_candidates = safe_realloc(
        Preview_candidates, (size_t) cap * sizeof(unsigned));
      Preview_candidates_capacity = cap;
    }
    if (Better_packed_index) {
      Better_hint_feature_count = safe_realloc(
        Better_hint_feature_count, (size_t) cap * sizeof(unsigned));
      Better_hint_match_fingerprint = safe_realloc(
        Better_hint_match_fingerprint,
        (size_t) cap * sizeof(unsigned long long));
      Better_hint_positive_count = safe_realloc(
        Better_hint_positive_count,
        (size_t) cap * sizeof(unsigned short));
      Better_hint_negative_count = safe_realloc(
        Better_hint_negative_count,
        (size_t) cap * sizeof(unsigned short));
      Better_intersection_member = safe_realloc(
        Better_intersection_member, (size_t) cap * sizeof(unsigned));
      Better_intersection_match = safe_realloc(
        Better_intersection_match, (size_t) cap * sizeof(unsigned));
      if (Preview_intersection_member != NULL) {
        Preview_intersection_member = safe_realloc(
          Preview_intersection_member, (size_t) cap * sizeof(unsigned));
        Preview_intersection_match = safe_realloc(
          Preview_intersection_match, (size_t) cap * sizeof(unsigned));
        Preview_intersection_ids = safe_realloc(
          Preview_intersection_ids, (size_t) cap * sizeof(unsigned));
        Preview_intersection_capacity = cap;
      }
    }
    memset(Packed_hint_by_id + old, 0,
           (size_t) (cap - old) * sizeof(Topform));
    memset(Packed_hint_active + old, 0, cap - old);
    memset(Packed_hint_anyconst + old, 0, cap - old);
    if (Packed_hint_rewrite_symbols != NULL)
      memset(Packed_hint_rewrite_symbols + old, 0,
             (size_t) (cap - old) * sizeof(unsigned long long));
    memset(Packed_hint_pos_features + old, 0,
           (size_t) (cap - old) * sizeof(unsigned long long));
    memset(Packed_hint_neg_features + old, 0,
           (size_t) (cap - old) * sizeof(unsigned long long));
    memset(Packed_candidate_mark + old, 0,
           (size_t) (cap - old) * sizeof(unsigned));
    if (Preview_candidate_mark != NULL)
      memset(Preview_candidate_mark + old, 0,
             (size_t) (cap - old) * sizeof(unsigned));
    if (Better_packed_index) {
      memset(Better_hint_feature_count + old, 0,
             (size_t) (cap - old) * sizeof(unsigned));
      memset(Better_hint_match_fingerprint + old, 0,
             (size_t) (cap - old) * sizeof(unsigned long long));
      memset(Better_hint_positive_count + old, 0,
             (size_t) (cap - old) * sizeof(unsigned short));
      memset(Better_hint_negative_count + old, 0,
             (size_t) (cap - old) * sizeof(unsigned short));
      memset(Better_intersection_member + old, 0,
             (size_t) (cap - old) * sizeof(unsigned));
      memset(Better_intersection_match + old, 0,
             (size_t) (cap - old) * sizeof(unsigned));
      if (Preview_intersection_member != NULL) {
        memset(Preview_intersection_member + old, 0,
               (size_t) (cap - old) * sizeof(unsigned));
        memset(Preview_intersection_match + old, 0,
               (size_t) (cap - old) * sizeof(unsigned));
      }
    }
    Packed_hint_capacity = cap;
  }
}

static void packed_reserve_preview_workspace(void)
{
  unsigned cap = Packed_hint_capacity;
  if (cap == 0)
    return;
  if (Preview_candidate_mark == NULL)
    Preview_candidate_mark = safe_calloc(cap, sizeof(unsigned));
  if (Preview_candidates == NULL) {
    Preview_candidates = safe_malloc((size_t) cap * sizeof(unsigned));
    Preview_candidates_capacity = cap;
  }
  if (Better_packed_index && Preview_intersection_member == NULL) {
    Preview_intersection_member = safe_calloc(cap, sizeof(unsigned));
    Preview_intersection_match = safe_calloc(cap, sizeof(unsigned));
    Preview_intersection_ids = safe_malloc((size_t) cap * sizeof(unsigned));
    Preview_intersection_capacity = cap;
  }
  if (Better_packed_index && Preview_key_scratch == NULL) {
    Preview_key_scratch_capacity = 32;
    Preview_key_scratch = safe_calloc(
      Preview_key_scratch_capacity, sizeof(*Preview_key_scratch));
  }
}  /* packed_reserve_preview_workspace */

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
  if (query && !Better_packed_index && packed_term_has_theory_symbol(t))
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
  Packed_hint_anyconst[id] = anyconst ? 1 : 0;
  Packed_hint_pos_features[id] = 0;
  Packed_hint_neg_features[id] = 0;
  if (!Better_packed_index)
    Packed_hint_rewrite_symbols[id] = 0;
  for (lit = h->literals; lit != NULL; lit = lit->next) {
    int i;
    unsigned long long mask = packed_term_feature_mask(lit->atom, FALSE);
    if (lit->sign)
      Packed_hint_pos_features[id] |= mask;
    else
      Packed_hint_neg_features[id] |= mask;
    if (Back_demod_hints && !Better_packed_index && !anyconst) {
      for (i = 0; i < ARITY(lit->atom); i++)
        Packed_hint_rewrite_symbols[id] |=
          packed_rewrite_symbol_bits(ARG(lit->atom,i));
    }
  }
  if (!Better_packed_index) {
    for (bit = 0; bit < 64; bit++) {
      unsigned long long b = 1ULL << bit;
      if (Packed_hint_pos_features[id] & b)
        packed_add_feature_ref(1, bit, id);
      if (Packed_hint_neg_features[id] & b)
        packed_add_feature_ref(0, bit, id);
    }
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

static void packed_collect_term_candidates(Term t, int sign,
                                           enum packed_hint_operation op)
{
  unsigned long long mask = packed_term_feature_mask(t, TRUE);
  unsigned bit, best = 0;
  unsigned best_count = UINT_MAX;
  unsigned word;
  if (mask == 0) {
    unsigned id;
    for (id = 1; id < Packed_hint_capacity; id++) {
      Packed_operation_stats[op].posting_candidates++;
      if (!Packed_hint_active[id])
        Packed_operation_stats[op].stale_skips++;
      packed_add_candidate(id);
    }
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
      Packed_operation_stats[op].posting_candidates++;
      if (id >= Packed_hint_capacity || !Packed_hint_active[id])
        Packed_operation_stats[op].stale_skips++;
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

static void packed_finish_candidates(BOOL include_anyconst,
                                     enum packed_hint_operation op)
{
  unsigned id;
  if (include_anyconst) {
    if (Better_packed_index) {
      unsigned i;
      for (i = 0; i < Better_anyconst_reference_count; i++) {
        id = Better_anyconst_references[i];
        Packed_operation_stats[op].posting_candidates++;
        if (id == 0 || id >= Packed_hint_capacity ||
            !Packed_hint_active[id] || !Packed_hint_anyconst[id])
          Packed_operation_stats[op].stale_skips++;
        packed_add_candidate(id);
      }
    }
    else {
      for (id = 1; id < Packed_hint_capacity; id++) {
        if (Packed_hint_anyconst[id]) {
          Packed_operation_stats[op].posting_candidates++;
          if (!Packed_hint_active[id])
            Packed_operation_stats[op].stale_skips++;
          packed_add_candidate(id);
        }
      }
    }
  }
  qsort(Packed_candidates, Packed_candidates_count, sizeof(unsigned),
        packed_id_decreasing);
}

static unsigned long long better_feature_key(unsigned kind, unsigned path,
                                             int symbol)
{
  return ((unsigned long long) kind << 56) |
         ((unsigned long long) (path & 0x00ffffffU) << 32) |
         (unsigned) symbol;
}

/* A second, independent conservative signature complements the original
   packed path mask.  Every exact shallow match feature contributes one bit;
   collisions admit extra candidates only.  This lets ordinary matching scan
   one rare posting and test all remaining query features in O(1), instead of
   traversing a second broad posting for every query. */
static unsigned long long better_match_fingerprint(void)
{
  unsigned i;
  unsigned long long fingerprint = 0;
  for (i = 0; i < Better_key_scratch_count; i++) {
    unsigned long long key = Better_key_scratch[i];
    unsigned kind = (unsigned) (key >> 56);
    if (kind == BETTER_FEATURE_MATCH_POS ||
        kind == BETTER_FEATURE_MATCH_NEG) {
      unsigned long long x = key;
      x ^= x >> 30;
      x *= 0xbf58476d1ce4e5b9ULL;
      x ^= x >> 27;
      x *= 0x94d049bb133111ebULL;
      x ^= x >> 31;
      fingerprint |= 1ULL << (x >> 58);
    }
  }
  return fingerprint;
}

/* Correlate a descendant with the root of the same possible rewrite
   occurrence.  The low 56 bits are a hash, so collisions admit extra
   candidates only; rewritable_clause_type remains the exact authority. */
static unsigned long long better_back_correlated_key(
  int root_symbol, unsigned path, int symbol)
{
  unsigned long long x = ((unsigned long long) (unsigned) root_symbol << 32) |
                         (unsigned) symbol;
  x ^= (unsigned long long) path * 0x9e3779b97f4a7c15ULL;
  x ^= x >> 30;
  x *= 0xbf58476d1ce4e5b9ULL;
  x ^= x >> 27;
  x *= 0x94d049bb133111ebULL;
  x ^= x >> 31;
  return ((unsigned long long) BETTER_FEATURE_BACK_CORRELATED << 56) |
         (x & 0x00ffffffffffffffULL);
}

static unsigned long long better_equivalence_key(
  unsigned long long positive_mask, unsigned long long negative_mask,
  unsigned positive, unsigned negative, unsigned query_literals)
{
  unsigned long long x = positive_mask ^
    ((negative_mask << 29) | (negative_mask >> 35)) ^
    ((unsigned long long) (positive != 0) << 48) ^
    ((unsigned long long) (negative != 0) << 32) ^
    (unsigned long long) query_literals * 0x9e3779b97f4a7c15ULL;
  x ^= x >> 30;
  x *= 0xbf58476d1ce4e5b9ULL;
  x ^= x >> 27;
  x *= 0x94d049bb133111ebULL;
  return x ^ (x >> 31);
}

static void better_equivalence_reserve_references(unsigned needed)
{
  if (needed > Better_equivalence_reference_capacity) {
    unsigned capacity = Better_equivalence_reference_capacity == 0 ? 1024 :
                        Better_equivalence_reference_capacity;
    while (capacity < needed) {
      unsigned grown = capacity + (capacity + 1) / 2;
      if (grown <= capacity)
        fatal_error("better equivalence reference capacity overflow");
      capacity = grown;
    }
    Better_equivalence_references = safe_realloc(
      Better_equivalence_references,
      (size_t) capacity * sizeof(struct better_equivalence_reference));
    Better_equivalence_reference_capacity = capacity;
  }
}

static unsigned better_equivalence_memberships(unsigned id)
{
  unsigned literals = (unsigned) Better_hint_positive_count[id] +
                      Better_hint_negative_count[id];
  return literals == 0 ? 1 : literals;
}

static void better_equivalence_append(unsigned id)
{
  unsigned literals = (unsigned) Better_hint_positive_count[id] +
                      Better_hint_negative_count[id];
  unsigned first_query_literals = literals == 0 ? 0 : 1;
  unsigned last_query_literals = literals;
  unsigned query_literals;
  for (query_literals = first_query_literals; ; query_literals++) {
    unsigned long long key = better_equivalence_key(
      Packed_hint_pos_features[id], Packed_hint_neg_features[id],
      Better_hint_positive_count[id], Better_hint_negative_count[id],
      query_literals);
    unsigned bucket = (unsigned) key &
                      (Better_equivalence_bucket_capacity - 1);
    unsigned position = Better_equivalence_reference_count;
    better_equivalence_reserve_references(position + 1);
    Better_equivalence_references[position].id = id;
    Better_equivalence_references[position].next =
      Better_equivalence_buckets[bucket];
    Better_equivalence_buckets[bucket] = position;
    Better_equivalence_reference_count++;
    if (query_literals == last_query_literals)
      break;
  }
}

static void better_equivalence_rebuild(unsigned requested_capacity)
{
  unsigned capacity = requested_capacity < 1024 ? 1024 : requested_capacity;
  unsigned id, i;
  while ((Better_equivalence_live_count + 1ULL) * 10ULL >=
         (unsigned long long) capacity * 7) {
    if (capacity > UINT_MAX / 2)
      fatal_error("better equivalence bucket capacity overflow");
    capacity *= 2;
  }
  if (Better_equivalence_buckets != NULL)
    safe_free(Better_equivalence_buckets);
  Better_equivalence_buckets = safe_malloc((size_t) capacity * sizeof(unsigned));
  Better_equivalence_bucket_capacity = capacity;
  for (i = 0; i < capacity; i++)
    Better_equivalence_buckets[i] = UINT_MAX;
  Better_equivalence_reference_count = 0;
  for (id = 1; id < Packed_hint_capacity; id++) {
    if (Packed_hint_active[id] && Packed_hint_by_id[id] != NULL)
      better_equivalence_append(id);
  }
  if (Better_equivalence_reference_count != Better_equivalence_live_count)
    fatal_error("better equivalence live count mismatch");
}

static void better_equivalence_add(unsigned id)
{
  unsigned memberships = better_equivalence_memberships(id);
  if (Better_equivalence_bucket_capacity == 0 ||
      ((unsigned long long) Better_equivalence_reference_count +
       memberships) * 10 >=
      (unsigned long long) Better_equivalence_bucket_capacity * 7) {
    unsigned capacity;
    if (Better_equivalence_bucket_capacity == 0)
      capacity = 1024;
    else {
      if (Better_equivalence_bucket_capacity > UINT_MAX / 2)
        fatal_error("better equivalence bucket capacity overflow");
      capacity = Better_equivalence_bucket_capacity * 2;
    }
    better_equivalence_rebuild(capacity);
  }
  else
    better_equivalence_append(id);
}

static void better_anyconst_add(unsigned id)
{
  if (Better_anyconst_reference_count == Better_anyconst_reference_capacity) {
    unsigned old = Better_anyconst_reference_capacity;
    unsigned capacity = old == 0 ? 64 : old + (old + 1) / 2;
    if (capacity <= old)
      fatal_error("better_anyconst_add: capacity overflow");
    Better_anyconst_references = safe_realloc(
      Better_anyconst_references, (size_t) capacity * sizeof(unsigned));
    Better_anyconst_reference_capacity = capacity;
  }
  Better_anyconst_references[Better_anyconst_reference_count++] = id;
}

static void better_scratch_clear(void)
{
  Better_key_scratch_count = 0;
}

static void better_scratch_add(unsigned long long key)
{
  unsigned i;
  for (i = 0; i < Better_key_scratch_count; i++) {
    if (Better_key_scratch[i] == key)
      return;
  }
  if (Better_key_scratch_count == Better_key_scratch_capacity) {
    unsigned capacity = Better_key_scratch_capacity == 0 ? 32 :
                        Better_key_scratch_capacity * 2;
    Better_key_scratch = safe_realloc(
      Better_key_scratch,
      (size_t) capacity * sizeof(unsigned long long));
    Better_key_scratch_capacity = capacity;
  }
  Better_key_scratch[Better_key_scratch_count++] = key;
}

static unsigned better_child_path(unsigned path, unsigned depth,
                                  unsigned child)
{
  if (child >= 255)
    return UINT_MAX;
  if (depth == 0)
    return 0x010000U | ((child + 1) << 8);
  else
    return 0x020000U | (path & 0x0000ff00U) | (child + 1);
}

/* Collect exact positional symbols that a one-way syntactic match must
   preserve.  Variables contribute no restriction. */
static void better_collect_relative_features(Term t, unsigned kind,
                                             unsigned path, unsigned depth,
                                             unsigned maximum_depth)
{
  unsigned i;
  if (VARIABLE(t))
    return;
  better_scratch_add(better_feature_key(kind, path, SYMNUM(t)));
  if (depth >= maximum_depth)
    return;
  for (i = 0; i < (unsigned) ARITY(t); i++) {
    unsigned child_path = better_child_path(path, depth, i);
    if (child_path != UINT_MAX)
      better_collect_relative_features(ARG(t,i), kind, child_path, depth + 1,
                                       maximum_depth);
  }
}

static void better_collect_back_correlated_features(
  Term t, int root_symbol, unsigned path, unsigned depth)
{
  unsigned i;
  if (VARIABLE(t) || depth >= BETTER_BACK_FEATURE_DEPTH)
    return;
  for (i = 0; i < (unsigned) ARITY(t); i++) {
    Term child = ARG(t,i);
    unsigned child_path = better_child_path(path, depth, i);
    if (child_path != UINT_MAX && !VARIABLE(child)) {
      better_scratch_add(better_back_correlated_key(
        root_symbol, child_path, SYMNUM(child)));
      better_collect_back_correlated_features(
        child, root_symbol, child_path, depth + 1);
    }
  }
}

static void better_collect_back_features(Term t)
{
  if (VARIABLE(t))
    return;
  better_collect_relative_features(t, BETTER_FEATURE_BACK, 0, 0,
                                   BETTER_BACK_FEATURE_DEPTH);
  better_collect_back_correlated_features(t, SYMNUM(t), 0, 0);
}

static void better_collect_back_occurrences(Term t)
{
  int i;
  if (VARIABLE(t))
    return;
  better_collect_back_features(t);
  for (i = 0; i < ARITY(t); i++)
    better_collect_back_occurrences(ARG(t,i));
}

static void better_collect_hint_features(Topform h, BOOL anyconst)
{
  Literals lit;
  unsigned positive = 0, negative = 0;
  better_scratch_clear();
  for (lit = h->literals; lit != NULL; lit = lit->next) {
    if (lit->sign)
      positive++;
    else
      negative++;
    if (!anyconst) {
      int i;
      unsigned kind = lit->sign ? BETTER_FEATURE_MATCH_POS :
                                  BETTER_FEATURE_MATCH_NEG;
      better_collect_relative_features(lit->atom, kind, 0, 0,
                                       Fast_packed_index ?
                                         FAST_MATCH_FEATURE_DEPTH :
                                         BETTER_MATCH_FEATURE_DEPTH);
      for (i = 0; i < ARITY(lit->atom); i++)
        better_collect_back_occurrences(ARG(lit->atom,i));
    }
  }
  if (positive > USHRT_MAX || negative > USHRT_MAX)
    fatal_error("better_collect_hint_features: too many literals");
  Better_hint_positive_count[h->id] = (unsigned short) positive;
  Better_hint_negative_count[h->id] = (unsigned short) negative;
  Better_hint_match_fingerprint[h->id] = better_match_fingerprint();
}

static void better_rebuild_postings(void)
{
  Hint_postings postings = hint_postings_init();
  unsigned long long live = 0;
  unsigned long long equivalence_live = 0;
  unsigned long long anyconst_live = 0;
  unsigned id;
  Better_anyconst_reference_count = 0;
  for (id = 1; id < Packed_hint_capacity; id++) {
    Topform h = Packed_hint_by_id[id];
    Better_hint_feature_count[id] = 0;
    Better_hint_match_fingerprint[id] = 0;
    Better_hint_positive_count[id] = 0;
    Better_hint_negative_count[id] = 0;
    if (Packed_hint_active[id] && h != NULL) {
      BOOL was_compressed = h->compressed != NULL;
      unsigned i;
      if (was_compressed) {
        Better_posting_rebuild_materializations++;
        if (!materialize_clause(h))
          fatal_error("better_rebuild_postings: invalid packed hint");
      }
      better_collect_hint_features(h, Packed_hint_anyconst[id]);
      Better_hint_feature_count[id] = Better_key_scratch_count;
      live += Better_key_scratch_count;
      for (i = 0; i < Better_key_scratch_count; i++)
        hint_postings_add(postings, Better_key_scratch[i], id);
      equivalence_live += better_equivalence_memberships(id);
      if (Packed_hint_anyconst[id]) {
        better_anyconst_add(id);
        anyconst_live++;
      }
      if (was_compressed && !recompress_clause(h))
        fatal_error("better_rebuild_postings: cannot recompress hint");
      if (!Packed_hint_active[id]) {
        /* Defensive lifecycle check: rebuilding is synchronous and must not
           change hint membership. */
        fatal_error("better_rebuild_postings: hint changed during rebuild");
      }
    }
  }
  hint_postings_destroy(Better_postings);
  Better_postings = postings;
  if (Fast_packed_index)
    fast_cache_invalidate_all();
  Better_feature_live_count = live;
  Better_equivalence_live_count = equivalence_live;
  Better_anyconst_live_count = anyconst_live;
  better_equivalence_rebuild(Better_equivalence_bucket_capacity);
  Better_posting_rebuilds++;
  Better_posting_rebuild_refs += live + equivalence_live;
}

static void better_maybe_rebuild_postings(void)
{
  struct hint_postings_stats stats;
  unsigned long long stale;
  hint_postings_get_stats(Better_postings, &stats);
  if (stats.references < Better_feature_live_count ||
      Better_equivalence_reference_count < Better_equivalence_live_count)
    fatal_error("better_maybe_rebuild_postings: reference count underflow");
  stale = stats.references - Better_feature_live_count;
  stale += Better_equivalence_reference_count -
           Better_equivalence_live_count;
  if (stale >= BETTER_REBUILD_STALE_MIN &&
      stale > (Better_feature_live_count +
               Better_equivalence_live_count) / 4)
    better_rebuild_postings();
}

static void better_deactivate_hint(unsigned id)
{
  unsigned count;
  if (!Better_packed_index || id == 0 || id >= Packed_hint_capacity)
    return;
  count = Better_hint_feature_count[id];
  if (count > Better_feature_live_count)
    fatal_error("better_deactivate_hint: live feature count underflow");
  Better_feature_live_count -= count;
  if (better_equivalence_memberships(id) > Better_equivalence_live_count)
    fatal_error("better_deactivate_hint: equivalence count underflow");
  Better_equivalence_live_count -= better_equivalence_memberships(id);
  if (Packed_hint_anyconst[id]) {
    if (Better_anyconst_live_count == 0)
      fatal_error("better_deactivate_hint: AnyConst count underflow");
    Better_anyconst_live_count--;
  }
  Better_hint_feature_count[id] = 0;
  Better_hint_match_fingerprint[id] = 0;
  Better_hint_positive_count[id] = 0;
  Better_hint_negative_count[id] = 0;
  better_maybe_rebuild_postings();
}

static void better_index_hint_terms(Topform h, BOOL anyconst)
{
  unsigned id = (unsigned) h->id;
  unsigned i;
  if (!Better_packed_index)
    return;
  if (Better_hint_feature_count[id] != 0)
    fatal_error("better_index_hint_terms: hint already has live features");
  better_collect_hint_features(h, anyconst);
  Better_hint_feature_count[id] = Better_key_scratch_count;
  for (i = 0; i < Better_key_scratch_count; i++) {
    unsigned long long key = Better_key_scratch[i];
    hint_postings_add(Better_postings, key, id);
  }
  Better_feature_live_count += Better_key_scratch_count;
  if (Fast_packed_index && anyconst)
    fast_cache_invalidate_all();
  Better_equivalence_live_count += better_equivalence_memberships(id);
  if (anyconst) {
    Better_anyconst_live_count++;
    better_anyconst_add(id);
  }
  better_equivalence_add(id);
  better_maybe_rebuild_postings();
}

static void better_intersect_scratch_candidates(
  enum packed_hint_operation op, BOOL exclude_anyconst, BOOL all_features)
{
  unsigned i, j;
  unsigned keys_to_scan;
  unsigned long long required_fingerprint;
  if (Better_key_scratch_count == 0)
    return;

  /* Ordinary matching uses the rarest exact posting as its seed, then tests
     the conservative per-hint match fingerprint plus the original packed
     profile/path masks before authoritative subsumption.  The fingerprint
     preserves the selectivity of a second exact key without scanning that
     often-broad posting.  Back-demodulation still asks for a true
     correlated-feature intersection. */
  keys_to_scan = all_features ? Better_key_scratch_count :
                 1;
  required_fingerprint = all_features ? 0 : better_match_fingerprint();

  /* Put posting keys in increasing raw-count order.  Stale entries can only
     make a posting appear less selective; they cannot remove an answer. */
  for (i = 0; i < keys_to_scan; i++) {
    unsigned best = i;
    unsigned best_count = UINT_MAX;
    for (j = i; j < Better_key_scratch_count; j++) {
      unsigned count;
      hint_postings_get(Better_postings, Better_key_scratch[j], &count);
      if (count == 0)
        return;
      if (count < best_count) {
        best = j;
        best_count = count;
      }
    }
    if (best != i) {
      unsigned long long key = Better_key_scratch[i];
      Better_key_scratch[i] = Better_key_scratch[best];
      Better_key_scratch[best] = key;
    }
  }

  Better_intersection_serial++;
  if (Better_intersection_serial == 0) {
    memset(Better_intersection_member, 0,
           (size_t) Packed_hint_capacity * sizeof(unsigned));
    Better_intersection_serial = 1;
  }
  Better_intersection_count = 0;
  for (i = 0; i < keys_to_scan; i++) {
    unsigned count;
    const unsigned *ids;
    Packed_operation_stats[op].posting_lists++;
    ids = hint_postings_get(Better_postings, Better_key_scratch[i], &count);
    Better_match_serial++;
    if (Better_match_serial == 0) {
      memset(Better_intersection_match, 0,
             (size_t) Packed_hint_capacity * sizeof(unsigned));
      Better_match_serial = 1;
    }
    for (j = 0; j < count; j++) {
      unsigned id = ids[j];
      Packed_operation_stats[op].posting_candidates++;
      if (id == 0 || id >= Packed_hint_capacity ||
          !Packed_hint_active[id] ||
          (exclude_anyconst && Packed_hint_anyconst[id])) {
        Packed_operation_stats[op].stale_skips++;
        continue;
      }
      if (!all_features &&
          (Better_hint_match_fingerprint[id] & required_fingerprint) !=
            required_fingerprint) {
        Packed_operation_stats[op].fingerprint_rejects++;
        continue;
      }
      if (i == 0) {
        if (Better_intersection_member[id] != Better_intersection_serial) {
          if (Better_intersection_count == Better_intersection_capacity) {
            unsigned capacity = Better_intersection_capacity == 0 ? 1024 :
                                Better_intersection_capacity * 2;
            Better_intersection_ids = safe_realloc(
              Better_intersection_ids,
              (size_t) capacity * sizeof(unsigned));
            Better_intersection_capacity = capacity;
          }
          Better_intersection_member[id] = Better_intersection_serial;
          Better_intersection_ids[Better_intersection_count++] = id;
        }
      }
      else if (Better_intersection_member[id] == Better_intersection_serial)
        Better_intersection_match[id] = Better_match_serial;
    }
    if (i != 0) {
      unsigned keep = 0;
      for (j = 0; j < Better_intersection_count; j++) {
        unsigned id = Better_intersection_ids[j];
        if (Better_intersection_match[id] == Better_match_serial)
          Better_intersection_ids[keep++] = id;
        else
          Better_intersection_member[id] = 0;
      }
      Better_intersection_count = keep;
      if (keep == 0)
        return;
    }
  }
  for (i = 0; i < Better_intersection_count; i++)
    packed_add_candidate(Better_intersection_ids[i]);
}

static void better_collect_back_pattern_candidates(
  Term pattern, enum packed_hint_operation op)
{
  if (VARIABLE(pattern)) {
    unsigned id;
    for (id = 1; id < Packed_hint_capacity; id++) {
      if (Packed_hint_active[id] && !Packed_hint_anyconst[id]) {
        Packed_operation_stats[op].posting_candidates++;
        packed_add_candidate(id);
      }
    }
    return;
  }
  better_scratch_clear();
  better_collect_back_correlated_features(pattern, SYMNUM(pattern), 0, 0);
  if (Better_key_scratch_count == 0)
    better_scratch_add(better_feature_key(
      BETTER_FEATURE_BACK, 0, SYMNUM(pattern)));
  better_intersect_scratch_candidates(op, TRUE, TRUE);
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

static unsigned long long fast_profile_hash(
  const unsigned long long *keys, unsigned key_count,
  unsigned long long first_mask, unsigned positive, unsigned negative)
{
  unsigned i;
  unsigned long long h = first_mask ^ 0x9e3779b97f4a7c15ULL;
  h ^= ((unsigned long long) positive << 32) | negative;
  h ^= (unsigned long long) key_count * 0xbf58476d1ce4e5b9ULL;
  for (i = 0; i < key_count; i++) {
    unsigned long long x = keys[i];
    x ^= x >> 30;
    x *= 0xbf58476d1ce4e5b9ULL;
    x ^= x >> 27;
    x *= 0x94d049bb133111ebULL;
    x ^= x >> 31;
    h ^= x + 0x9e3779b97f4a7c15ULL + (h << 6) + (h >> 2);
  }
  h ^= h >> 30;
  h *= 0xbf58476d1ce4e5b9ULL;
  h ^= h >> 27;
  h *= 0x94d049bb133111ebULL;
  return h ^ (h >> 31);
}

static BOOL fast_canonical_profile(unsigned long long *keys,
                                   unsigned *key_count)
{
  unsigned i;
  if (Better_key_scratch_count > FAST_MATCH_CACHE_KEYS)
    return FALSE;
  *key_count = Better_key_scratch_count;
  for (i = 0; i < *key_count; i++) {
    unsigned j = i;
    keys[i] = Better_key_scratch[i];
    while (j > 0 && keys[j-1] > keys[j]) {
      unsigned long long tmp = keys[j-1];
      keys[j-1] = keys[j];
      keys[j] = tmp;
      j--;
    }
  }
  return TRUE;
}

static struct fast_match_cache_entry *fast_cache_slot(
  const unsigned long long *keys, unsigned key_count,
  unsigned long long first_mask, unsigned positive, unsigned negative)
{
  unsigned long long hash = fast_profile_hash(
    keys, key_count, first_mask, positive, negative);
  return Fast_match_cache +
    ((unsigned) hash & (FAST_MATCH_CACHE_CAPACITY - 1));
}

static BOOL fast_cache_lookup(
  const unsigned long long *keys, unsigned key_count,
  unsigned long long first_mask, unsigned positive, unsigned negative)
{
  struct fast_match_cache_entry *e = fast_cache_slot(
    keys, key_count, first_mask, positive, negative);
  unsigned i;
  if (!Hint_preview_active) {
    Fast_cache_queries++;
    Fast_cache_eligible++;
  }
  if (!e->valid || e->first_mask != first_mask ||
      e->positive != positive || e->negative != negative ||
      e->key_count != key_count ||
      memcmp(e->keys, keys,
             (size_t) key_count * sizeof(unsigned long long)) != 0) {
    if (!Hint_preview_active) {
      Fast_cache_misses++;
      Fast_cache_profile_misses++;
    }
    return FALSE;
  }
  if ((key_count == 0 ? e->seed_generation != Hint_state_epoch :
       e->seed_generation !=
         hint_postings_generation(Better_postings, e->seed_key))) {
    if (!Hint_preview_active) {
      Fast_cache_misses++;
      Fast_cache_dependency_misses++;
    }
    return FALSE;
  }
  for (i = 0; i < e->candidate_count; i++)
    packed_add_candidate(e->candidates[i]);
  if (!Hint_preview_active) {
    Fast_cache_hits++;
    Fast_cache_posting_candidates_avoided +=
      e->source_posting_candidates;
  }
  return TRUE;
}

static void fast_cache_store(
  const unsigned long long *keys, unsigned key_count,
  unsigned long long first_mask, unsigned positive, unsigned negative,
  unsigned long long source_posting_candidates)
{
  struct fast_match_cache_entry *e;
  unsigned seed_count = UINT_MAX;
  unsigned i;
  if (Hint_preview_active)
    return;
  if (Packed_candidates_count > FAST_MATCH_CACHE_CANDIDATES) {
    Fast_cache_candidate_overflow++;
    return;
  }
  e = fast_cache_slot(keys, key_count, first_mask, positive, negative);
  memset(e, 0, sizeof(*e));
  if (key_count == 0)
    e->seed_generation = Hint_state_epoch;
  else {
    for (i = 0; i < key_count; i++) {
      unsigned count;
      hint_postings_get(Better_postings, keys[i], &count);
      if (count < seed_count) {
        seed_count = count;
        e->seed_key = keys[i];
      }
    }
    e->seed_generation =
      hint_postings_generation(Better_postings, e->seed_key);
  }
  e->first_mask = first_mask;
  memcpy(e->keys, keys, (size_t) key_count * sizeof(unsigned long long));
  memcpy(e->candidates, Packed_candidates,
         (size_t) Packed_candidates_count * sizeof(unsigned));
  e->source_posting_candidates = source_posting_candidates;
  e->positive = (unsigned short) positive;
  e->negative = (unsigned short) negative;
  e->key_count = (unsigned char) key_count;
  e->candidate_count = (unsigned char) Packed_candidates_count;
  e->valid = 1;
  Fast_cache_stores++;
}

/* Intersect broad exact postings a machine word at a time.  Each posting has
   a one-bit-per-data-word summary, so blocks that cannot contain a common ID
   are skipped before any full bitset row is touched.  Posting bits may retain
   stale IDs after rewrites, but they never lose a newly indexed feature;
   activity and the authoritative matcher therefore preserve correctness. */
static BOOL fast_dense_collect_candidates(
  const unsigned long long *keys, unsigned key_count,
  enum packed_hint_operation op, BOOL exclude_anyconst)
{
  struct hint_dense_view views[FAST_DENSE_MAX_KEYS];
  unsigned minimum = UINT_MAX;
  unsigned seed = 0;
  unsigned i, summary_word;
  BOOL create = !Hint_preview_active;
  if (!Hint_preview_active)
    Fast_dense_queries++;
  if (key_count < 2 || key_count > FAST_DENSE_MAX_KEYS)
    return FALSE;
  for (i = 0; i < key_count; i++) {
    unsigned count;
    hint_postings_get(Better_postings, keys[i], &count);
    if (count == 0)
      return TRUE;
    if (count < minimum) {
      minimum = count;
      seed = i;
    }
  }
  for (i = 0; i < key_count; i++) {
    if (!hint_postings_dense_view(Better_postings, keys[i],
                                  Packed_hint_capacity, create,
                                  views + i)) {
      return FALSE;
    }
  }
  if (minimum < FAST_DENSE_MIN_POSTING) {
    unsigned count, j;
    const unsigned *ids = hint_postings_get(
      Better_postings, keys[seed], &count);
    if (!Hint_preview_active) {
      Fast_sparse_used++;
      Fast_sparse_seed_ids += count;
      Packed_operation_stats[op].posting_lists += key_count;
    }
    for (j = 0; j < count; j++) {
      unsigned id = ids[j];
      BOOL keep = TRUE;
      if (!Hint_preview_active)
        Packed_operation_stats[op].posting_candidates++;
      if (id == 0 || id >= Packed_hint_capacity ||
          !Packed_hint_active[id] ||
          (exclude_anyconst && Packed_hint_anyconst[id])) {
        if (!Hint_preview_active)
          Packed_operation_stats[op].stale_skips++;
        continue;
      }
      for (i = 0; i < key_count && keep; i++) {
        unsigned word;
        if (i == seed)
          continue;
        word = id / 64;
        if (!Hint_preview_active)
          Fast_sparse_feature_tests++;
        keep = word < views[i].words &&
               (views[i].bits[word] & (1ULL << (id % 64))) != 0;
      }
      if (keep)
        packed_add_candidate(id);
      else if (!Hint_preview_active)
        Fast_sparse_rejects++;
    }
    return TRUE;
  }
  if (!Hint_preview_active) {
    Fast_dense_used++;
    Fast_dense_seed_ids_avoided += minimum;
    Packed_operation_stats[op].posting_lists += key_count;
  }
  for (summary_word = 0;
       summary_word < views[0].summary_words; summary_word++) {
    unsigned long long common = views[0].summary[summary_word];
    unsigned j;
    for (j = 1; j < key_count && common != 0; j++)
      common &= views[j].summary[summary_word];
    if (!Hint_preview_active)
      Fast_dense_summary_words++;
    while (common != 0) {
      unsigned summary_bit = (unsigned) __builtin_ctzll(common);
      unsigned word = summary_word * 64 + summary_bit;
      unsigned long long bits;
      if (word >= views[0].words)
        break;
      bits = views[0].bits[word];
      for (j = 1; j < key_count && bits != 0; j++)
        bits &= views[j].bits[word];
      if (!Hint_preview_active)
        Fast_dense_data_words++;
      while (bits != 0) {
        unsigned bit = (unsigned) __builtin_ctzll(bits);
        unsigned id = word * 64 + bit;
        if (!Hint_preview_active)
          Packed_operation_stats[op].posting_candidates++;
        if (id == 0 || id >= Packed_hint_capacity ||
            !Packed_hint_active[id] ||
            (exclude_anyconst && Packed_hint_anyconst[id])) {
          if (!Hint_preview_active)
            Packed_operation_stats[op].stale_skips++;
        }
        else {
          packed_add_candidate(id);
          if (!Hint_preview_active)
            Fast_dense_result_ids++;
        }
        bits &= bits - 1;
      }
      common &= common - 1;
    }
  }
  return TRUE;
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
		BOOL better_packed_index,
		BOOL fast_packed_index,
		void (*demod_proc) (Topform, int, int, BOOL, BOOL))
{
  Bsub_wt_attr = bsub_wt_attr;
  Collect_labels = collect_labels;
  Back_demod_hints = back_demod_hints;
  Packed_index = packed_index;
  Better_packed_index = better_packed_index;
  Fast_packed_index = fast_packed_index;
  Demod_proc = demod_proc;
  if (Better_packed_index && !Packed_index)
    fatal_error("init_hints: better packed index requires packed hint bank");
  if (Fast_packed_index && !Better_packed_index)
    fatal_error("init_hints: fast packed index requires better packed index");
  if (Fast_packed_index)
    Fast_match_cache = safe_calloc(
      FAST_MATCH_CACHE_CAPACITY, sizeof(struct fast_match_cache_entry));
  if (Better_packed_index)
    Better_postings = hint_postings_init();
  if (packed_index) {
    unsigned i;
    for (i = 0; i < PACKED_HINT_OPERATIONS; i++)
      Packed_operation_stats[i].clock =
        clock_init((char *) Packed_operation_names[i]);
  }
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
  if (Preview_candidate_mark) safe_free(Preview_candidate_mark);
  if (Preview_candidates) safe_free(Preview_candidates);
  if (Better_hint_feature_count) safe_free(Better_hint_feature_count);
  if (Better_hint_match_fingerprint)
    safe_free(Better_hint_match_fingerprint);
  if (Better_hint_positive_count) safe_free(Better_hint_positive_count);
  if (Better_hint_negative_count) safe_free(Better_hint_negative_count);
  if (Better_intersection_member) safe_free(Better_intersection_member);
  if (Better_intersection_match) safe_free(Better_intersection_match);
  if (Better_intersection_ids) safe_free(Better_intersection_ids);
  if (Better_key_scratch) safe_free(Better_key_scratch);
  if (Preview_intersection_member) safe_free(Preview_intersection_member);
  if (Preview_intersection_match) safe_free(Preview_intersection_match);
  if (Preview_intersection_ids) safe_free(Preview_intersection_ids);
  if (Preview_key_scratch) safe_free(Preview_key_scratch);
  if (Fast_match_cache) safe_free(Fast_match_cache);
  hint_postings_destroy(Better_postings);
  if (Better_equivalence_buckets) safe_free(Better_equivalence_buckets);
  if (Better_equivalence_references) safe_free(Better_equivalence_references);
  if (Better_anyconst_references) safe_free(Better_anyconst_references);
  Packed_hint_by_id = NULL; Packed_hint_active = NULL;
  Packed_hint_anyconst = NULL; Packed_candidate_mark = NULL;
  Packed_hint_rewrite_symbols = NULL;
  Packed_hint_pos_features = Packed_hint_neg_features = NULL;
  Packed_feature_bitsets = NULL;
  Packed_feature_words = 0;
  Packed_candidates = NULL;
  Preview_candidate_mark = NULL;
  Preview_candidates = NULL;
  Better_postings = NULL;
  Better_equivalence_buckets = NULL;
  Better_equivalence_references = NULL;
  Better_anyconst_references = NULL;
  Better_equivalence_bucket_capacity = 0;
  Better_equivalence_reference_count = 0;
  Better_equivalence_reference_capacity = 0;
  Better_anyconst_reference_count = 0;
  Better_anyconst_reference_capacity = 0;
  Better_hint_feature_count = NULL;
  Better_hint_match_fingerprint = NULL;
  Better_hint_positive_count = Better_hint_negative_count = NULL;
  Better_intersection_member = Better_intersection_match = NULL;
  Better_intersection_ids = NULL;
  Better_key_scratch = NULL;
  Preview_intersection_member = Preview_intersection_match = NULL;
  Preview_intersection_ids = NULL;
  Preview_key_scratch = NULL;
  Fast_match_cache = NULL;
  Better_feature_live_count = 0;
  Better_equivalence_live_count = 0;
  Better_anyconst_live_count = 0;
  Better_intersection_serial = Better_match_serial = 1;
  Better_intersection_count = Better_intersection_capacity = 0;
  Better_key_scratch_count = Better_key_scratch_capacity = 0;
  Preview_intersection_serial = Preview_match_serial = 1;
  Preview_intersection_count = Preview_intersection_capacity = 0;
  Preview_key_scratch_count = Preview_key_scratch_capacity = 0;
  Better_posting_rebuilds = Better_posting_rebuild_refs = 0;
  Better_posting_rebuild_materializations = 0;
  Fast_cache_queries = Fast_cache_eligible = 0;
  Fast_cache_hits = Fast_cache_misses = Fast_cache_stores = 0;
  Fast_cache_key_overflow = Fast_cache_candidate_overflow = 0;
  Fast_cache_posting_candidates_avoided = 0;
  Fast_cache_dependency_misses = Fast_cache_profile_misses = 0;
  Fast_dense_queries = Fast_dense_used = 0;
  Fast_sparse_used = Fast_sparse_seed_ids = 0;
  Fast_sparse_feature_tests = Fast_sparse_rejects = 0;
  Fast_dense_seed_ids_avoided = Fast_dense_summary_words = 0;
  Fast_dense_data_words = Fast_dense_result_ids = 0;
  Packed_hint_capacity = 0;
  Packed_candidates_count = Packed_candidates_capacity = 0;
  Preview_candidate_serial = 1;
  Preview_candidates_count = Preview_candidates_capacity = 0;
  memset(Packed_feature_counts, 0, sizeof(Packed_feature_counts));
  Packed_candidate_checks = 0;
  {
    unsigned i;
    for (i = 0; i < PACKED_HINT_OPERATIONS; i++) {
      if (Packed_operation_stats[i].clock != NULL)
        free_clock(Packed_operation_stats[i].clock);
    }
    memset(Packed_operation_stats, 0, sizeof(Packed_operation_stats));
  }
  Packed_index = FALSE;
  Better_packed_index = FALSE;
  Fast_packed_index = FALSE;
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

static BOOL hint_contains_anyconst(Topform c);

static void better_collect_clause_candidates(
  Topform c, enum packed_hint_operation op)
{
  Literals lit;
  Literals first = c->literals;
  unsigned positive = 0, negative = 0;
  unsigned i, keep;
  unsigned long long positive_mask = 0, negative_mask = 0;
  unsigned long long fast_keys[FAST_MATCH_CACHE_KEYS];
  unsigned fast_key_count = 0;
  unsigned long long posting_candidates_before = 0;
  BOOL fast_eligible = FALSE;
  unsigned long long first_mask = first == NULL ? 0 :
    packed_term_feature_mask(first->atom, TRUE);
  BOOL equivalence = op == PACKED_HINT_EQUIVALENCE;
  BOOL query_anyconst = MATCH_HINTS_ANYCONST && AnyConstsEnabled &&
                        hint_contains_anyconst(c);
  for (lit = c->literals; lit != NULL; lit = lit->next) {
    unsigned long long mask = packed_term_feature_mask(lit->atom, FALSE);
    if (lit->sign) {
      positive++;
      positive_mask |= mask;
    }
    else {
      negative++;
      negative_mask |= mask;
    }
  }
  packed_begin_candidates();
  if (equivalence) {
    unsigned long long key = better_equivalence_key(
      positive_mask, negative_mask, positive, negative,
      positive + negative);
    unsigned position = Better_equivalence_bucket_capacity == 0 ? UINT_MAX :
      Better_equivalence_buckets[(unsigned) key &
        (Better_equivalence_bucket_capacity - 1)];
    while (position != UINT_MAX) {
      unsigned id = Better_equivalence_references[position].id;
      Packed_operation_stats[op].posting_candidates++;
      if (id == 0 || id >= Packed_hint_capacity ||
          !Packed_hint_active[id])
        Packed_operation_stats[op].stale_skips++;
      else
        packed_add_candidate(id);
      position = Better_equivalence_references[position].next;
    }
  }
  else if (first != NULL && !query_anyconst) {
    unsigned kind = first->sign ? BETTER_FEATURE_MATCH_POS :
                                    BETTER_FEATURE_MATCH_NEG;
    better_scratch_clear();
    better_collect_relative_features(first->atom, kind, 0, 0,
                                     Fast_packed_index ?
                                       FAST_MATCH_FEATURE_DEPTH :
                                       BETTER_MATCH_FEATURE_DEPTH);
    if (Fast_packed_index) {
      fast_eligible = positive <= USHRT_MAX && negative <= USHRT_MAX &&
                      fast_canonical_profile(fast_keys, &fast_key_count);
      if (!fast_eligible && !Hint_preview_active) {
        Fast_cache_queries++;
        Fast_cache_key_overflow++;
      }
      if (fast_eligible && fast_cache_lookup(
            fast_keys, fast_key_count, first_mask, positive, negative)) {
        packed_operation_candidates(op);
        return;
      }
      if (fast_eligible)
        posting_candidates_before =
          Packed_operation_stats[op].posting_candidates;
    }
    if (!Fast_packed_index ||
        !fast_dense_collect_candidates(
          Better_key_scratch, Better_key_scratch_count, op, TRUE))
      better_intersect_scratch_candidates(op, TRUE, FALSE);
  }

  /* AnyConst can stand on either side of match_hints.  A query containing it
     can therefore match any active hint; otherwise all AnyConst hints must be
     admitted even though their concrete structural keys are unknown. */
  if (!equivalence && query_anyconst) {
    for (i = 1; i < Packed_hint_capacity; i++) {
      if (Packed_hint_active[i]) {
        Packed_operation_stats[op].posting_candidates++;
        packed_add_candidate(i);
      }
    }
  }
  packed_finish_candidates(!equivalence && !query_anyconst &&
                           MATCH_HINTS_ANYCONST, op);
  keep = 0;
  for (i = 0; i < Packed_candidates_count; i++) {
    unsigned id = Packed_candidates[i];
    unsigned long long stored = first != NULL && first->sign ?
      Packed_hint_pos_features[id] : Packed_hint_neg_features[id];
    BOOL profile_ok = equivalence ?
      (Better_hint_positive_count[id] != 0) == (positive != 0) &&
        (Better_hint_negative_count[id] != 0) == (negative != 0) :
      Better_hint_positive_count[id] >= positive &&
        Better_hint_negative_count[id] >= negative;
    BOOL feature_ok = equivalence ?
      Packed_hint_pos_features[id] == positive_mask &&
        Packed_hint_neg_features[id] == negative_mask :
      first_mask == 0 || query_anyconst || Packed_hint_anyconst[id] ||
        (stored & first_mask) == first_mask;
    if (profile_ok && feature_ok)
      Packed_candidates[keep++] = id;
  }
  Packed_candidates_count = keep;
  if (fast_eligible)
    fast_cache_store(
      fast_keys, fast_key_count, first_mask, positive, negative,
      Packed_operation_stats[op].posting_candidates -
        posting_candidates_before);
  packed_operation_candidates(op);
}

static void packed_collect_clause_candidates(Topform c,
                                             enum packed_hint_operation op)
{
  if (Better_packed_index) {
    better_collect_clause_candidates(c, op);
    return;
  }
  Literals first = c->literals;
  packed_begin_candidates();
  if (first != NULL)
    packed_collect_term_candidates(first->atom, first->sign ? 1 : 0, op);
  packed_finish_candidates(MATCH_HINTS_ANYCONST, op);
  packed_operation_candidates(op);
}

static Topform packed_find_equivalent_hint(Topform c)
{
  unsigned i;
  int nc = number_of_literals(c->literals);
  enum packed_hint_operation op = PACKED_HINT_EQUIVALENCE;
  packed_operation_begin(op);
  packed_collect_clause_candidates(c, op);
  for (i = 0; i < Packed_candidates_count; i++) {
    Topform h = Packed_hint_by_id[Packed_candidates[i]];
    BOOL was_compressed;
    BOOL c_sub_h = FALSE, h_sub_c = FALSE;
    BOOL direct = FALSE;
    if (h == NULL || h == c)
      continue;
    was_compressed = h->compressed != NULL;
    if (Fast_packed_index && was_compressed && nc == 1 &&
        Better_hint_positive_count[h->id] +
          Better_hint_negative_count[h->id] == 1 &&
        !Packed_hint_anyconst[h->id] &&
        !(MATCH_HINTS_ANYCONST && AnyConstsEnabled &&
          hint_contains_anyconst(c))) {
      Packed_operation_stats[op].direct_attempts++;
      direct = compressed_unit_target_matches(c->literals, h, &c_sub_h);
      if (direct && c_sub_h)
        direct = compressed_unit_pattern_matches(h, c->literals, &h_sub_c);
      if (direct) {
        Packed_operation_stats[op].direct_handled++;
        if (c_sub_h)
          Packed_operation_stats[op].direct_matches++;
        if (c_sub_h && h_sub_c)
          Packed_operation_stats[op].direct_equivalences++;
      }
    }
    if (!direct && was_compressed) {
      Packed_operation_stats[op].materializations++;
      if (!materialize_clause(h))
        fatal_error("packed_find_equivalent_hint: invalid packed hint");
    }
    if (!direct) {
      Packed_candidate_checks++;
      c_sub_h = nc <= number_of_literals(h->literals) && subsumes(c, h);
      h_sub_c = c_sub_h && subsumes(h, c);
      if (was_compressed && !recompress_clause(h))
        fatal_error("packed_find_equivalent_hint: cannot recompress hint");
    }
    else
      Packed_candidate_checks++;
    if (h_sub_c) {
      Packed_operation_stats[op].exact_positives++;
      packed_operation_end(op);
      return h;
    }
  }
  packed_operation_end(op);
  return NULL;
}

static Topform packed_find_matching_hint(Topform c, BOOL flipped)
{
  unsigned i;
  int nc = number_of_literals(c->literals);
  Topform match_hint = NULL;
  enum packed_hint_operation op = flipped ? PACKED_HINT_FLIPPED_MATCH :
                                            PACKED_HINT_MATCH;
  packed_operation_begin(op);
  packed_collect_clause_candidates(c, op);
  /* Legacy back_subsume() returns hints in decreasing clause-ID order.
     It chooses the first equivalent hint, or the last proper subsumee.
     Preserve that tie-breaking exactly, independent of trie traversal. */
  for (i = 0; i < Packed_candidates_count; i++) {
    Topform h = Packed_hint_by_id[Packed_candidates[i]];
    BOOL was_compressed;
    BOOL c_sub_h = FALSE, equivalent = FALSE;
    BOOL direct = FALSE;
    if (h == NULL || h == c)
      continue;
    was_compressed = h->compressed != NULL;
    if (Fast_packed_index && was_compressed && nc == 1 &&
        Better_hint_positive_count[h->id] +
          Better_hint_negative_count[h->id] == 1 &&
        !Packed_hint_anyconst[h->id] &&
        !(MATCH_HINTS_ANYCONST && AnyConstsEnabled &&
          hint_contains_anyconst(c))) {
      BOOL h_sub_c = FALSE;
      Packed_operation_stats[op].direct_attempts++;
      direct = compressed_unit_target_matches(c->literals, h, &c_sub_h);
      if (direct && c_sub_h)
        direct = compressed_unit_pattern_matches(h, c->literals, &h_sub_c);
      if (direct) {
        equivalent = c_sub_h && h_sub_c;
        Packed_operation_stats[op].direct_handled++;
        if (c_sub_h)
          Packed_operation_stats[op].direct_matches++;
        if (equivalent)
          Packed_operation_stats[op].direct_equivalences++;
      }
    }
    if (!direct && was_compressed) {
      Packed_operation_stats[op].materializations++;
      if (!materialize_clause(h))
        fatal_error("packed_find_matching_hint: invalid packed hint");
    }
    if (!direct) {
      Packed_candidate_checks++;
      c_sub_h = nc <= number_of_literals(h->literals) && subsumes(c, h);
      equivalent = c_sub_h && subsumes(h, c);
    }
    else
      Packed_candidate_checks++;
    if (c_sub_h) {
      Packed_operation_stats[op].exact_positives++;
      match_hint = h;
    }
    if (!direct && was_compressed && !recompress_clause(h))
      fatal_error("packed_find_matching_hint: cannot recompress hint");
    if (equivalent)
      break;
  }
  packed_operation_end(op);
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
 *   hint_contains_anyconst() -- check for generic or numbered _AnyConst
 *
 *************/

static BOOL term_contains_anyconst(Term t)
{
  int i;
  if (VARIABLE(t))
    return FALSE;
  if (ARITY(t) == 0 && any_const(SYMNUM(t)) != -1)
    return TRUE;
  for (i = 0; i < ARITY(t); i++) {
    if (term_contains_anyconst(ARG(t,i)))
      return TRUE;
  }
  return FALSE;
}

static BOOL hint_contains_anyconst(Topform c)
{
  Literals lit;
  for (lit = c->literals; lit; lit = lit->next) {
    if (lit->atom != NULL && term_contains_anyconst(lit->atom))
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
      better_index_hint_terms(c, anyconst);
      if (compress_clause(c) == CLAUSE_COMPRESS_INVALID)
        fatal_error("index_hint: cannot compact active packed hint");
    }
    else
      lindex_update(Hints_idx, c, INSERT);
    if (Back_demod_hints && !Packed_index) {
      /* Do not index hints containing any _AnyConst for back-demod.
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
      if (c->id < Packed_hint_capacity) {
        Packed_hint_active[c->id] = 0;
        better_deactivate_hint((unsigned) c->id);
      }
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
  Topform hint = Packed_index ? packed_find_matching_hint(c, FALSE) :
                                find_matching_hint(c, Hints_idx);

  if (hint == NULL &&
      unit_clause(c->literals) &&
      eq_term(c->literals->atom) &&
      !oriented_eq(c->literals->atom)) {

    /* Try to find a hint that matches the flipped equality. */

    Term save_atom = c->literals->atom;
    c->literals->atom = top_flip(save_atom);
    hint = Packed_index ? packed_find_matching_hint(c, TRUE) :
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
 *   preview_weight_with_hints()
 *
 *   Exact, side-effect-free hint/weight query for scheduler ranking.  Keep
 *   all policy in lockstep with adjust_weight_with_hints(), but deliberately
 *   omit labels, matching_hint assignment, and flip labels.  Packed-index
 *   accounting is restored after the query; its reusable candidate scratch
 *   storage is not persistent hint state.
 *
 *************/

/* PUBLIC */
Topform preview_weight_with_hints(Topform c,
				  double raw_weight,
				  BOOL degrade,
				  BOOL breadth_first_hints,
				  double *adjusted_weight,
				  BOOL *flipped)
{
  struct packed_hint_operation_stats saved_stats[PACKED_HINT_OPERATIONS];
  unsigned long long saved_checks = Packed_candidate_checks;
  unsigned *saved_candidate_mark = NULL, *saved_candidates = NULL;
  unsigned saved_candidate_serial = 0, saved_candidates_count = 0;
  unsigned saved_candidates_capacity = 0;
  unsigned *saved_intersection_member = NULL;
  unsigned *saved_intersection_match = NULL;
  unsigned *saved_intersection_ids = NULL;
  unsigned saved_intersection_serial = 0, saved_match_serial = 0;
  unsigned saved_intersection_count = 0, saved_intersection_capacity = 0;
  unsigned long long *saved_key_scratch = NULL;
  unsigned saved_key_scratch_count = 0, saved_key_scratch_capacity = 0;
  Topform hint;
  BOOL flip_match = FALSE;
  double weight = raw_weight;

  if (Hint_preview_active)
    fatal_error("preview_weight_with_hints: nested preview");
  memcpy(saved_stats, Packed_operation_stats, sizeof(saved_stats));
  Hint_preview_active = TRUE;

  /* Packed retrieval uses global arrays only as reusable query workspace.
     Lazily reserve preview-owned scratch, then temporarily swap it in so
     authoritative serial marks, capacities, and scratch objects are exactly
     unchanged when this function returns. */
  if (Packed_index) {
    packed_reserve_preview_workspace();
    saved_candidate_mark = Packed_candidate_mark;
    saved_candidates = Packed_candidates;
    saved_candidate_serial = Packed_candidate_serial;
    saved_candidates_count = Packed_candidates_count;
    saved_candidates_capacity = Packed_candidates_capacity;
    Packed_candidate_mark = Preview_candidate_mark;
    Packed_candidates = Preview_candidates;
    Packed_candidate_serial = Preview_candidate_serial;
    Packed_candidates_count = Preview_candidates_count;
    Packed_candidates_capacity = Preview_candidates_capacity;
    if (Better_packed_index) {
      saved_intersection_member = Better_intersection_member;
      saved_intersection_match = Better_intersection_match;
      saved_intersection_ids = Better_intersection_ids;
      saved_intersection_serial = Better_intersection_serial;
      saved_match_serial = Better_match_serial;
      saved_intersection_count = Better_intersection_count;
      saved_intersection_capacity = Better_intersection_capacity;
      saved_key_scratch = Better_key_scratch;
      saved_key_scratch_count = Better_key_scratch_count;
      saved_key_scratch_capacity = Better_key_scratch_capacity;
      Better_intersection_member = Preview_intersection_member;
      Better_intersection_match = Preview_intersection_match;
      Better_intersection_ids = Preview_intersection_ids;
      Better_intersection_serial = Preview_intersection_serial;
      Better_match_serial = Preview_match_serial;
      Better_intersection_count = Preview_intersection_count;
      Better_intersection_capacity = Preview_intersection_capacity;
      Better_key_scratch = Preview_key_scratch;
      Better_key_scratch_count = Preview_key_scratch_count;
      Better_key_scratch_capacity = Preview_key_scratch_capacity;
    }
  }

  hint = Packed_index ? packed_find_matching_hint(c, FALSE) :
                        find_matching_hint(c, Hints_idx);
  if (hint == NULL &&
      unit_clause(c->literals) &&
      eq_term(c->literals->atom) &&
      !oriented_eq(c->literals->atom)) {
    Term save_atom = c->literals->atom;
    c->literals->atom = top_flip(save_atom);
    hint = Packed_index ? packed_find_matching_hint(c, TRUE) :
                          find_matching_hint(c, Hints_idx);
    zap_top_flip(c->literals->atom);
    c->literals->atom = save_atom;
    flip_match = hint != NULL;
  }

  if (Packed_index) {
    Preview_candidate_mark = Packed_candidate_mark;
    Preview_candidates = Packed_candidates;
    Preview_candidate_serial = Packed_candidate_serial;
    Preview_candidates_count = Packed_candidates_count;
    Preview_candidates_capacity = Packed_candidates_capacity;
    Packed_candidate_mark = saved_candidate_mark;
    Packed_candidates = saved_candidates;
    Packed_candidate_serial = saved_candidate_serial;
    Packed_candidates_count = saved_candidates_count;
    Packed_candidates_capacity = saved_candidates_capacity;
    if (Better_packed_index) {
      Preview_intersection_member = Better_intersection_member;
      Preview_intersection_match = Better_intersection_match;
      Preview_intersection_ids = Better_intersection_ids;
      Preview_intersection_serial = Better_intersection_serial;
      Preview_match_serial = Better_match_serial;
      Preview_intersection_count = Better_intersection_count;
      Preview_intersection_capacity = Better_intersection_capacity;
      Preview_key_scratch = Better_key_scratch;
      Preview_key_scratch_count = Better_key_scratch_count;
      Preview_key_scratch_capacity = Better_key_scratch_capacity;
      Better_intersection_member = saved_intersection_member;
      Better_intersection_match = saved_intersection_match;
      Better_intersection_ids = saved_intersection_ids;
      Better_intersection_serial = saved_intersection_serial;
      Better_match_serial = saved_match_serial;
      Better_intersection_count = saved_intersection_count;
      Better_intersection_capacity = saved_intersection_capacity;
      Better_key_scratch = saved_key_scratch;
      Better_key_scratch_count = saved_key_scratch_count;
      Better_key_scratch_capacity = saved_key_scratch_capacity;
    }
  }
  Hint_preview_active = FALSE;
  memcpy(Packed_operation_stats, saved_stats, sizeof(saved_stats));
  Packed_candidate_checks = saved_checks;

  if (hint != NULL) {
    int bsub_wt = get_int_attribute(hint->attributes, Bsub_wt_attr, 1);
    if (bsub_wt != INT_MAX)
      weight = bsub_wt;
    else if (breadth_first_hints)
      weight = 0;
    if (degrade)
      weight += hint->weight * 1000;
  }
  if (adjusted_weight != NULL)
    *adjusted_weight = weight;
  if (flipped != NULL)
    *flipped = flip_match;
  return hint;
}  /* preview_weight_with_hints */

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
      if (hint->id < Packed_hint_capacity) {
        Packed_hint_active[hint->id] = 0;
        better_deactivate_hint((unsigned) hint->id);
      }
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
    enum packed_hint_operation op = PACKED_HINT_BACK_DEMOD;
    packed_operation_begin(op);
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
    if (Better_packed_index) {
      if (type == ORIENTED || type == LEX_DEP_LR || type == LEX_DEP_BOTH)
        better_collect_back_pattern_candidates(alpha, op);
      if (type == LEX_DEP_RL || type == LEX_DEP_BOTH)
        better_collect_back_pattern_candidates(beta, op);
    }
    else {
      for (i = 1; i < Packed_hint_capacity; i++) {
        if (Packed_hint_active[i])
          Packed_operation_stats[op].posting_candidates++;
        if (Packed_hint_active[i] &&
            (wanted == ULLONG_MAX ||
             (Packed_hint_rewrite_symbols[i] & wanted) != 0))
          packed_add_candidate(i);
      }
    }
    packed_finish_candidates(FALSE, op);
    packed_operation_candidates(op);
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
          (MATCH_HINTS_ANYCONST && Packed_hint_anyconst[id])) {
        if (hint == NULL || !Packed_hint_active[id])
          Packed_operation_stats[op].stale_skips++;
        continue;
      }
      if (hint->compressed != NULL) {
        Packed_operation_stats[op].materializations++;
        if (!materialize_clause(hint))
          fatal_error("back_demod_hints: invalid packed hint");
      }
      if (!rewritable_clause_type(demod, hint, type, lex_order_vars)) {
        if (compress_clause(hint) == CLAUSE_COMPRESS_INVALID)
          fatal_error("back_demod_hints: cannot recompress filtered hint");
        continue;
      }
      Packed_operation_stats[op].exact_positives++;
      before = copy_clause(hint);
      (*Demod_proc)(hint, 1000, 1000, FALSE, lex_order_vars);
      changed = !clause_ident(before->literals, hint->literals);
      zap_topform(before);
      if (changed) {
        Packed_operation_stats[op].rewrites++;
        unindex_hint(hint);
        orient_equalities(hint, TRUE);
        simplify_literals2(hint);
        merge_literals(hint);
        renumber_variables(hint, MAX_VARS);
        index_hint(hint);
        Packed_operation_stats[op].reindexes++;
        hint->weight = 0;
      }
      else if (compress_clause(hint) == CLAUSE_COMPRESS_INVALID)
        fatal_error("back_demod_hints: cannot recompress packed hint");
      discard_packed_hint_proof(hint);
    }
    if (candidate_ids != NULL)
      safe_free(candidate_ids);
    packed_operation_end(op);
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
  struct hint_postings_stats posting_stats;
  *node_bytes = 0;
  *reference_bytes = Packed_feature_bitsets != NULL ?
    (unsigned long long) 128 * Packed_feature_words *
      sizeof(unsigned long long) : 0;
  *table_bytes = Packed_index ?
    (unsigned long long) Packed_hint_capacity *
      (sizeof(Topform) + 2 * sizeof(unsigned char) + sizeof(unsigned) +
       2 * sizeof(unsigned long long)) +
    (Packed_hint_rewrite_symbols == NULL ? 0 :
      (unsigned long long) Packed_hint_capacity *
        sizeof(unsigned long long)) +
    (unsigned long long) Packed_candidates_capacity * sizeof(unsigned) : 0;
  *candidate_checks = Packed_candidate_checks;
  if (Better_packed_index) {
    hint_postings_get_stats(Better_postings, &posting_stats);
    *node_bytes += posting_stats.table_bytes;
    *reference_bytes += posting_stats.reference_bytes +
      posting_stats.dense_bit_bytes + posting_stats.dense_summary_bytes +
      (unsigned long long) Better_equivalence_reference_capacity *
        sizeof(struct better_equivalence_reference);
    *table_bytes += (unsigned long long) Packed_hint_capacity *
      (3 * sizeof(unsigned) + sizeof(unsigned long long) +
       2 * sizeof(unsigned short)) +
      (unsigned long long) Better_key_scratch_capacity *
        sizeof(unsigned long long) +
      (unsigned long long) Better_intersection_capacity * sizeof(unsigned) +
      (unsigned long long) Better_equivalence_bucket_capacity *
        sizeof(unsigned) +
      (unsigned long long) Better_anyconst_reference_capacity *
        sizeof(unsigned);
  }
  if (Fast_packed_index)
    *table_bytes += (unsigned long long) FAST_MATCH_CACHE_CAPACITY *
      sizeof(struct fast_match_cache_entry);
}

static unsigned long long packed_preview_workspace_bytes(void)
{
  unsigned long long bytes = 0;
  if (Preview_candidate_mark != NULL)
    bytes += (unsigned long long) Packed_hint_capacity * sizeof(unsigned);
  if (Preview_candidates != NULL)
    bytes += (unsigned long long) Preview_candidates_capacity *
             sizeof(unsigned);
  if (Preview_intersection_member != NULL)
    bytes += (unsigned long long) Packed_hint_capacity *
             2 * sizeof(unsigned);
  if (Preview_intersection_ids != NULL)
    bytes += (unsigned long long) Preview_intersection_capacity *
             sizeof(unsigned);
  if (Preview_key_scratch != NULL)
    bytes += (unsigned long long) Preview_key_scratch_capacity *
             sizeof(unsigned long long);
  return bytes;
}  /* packed_preview_workspace_bytes */

/* PUBLIC */
void fprint_packed_hint_operation_stats(FILE *fp)
{
  unsigned i;
  if (!Packed_index)
    return;
  for (i = 0; i < PACKED_HINT_OPERATIONS; i++) {
    struct packed_hint_operation_stats *s = Packed_operation_stats + i;
    fprintf(fp,
            "Packed_hint_operation: op=%s, seconds=%.3f, queries=%llu, "
            "posting_lists=%llu, posting_candidates=%llu, "
            "unique_candidates=%llu, mean=%.2f, "
            "max=%llu, materialized=%llu, exact_positive=%llu, rewrites=%llu, "
            "reindexes=%llu, stale_skips=%llu, fingerprint_rejects=%llu, "
            "direct_attempts=%llu, direct_handled=%llu, "
            "direct_matches=%llu, direct_equivalences=%llu, "
            "buckets=0:%llu/1:%llu/2-7:%llu/8-31:%llu/32-127:%llu/"
            "128-1023:%llu/1024-16383:%llu/16384+:%llu.\n",
            Packed_operation_names[i], clock_seconds(s->clock), s->queries,
            s->posting_lists, s->posting_candidates, s->unique_candidates,
            s->queries == 0 ? 0.0 :
              (double) s->unique_candidates / (double) s->queries,
            s->candidate_max, s->materializations, s->exact_positives,
            s->rewrites, s->reindexes, s->stale_skips,
            s->fingerprint_rejects,
            s->direct_attempts, s->direct_handled,
            s->direct_matches, s->direct_equivalences,
            s->candidate_buckets[0], s->candidate_buckets[1],
            s->candidate_buckets[2], s->candidate_buckets[3],
            s->candidate_buckets[4], s->candidate_buckets[5],
            s->candidate_buckets[6], s->candidate_buckets[7]);
  }
  fprintf(fp,
          "Packed_hint_preview_workspace: initialized=%d, bytes=%llu.\n",
          Preview_candidate_mark != NULL,
          packed_preview_workspace_bytes());
  if (Better_packed_index) {
    struct hint_postings_stats s;
    unsigned long long equivalence_stale;
    unsigned long long table_bytes;
    unsigned long long reference_bytes;
    hint_postings_get_stats(Better_postings, &s);
    if (Better_equivalence_reference_count < Better_equivalence_live_count)
      fatal_error("fprint_packed_hint_operation_stats: reference underflow");
    equivalence_stale = Better_equivalence_reference_count -
                        Better_equivalence_live_count;
    table_bytes = s.table_bytes +
      (unsigned long long) Better_equivalence_bucket_capacity *
        sizeof(unsigned) +
      (unsigned long long) Better_anyconst_reference_capacity *
        sizeof(unsigned);
    reference_bytes = s.reference_bytes +
      (unsigned long long) Better_equivalence_reference_capacity *
        sizeof(struct better_equivalence_reference);
    fprintf(fp,
            "Better_packed_postings: keys=%llu, references=%llu, "
            "live_features=%llu, stale_features=%llu, max_posting=%llu, "
            "equivalence_buckets=%u, equivalence_references=%u, "
            "equivalence_live=%llu, equivalence_stale=%llu, "
            "anyconst_references=%u, anyconst_live=%llu, "
            "table_bytes=%llu, reference_bytes=%llu, fingerprint_bytes=%llu, "
            "dense_keys=%llu, dense_bit_bytes=%llu, "
            "dense_summary_bytes=%llu, "
            "rebuilds=%llu, "
            "rebuilt_references=%llu, rebuild_materializations=%llu.\n",
            s.keys, s.references, Better_feature_live_count,
            s.references - Better_feature_live_count,
            s.maximum_posting, Better_equivalence_bucket_capacity,
            Better_equivalence_reference_count,
            Better_equivalence_live_count,
            equivalence_stale,
            Better_anyconst_reference_count, Better_anyconst_live_count,
            table_bytes, reference_bytes,
            (unsigned long long) Packed_hint_capacity *
              sizeof(unsigned long long),
            s.dense_keys, s.dense_bit_bytes, s.dense_summary_bytes,
            Better_posting_rebuilds, Better_posting_rebuild_refs,
            Better_posting_rebuild_materializations);
  }
  if (Fast_packed_index) {
    fprintf(fp,
            "Packed_fast_cache: entries=%u, entry_bytes=%llu, "
            "table_bytes=%llu, queries=%llu, eligible=%llu, hits=%llu, "
            "misses=%llu, hit_rate=%.2f, stores=%llu, key_overflow=%llu, "
            "candidate_overflow=%llu, dependency_misses=%llu, "
            "profile_misses=%llu, posting_candidates_avoided=%llu.\n",
            FAST_MATCH_CACHE_CAPACITY,
            (unsigned long long) sizeof(struct fast_match_cache_entry),
            (unsigned long long) FAST_MATCH_CACHE_CAPACITY *
              sizeof(struct fast_match_cache_entry),
            Fast_cache_queries, Fast_cache_eligible, Fast_cache_hits,
            Fast_cache_misses,
            Fast_cache_eligible == 0 ? 0.0 :
              100.0 * (double) Fast_cache_hits /
                (double) Fast_cache_eligible,
            Fast_cache_stores, Fast_cache_key_overflow,
            Fast_cache_candidate_overflow,
            Fast_cache_dependency_misses, Fast_cache_profile_misses,
            Fast_cache_posting_candidates_avoided);
    fprintf(fp,
            "Packed_fast_dense: threshold=%u, queries=%llu, used=%llu, "
            "sparse_used=%llu, sparse_seed_ids=%llu, "
            "sparse_feature_tests=%llu, sparse_rejects=%llu, "
            "seed_ids_avoided=%llu, summary_words=%llu, data_words=%llu, "
            "result_ids=%llu.\n",
            FAST_DENSE_MIN_POSTING, Fast_dense_queries, Fast_dense_used,
            Fast_sparse_used, Fast_sparse_seed_ids,
            Fast_sparse_feature_tests, Fast_sparse_rejects,
            Fast_dense_seed_ids_avoided, Fast_dense_summary_words,
            Fast_dense_data_words, Fast_dense_result_ids);
  }
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
 *   matched_hints()
 *
 *   Return the number of input hints that have been matched at least once.
 *   Hint weights are the authoritative cumulative match counters.
 *
 *************/

/* PUBLIC */
int matched_hints(Clist hint_list)
{
  int n = 0;
  Clist_pos p;

  if (hint_list == NULL)
    return 0;
  for (p = hint_list->first; p; p = p->next) {
    if (p->c->weight > 0)
      n++;
  }
  return n;
}  /* matched_hints */

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
  total = matched_hints(hint_list);

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
