#ifndef TP_COMPACT_BACK_DEMOD_H
#define TP_COMPACT_BACK_DEMOD_H

#include "../ladr/ladr.h"
#include "compact_profile.h"
#include "compact_term_pool.h"

typedef struct compact_back_demod_index * Compact_back_demod_index;

typedef enum {
  COMPACT_BACK_DEMOD_MASK8,
  COMPACT_BACK_DEMOD_SIGNATURE32,
  COMPACT_BACK_DEMOD_CODE_TREE,
  COMPACT_BACK_DEMOD_HYBRID_TREE,
  COMPACT_BACK_DEMOD_HOT_ROOT_TREE,
  COMPACT_BACK_DEMOD_POSITION,
  COMPACT_BACK_DEMOD_ADAPTIVE
} Compact_back_demod_strategy;

struct compact_back_demod_stats {
  Compact_back_demod_strategy strategy;
  unsigned long long active;
  unsigned long long peak;
  unsigned long long retired;
  unsigned long long physical;
  unsigned long long compactions;
  unsigned long long bytes_reclaimed;
  unsigned long long queries;
  unsigned long long candidates;
  unsigned long long exact_tests;
  unsigned long long posting_groups;
  unsigned long long path_buckets;
  unsigned long long tree_nodes;
  unsigned long long tree_terminals;
  unsigned long long tree_queries;
  unsigned long long tree_nodes_examined;
  unsigned long long tree_sibling_checks;
  unsigned long long tree_child_cache_lookups;
  unsigned long long tree_child_cache_hits;
  unsigned long long tree_child_cache_misses;
  unsigned long long tree_child_cache_replacements;
  unsigned long long tree_child_cache_growth_denials;
  unsigned long long tree_child_cache_parents;
  unsigned long long tree_child_cache_bytes;
  unsigned long long tree_posting_groups;
  unsigned long long tree_budget_bytes;
  unsigned long long tree_estimated_bytes;
  unsigned long long tree_budget_exhaustions;
  unsigned long long tree_root_admissions;
  unsigned long long tree_root_rejections;
  unsigned long long tree_root_cost_deferrals;
  unsigned long long tree_root_censuses;
  unsigned long long tree_root_census_occurrences;
  unsigned long long tree_root_demotions;
  unsigned long long tree_root_backfill_groups;
  unsigned long long tree_root_backfill_occurrences;
  unsigned long long tree_fallback_work;
  unsigned tree_min_tokens;
  unsigned tree_admit_work;
  unsigned tree_build_factor;
  BOOL tree_complete;
  unsigned long long position_features;
  unsigned long long position_physical_features;
  unsigned long long position_postings;
  unsigned long long position_queries;
  unsigned long long position_intersection_queries;
  unsigned long long position_dense_intersection_queries;
  unsigned long long position_intersection_scans;
  unsigned long long position_intersection_bit_checks;
  unsigned long long position_bitmap_word_checks;
  unsigned long long position_intersection_records;
  unsigned long long position_records_examined;
  unsigned long long position_admissions;
  unsigned long long position_rejections;
  unsigned long long position_demotions;
  unsigned long long position_cost_deferrals;
  unsigned long long position_probation_updates;
  unsigned long long position_probation_replacements;
  unsigned long long position_backfill_records;
  unsigned long long position_budget_bytes;
  unsigned long long position_effective_budget_bytes;
  unsigned long long position_estimated_bytes;
  unsigned long long position_probation_bytes;
  unsigned long long position_bitmap_bytes;
  unsigned long long position_budget_exhaustions;
  unsigned position_budget_pct;
  unsigned position_admit_work;
  unsigned position_min_gain;
  unsigned position_build_factor;
  BOOL position_admission_enabled;
  BOOL position_complete;
  unsigned long long route_profile_capacity;
  unsigned long long route_profile_occupied;
  unsigned long long route_profile_bytes;
  unsigned long long route_profile_collisions;
  unsigned long long route_profile_replacements;
  unsigned long long route_mask_choices;
  unsigned long long route_tree_choices;
  unsigned long long route_position_choices;
  unsigned long long route_mask_probes;
  unsigned long long route_tree_probes;
  unsigned long long route_position_probes;
  unsigned long long route_switches;
  unsigned long long route_reversions;
  unsigned long long route_hysteresis_holds;
  unsigned long long route_mask_observed_cost;
  unsigned long long route_tree_observed_cost;
  unsigned long long route_position_observed_cost;
  unsigned long long route_mask_estimated_cost;
  unsigned long long route_tree_estimated_cost;
  unsigned long long route_position_estimated_cost;
  unsigned long long route_mask_candidates;
  unsigned long long route_tree_candidates;
  unsigned long long route_position_candidates;
  unsigned long long symbol_occurrences;
  unsigned long long posting_groups_examined;
  unsigned long long occurrences_examined;
  unsigned long long path_filter_checks;
  unsigned long long path_filter_rejects;
  unsigned long long inactive_groups_examined;
  unsigned long long duplicate_groups_examined;
  unsigned long long posting_bytes_decoded;
  unsigned long long worst_query_id;
  unsigned long long worst_query_groups;
  unsigned long long worst_query_occurrences;
  unsigned long long worst_query_candidates;
  unsigned long long query_input_fingerprint;
  unsigned long long query_output_fingerprint;
  struct compact_query_profile query_profile;
  double lookup_seconds;
  unsigned long long lookup_timing_eligible;
  unsigned long long lookup_timing_samples;
  unsigned timing_sample_rate;
  double maintenance_seconds;
  unsigned long long materialized_file_snapshots;
  unsigned long long materialized_snapshot_ids;
  unsigned long long posting_bytes;
  unsigned long long posting_stream_used;
  unsigned long long posting_stream_bytes;
  unsigned long long occurrence_bytes;
  unsigned long long occurrence_stream_bytes;
  unsigned long long record_bytes;
  unsigned long long root_bytes;
  unsigned long long token_bytes;
  unsigned long long hash_bytes;
  unsigned long long scratch_bytes;
  unsigned long long total_bytes;
  unsigned long long peak_bytes;
};

Compact_back_demod_index compact_back_demod_init(void);

Compact_back_demod_index compact_back_demod_init_with_pool(
  Compact_term_pool pool);

void compact_back_demod_set_compaction_stale_pct(unsigned percentage);

void compact_back_demod_set_strategy(Compact_back_demod_strategy strategy);

void compact_back_demod_set_tree_min_tokens(unsigned tokens);

void compact_back_demod_set_tree_budget_kb(unsigned kilobytes);

void compact_back_demod_set_tree_admit_work(unsigned groups);

void compact_back_demod_set_tree_build_factor(unsigned factor);

void compact_back_demod_set_position_options(unsigned admit_work,
                                             unsigned min_gain,
                                             unsigned build_factor,
                                             unsigned budget_kb,
                                             unsigned budget_pct,
                                             BOOL admission_enabled);

BOOL compact_back_demod_add(Compact_back_demod_index index, Topform clause);

BOOL compact_back_demod_remove(Compact_back_demod_index index,
                               unsigned long long proof_id);

BOOL compact_back_demod_compaction_needed(Compact_back_demod_index index);

void compact_back_demod_compact(Compact_back_demod_index index);

typedef Topform (*Compact_back_demod_materializer)(unsigned long long id,
                                                    void *context);
typedef void (*Compact_back_demod_materialized_releaser)(Topform clause,
                                                          void *context);
typedef void (*Compact_back_demod_materialized_batch_adviser)(
  const unsigned long long *ids, size_t count, void *context);

/* Rebuild from stable proof IDs after releasing all predecessor arrays.
   This is the bounded-memory path when clauses can be materialized from the
   dense/archive store; compact_back_demod_compact() remains the standalone
   encoded-stream fallback. */
void compact_back_demod_compact_materialized(
  Compact_back_demod_index index,
  Compact_back_demod_materializer materialize,
  Compact_back_demod_materialized_releaser release,
  Compact_back_demod_materialized_batch_adviser advise,
  void *context);

void compact_back_demod_compact_all_stale(Compact_back_demod_index index);

void compact_back_demod_copy_live_clauses(
  Compact_back_demod_index index, Compact_term_pool destination,
  Compact_term_rebase_map map);

void compact_back_demod_retain_live_clauses(
  Compact_back_demod_index index, Compact_term_rebase_map map);

void compact_back_demod_rebase_term_pool(
  Compact_back_demod_index index, Compact_term_pool pool,
  Compact_term_rebase_map map);

/* Return a conservative set of live clause IDs that can contain a redex for
   DEMOD/TYPE.  The owned result is sorted by decreasing proof ID.  Callers
   perform Prover9's exact rewritability test before acting on a candidate. */
unsigned long long *compact_back_demod_candidate_ids(
  Compact_back_demod_index index, Topform demod, int type, size_t *count);

void compact_back_demod_note_exact_query(
  Compact_back_demod_index index, size_t tests, size_t successes,
  size_t materializations);

void compact_back_demod_get_stats(Compact_back_demod_index index,
                                  struct compact_back_demod_stats *stats);

void compact_back_demod_free(Compact_back_demod_index index);

#endif
