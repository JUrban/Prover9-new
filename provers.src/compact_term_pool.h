#ifndef TP_COMPACT_TERM_POOL_H
#define TP_COMPACT_TERM_POOL_H

#include "../ladr/ladr.h"

#include <stdint.h>

typedef struct compact_term_pool * Compact_term_pool;

struct compact_term_pool_stats {
  unsigned long long clause_entries;
  unsigned long long serializations;
  unsigned long long lookups;
  unsigned long long hits;
  unsigned long long reused_tokens;
  unsigned long long logical_tokens;
  unsigned long long token_bytes;
  unsigned long long directory_bytes;
  unsigned long long total_bytes;
  unsigned long long peak_bytes;
};

Compact_term_pool compact_term_pool_init(void);

/* Return a stable 32-bit offset for TARGET's prefix-token representation.
   The first request for PROOF_ID serializes all clause atoms; later compact
   indexes reuse subterm slices from that immutable clause interval. */
uint32_t compact_term_pool_intern(Compact_term_pool pool,
                                  unsigned long long proof_id,
                                  Literals literals, Term target,
                                  uint32_t *length);

/* Append an already validated prefix-token sequence.  This is used when an
   owning compact index rebuilds into a fresh private pool. */
uint32_t compact_term_pool_append(Compact_term_pool pool,
                                  const int32_t *tokens, uint32_t length);

const int32_t *compact_term_pool_tokens(Compact_term_pool pool);

void compact_term_pool_get_stats(Compact_term_pool pool,
                                 struct compact_term_pool_stats *stats);

void compact_term_pool_free(Compact_term_pool pool);

#endif
