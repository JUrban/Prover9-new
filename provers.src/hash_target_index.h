/* Immutable, budgeted entry directory for reconstructable generalized-hash
   unit-equation targets.  The directory deliberately stores recipe IDs,
   never pointers into packed hints or temporary target terms. */

#ifndef PROVERS_HASH_TARGET_INDEX_H
#define PROVERS_HASH_TARGET_INDEX_H

#include "../ladr/hints.h"

typedef struct hash_target_index *Hash_target_index;

typedef struct hash_target_query {
  Hash_target_index index;
  unsigned long long first;
  unsigned long long first_end;
  unsigned filter_count;
  struct {
    unsigned long long first;
    unsigned long long end;
  } filters[32];
  unsigned all_next;
  BOOL all_targets;
} Hash_target_query;

struct hash_target_index_stats {
  unsigned targets;
  unsigned root_keys;
  unsigned long long root_records;
  unsigned long long rigid_root_records;
  unsigned long long variable_root_records;
  unsigned feature_keys;
  unsigned long long feature_records;
  unsigned long long reconstructed_targets;
  unsigned long long reconstructed_positions;
  unsigned long long recipe_bytes;
  unsigned long long index_bytes;
  unsigned long long construction_peak_bytes;
  unsigned long long budget_bytes;
  unsigned long long queries;
  unsigned long long variable_queries;
  unsigned long long candidates;
  unsigned long long feature_tests;
  unsigned long long feature_rejects;
  unsigned long long maximum_posting;
  double build_seconds;
};

Hash_target_index hash_target_index_build(unsigned long long budget_bytes);

void hash_target_index_destroy(Hash_target_index index);

void hash_target_query_init(Hash_target_index index, Term replacement,
                            Hash_target_query *query);

BOOL hash_target_query_next(Hash_target_query *query, unsigned *recipe_id);

void hash_target_index_get_stats(Hash_target_index index,
                                 struct hash_target_index_stats *stats);

void fprint_hash_target_index_stats(FILE *fp, Hash_target_index index);

#endif
