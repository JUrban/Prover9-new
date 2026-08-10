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
  unsigned long long queries;
  unsigned long long candidates;
  unsigned long long exact_tests;
  unsigned long long posting_groups;
  unsigned long long symbol_occurrences;
  unsigned long long posting_bytes;
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

BOOL compact_back_demod_add(Compact_back_demod_index index, Topform clause);

BOOL compact_back_demod_remove(Compact_back_demod_index index,
                               unsigned long long proof_id);

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
