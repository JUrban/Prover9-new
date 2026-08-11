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
static Clock Compact_back_demod_exact_clock;
static Clock Compact_back_demod_materialize_clock;
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
  struct compact_back_demod_stats stats;
  compact_back_demod_get_stats(Compact_back_demod_idx, &stats);
  return stats.active;
}

unsigned long long compact_back_demod_physical_count(void)
{
  struct compact_back_demod_stats stats;
  compact_back_demod_get_stats(Compact_back_demod_idx, &stats);
  return stats.physical;
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
    Compact_back_demod_exact_clock = clock_init("compact_back_demod_exact");
    Compact_back_demod_materialize_clock =
      clock_init("compact_back_demod_materialize");
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
  free_clock(Compact_back_demod_exact_clock);
  Compact_back_demod_exact_clock = NULL;
  free_clock(Compact_back_demod_materialize_clock);
  Compact_back_demod_materialize_clock = NULL;
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
      clock_start(Compact_back_demod_materialize_clock);
      candidate = Compact_back_demod_resolve(
        ids[i], Compact_back_demod_context);
      clock_stop(Compact_back_demod_materialize_clock);
      materializations++;
    }
    if (candidate == NULL)
      fatal_error("compact_back_demodulatable: candidate is not resident");
    clock_start(Compact_back_demod_exact_clock);
    if (rewritable_clause_type(demod, candidate, type, lex_order_vars)) {
      Plist cell = get_plist();
      clock_stop(Compact_back_demod_exact_clock);
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
      clock_stop(Compact_back_demod_exact_clock);
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
          "path_buckets=%llu, tree_nodes=%llu, tree_terminals=%llu, "
          "tree_queries=%llu, tree_nodes_examined=%llu, "
          "symbol_occurrences=%llu, groups_examined=%llu, "
          "occurrences_examined=%llu, path_checks=%llu, "
          "path_rejects=%llu, file_snapshots=%llu, snapshot_ids=%llu, "
          "posting_stream_used=%llu, "
          "posting_stream=%llu, occurrence_stream_used=%llu, "
          "occurrence_stream=%llu, postings=%llu, records=%llu, roots=%llu, "
          "tokens=%llu, hash=%llu, scratch=%llu, bytes=%llu, peak_bytes=%llu.\n",
          Compact_back_demod_authoritative ? "authoritative" : "audit",
          stats.strategy == COMPACT_BACK_DEMOD_SIGNATURE32 ? "signature32" :
          stats.strategy == COMPACT_BACK_DEMOD_CODE_TREE ? "code_tree" :
            "mask8",
          Compact_back_demod_failures, stats.active, stats.peak,
          stats.retired, stats.physical, stats.compactions,
          stats.bytes_reclaimed, stats.queries, stats.candidates,
          stats.exact_tests, stats.posting_groups, stats.path_buckets,
          stats.tree_nodes, stats.tree_terminals, stats.tree_queries,
          stats.tree_nodes_examined,
          stats.symbol_occurrences,
          stats.posting_groups_examined, stats.occurrences_examined,
          stats.path_filter_checks, stats.path_filter_rejects,
          stats.materialized_file_snapshots,
          stats.materialized_snapshot_ids,
          stats.posting_stream_used, stats.posting_stream_bytes,
          stats.occurrence_stream_bytes, stats.occurrence_bytes,
          stats.posting_bytes, stats.record_bytes, stats.root_bytes,
          stats.token_bytes, stats.hash_bytes, stats.scratch_bytes,
          stats.total_bytes, stats.peak_bytes);
  compact_profile_fprint(fp, "back_demod", "candidate_lookup",
                         &stats.query_profile, stats.lookup_seconds);
  fprintf(fp,
          "Compact_index_timing: component=back_demod, lookup_seconds=%.3f, "
          "exact_seconds=%.3f, materialize_seconds=%.3f, "
          "maintenance_seconds=%.3f.\n",
          stats.lookup_seconds, clock_seconds(Compact_back_demod_exact_clock),
          clock_seconds(Compact_back_demod_materialize_clock),
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
