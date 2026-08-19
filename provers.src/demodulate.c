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

#include "demodulate.h"
#include "compact_back_demod.h"

/* Private definitions and types */

/* Maintain the indexes for forward and backward demodulation.
 */

static Mindex Demod_idx;
static Mindex Back_demod_idx;
static Compact_back_demod_index Compact_back_demod_idx;
static Compact_term_pool Compact_back_demod_terms;
static BOOL Compact_back_demod_audit;
static BOOL Compact_back_demod_authoritative;
static unsigned long long Compact_back_demod_failures;
static struct compact_query_timer Compact_back_demod_exact_timer;
static struct compact_query_timer Compact_back_demod_materialize_timer;
static Compact_back_demod_resolver Compact_back_demod_resolve;
static Compact_back_demod_releaser Compact_back_demod_release;
static Compact_back_demod_batch_adviser Compact_back_demod_advise;
static void *Compact_back_demod_context;

/* PUBLIC */
void configure_compact_back_demod_access(
  Compact_back_demod_resolver resolver,
  Compact_back_demod_releaser releaser,
  Compact_back_demod_batch_adviser adviser,
  void *context)
{
  Compact_back_demod_resolve = resolver;
  Compact_back_demod_release = releaser;
  Compact_back_demod_advise = adviser;
  Compact_back_demod_context = context;
}

/* PUBLIC */
void configure_compact_back_demod(BOOL audit, BOOL authoritative)
{
  if (Back_demod_idx != NULL || Compact_back_demod_idx != NULL)
    fatal_error("configure_compact_back_demod: index is live");
  if (audit && authoritative)
    fatal_error("configure_compact_back_demod: audit and authoritative modes conflict");
  Compact_back_demod_audit = audit;
  Compact_back_demod_authoritative = authoritative;
  Compact_back_demod_failures = 0;
}

void configure_compact_back_demod_term_pool(Compact_term_pool pool)
{
  if (Compact_back_demod_idx != NULL)
    fatal_error("configure_compact_back_demod_term_pool: index is live");
  Compact_back_demod_terms = pool;
}

void configure_compact_back_demod_stale_pct(unsigned percentage)
{
  compact_back_demod_set_compaction_stale_pct(percentage);
}

void configure_compact_back_demod_strategy(
  Compact_back_demod_strategy strategy)
{
  compact_back_demod_set_strategy(strategy);
}

void configure_compact_back_demod_tree(unsigned min_tokens,
                                       unsigned budget_kb,
                                       unsigned budget_pct,
                                       unsigned admit_work,
                                       unsigned build_factor)
{
  compact_back_demod_set_tree_min_tokens(min_tokens);
  compact_back_demod_set_tree_budget_kb(budget_kb);
  compact_back_demod_set_tree_budget_pct(budget_pct);
  compact_back_demod_set_tree_admit_work(admit_work);
  compact_back_demod_set_tree_build_factor(build_factor);
}

void configure_compact_back_demod_position(unsigned admit_work,
                                           unsigned min_gain,
                                           unsigned build_factor,
                                           unsigned budget_kb,
                                           unsigned budget_pct,
                                           BOOL admission_enabled,
                                           BOOL sparse_positions)
{
  compact_back_demod_set_position_options(
    admit_work, min_gain, build_factor, budget_kb, budget_pct,
    admission_enabled, sparse_positions);
}

void configure_compact_back_demod_eager_position_depth(unsigned depth)
{
  compact_back_demod_set_eager_position_depth(depth);
}

void configure_compact_back_demod_edge_filter(BOOL enabled)
{
  compact_back_demod_set_edge_filter(enabled);
}

static BOOL compact_back_demod_mode(void)
{
  return Compact_back_demod_audit || Compact_back_demod_authoritative;
}

static Topform compact_back_demod_materialize_clause(
  unsigned long long id, void *context)
{
  Topform clause = find_clause_by_id(id);
  (void) context;
  if (clause == NULL && Compact_back_demod_resolve != NULL)
    clause = Compact_back_demod_resolve(id, Compact_back_demod_context);
  return clause;
}

static void compact_back_demod_release_materialized(Topform clause,
                                                     void *context)
{
  (void) context;
  if (Compact_back_demod_release != NULL)
    Compact_back_demod_release(clause, Compact_back_demod_context);
}

static void compact_back_demod_advise_materialized_batch(
  const unsigned long long *ids, size_t count, void *context)
{
  (void) context;
  if (Compact_back_demod_advise != NULL)
    Compact_back_demod_advise(ids, count, Compact_back_demod_context);
}

unsigned long long compact_back_demod_active_count(void)
{
  return compact_back_demod_active_records(Compact_back_demod_idx);
}

unsigned long long compact_back_demod_physical_count(void)
{
  return compact_back_demod_physical_records(Compact_back_demod_idx);
}

void compact_back_demod_compact_all_stale_records(void)
{
  compact_back_demod_compact_all_stale(Compact_back_demod_idx);
}

void compact_back_demod_copy_term_clauses(Compact_term_pool destination,
                                          Compact_term_rebase_map map)
{
  compact_back_demod_copy_live_clauses(
    Compact_back_demod_idx, destination, map);
}

void compact_back_demod_retain_term_clauses(Compact_term_rebase_map map)
{
  compact_back_demod_retain_live_clauses(Compact_back_demod_idx, map);
}

void compact_back_demod_rebase_shared_term_pool(
  Compact_term_pool pool, Compact_term_rebase_map map)
{
  compact_back_demod_rebase_term_pool(Compact_back_demod_idx, pool, map);
  Compact_back_demod_terms = pool;
}

BOOL write_compact_back_demod_adaptive_state(const char *directory)
{
  return Compact_back_demod_idx == NULL ||
    compact_back_demod_write_adaptive_state(
      Compact_back_demod_idx, directory);
}

BOOL restore_compact_back_demod_adaptive_state(const char *directory)
{
  return Compact_back_demod_idx == NULL ||
    compact_back_demod_read_adaptive_state(
      Compact_back_demod_idx, directory);
}

/*************
 *
 *   init_demodulator_index()
 *
 *************/

/* DOCUMENTATION
*/

/* PUBLIC */
void init_demodulator_index(Mindextype mtype, Uniftype utype, int fpa_depth)
{
  Demod_idx = mindex_init(mtype, utype, fpa_depth);
}  /* init_demodulator_index */

/*************
 *
 *   init_back_demod_index()
 *
 *************/

/* DOCUMENTATION
*/

/* PUBLIC */
void init_back_demod_index(Mindextype mtype, Uniftype utype, int fpa_depth)
{
  Back_demod_idx = Compact_back_demod_authoritative ? NULL :
    mindex_init(mtype, utype, fpa_depth);
  Compact_back_demod_idx = compact_back_demod_mode() ?
    (Compact_back_demod_terms == NULL ? compact_back_demod_init() :
     compact_back_demod_init_with_pool(Compact_back_demod_terms)) : NULL;
  if (compact_back_demod_mode()) {
    memset(&Compact_back_demod_exact_timer, 0,
           sizeof(Compact_back_demod_exact_timer));
    memset(&Compact_back_demod_materialize_timer, 0,
           sizeof(Compact_back_demod_materialize_timer));
  }
}  /* init_back_demod_index */

/*************
 *
 *   index_demodulator()
 *
 *************/

/* DOCUMENTATION
*/

/* PUBLIC */
void index_demodulator(Topform c, int type, Indexop operation, Clock clock)
{
  clock_start(clock);
  idx_demodulator(c, type, operation, Demod_idx);
  clock_stop(clock);
}  /* index_demodulator */

/*************
 *
 *   index_back_demod()
 *
 *************/

/* DOCUMENTATION
*/

/* PUBLIC */
void index_back_demod(Topform c, Indexop operation, Clock clock, BOOL enabled)
{
  if (enabled) {
    clock_start(clock);
    if (compact_back_demod_mode()) {
      BOOL ok = operation == INSERT ?
        compact_back_demod_add(Compact_back_demod_idx, c) :
        compact_back_demod_remove(Compact_back_demod_idx, c->id);
      if (!ok)
        fatal_error(operation == INSERT ?
          "index_back_demod: duplicate compact clause" :
          "index_back_demod: missing compact clause");
      if (operation == DELETE &&
          compact_back_demod_compaction_needed(Compact_back_demod_idx)) {
        if (Compact_back_demod_resolve != NULL)
          compact_back_demod_compact_materialized(
            Compact_back_demod_idx,
            compact_back_demod_materialize_clause,
            compact_back_demod_release_materialized,
            compact_back_demod_advise_materialized_batch, NULL);
        else
          compact_back_demod_compact(Compact_back_demod_idx);
      }
    }
    if (!Compact_back_demod_authoritative)
      index_clause_back_demod(c, Back_demod_idx, operation);
    clock_stop(clock);
  }
}  /* index_back_demod */

/* PUBLIC */
BOOL unindex_compact_back_demod_id(unsigned long long id, Clock clock,
                                   BOOL enabled)
{
  BOOL ok;
  if (!enabled)
    return TRUE;
  if (!Compact_back_demod_authoritative ||
      Compact_back_demod_idx == NULL || id == 0)
    return FALSE;
  clock_start(clock);
  ok = compact_back_demod_remove(Compact_back_demod_idx, id);
  if (ok && compact_back_demod_compaction_needed(Compact_back_demod_idx)) {
    if (Compact_back_demod_resolve != NULL)
      compact_back_demod_compact_materialized(
        Compact_back_demod_idx,
        compact_back_demod_materialize_clause,
        compact_back_demod_release_materialized,
        compact_back_demod_advise_materialized_batch, NULL);
    else
      compact_back_demod_compact(Compact_back_demod_idx);
  }
  clock_stop(clock);
  return ok;
}

/*************
 *
 *   write_discrim_leaves() -- iterative DFS walk of DISCRIM tree
 *
 *   Uses the same n-counter logic as zap_discrim_tree in discrim.c:
 *   n tracks remaining symbols in the flattened term representation.
 *   When n==0, the node is a leaf with u.data (Plist of entries).
 *
 *************/

static
void write_discrim_leaves(FILE *fp, Discrim root)
{
  struct frame { Discrim node; int n; };
  int cap = 256, top = 0;
  struct frame *stack = (struct frame *) safe_malloc(cap * sizeof(struct frame));

  stack[top].node = root;
  stack[top].n = 1;
  top++;

  while (top > 0) {
    Discrim cur;
    int cur_n;

    top--;
    cur = stack[top].node;
    cur_n = stack[top].n;

    if (cur_n == 0) {
      /* Leaf node: write entries in Plist order */
      Plist p;
      for (p = cur->u.data; p != NULL; p = p->next) {
        Term t = (Term) p->v;
        Topform c = (Topform) t->container;
        int side = (t == ARG(c->literals->atom, 0)) ? 0 : 1;
        fprintf(fp, "%llu %d\n", c->id, side);
      }
    }
    else {
      /* Internal node: push children in REVERSE order (so first child
         is processed first when popped from stack) */
      Discrim k;
      Discrim *kids = NULL;
      int n_kids = 0, i;

      for (k = cur->u.kids; k != NULL; k = k->next)
        n_kids++;
      if (n_kids > 0) {
        kids = (Discrim *) safe_malloc(n_kids * sizeof(Discrim));
        i = 0;
        for (k = cur->u.kids; k != NULL; k = k->next)
          kids[i++] = k;
        for (i = n_kids - 1; i >= 0; i--) {
          int arity;
          k = kids[i];
          if (k->type == AC_ARG_TYPE || k->type == AC_NV_ARG_TYPE)
            arity = 0;
          else if (DVAR(k))
            arity = 0;
          else
            arity = sn_to_arity(k->symbol);
          if (top >= cap) {
            cap *= 2;
            stack = (struct frame *) safe_realloc(stack,
                                                  cap * sizeof(struct frame));
          }
          stack[top].node = k;
          stack[top].n = cur_n + arity - 1;
          top++;
        }
        safe_free(kids);
      }
    }
  }
  safe_free(stack);
}

/*************
 *
 *   write_demod_index() -- serialize demod DISCRIM tree leaf ordering
 *
 *************/

/* PUBLIC */
void write_demod_index(const char *dir)
{
  char path[600];
  FILE *fp;
  Discrim root;

  if (Demod_idx == NULL || Demod_idx->discrim_tree == NULL)
    return;

  snprintf(path, sizeof(path), "%s/demod_index.txt", dir);
  fp = fopen(path, "w");
  if (!fp)
    return;

  root = Demod_idx->discrim_tree;
  write_discrim_leaves(fp, root);
  fclose(fp);
}  /* write_demod_index */

/*************
 *
 *   restore_demod_index() -- rebuild demod index in saved leaf-list order
 *
 *   Reads (clause_id, side) pairs from demod_index.txt and inserts the
 *   corresponding terms into the demod DISCRIM tree in exactly the
 *   saved order, reproducing the original leaf-list ordering.
 *
 *************/

/* PUBLIC */
void restore_demod_index(const char *dir, Clock clock,
                         Demodulator_resolver resolver, void *context)
{
  char path[600];
  FILE *fp;
  unsigned long long clause_id;
  int side;

  snprintf(path, sizeof(path), "%s/demod_index.txt", dir);
  fp = fopen(path, "r");
  if (!fp)
    return;  /* no saved index - fall back to default insertion order */

  {
    int count = 0;
    while (fscanf(fp, "%llu %d", &clause_id, &side) == 2) {
      /* A rewrite-only clause can deliberately share an ID with a dense
         passive body that will be destroyed during bulk archival.  Give the
         owning rewrite store first refusal so the restored index never
         captures the temporary passive Topform. */
      Topform c = resolver == NULL ? NULL : resolver(clause_id, context);
      if (c == NULL)
        c = find_clause_by_id(clause_id);
      if (c != NULL && c->literals != NULL) {
        Term t = ARG(c->literals->atom, side);
        mindex_update(Demod_idx, t, INSERT);
        count++;
      }
    }
    printf("%%   Restored demod index: %d entries from %s\n", count,
           path);
  }
  fclose(fp);
}  /* restore_demod_index */

/*************
 *
 *   destroy_demodulation_index()
 *
 *************/

/* DOCUMENTATION
*/

/* PUBLIC */
void destroy_demodulation_index(void)
{
  mindex_destroy(Demod_idx);
  Demod_idx = NULL;
}  /* destroy_demodulation_index */

/*************
 *
 *   destroy_back_demod_index()
 *
 *************/

/* DOCUMENTATION
*/

/* PUBLIC */
void destroy_back_demod_index(void)
{
  if (Back_demod_idx != NULL)
    mindex_destroy(Back_demod_idx);
  Back_demod_idx = NULL;
  compact_back_demod_free(Compact_back_demod_idx);
  Compact_back_demod_idx = NULL;
  Compact_back_demod_terms = NULL;
  memset(&Compact_back_demod_exact_timer, 0,
         sizeof(Compact_back_demod_exact_timer));
  memset(&Compact_back_demod_materialize_timer, 0,
         sizeof(Compact_back_demod_materialize_timer));
}  /* destroy_back_demod_index */

/*************
 *
 *   demodulate_clause()
 *
 *************/

/* DOCUMENTATION
Demodulate Topform c, using demodulators alreadly known to this package.
If any rewriting occurs, the justification is appended to
the clause's existing justification.
*/

/* PUBLIC */
void demodulate_clause(Topform c, int step_limit, int increase_limit,
		       BOOL print, BOOL lex_order_vars)
{
  static int limit_hits = 0;
  int starting_step_limit;

  step_limit = step_limit == -1 ? INT_MAX : step_limit;
  increase_limit = increase_limit == -1 ? INT_MAX : increase_limit;
  starting_step_limit = step_limit;

  fdemod_clause(c, Demod_idx, &step_limit, &increase_limit, lex_order_vars);

  if (step_limit == 0 || increase_limit == -1) {
    limit_hits++;
    char *mess = (step_limit == 0 ? "step" : "increase");

    if (print) {
      if (limit_hits == 1) {
	fprintf(stderr, "Demod_%s_limit (see stdout)\n", mess);
	printf("\nDemod_%s_limit: ", mess); f_clause(c);
	printf("\nDemod_%s_limit (steps=%d, size=%d).\n"
	       "The most recent kept clause is %llu.\n"
	       "From here on, a short message will be printed\n"
	       "for each 100 times the limit is hit.\n\n",
	       mess,
	       starting_step_limit - step_limit,
	       clause_symbol_count(c->literals),
	       clause_ids_assigned());
	fflush(stdout);
      }
      else if (limit_hits % 100 == 0) {
	printf("Demod_limit hit %d times.\n", limit_hits);
	fflush(stdout);
      }
    }
  }
}  /* demodulate_clause */

/* PUBLIC */
void demodulate_clause_preview(Topform c, int step_limit, int increase_limit,
			       BOOL lex_order_vars)
{
  step_limit = step_limit == -1 ? INT_MAX : step_limit;
  increase_limit = increase_limit == -1 ? INT_MAX : increase_limit;
  set_fdemod_stat_counting(FALSE);
  fdemod_clause(c, Demod_idx, &step_limit, &increase_limit, lex_order_vars);
  set_fdemod_stat_counting(TRUE);
}  /* demodulate_clause_preview */

/*************
 *
 *   back_demodulatable()
 *
 *************/

/* DOCUMENTATION
*/

/* PUBLIC */
static Plist compact_back_demodulatable(Topform demod, int type,
                                        BOOL lex_order_vars)
{
  unsigned long long *ids;
  size_t count = 0, i, successes = 0, materializations = 0;
  Plist answer = NULL, tail = NULL;
  ids = compact_back_demod_candidate_ids(
    Compact_back_demod_idx, demod, type, &count);
  for (i = 0; i < count; i++) {
    Topform candidate = find_clause_by_id(ids[i]);
    if (candidate == NULL && Compact_back_demod_resolve != NULL) {
      compact_query_timer_start(&Compact_back_demod_materialize_timer);
      candidate = Compact_back_demod_resolve(
        ids[i], Compact_back_demod_context);
      compact_query_timer_stop(&Compact_back_demod_materialize_timer);
      materializations++;
    }
    if (candidate == NULL)
      fatal_error("compact_back_demodulatable: candidate is not resident");
    compact_query_timer_start(&Compact_back_demod_exact_timer);
    if (rewritable_clause_type(demod, candidate, type, lex_order_vars)) {
      Plist cell = get_plist();
      compact_query_timer_stop(&Compact_back_demod_exact_timer);
      successes++;
      cell->v = candidate;
      cell->next = NULL;
      if (tail == NULL)
        answer = cell;
      else
        tail->next = cell;
      tail = cell;
    }
    else {
      compact_query_timer_stop(&Compact_back_demod_exact_timer);
      if (Compact_back_demod_release != NULL)
        Compact_back_demod_release(candidate, Compact_back_demod_context);
    }
  }
  compact_back_demod_note_exact_query(
    Compact_back_demod_idx, count, successes, materializations);
  safe_free(ids);
  return answer;
}

static void audit_back_demod_answer(Topform demod, Plist legacy,
                                    Plist compact)
{
  Plist left = legacy, right = compact;
  size_t position = 0;
  while (left != NULL && right != NULL && left->v == right->v) {
    left = left->next;
    right = right->next;
    position++;
  }
  if (left != NULL || right != NULL) {
    Topform lc = left == NULL ? NULL : left->v;
    Topform cc = right == NULL ? NULL : right->v;
    Compact_back_demod_failures++;
    fprintf(stderr,
            "compact_back_demod_audit: mismatch for demodulator %llu at "
            "position %lu (legacy=%llu, compact=%llu)\n",
            demod->id, (unsigned long) position,
            lc == NULL ? 0 : lc->id, cc == NULL ? 0 : cc->id);
    fwrite_clause(stderr, demod, CL_FORM_STD);
    fatal_error("compact back-demodulation audit failed");
  }
}

/* PUBLIC */
Plist back_demodulatable(Topform demod, int type, BOOL lex_order_vars)
{
  if (Compact_back_demod_authoritative)
    return compact_back_demodulatable(demod, type, lex_order_vars);
  else {
    Plist legacy = back_demod_indexed(
      demod, type, Back_demod_idx, lex_order_vars);
    if (Compact_back_demod_audit) {
      Plist compact = compact_back_demodulatable(
        demod, type, lex_order_vars);
      audit_back_demod_answer(demod, legacy, compact);
      zap_plist(compact);
    }
    return legacy;
  }
}  /* back_demodulatable */

/* PUBLIC */
void fprint_compact_back_demod(FILE *fp)
{
  struct compact_back_demod_stats stats;
  if (!compact_back_demod_mode())
    return;
  compact_back_demod_get_stats(Compact_back_demod_idx, &stats);
  fprintf(fp,
          "Compact_back_demod: mode=%s, strategy=%s, failures=%llu, active=%llu, "
          "peak=%llu, retired=%llu, physical=%llu, compactions=%llu, "
          "reclaimed=%llu, queries=%llu, "
          "candidates=%llu, exact_tests=%llu, posting_groups=%llu, "
          "path_buckets=%llu, mask_directory_blocks=%llu, "
          "mask_directory_queries=%llu, "
          "mask_directory_blocks_examined=%llu, "
          "mask_directory_word_checks=%llu, "
          "mask_directory_buckets_selected=%llu, "
          "mask_cache_capacity=%llu, mask_cache_occupied=%llu, "
          "mask_cache_bytes=%llu, mask_cache_queries=%llu, "
          "mask_cache_bypasses=%llu, "
          "mask_cache_key_hits=%llu, mask_cache_hits=%llu, "
          "mask_cache_admissions=%llu, mask_cache_evictions=%llu, "
          "mask_cache_aged_evictions=%llu, "
          "mask_cache_budget_denials=%llu, "
          "mask_cache_incremental_slots=%llu, "
          "mask_cache_bucket_copies=%llu, mask_cache_min_blocks=%u, "
          "tree_nodes=%llu, tree_terminals=%llu, "
          "tree_queries=%llu, tree_nodes_examined=%llu, "
          "tree_sibling_checks=%llu, "
          "tree_child_lookups=%llu, tree_child_hits=%llu, "
          "tree_child_misses=%llu, tree_child_replacements=%llu, "
          "tree_child_growth_denials=%llu, tree_child_parents=%llu, "
          "tree_child_bytes=%llu, "
          "tree_insert_sibling_checks=%llu, "
          "tree_insert_cache_lookups=%llu, tree_insert_cache_hits=%llu, "
          "tree_insert_cache_misses=%llu, "
          "tree_posting_groups=%llu, tree_min_tokens=%u, "
          "tree_complete=%s, tree_budget=%llu, "
          "tree_effective_budget=%llu, tree_budget_pct=%u, "
          "tree_estimated=%llu, "
          "tree_budget_exhaustions=%llu, "
          "tree_admit_work=%u, tree_root_admissions=%llu, "
          "tree_build_factor=%u, tree_root_rejections=%llu, "
          "tree_cost_deferrals=%llu, tree_censuses=%llu, "
          "tree_census_occurrences=%llu, tree_root_demotions=%llu, "
          "tree_backfill_groups=%llu, "
          "tree_backfill_occurrences=%llu, tree_fallback_work=%llu, "
          "position_features=%llu, position_physical_features=%llu, "
          "position_active_roots=%llu, "
          "position_postings=%llu, "
          "position_queries=%llu, position_empty_queries=%llu, "
          "position_intersection_queries=%llu, "
          "position_sparse_intersection_queries=%llu, "
          "position_dense_intersection_queries=%llu, "
          "position_intersection_scans=%llu, "
          "position_intersection_bit_checks=%llu, "
          "position_bitmap_word_checks=%llu, "
          "position_intersection_records=%llu, "
          "position_records_examined=%llu, "
          "position_admissions=%llu, position_rejections=%llu, "
          "position_demotions=%llu, "
          "position_cost_deferrals=%llu, "
          "position_probation_updates=%llu, "
          "position_probation_replacements=%llu, "
          "position_retry_deferrals=%llu, "
          "position_backfill_records=%llu, position_census_records=%llu, "
          "position_append_records=%llu, position_append_root_scans=%llu, "
          "position_append_token_visits=%llu, "
          "position_append_feature_lookups=%llu, "
          "position_append_matches=%llu, "
          "position_append_bucket_groups=%llu, "
          "position_append_grouped_records=%llu, "
          "position_append_sort_fallbacks=%llu, "
          "position_credit_balance=%llu, position_credit_earned=%llu, "
          "position_credit_spent=%llu, position_credit_reservations=%llu, "
          "position_admission_freezes=%llu, position_complete=%s, "
          "position_store=%s, position_eager_depth=%u, "
          "position_eager_features=%llu, "
          "position_budget=%llu, position_effective_budget=%llu, "
          "position_budget_pct=%u, "
          "position_estimated=%llu, position_probation_bytes=%llu, "
          "position_bitmap_bytes=%llu, "
          "position_budget_exhaustions=%llu, "
          "position_admit_work=%u, position_min_gain=%u, "
          "position_build_factor=%u, position_admission=%s, "
          "position_admission_frozen=%s, "
          "symbol_occurrences=%llu, groups_examined=%llu, "
          "occurrences_examined=%llu, path_checks=%llu, "
          "path_rejects=%llu, query_input=%016llx, "
          "query_output=%016llx, query_answers=%016llx, "
          "file_snapshots=%llu, snapshot_ids=%llu, "
          "posting_stream_used=%llu, "
          "posting_stream=%llu, occurrence_stream_used=%llu, "
          "occurrence_stream=%llu, postings=%llu, records=%llu, roots=%llu, "
          "tokens=%llu, hash=%llu, scratch=%llu, bytes=%llu, peak_bytes=%llu.\n",
          Compact_back_demod_authoritative ? "authoritative" : "audit",
          stats.strategy == COMPACT_BACK_DEMOD_MASK32 ? "mask32" :
          stats.strategy == COMPACT_BACK_DEMOD_SIGNATURE32 ? "signature32" :
          stats.strategy == COMPACT_BACK_DEMOD_CODE_TREE ? "code_tree" :
          stats.strategy == COMPACT_BACK_DEMOD_HYBRID_TREE ? "hybrid_tree" :
          stats.strategy == COMPACT_BACK_DEMOD_HOT_ROOT_TREE ? "hot_root_tree" :
          stats.strategy == COMPACT_BACK_DEMOD_POSITION ? "position" :
          stats.strategy == COMPACT_BACK_DEMOD_ADAPTIVE32 ? "adaptive32" :
          stats.strategy == COMPACT_BACK_DEMOD_ADAPTIVE ? "adaptive" :
            "mask8",
          Compact_back_demod_failures, stats.active, stats.peak,
          stats.retired, stats.physical, stats.compactions,
          stats.bytes_reclaimed, stats.queries, stats.candidates,
          stats.exact_tests, stats.posting_groups, stats.path_buckets,
          stats.mask_directory_blocks, stats.mask_directory_queries,
          stats.mask_directory_blocks_examined,
          stats.mask_directory_word_checks,
          stats.mask_directory_buckets_selected,
          stats.mask_result_cache_capacity,
          stats.mask_result_cache_occupied,
          stats.mask_result_cache_bytes,
          stats.mask_result_cache_queries,
          stats.mask_result_cache_bypasses,
          stats.mask_result_cache_key_hits,
          stats.mask_result_cache_hits,
          stats.mask_result_cache_admissions,
          stats.mask_result_cache_evictions,
          stats.mask_result_cache_aged_evictions,
          stats.mask_result_cache_budget_denials,
          stats.mask_result_cache_incremental_slots,
          stats.mask_result_cache_bucket_copies,
          stats.mask_result_cache_min_blocks,
          stats.tree_nodes, stats.tree_terminals, stats.tree_queries,
          stats.tree_nodes_examined,
          stats.tree_sibling_checks,
          stats.tree_child_cache_lookups,
          stats.tree_child_cache_hits,
          stats.tree_child_cache_misses,
          stats.tree_child_cache_replacements,
          stats.tree_child_cache_growth_denials,
          stats.tree_child_cache_parents,
          stats.tree_child_cache_bytes,
          stats.tree_insert_sibling_checks,
          stats.tree_insert_cache_lookups,
          stats.tree_insert_cache_hits,
          stats.tree_insert_cache_misses,
          stats.tree_posting_groups, stats.tree_min_tokens,
          stats.tree_complete ? "yes" : "no",
          stats.tree_budget_bytes, stats.tree_effective_budget_bytes,
          stats.tree_budget_pct, stats.tree_estimated_bytes,
          stats.tree_budget_exhaustions,
          stats.tree_admit_work, stats.tree_root_admissions,
          stats.tree_build_factor, stats.tree_root_rejections,
          stats.tree_root_cost_deferrals, stats.tree_root_censuses,
          stats.tree_root_census_occurrences, stats.tree_root_demotions,
          stats.tree_root_backfill_groups,
          stats.tree_root_backfill_occurrences, stats.tree_fallback_work,
          stats.position_features, stats.position_physical_features,
          stats.position_active_roots,
          stats.position_postings,
          stats.position_queries, stats.position_empty_queries,
          stats.position_intersection_queries,
          stats.position_sparse_intersection_queries,
          stats.position_dense_intersection_queries,
          stats.position_intersection_scans,
          stats.position_intersection_bit_checks,
          stats.position_bitmap_word_checks,
          stats.position_intersection_records,
          stats.position_records_examined,
          stats.position_admissions, stats.position_rejections,
          stats.position_demotions,
          stats.position_cost_deferrals,
          stats.position_probation_updates,
          stats.position_probation_replacements,
          stats.position_retry_deferrals,
          stats.position_backfill_records,
          stats.position_census_records,
          stats.position_append_records,
          stats.position_append_root_scans,
          stats.position_append_token_visits,
          stats.position_append_feature_lookups,
          stats.position_append_matches,
          stats.position_append_bucket_groups,
          stats.position_append_grouped_records,
          stats.position_append_sort_fallbacks,
          stats.position_credit_balance, stats.position_credit_earned,
          stats.position_credit_spent, stats.position_credit_reservations,
          stats.position_admission_freezes,
          stats.position_complete ? "yes" : "no",
          stats.position_sparse ? "sparse" : "bitmap",
          stats.position_eager_depth,
          stats.position_eager_features,
          stats.position_budget_bytes,
          stats.position_effective_budget_bytes,
          stats.position_budget_pct,
          stats.position_estimated_bytes,
          stats.position_probation_bytes,
          stats.position_bitmap_bytes,
          stats.position_budget_exhaustions,
          stats.position_admit_work, stats.position_min_gain,
          stats.position_build_factor,
          stats.position_admission_enabled ? "yes" : "no",
          stats.position_admission_frozen ? "yes" : "no",
          stats.symbol_occurrences,
          stats.posting_groups_examined, stats.occurrences_examined,
          stats.path_filter_checks, stats.path_filter_rejects,
          stats.query_input_fingerprint,
          stats.query_output_fingerprint,
          stats.query_answer_fingerprint,
          stats.materialized_file_snapshots,
          stats.materialized_snapshot_ids,
          stats.posting_stream_used, stats.posting_stream_bytes,
          stats.occurrence_stream_bytes, stats.occurrence_bytes,
          stats.posting_bytes, stats.record_bytes, stats.root_bytes,
          stats.token_bytes, stats.hash_bytes, stats.scratch_bytes,
          stats.total_bytes, stats.peak_bytes);
  fprintf(fp,
          "Compact_back_edge: enabled=%s, features=%llu, postings=%llu, "
          "queries=%llu, empty_queries=%llu, bypass_queries=%llu, "
          "intersection_queries=%llu, query_features=%llu, "
          "selected_features=%llu, posting_records=%llu, "
          "candidate_records=%llu, exact_rejects=%llu, "
          "append_records=%llu, append_token_visits=%llu, "
          "append_feature_lookups=%llu, bytes=%llu.\n",
          stats.edge_enabled ? "yes" : "no",
          stats.edge_features, stats.edge_postings, stats.edge_queries,
          stats.edge_empty_queries, stats.edge_bypass_queries,
          stats.edge_intersection_queries, stats.edge_query_features,
          stats.edge_selected_features,
          stats.edge_posting_records_examined,
          stats.edge_candidate_records, stats.edge_exact_rejects,
          stats.edge_append_records, stats.edge_append_token_visits,
          stats.edge_append_feature_lookups, stats.edge_bytes);
  fprintf(fp,
          "Compact_back_route: capacity=%llu, occupied=%llu, bytes=%llu, "
          "collisions=%llu, replacements=%llu, mask_choices=%llu, "
          "frequency_capacity=%llu, frequency_bytes=%llu, "
          "profile_hits=%llu, profile_misses=%llu, cold_fallbacks=%llu, "
          "admission_attempts=%llu, admission_rejections=%llu, "
          "aged_replacements=%llu, frequency_decays=%llu, "
          "pre_tree_observations=%llu, pre_tree_hot_observations=%llu, "
          "tree_choices=%llu, position_choices=%llu, mask_probes=%llu, "
          "tree_probes=%llu, position_probes=%llu, "
          "tree_probe_aborts=%llu, tree_probe_budget=%llu, "
          "tree_probe_discarded_candidates=%llu, switches=%llu, "
          "reversions=%llu, hysteresis_holds=%llu, "
          "mask_observed_cost=%llu, tree_observed_cost=%llu, "
          "position_observed_cost=%llu, mask_estimated_cost=%llu, "
          "tree_estimated_cost=%llu, position_estimated_cost=%llu, "
          "mask_candidates=%llu, tree_candidates=%llu, "
          "position_candidates=%llu.\n",
          stats.route_profile_capacity, stats.route_profile_occupied,
          stats.route_profile_bytes, stats.route_profile_collisions,
          stats.route_profile_replacements, stats.route_mask_choices,
          stats.route_frequency_capacity, stats.route_frequency_bytes,
          stats.route_profile_hits, stats.route_profile_misses,
          stats.route_cold_fallbacks, stats.route_admission_attempts,
          stats.route_admission_rejections, stats.route_aged_replacements,
          stats.route_frequency_decays,
          stats.route_pre_tree_observations,
          stats.route_pre_tree_hot_observations,
          stats.route_tree_choices, stats.route_position_choices,
          stats.route_mask_probes, stats.route_tree_probes,
          stats.route_position_probes, stats.route_tree_probe_aborts,
          stats.route_tree_probe_budget,
          stats.route_tree_probe_discarded_candidates,
          stats.route_switches,
          stats.route_reversions, stats.route_hysteresis_holds,
          stats.route_mask_observed_cost, stats.route_tree_observed_cost,
          stats.route_position_observed_cost,
          stats.route_mask_estimated_cost, stats.route_tree_estimated_cost,
          stats.route_position_estimated_cost,
          stats.route_mask_candidates, stats.route_tree_candidates,
          stats.route_position_candidates);
  compact_profile_fprint(fp, "back_demod", "candidate_lookup",
                         &stats.query_profile, stats.lookup_seconds);
  fprintf(fp,
          "Compact_index_timing: component=back_demod, lookup_seconds=%.3f, "
          "lookup_timing=sampled, lookup_rate=1/%u, "
          "lookup_eligible=%llu, lookup_samples=%llu, "
          "exact_timing=sampled, exact_eligible=%llu, exact_samples=%llu, "
          "materialize_timing=sampled, materialize_eligible=%llu, "
          "materialize_samples=%llu, "
          "exact_seconds=%.3f, materialize_seconds=%.3f, "
          "maintenance_seconds=%.3f.\n",
          stats.lookup_seconds, stats.timing_sample_rate,
          stats.lookup_timing_eligible, stats.lookup_timing_samples,
          Compact_back_demod_exact_timer.eligible,
          Compact_back_demod_exact_timer.samples,
          Compact_back_demod_materialize_timer.eligible,
          Compact_back_demod_materialize_timer.samples,
          Compact_back_demod_exact_timer.estimated_seconds,
          Compact_back_demod_materialize_timer.estimated_seconds,
          stats.maintenance_seconds);
  fprintf(fp,
          "Compact_worst_query: component=back_demod, proof_id=%llu, "
          "groups=%llu, occurrences=%llu, candidates=%llu, inactive=%llu, "
          "duplicates=%llu, posting_bytes_decoded=%llu.\n",
          stats.worst_query_id, stats.worst_query_groups,
          stats.worst_query_occurrences, stats.worst_query_candidates,
          stats.inactive_groups_examined, stats.duplicate_groups_examined,
          stats.posting_bytes_decoded);
}

/*************
 *
 *   back_demod_idx_report()
 *
 *************/

/* DOCUMENTATION
*/

/* PUBLIC */
void back_demod_idx_report(void)
{
  if (Compact_back_demod_authoritative)
    fprint_compact_back_demod(stdout);
  else {
    printf("Back demod index: ");
    p_fpa_density(Back_demod_idx->fpa);
  }
}  /* back_demod_idx_report */

/*************
 *
 *   write_fpa_back_demod_index() / restore_fpa_back_demod_index()
 *
 *************/

/* PUBLIC */
void write_fpa_back_demod_index(const char *dir)
{
  char path[600];
  FILE *fp;

  if (Compact_back_demod_authoritative)
    fatal_error("write_fpa_back_demod_index: compact checkpoint is not implemented");
  snprintf(path, sizeof(path), "%s/fpa_back_demod_index.txt", dir);
  fp = fopen(path, "w");
  if (!fp) return;

  fpa_write_index(fp, Back_demod_idx->fpa);

  fclose(fp);
}  /* write_fpa_back_demod_index */

/* PUBLIC */
BOOL restore_fpa_back_demod_index(const char *dir)
{
  char path[600];
  FILE *fp;
  BOOL ok;

  if (Compact_back_demod_authoritative)
    fatal_error("restore_fpa_back_demod_index: compact checkpoint is not implemented");
  snprintf(path, sizeof(path), "%s/fpa_back_demod_index.txt", dir);
  fp = fopen(path, "r");
  if (!fp) return FALSE;

  ok = fpa_restore_index(fp, Back_demod_idx->fpa);

  fclose(fp);
  if (ok)
    printf("%%   Restored FPA back-demod index from %s\n", path);
  return ok;
}  /* restore_fpa_back_demod_index */
