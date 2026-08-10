#ifndef TP_COMPACT_UNIT_INDEX_H
#define TP_COMPACT_UNIT_INDEX_H

#include "../ladr/ladr.h"
#include "compact_term_pool.h"

typedef struct compact_unit_index * Compact_unit_index;

struct compact_unit_index_stats {
  unsigned long long active;
  unsigned long long peak;
  unsigned long long retired;
  unsigned long long physical;
  unsigned long long compactions;
  unsigned long long bytes_reclaimed;
  unsigned long long generalization_queries;
  unsigned long long instance_queries;
  unsigned long long instance_exact_tests;
  unsigned long long unifier_queries;
  unsigned long long unifier_exact_tests;
  unsigned long long node_items;
  unsigned long long posting_items;
  unsigned long long node_bytes;
  unsigned long long posting_bytes;
  unsigned long long record_bytes;
  unsigned long long root_bytes;
  unsigned long long token_bytes;
  unsigned long long hash_bytes;
  unsigned long long scratch_bytes;
  unsigned long long total_bytes;
  unsigned long long peak_bytes;
};

Compact_unit_index compact_unit_index_init(void);

Compact_unit_index compact_unit_index_init_with_pool(Compact_term_pool pool);

void compact_unit_index_set_compaction_stale_pct(unsigned percentage);

BOOL compact_unit_index_add(Compact_unit_index index, Topform unit);

BOOL compact_unit_index_remove(Compact_unit_index index,
                               unsigned long long proof_id);

BOOL compact_unit_index_contains(Compact_unit_index index,
                                 unsigned long long proof_id);

BOOL compact_unit_index_compaction_needed(Compact_unit_index index);

void compact_unit_index_compact(Compact_unit_index index);

void compact_unit_index_compact_all_stale(Compact_unit_index index);

void compact_unit_index_copy_live_clauses(Compact_unit_index index,
                                          Compact_term_pool destination,
                                          Compact_term_rebase_map map);

void compact_unit_index_retain_live_clauses(Compact_unit_index index,
                                            Compact_term_rebase_map map);

void compact_unit_index_rebase_term_pool(Compact_unit_index index,
                                         Compact_term_pool pool,
                                         Compact_term_rebase_map map);

/* Return the first live unit, in legacy discrimination-tree order, whose
   literal has SIGN and whose atom matches TARGET.  EXCLUDE_ID may be zero. */
unsigned long long compact_unit_generalization_first(
  Compact_unit_index index, Term target, BOOL sign,
  unsigned long long exclude_id);

/* Return all live unit IDs whose atoms are instances of PATTERN and whose
   literal has SIGN.  The owned result is sorted by decreasing proof ID,
   matching the public order of back_subsume(); EXCLUDE_ID may be zero. */
unsigned long long *compact_unit_instance_ids(
  Compact_unit_index index, Term pattern, BOOL sign,
  unsigned long long exclude_id, size_t *count);

/* Return all live unit IDs whose atoms unify with QUERY and whose literals
   have SIGN.  Results are in decreasing proof-ID/FPA order. */
unsigned long long *compact_unit_unifier_ids(
  Compact_unit_index index, Term query, BOOL sign,
  unsigned long long exclude_id, size_t *count);

void compact_unit_index_get_stats(Compact_unit_index index,
                                  struct compact_unit_index_stats *stats);

void compact_unit_index_free(Compact_unit_index index);

#endif
