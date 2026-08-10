#ifndef TP_COMPACT_FEATURE_INDEX_H
#define TP_COMPACT_FEATURE_INDEX_H

#include "../ladr/ladr.h"

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
  unsigned long long node_bytes;
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

void compact_feature_index_get_stats(Compact_feature_index index,
                                     struct compact_feature_index_stats *stats);

void compact_feature_index_free(Compact_feature_index index);

#endif
