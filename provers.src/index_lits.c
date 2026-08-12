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

#include "index_lits.h"
#include "compact_unit_index.h"
#include "compact_feature_index.h"

/* Private definitions and types */

static Lindex  Unit_fpa_idx;          /* unit bsub, unit conflict */
static Lindex  Nonunit_fpa_idx;       /* back unit del */

static Lindex  Unit_discrim_idx;      /* unit fsub, unit del */
static Di_tree Nonunit_features_idx;  /* nonunit fsub, nonunit bsub */

static BOOL Compact_unit_subsumption_audit;
static BOOL Compact_unit_authoritative;
static Compact_unit_index Compact_units;
static Compact_term_pool Compact_unit_terms;
static unsigned long long Compact_unit_audit_failures;
static BOOL Compact_nonunit_audit;
static BOOL Compact_nonunit_authoritative;
static BOOL Compact_nonunit_path_filter;
static Compact_feature_index Compact_nonunits;
static unsigned long long Compact_nonunit_audit_failures;
static unsigned long long Compact_nonunit_forward_exact_tests;
static unsigned long long Compact_nonunit_back_exact_tests;
static Clock Compact_nonunit_exact_clock;
static Clock Compact_nonunit_materialize_clock;
static Clock Compact_nonunit_summary_clock;
static Compact_clause_resolver Compact_clause_resolve;
static Compact_clause_releaser Compact_clause_release;
static void *Compact_clause_context;

void configure_compact_clause_access(Compact_clause_resolver resolver,
                                     Compact_clause_releaser releaser,
                                     void *context)
{
  Compact_clause_resolve = resolver;
  Compact_clause_release = releaser;
  Compact_clause_context = context;
}

static Topform resolve_compact_index_clause_profile(unsigned long long id,
                                                    BOOL *materialized)
{
  Topform clause = find_clause_by_id(id);
  if (materialized != NULL)
    *materialized = FALSE;
  if (clause == NULL && Compact_clause_resolve != NULL) {
    if (materialized != NULL)
      clock_start(Compact_nonunit_materialize_clock);
    clause = Compact_clause_resolve(id, Compact_clause_context);
    if (materialized != NULL) {
      clock_stop(Compact_nonunit_materialize_clock);
      *materialized = TRUE;
    }
  }
  return clause;
}

static Topform resolve_compact_index_clause(unsigned long long id)
{
  return resolve_compact_index_clause_profile(id, NULL);
}

void release_compact_index_clause(Topform clause)
{
  if (clause != NULL && Compact_clause_release != NULL)
    Compact_clause_release(clause, Compact_clause_context);
}

void configure_compact_unit_index(BOOL audit, BOOL authoritative)
{
  if (Compact_units != NULL)
    fatal_error("configure_compact_unit_index: index is live");
  if (audit && authoritative)
    fatal_error("configure_compact_unit_index: audit and authoritative modes conflict");
  Compact_unit_subsumption_audit = audit;
  Compact_unit_authoritative = authoritative;
  Compact_unit_audit_failures = 0;
}

void configure_compact_unit_term_pool(Compact_term_pool pool)
{
  if (Compact_units != NULL)
    fatal_error("configure_compact_unit_term_pool: index is live");
  Compact_unit_terms = pool;
}

void configure_compact_unit_stale_pct(unsigned percentage)
{
  compact_unit_index_set_compaction_stale_pct(percentage);
}

static BOOL compact_unit_index_mode(void)
{
  return Compact_unit_subsumption_audit || Compact_unit_authoritative;
}

unsigned long long compact_unit_active_count(void)
{
  struct compact_unit_index_stats stats;
  compact_unit_index_get_stats(Compact_units, &stats);
  return stats.active;
}

unsigned long long compact_unit_physical_count(void)
{
  struct compact_unit_index_stats stats;
  compact_unit_index_get_stats(Compact_units, &stats);
  return stats.physical;
}

void compact_unit_compact_all_stale(void)
{
  compact_unit_index_compact_all_stale(Compact_units);
}

void compact_unit_copy_term_clauses(Compact_term_pool destination,
                                    Compact_term_rebase_map map)
{
  compact_unit_index_copy_live_clauses(Compact_units, destination, map);
}

void compact_unit_retain_term_clauses(Compact_term_rebase_map map)
{
  compact_unit_index_retain_live_clauses(Compact_units, map);
}

void compact_unit_rebase_term_pool(Compact_term_pool pool,
                                   Compact_term_rebase_map map)
{
  compact_unit_index_rebase_term_pool(Compact_units, pool, map);
  Compact_unit_terms = pool;
}

void configure_compact_nonunit_index(BOOL audit, BOOL authoritative,
                                     BOOL path_filter)
{
  if (Compact_nonunits != NULL)
    fatal_error("configure_compact_nonunit_index: index is live");
  if (audit && authoritative)
    fatal_error("configure_compact_nonunit_index: audit and authoritative modes conflict");
  Compact_nonunit_audit = audit;
  Compact_nonunit_authoritative = authoritative;
  Compact_nonunit_path_filter = path_filter;
  Compact_nonunit_audit_failures = 0;
  Compact_nonunit_forward_exact_tests = 0;
  Compact_nonunit_back_exact_tests = 0;
}

void configure_compact_nonunit_stale_pct(unsigned percentage)
{
  compact_feature_index_set_compaction_stale_pct(percentage);
}

static BOOL compact_nonunit_index_mode(void)
{
  return Compact_nonunit_audit || Compact_nonunit_authoritative;
}

void fprint_compact_nonunit_index(FILE *fp)
{
  struct compact_feature_index_stats stats;
  if (!compact_nonunit_index_mode())
    return;
  compact_feature_index_get_stats(Compact_nonunits, &stats);
  fprintf(fp,
          "Compact_nonunit_index: mode=%s, failures=%llu, active=%llu, "
          "peak=%llu, retired=%llu, physical=%llu, forward_queries=%llu, "
          "forward_candidates=%llu, forward_exact_tests=%llu, "
          "forward_path_rejects=%llu, forward_variable_rejects=%llu, "
          "back_queries=%llu, "
          "back_candidates=%llu, back_exact_tests=%llu, "
          "back_path_rejects=%llu, back_variable_rejects=%llu, "
          "compactions=%llu, reclaimed=%llu, snapshot_records=%llu, "
          "snapshot_bytes=%llu, maintenance_scratch_peak=%llu, "
          "nodes=%llu, labels=%llu, postings=%llu, "
          "records=%llu, structural=%llu, hash=%llu, "
          "scratch=%llu, bytes=%llu, peak_bytes=%llu.\n",
          Compact_nonunit_authoritative ? "authoritative" : "audit",
          Compact_nonunit_audit_failures, stats.active, stats.peak,
          stats.retired, stats.physical, stats.forward_queries,
          stats.forward_candidates, Compact_nonunit_forward_exact_tests,
          stats.forward_structural_rejects,
          stats.forward_variable_rejects,
          stats.back_queries, stats.back_candidates,
          Compact_nonunit_back_exact_tests, stats.back_structural_rejects,
          stats.back_variable_rejects,
          stats.compactions, stats.bytes_reclaimed, stats.snapshot_records,
          stats.snapshot_bytes, stats.maintenance_scratch_peak,
          stats.node_bytes, stats.label_bytes, stats.posting_bytes,
          stats.record_bytes, stats.structural_bytes, stats.hash_bytes,
          stats.scratch_bytes, stats.total_bytes,
          stats.peak_bytes);
  compact_profile_fprint(fp, "nonunit", "forward_subsumption",
                         &stats.forward_profile,
                         stats.forward_lookup_seconds);
  compact_profile_fprint(fp, "nonunit", "back_subsumption",
                         &stats.back_profile, stats.back_lookup_seconds);
  fprintf(fp,
          "Compact_index_timing: component=nonunit, "
          "summary_seconds=%.3f, forward_lookup_seconds=%.3f, "
          "back_lookup_seconds=%.3f, "
          "exact_seconds=%.3f, materialize_seconds=%.3f, "
          "maintenance_seconds=%.3f.\n",
          clock_seconds(Compact_nonunit_summary_clock),
          stats.forward_lookup_seconds, stats.back_lookup_seconds,
          clock_seconds(Compact_nonunit_exact_clock),
          clock_seconds(Compact_nonunit_materialize_clock),
          stats.maintenance_seconds);
}

static void compact_nonunit_audit_mismatch(const char *operation,
                                           Topform query,
                                           unsigned long long legacy,
                                           unsigned long long compact)
{
  Compact_nonunit_audit_failures++;
  fprintf(stderr,
          "compact_nonunit_subsumption_audit: %s mismatch for query %llu "
          "(legacy=%llu, compact=%llu)\n",
          operation, query == NULL ? 0 : query->id, legacy, compact);
  if (query != NULL)
    fwrite_clause(stderr, query, CL_FORM_STD);
  fatal_error("compact nonunit subsumption audit failed");
}

unsigned long long compact_unit_subsumption_audit_failures(void)
{
  return Compact_unit_audit_failures;
}

void fprint_compact_unit_index(FILE *fp)
{
  struct compact_unit_index_stats stats;
  if (!compact_unit_index_mode())
    return;
  compact_unit_index_get_stats(Compact_units, &stats);
  fprintf(fp,
          "Compact_unit_index: mode=%s, strategy=%s, failures=%llu, active=%llu, "
          "peak=%llu, retired=%llu, physical=%llu, compactions=%llu, "
          "reclaimed=%llu, forward_queries=%llu, "
          "back_queries=%llu, back_exact_tests=%llu, "
          "instance_tree_queries=%llu, instance_tree_nodes=%llu, "
          "instance_tree_postings=%llu, conflict_queries=%llu, "
          "conflict_exact_tests=%llu, position_queries=%llu, "
          "position_fallbacks=%llu, position_postings=%llu, "
          "position_duplicates=%llu, code_tree_queries=%llu, "
          "code_tree_nodes=%llu, code_tree_postings=%llu, "
          "node_items=%llu, posting_items=%llu, "
          "feature_items=%llu, feature_posting_items=%llu, "
          "nodes=%llu, postings=%llu, "
          "records=%llu, roots=%llu, features=%llu, tokens=%llu, hash=%llu, scratch=%llu, bytes=%llu, "
          "peak_bytes=%llu.\n",
          Compact_unit_authoritative ? "authoritative" : "audit",
          stats.strategy == COMPACT_UNIT_POSITION ? "position" :
          stats.strategy == COMPACT_UNIT_CODE_TREE ? "code_tree" :
          "root_scan",
          Compact_unit_audit_failures, stats.active, stats.peak,
          stats.retired, stats.physical, stats.compactions,
          stats.bytes_reclaimed, stats.generalization_queries,
          stats.instance_queries, stats.instance_exact_tests,
          stats.instance_tree_queries, stats.instance_tree_nodes_examined,
          stats.instance_tree_postings_examined,
          stats.unifier_queries, stats.unifier_exact_tests,
          stats.position_queries, stats.position_fallback_queries,
          stats.position_postings_examined,
          stats.position_duplicate_postings,
          stats.code_tree_queries, stats.code_tree_nodes_examined,
          stats.code_tree_postings_examined,
          stats.node_items, stats.posting_items,
          stats.feature_items, stats.feature_posting_items,
          stats.node_bytes,
          stats.posting_bytes, stats.record_bytes, stats.root_bytes,
          stats.feature_bytes,
          stats.token_bytes,
          stats.hash_bytes, stats.scratch_bytes,
          stats.total_bytes, stats.peak_bytes);
  compact_profile_fprint(fp, "unit", "generalization",
                         &stats.generalization_profile,
                         stats.generalization_seconds);
  compact_profile_fprint(fp, "unit", "instance",
                         &stats.instance_profile, stats.instance_seconds);
  compact_profile_fprint(fp, "unit", "unification",
                         &stats.unifier_profile, stats.unifier_seconds);
  fprintf(fp,
          "Compact_index_timing: component=unit, sort_seconds=%.3f, "
          "maintenance_seconds=%.3f.\n",
          stats.sort_seconds, stats.maintenance_seconds);
}

static void compact_unit_audit_mismatch(const char *operation, Topform query,
                                        unsigned long long legacy,
                                        unsigned long long compact)
{
  Compact_unit_audit_failures++;
  fprintf(stderr,
          "compact_unit_subsumption_audit: %s mismatch for query %llu "
          "(legacy=%llu, compact=%llu)\n",
          operation, query == NULL ? 0 : query->id, legacy, compact);
  if (query != NULL)
    fwrite_clause(stderr, query, CL_FORM_STD);
  fatal_error("compact unit subsumption audit failed");
}



/*************
 *
 *   write_discrim_entries() -- walk DISCRIM tree, write clause IDs
 *
 *   Uses the same n-counter as zap_discrim_tree in discrim.c.
 *   For each leaf entry (an atom Term), navigates atom->container
 *   to get the clause ID.
 *
 *************/

static
void write_discrim_entries(FILE *fp, Discrim root)
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
      Plist p;
      for (p = cur->u.data; p != NULL; p = p->next) {
        Term atom = (Term) p->v;
        Topform c = (Topform) atom->container;
        fprintf(fp, "%llu\n", c->id);
      }
    }
    else {
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
 *   write_unit_discrim_index() -- serialize Unit_discrim_idx leaf ordering
 *
 *************/

/* PUBLIC */
void write_unit_discrim_index(const char *dir)
{
  char path[600];
  FILE *fp;

  if (Unit_discrim_idx == NULL)
    return;

  snprintf(path, sizeof(path), "%s/unit_discrim_index.txt", dir);
  fp = fopen(path, "w");
  if (!fp)
    return;

  fprintf(fp, "POS\n");
  if (Unit_discrim_idx->pos && Unit_discrim_idx->pos->discrim_tree)
    write_discrim_entries(fp, Unit_discrim_idx->pos->discrim_tree);
  fprintf(fp, "NEG\n");
  if (Unit_discrim_idx->neg && Unit_discrim_idx->neg->discrim_tree)
    write_discrim_entries(fp, Unit_discrim_idx->neg->discrim_tree);
  fclose(fp);
}  /* write_unit_discrim_index */

/*************
 *
 *   restore_unit_discrim_index() -- rebuild from serialized leaf ordering
 *
 *************/

/* PUBLIC */
void restore_unit_discrim_index(const char *dir)
{
  char path[600];
  FILE *fp;
  char line[128];
  int section = -1;  /* 0 = POS, 1 = NEG */
  int count = 0;

  snprintf(path, sizeof(path), "%s/unit_discrim_index.txt", dir);
  fp = fopen(path, "r");
  if (!fp)
    return;

  while (fgets(line, sizeof(line), fp)) {
    unsigned long long clause_id;
    if (strncmp(line, "POS", 3) == 0) {
      section = 0;
      continue;
    }
    if (strncmp(line, "NEG", 3) == 0) {
      section = 1;
      continue;
    }
    if (section < 0)
      continue;
    if (sscanf(line, "%llu", &clause_id) == 1) {
      Topform c = find_clause_by_id(clause_id);
      if (c != NULL && c->literals != NULL) {
        Term atom = c->literals->atom;
        if (section == 0)
          mindex_update(Unit_discrim_idx->pos, atom, INSERT);
        else
          mindex_update(Unit_discrim_idx->neg, atom, INSERT);
        count++;
      }
    }
  }
  fclose(fp);
  printf("%%   Restored unit discrim index: %d entries from %s\n", count,
         path);
}  /* restore_unit_discrim_index */


/*************
 *
 *   index_literals_fpa_only() -- index only into FPA indexes (skip DISCRIM)
 *
 *   Used during checkpoint resume: DISCRIM indexes are restored from
 *   serialized leaf ordering; FPA indexes are order-independent.
 *
 *************/

/* PUBLIC */
void index_literals_fpa_only(Topform c, Indexop op, Clock clock, BOOL no_fapl)
{
  BOOL unit = (number_of_literals(c->literals) == 1);
  clock_start(clock);
  if (!no_fapl || !positive_clause(c->literals))
    lindex_update(unit ? Unit_fpa_idx : Nonunit_fpa_idx, c, op);
  /* Unit_discrim_idx and Nonunit_features_idx are restored separately. */
  clock_stop(clock);
}  /* index_literals_fpa_only */

/*************
 *
 *   index_literals_features_only() -- index the nonunit subsumption filter
 *
 *   FPA checkpoint files do not contain this discrimination tree.  Restore
 *   must rebuild it even when all serialized FPA tries loaded successfully.
 *
 *************/

/* PUBLIC */
void index_literals_features_only(Topform c, Indexop op, Clock clock)
{
  if (number_of_literals(c->literals) != 1) {
    int *f;
    int flen;

    clock_start(clock);
    f = features(c->literals);
    flen = feature_length();
    if (op == INSERT)
      di_tree_insert(f, flen, Nonunit_features_idx, c);
    else
      di_tree_delete(f, flen, Nonunit_features_idx, c);
    clock_stop(clock);
  }
}  /* index_literals_features_only */


/*************
 *
 *   init_lits_index()
 *
 *************/

/* DOCUMENTATION
*/

/* PUBLIC */
void init_literals_index(int depth)
{
  Unit_fpa_idx = Compact_unit_authoritative ? NULL :
    lindex_init(FPA, ORDINARY_UNIF, depth, FPA, ORDINARY_UNIF, depth);

  Nonunit_fpa_idx = Compact_nonunit_authoritative ? NULL :
    lindex_init(FPA, ORDINARY_UNIF, depth,
                FPA, ORDINARY_UNIF, depth);

  Unit_discrim_idx = Compact_unit_authoritative ? NULL :
    lindex_init(DISCRIM_BIND, ORDINARY_UNIF, depth,
                DISCRIM_BIND, ORDINARY_UNIF, depth);

  Nonunit_features_idx = Compact_nonunit_authoritative ? NULL :
    init_di_tree();
  Compact_units = compact_unit_index_mode() ?
    (Compact_unit_terms == NULL ? compact_unit_index_init() :
     compact_unit_index_init_with_pool(Compact_unit_terms)) : NULL;
  Compact_nonunits = compact_nonunit_index_mode() ?
    compact_feature_index_init(feature_length(),
                               Compact_nonunit_path_filter) : NULL;
  if (compact_nonunit_index_mode()) {
    Compact_nonunit_exact_clock = clock_init("compact_nonunit_exact");
    Compact_nonunit_materialize_clock =
      clock_init("compact_nonunit_materialize");
    Compact_nonunit_summary_clock = clock_init("compact_nonunit_summary");
  }
}  /* init_lits_index */

/*************
 *
 *   lits_destroy_index()
 *
 *************/

/* DOCUMENTATION
*/

/* PUBLIC */
void destroy_literals_index(void)
{
  if (Unit_fpa_idx != NULL)
    lindex_destroy(Unit_fpa_idx);
  Unit_fpa_idx = NULL;
  if (Nonunit_fpa_idx != NULL)
    lindex_destroy(Nonunit_fpa_idx);
  Nonunit_fpa_idx = NULL;
  if (Unit_discrim_idx != NULL)
    lindex_destroy(Unit_discrim_idx);
  Unit_discrim_idx = NULL;
  if (Nonunit_features_idx != NULL)
    zap_di_tree(Nonunit_features_idx, feature_length());
  Nonunit_features_idx = NULL;
  compact_unit_index_free(Compact_units); Compact_units = NULL;
  Compact_unit_terms = NULL;
  compact_feature_index_free(Compact_nonunits); Compact_nonunits = NULL;
  free_clock(Compact_nonunit_exact_clock); Compact_nonunit_exact_clock = NULL;
  free_clock(Compact_nonunit_materialize_clock);
  Compact_nonunit_materialize_clock = NULL;
  free_clock(Compact_nonunit_summary_clock);
  Compact_nonunit_summary_clock = NULL;
}  /* lits_destroy_index */

/*************
 *
 *   index_literals()
 *
 *************/

/* DOCUMENTATION
*/

/* PUBLIC */
void index_literals(Topform c, Indexop op, Clock clock, BOOL no_fapl)
{
  BOOL unit = (number_of_literals(c->literals) == 1);
  clock_start(clock);
  if (unit && compact_unit_index_mode()) {
    BOOL ok = op == INSERT ? compact_unit_index_add(Compact_units, c) :
                             compact_unit_index_remove(Compact_units, c->id);
    if (!ok)
      fatal_error(op == INSERT ?
        "index_literals: duplicate compact unit" :
        "index_literals: missing compact unit");
    if (op == DELETE && compact_unit_index_compaction_needed(Compact_units))
      compact_unit_index_compact(Compact_units);
  }
  if ((unit ? !Compact_unit_authoritative : !Compact_nonunit_authoritative) &&
      (!no_fapl || !positive_clause(c->literals)))
    lindex_update(unit ? Unit_fpa_idx : Nonunit_fpa_idx, c, op);

  if (unit) {
    if (!Compact_unit_authoritative)
      lindex_update(Unit_discrim_idx, c, op);
  }
  else {
    int *f = features(c->literals);
    int flen = feature_length();
    if (compact_nonunit_index_mode()) {
      struct compact_feature_structural_summary structural;
      memset(&structural, 0, sizeof(structural));
      if (Compact_nonunit_path_filter && op == INSERT) {
        clock_start(Compact_nonunit_summary_clock);
        structural = compact_feature_clause_summary(c);
        clock_stop(Compact_nonunit_summary_clock);
      }
      BOOL ok = op == INSERT ?
        compact_feature_index_add(
          Compact_nonunits, c->id, f, structural) :
        compact_feature_index_remove(Compact_nonunits, c->id);
      if (!ok)
        fatal_error(op == INSERT ?
          "index_literals: duplicate compact nonunit" :
          "index_literals: missing compact nonunit");
      if (op == DELETE &&
          compact_feature_index_compaction_needed(Compact_nonunits))
        compact_feature_index_compact(Compact_nonunits);
    }
    if (!Compact_nonunit_authoritative) {
      if (op == INSERT)
        di_tree_insert(f, flen, Nonunit_features_idx, c);
      else
        di_tree_delete(f, flen, Nonunit_features_idx, c);
    }
  }
  clock_stop(clock);
}  /* index_literals */

/*************
 *
 *   index_denial()
 *
 *************/

/* DOCUMENTATION
*/

/* PUBLIC */
void index_denial(Topform c, Indexop op, Clock clock)
{
  BOOL unit = (number_of_literals(c->literals) == 1);
  clock_start(clock);
  if (unit && compact_unit_index_mode()) {
    BOOL ok = op == INSERT ? compact_unit_index_add(Compact_units, c) :
                             compact_unit_index_remove(Compact_units, c->id);
    if (!ok)
      fatal_error(op == INSERT ?
        "index_denial: duplicate compact unit" :
        "index_denial: missing compact unit");
  }
  if (unit ? !Compact_unit_authoritative : !Compact_nonunit_authoritative)
    lindex_update(unit ? Unit_fpa_idx : Nonunit_fpa_idx, c, op);
  clock_stop(clock);
}  /* index_denial */

/*************
 *
 *   unit_conflict()
 *
 *************/

/* DOCUMENTATION
Look for conflicting units.  Send any that are found to empty_proc().
*/

/* PUBLIC */
void unit_conflict(Topform c, void (*empty_proc) (Topform))
{
  if (Compact_unit_subsumption_audit &&
      number_of_literals(c->literals) == 1) {
    Plist legacy = unit_conflict_candidates_by_index(c, Unit_fpa_idx);
    Literals literal = c->literals;
    unsigned long long *direct, *flipped = NULL;
    size_t direct_count = 0, flipped_count = 0, at = 0;
    Plist p;
    direct = compact_unit_unifier_ids(
      Compact_units, literal->atom, !literal->sign, c->id, &direct_count);
    if (eq_term(literal->atom) && !renamable_flip_eq(literal->atom)) {
      Term flip = top_flip(literal->atom);
      flipped = compact_unit_unifier_ids(
        Compact_units, flip, !literal->sign, c->id, &flipped_count);
      zap_top_flip(flip);
    }
    for (p = legacy; p != NULL; p = p->next, at++) {
      unsigned long long compact = at < direct_count ? direct[at] :
        at < direct_count + flipped_count ? flipped[at - direct_count] : 0;
      if (((Topform) p->v)->id != compact)
        compact_unit_audit_mismatch(
          "conflict", c, ((Topform) p->v)->id, compact);
    }
    if (at != direct_count + flipped_count) {
      unsigned long long compact = at < direct_count ? direct[at] :
        flipped[at - direct_count];
      compact_unit_audit_mismatch("conflict", c, 0, compact);
    }
    zap_plist(legacy);
    safe_free(direct);
    safe_free(flipped);
  }
  if (Compact_unit_authoritative && number_of_literals(c->literals) == 1) {
    Literals literal = c->literals;
    unsigned long long *direct, *flipped = NULL;
    size_t direct_count = 0, flipped_count = 0, i;
    direct = compact_unit_unifier_ids(
      Compact_units, literal->atom, !literal->sign, c->id, &direct_count);
    if (eq_term(literal->atom) && !renamable_flip_eq(literal->atom)) {
      Term flip = top_flip(literal->atom);
      flipped = compact_unit_unifier_ids(
        Compact_units, flip, !literal->sign, c->id, &flipped_count);
      zap_top_flip(flip);
    }
    if (direct_count + flipped_count != 0) {
      if (c->id == 0)
        assign_clause_id(c);
      c->used = TRUE;
    }
    for (i = 0; i < direct_count + flipped_count; i++) {
      BOOL flip = i >= direct_count;
      unsigned long long id = flip ? flipped[i - direct_count] : direct[i];
      Topform candidate = resolve_compact_index_clause(id);
      Topform empty;
      if (candidate == NULL)
        fatal_error("unit_conflict: compact candidate is not resident");
      empty = flip ? try_unit_conflict_flipped(c, candidate) :
                     try_unit_conflict(c, candidate);
      if (empty == NULL)
        fatal_error("unit_conflict: compact candidate no longer unifies");
      (*empty_proc)(empty);
      release_compact_index_clause(candidate);
    }
    safe_free(direct);
    safe_free(flipped);
  }
  else
    unit_conflict_by_index(c, Unit_fpa_idx, empty_proc);
}  /* unit_conflict */

/*************
 *
 *   unit_deletion()
 *
 *************/

/* DOCUMENTATION
*/

/* PUBLIC */
void unit_deletion(Topform c)
{
  if (Compact_unit_authoritative)
    fatal_error("unit_deletion: authoritative compact unit index does not yet support unit deletion");
  unit_delete(c, Unit_discrim_idx);
}  /* unit_deletion */

/*************
 *
 *   back_unit_deletable()
 *
 *************/

/* DOCUMENTATION
Return the list of clauses that can be back unit deleted by
the given clause.
*/

/* PUBLIC */
Plist back_unit_deletable(Topform c)
{
  return back_unit_del_by_index(c, Nonunit_fpa_idx);
}  /* back_unit_deletable */

/*************
 *
 *   forward_subsumption()
 *
 *************/

static Topform compact_nonunit_forward_subsumption(Topform query)
{
  int *vector = features(query->literals);
  struct compact_feature_structural_summary structural;
  unsigned long long *ids;
  size_t count = 0, i, exact = 0, materialized = 0;
  Topform result = NULL;
  memset(&structural, 0, sizeof(structural));
  if (Compact_nonunit_path_filter) {
    clock_start(Compact_nonunit_summary_clock);
    structural = compact_feature_clause_summary(query);
    clock_stop(Compact_nonunit_summary_clock);
  }
  ids = compact_feature_forward_candidates(
    Compact_nonunits, vector, structural, &count);
  for (i = 0; i < count && result == NULL; i++) {
    BOOL was_materialized;
    Topform candidate = resolve_compact_index_clause_profile(
      ids[i], &was_materialized);
    if (was_materialized)
      materialized++;
    if (candidate == NULL)
      fatal_error("compact_nonunit_forward_subsumption: candidate is not resident");
    Compact_nonunit_forward_exact_tests++;
    exact++;
    clock_start(Compact_nonunit_exact_clock);
    if (feature_subsumes_raw(candidate, query)) {
      clock_stop(Compact_nonunit_exact_clock);
      result = candidate;
    }
    else {
      clock_stop(Compact_nonunit_exact_clock);
      release_compact_index_clause(candidate);
    }
  }
  compact_feature_note_exact_query(Compact_nonunits, TRUE, exact,
                                   result == NULL ? 0 : 1, materialized);
  safe_free(ids);
  return result;
}

static Plist compact_nonunit_back_subsumption(Topform query)
{
  int *vector = features(query->literals);
  struct compact_feature_structural_summary structural;
  unsigned long long *ids;
  size_t count = 0, i, exact = 0, successes = 0, materialized = 0;
  Plist result = NULL;
  memset(&structural, 0, sizeof(structural));
  if (Compact_nonunit_path_filter) {
    clock_start(Compact_nonunit_summary_clock);
    structural = compact_feature_clause_summary(query);
    clock_stop(Compact_nonunit_summary_clock);
  }
  ids = compact_feature_back_candidates(
    Compact_nonunits, vector, structural, &count);
  for (i = 0; i < count; i++) {
    BOOL was_materialized;
    Topform candidate = resolve_compact_index_clause_profile(
      ids[i], &was_materialized);
    if (was_materialized)
      materialized++;
    if (candidate == NULL)
      fatal_error("compact_nonunit_back_subsumption: candidate is not resident");
    if (candidate != query) {
      Compact_nonunit_back_exact_tests++;
      exact++;
      clock_start(Compact_nonunit_exact_clock);
      if (feature_subsumes_raw(query, candidate)) {
        clock_stop(Compact_nonunit_exact_clock);
        successes++;
        result = plist_prepend(result, candidate);
      }
      else {
        clock_stop(Compact_nonunit_exact_clock);
        release_compact_index_clause(candidate);
      }
    }
    else
      release_compact_index_clause(candidate);
  }
  compact_feature_note_exact_query(Compact_nonunits, FALSE, exact,
                                   successes, materialized);
  safe_free(ids);
  return result;
}

/* DOCUMENTATION
*/

/* PUBLIC */
Topform forward_subsumption(Topform d)
{
  Topform subsumer = NULL;
  if (Compact_unit_authoritative) {
    Literals literal;
    unsigned long long compact = 0;
    for (literal = d->literals; literal != NULL && compact == 0;
         literal = literal->next)
      compact = compact_unit_generalization_first(
        Compact_units, literal->atom, literal->sign, 0);
    if (compact != 0) {
      subsumer = resolve_compact_index_clause(compact);
      if (subsumer == NULL)
        fatal_error("forward_subsumption: compact subsumer is not resident");
    }
  }
  else
    subsumer = forward_subsume(d, Unit_discrim_idx);
  if (Compact_unit_subsumption_audit) {
    Literals literal;
    unsigned long long compact = 0;
    for (literal = d->literals; literal != NULL && compact == 0;
         literal = literal->next)
      compact = compact_unit_generalization_first(
        Compact_units, literal->atom, literal->sign, 0);
    if ((subsumer == NULL ? 0 : subsumer->id) != compact)
      compact_unit_audit_mismatch("forward", d,
                                  subsumer == NULL ? 0 : subsumer->id,
                                  compact);
  }
  if (!subsumer) {
    if (Compact_nonunit_authoritative)
      subsumer = compact_nonunit_forward_subsumption(d);
    else {
      Topform compact = NULL;
      subsumer = forward_feature_subsume(d, Nonunit_features_idx);
      if (Compact_nonunit_audit) {
        compact = compact_nonunit_forward_subsumption(d);
        if (subsumer != compact)
          compact_nonunit_audit_mismatch(
            "forward", d, subsumer == NULL ? 0 : subsumer->id,
            compact == NULL ? 0 : compact->id);
      }
    }
  }
  return subsumer;
}  /* forward_subsumption */

/*************
 *
 *   forward_subsumption_filter()
 *
 *   Variant of forward_subsumption that skips candidates rejected by
 *   accept_cb.  Used to support set(ancestor_subsume): when the first
 *   subsumer is an alphabetic variant whose proof is longer than the
 *   new clause's, skip it and try the next candidate.  Matches Otter's
 *   forward_subsume which iterates past anc_subsume blocks.
 *
 *************/

/* PUBLIC */
Topform forward_subsumption_filter(Topform d,
                                   BOOL (*accept_cb)(Topform subsumer,
                                                     Topform new_clause,
                                                     void *arg),
                                   void *cb_arg)
{
  if (compact_unit_index_mode() || compact_nonunit_index_mode())
    fatal_error("compact subsumption indexes do not support ancestor_subsume");
  Topform subsumer = forward_subsume_filter(d, Unit_discrim_idx,
                                            accept_cb, cb_arg);
  if (!subsumer)
    subsumer = forward_feature_subsume(d, Nonunit_features_idx);
  return subsumer;
}  /* forward_subsumption_filter */

/*************
 *
 *   back_subsumption()
 *
 *************/

/* DOCUMENTATION
Return the list of clauses that can ar back subsumed by the given clause.
*/

/* PUBLIC */
Plist back_subsumption(Topform c)
{
  Plist p1 = NULL;
  if (Compact_unit_authoritative && number_of_literals(c->literals) == 1) {
    unsigned long long *ids;
    size_t count = 0, i;
    Plist tail = NULL;
    ids = compact_unit_instance_ids(
      Compact_units, c->literals->atom, c->literals->sign, c->id, &count);
    for (i = 0; i < count; i++) {
      Topform candidate = resolve_compact_index_clause(ids[i]);
      Plist cell;
      if (candidate == NULL)
        fatal_error("back_subsumption: compact subsumee is not resident");
      cell = get_plist();
      cell->v = candidate;
      cell->next = NULL;
      if (tail == NULL)
        p1 = cell;
      else
        tail->next = cell;
      tail = cell;
    }
    safe_free(ids);
  }
  else if (!Compact_unit_authoritative)
    p1 = back_subsume(c, Unit_fpa_idx);
  if (Compact_unit_subsumption_audit) {
    unsigned long long *compact = NULL;
    size_t compact_count = 0, at = 0;
    Plist p;
    if (number_of_literals(c->literals) == 1)
      compact = compact_unit_instance_ids(
        Compact_units, c->literals->atom, c->literals->sign, c->id,
        &compact_count);
    for (p = p1; p != NULL; p = p->next, at++)
      if (at >= compact_count || ((Topform) p->v)->id != compact[at])
        compact_unit_audit_mismatch(
          "back", c, ((Topform) p->v)->id,
          at < compact_count ? compact[at] : 0);
    if (at != compact_count)
      compact_unit_audit_mismatch("back", c, 0, compact[at]);
    safe_free(compact);
  }
  Plist p2;
  if (Compact_nonunit_authoritative)
    p2 = compact_nonunit_back_subsumption(c);
  else {
    p2 = back_feature_subsume(c, Nonunit_features_idx);
    if (Compact_nonunit_audit) {
      Plist compact = compact_nonunit_back_subsumption(c);
      Plist legacy_at = p2, compact_at = compact;
      while (legacy_at != NULL && compact_at != NULL &&
             legacy_at->v == compact_at->v) {
        legacy_at = legacy_at->next;
        compact_at = compact_at->next;
      }
      if (legacy_at != NULL || compact_at != NULL) {
        Topform legacy_clause = legacy_at == NULL ? NULL : legacy_at->v;
        Topform compact_clause = compact_at == NULL ? NULL : compact_at->v;
        compact_nonunit_audit_mismatch(
          "back", c,
          legacy_clause == NULL ? 0 : legacy_clause->id,
          compact_clause == NULL ? 0 : compact_clause->id);
      }
      zap_plist(compact);
    }
  }

  Plist p3 = plist_cat(p1, p2);
  return p3;
}  /* back_subsumption */

/*************
 *
 *   lits_idx_report()
 *
 *************/

/* DOCUMENTATION
*/

/* PUBLIC */
void lits_idx_report(void)
{
  if (Compact_unit_authoritative)
    fprint_compact_unit_index(stdout);
  else {
    printf("Pos unit lits index: ");
    p_fpa_density(Unit_fpa_idx->pos->fpa);
    printf("Neg unit lits index: ");
    p_fpa_density(Unit_fpa_idx->neg->fpa);
  }
  if (Compact_nonunit_authoritative)
    fprint_compact_nonunit_index(stdout);
  else {
    printf("Pos nonunit lits index: ");
    p_fpa_density(Nonunit_fpa_idx->pos->fpa);
    printf("Neg nonunit lits index: ");
    p_fpa_density(Nonunit_fpa_idx->neg->fpa);
  }
}  /* lits_idx_report */

/*************
 *
 *   write_fpa_lits_index() / restore_fpa_lits_index()
 *
 *   Serialize/deserialize the 4 FPA literal indexes for checkpoint.
 *
 *************/

/* PUBLIC */
void write_fpa_lits_index(const char *dir)
{
  char path[600];
  FILE *fp;

  if (Compact_unit_authoritative || Compact_nonunit_authoritative)
    fatal_error("write_fpa_lits_index: compact literal checkpoint is not implemented");
  snprintf(path, sizeof(path), "%s/fpa_lits_index.txt", dir);
  fp = fopen(path, "w");
  if (!fp) return;

  fprintf(fp, "SECTION unit_pos\n");
  fpa_write_index(fp, Unit_fpa_idx->pos->fpa);
  fprintf(fp, "SECTION unit_neg\n");
  fpa_write_index(fp, Unit_fpa_idx->neg->fpa);
  fprintf(fp, "SECTION nonunit_pos\n");
  fpa_write_index(fp, Nonunit_fpa_idx->pos->fpa);
  fprintf(fp, "SECTION nonunit_neg\n");
  fpa_write_index(fp, Nonunit_fpa_idx->neg->fpa);
  fprintf(fp, "END\n");

  fclose(fp);
}  /* write_fpa_lits_index */

/* PUBLIC */
BOOL restore_fpa_lits_index(const char *dir)
{
  char path[600], buf[64];
  FILE *fp;
  int restored = 0;

  if (Compact_unit_authoritative || Compact_nonunit_authoritative)
    fatal_error("restore_fpa_lits_index: compact literal checkpoint is not implemented");
  snprintf(path, sizeof(path), "%s/fpa_lits_index.txt", dir);
  fp = fopen(path, "r");
  if (!fp) return FALSE;

  while (fscanf(fp, " %63s", buf) == 1) {
    if (strcmp(buf, "END") == 0) break;
    if (strcmp(buf, "SECTION") != 0) continue;
    if (fscanf(fp, " %63s", buf) != 1) break;

    if (strcmp(buf, "unit_pos") == 0) {
      if (fpa_restore_index(fp, Unit_fpa_idx->pos->fpa)) restored++;
    } else if (strcmp(buf, "unit_neg") == 0) {
      if (fpa_restore_index(fp, Unit_fpa_idx->neg->fpa)) restored++;
    } else if (strcmp(buf, "nonunit_pos") == 0) {
      if (fpa_restore_index(fp, Nonunit_fpa_idx->pos->fpa)) restored++;
    } else if (strcmp(buf, "nonunit_neg") == 0) {
      if (fpa_restore_index(fp, Nonunit_fpa_idx->neg->fpa)) restored++;
    }
  }

  fclose(fp);
  printf("%%   Restored FPA literals index: %d sections from %s\n",
         restored, path);
  return (restored == 4);
}  /* restore_fpa_lits_index */
