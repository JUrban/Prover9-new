#ifndef TP_COMPACT_FEATURE_INDEX_H
#define TP_COMPACT_FEATURE_INDEX_H

#include "../ladr/ladr.h"
#include "compact_profile.h"

typedef struct compact_feature_index * Compact_feature_index;

struct compact_feature_index_stats {
  unsigned long long active;
  unsigned long long peak;
  unsigned long long retired;
  unsigned long long physical;
  unsigned long long forward_queries;
  unsigned long long forward_candidates;
  unsigned long long back_queries;
  unsigned long long back_candidates;
  struct compact_query_profile forward_profile;
  struct compact_query_profile back_profile;
  double forward_lookup_seconds;
  double back_lookup_seconds;
  unsigned long long node_bytes;
  unsigned long long label_bytes;
  unsigned long long posting_bytes;
  unsigned long long record_bytes;
  unsigned long long hash_bytes;
  unsigned long long scratch_bytes;
  unsigned long long total_bytes;
  unsigned long long peak_bytes;
};

Compact_feature_index compact_feature_index_init(int feature_length);

BOOL compact_feature_index_add(Compact_feature_index index,
                               unsigned long long proof_id,
                               const int *features);

BOOL compact_feature_index_remove(Compact_feature_index index,
                                  unsigned long long proof_id);

/* Candidate orders exactly follow di_tree_forward()/di_tree_back().  The
   caller owns the result and applies the exact subsumption test. */
unsigned long long *compact_feature_forward_candidates(
  Compact_feature_index index, const int *query, size_t *count);

unsigned long long *compact_feature_back_candidates(
  Compact_feature_index index, const int *query, size_t *count);

/* Complete the per-query profile after the caller's authoritative exact
   subsumption loop.  This is separate from retrieval because only the caller
   knows which archived clauses were materialized and where forward search
   stopped after its first success. */
void compact_feature_note_exact_query(
  Compact_feature_index index, BOOL forward,
  size_t exact_tests, size_t successes, size_t materializations);

void compact_feature_index_get_stats(Compact_feature_index index,
                                     struct compact_feature_index_stats *stats);

void compact_feature_index_free(Compact_feature_index index);

#endif
