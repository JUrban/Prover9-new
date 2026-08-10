#ifndef TP_COMPACT_BACK_DEMOD_H
#define TP_COMPACT_BACK_DEMOD_H

#include "../ladr/ladr.h"
#include "compact_term_pool.h"

typedef struct compact_back_demod_index * Compact_back_demod_index;

struct compact_back_demod_stats {
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
  unsigned long long symbol_occurrences;
  unsigned long long posting_groups_examined;
  unsigned long long occurrences_examined;
  unsigned long long path_filter_checks;
  unsigned long long path_filter_rejects;
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

void compact_back_demod_note_exact_tests(Compact_back_demod_index index,
                                         size_t count);

void compact_back_demod_get_stats(Compact_back_demod_index index,
                                  struct compact_back_demod_stats *stats);

void compact_back_demod_free(Compact_back_demod_index index);

#endif
