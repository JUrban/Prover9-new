#ifndef TP_COMPACT_REWRITE_H
#define TP_COMPACT_REWRITE_H

#include "../ladr/ladr.h"
#include "compact_term_pool.h"

typedef struct compact_rewrite_bank * Compact_rewrite_bank;

struct compact_rewrite_stats {
  unsigned long long rules_current;
  unsigned long long rules_peak;
  unsigned long long rules_retired;
  unsigned long long rules_physical;
  unsigned long long compactions;
  unsigned long long bytes_reclaimed;
  unsigned long long attempts;
  unsigned long long rewrites;
  unsigned long long subject_atoms;
  unsigned long long subject_initial_nodes;
  unsigned long long subject_target_nodes;
  unsigned long long node_items;
  unsigned long long posting_items;
  unsigned long long node_bytes;
  unsigned long long posting_bytes;
  unsigned long long child_cache_bytes;
  unsigned long long child_cache_capacity;
  unsigned long long deep_child_cache_bytes;
  unsigned long long deep_child_cache_budget_bytes;
  unsigned long long deep_child_cache_capacity;
  unsigned long long deep_child_cache_parents;
  unsigned long long deep_child_cache_lookups;
  unsigned long long deep_child_cache_hits;
  unsigned long long deep_child_cache_misses;
  unsigned long long deep_child_cache_replacements;
  unsigned long long deep_child_cache_growth_denials;
  unsigned long long variable_sibling_checks;
  unsigned long long rigid_sibling_checks;
  unsigned long long occurrence_bytes;
  unsigned long long occurrence_stream_used;
  unsigned long long occurrence_stream_bytes;
  unsigned long long rule_bytes;
  unsigned long long term_bytes;
  unsigned long long hash_bytes;
  unsigned long long total_bytes;
  unsigned long long peak_bytes;
};

typedef void (*Compact_rewrite_overlap_fn)(unsigned long long proof_id,
                                           void *context);

Compact_rewrite_bank compact_rewrite_init(void);

Compact_rewrite_bank compact_rewrite_init_with_pool(Compact_term_pool pool);

void compact_rewrite_set_compaction_stale_pct(unsigned percentage);

void compact_rewrite_set_deep_child_cache_kb(unsigned kilobytes);

BOOL compact_rewrite_add(Compact_rewrite_bank bank, Topform clause, int type);

BOOL compact_rewrite_remove(Compact_rewrite_bank bank,
                            unsigned long long proof_id);

/* Deactivate a cold rule for selected-clause materialization without
   charging a semantic retirement; selection may immediately reinsert it. */
BOOL compact_rewrite_suspend(Compact_rewrite_bank bank,
                             unsigned long long proof_id);

void compact_rewrite_note_suspended_retirement(Compact_rewrite_bank bank);

BOOL compact_rewrite_contains(Compact_rewrite_bank bank,
                              unsigned long long proof_id);

void compact_rewrite_copy_live_clauses(Compact_rewrite_bank bank,
                                       Compact_term_pool destination,
                                       Compact_term_rebase_map map);

void compact_rewrite_retain_live_clauses(Compact_rewrite_bank bank,
                                         Compact_term_rebase_map map);

void compact_rewrite_rebase_term_pool(Compact_rewrite_bank bank,
                                      Compact_term_pool pool,
                                      Compact_term_rebase_map map);

/* Visit live rules whose rewrite source side contains a root symbol used by
   the newly admitted rule.  This is a conservative redex filter: callers
   perform the exact normalization, and may coalesce duplicate visits. */
void compact_rewrite_visit_overlaps(Compact_rewrite_bank bank,
                                    unsigned long long new_proof_id,
                                    Compact_rewrite_overlap_fn visit,
                                    void *context);

unsigned long long compact_rewrite_identity_hash(Compact_rewrite_bank bank);

void compact_rewrite_restore_counters(Compact_rewrite_bank bank,
                                      unsigned long long rules_peak,
                                      unsigned long long rules_retired,
                                      unsigned long long attempts,
                                      unsigned long long rewrites,
                                      unsigned long long subject_atoms,
                                      unsigned long long subject_initial_nodes,
                                      unsigned long long subject_target_nodes,
                                      unsigned long long compactions,
                                      unsigned long long bytes_reclaimed);

void compact_rewrite_clause(Compact_rewrite_bank bank, Topform clause,
                            int step_limit, int increase_limit,
                            BOOL lex_order_vars, BOOL count_stats);

void compact_rewrite_get_stats(Compact_rewrite_bank bank,
                               struct compact_rewrite_stats *stats);

/* Hot search/lifecycle predicates need only maintained populations; avoid
   constructing the complete diagnostic snapshot for those scalar reads. */
unsigned long long compact_rewrite_active_rules(Compact_rewrite_bank bank);

unsigned long long compact_rewrite_physical_rules(Compact_rewrite_bank bank);

BOOL compact_rewrite_compaction_needed(Compact_rewrite_bank bank);

void compact_rewrite_compact(Compact_rewrite_bank bank);

void compact_rewrite_compact_all_stale(Compact_rewrite_bank bank);

void compact_rewrite_free(Compact_rewrite_bank bank);

#endif
