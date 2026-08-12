#ifndef TP_COMPACT_TERM_POOL_H
#define TP_COMPACT_TERM_POOL_H

#include "../ladr/ladr.h"

#include <stdint.h>

typedef struct compact_term_pool * Compact_term_pool;
typedef struct compact_term_rebase_map * Compact_term_rebase_map;
typedef uint64_t Compact_term_slice;

/* One hot term reference must remain the same eight bytes as the historical
   (uint32_t offset, uint32_t length) pair.  Forty offset bits cover four TiB
   of 32-bit tokens; twenty-four length bits cover a single 16M-token term. */
#define COMPACT_TERM_SLICE_OFFSET_BITS 40
#define COMPACT_TERM_SLICE_LENGTH_BITS 24
#define COMPACT_TERM_SLICE_OFFSET_MAX UINT64_C(0xffffffffff)
#define COMPACT_TERM_SLICE_LENGTH_MAX UINT32_C(0x00ffffff)

struct compact_term_pool_stats {
  unsigned long long clause_entries;
  unsigned long long serializations;
  unsigned long long lookups;
  unsigned long long hits;
  unsigned long long reused_tokens;
  unsigned long long logical_tokens;
  unsigned long long token_bytes;
  unsigned long long token_growths;
  unsigned long long token_copy_bytes;
  unsigned long long rebase_growths;
  unsigned long long rebase_copy_bytes;
  unsigned long long streamed_rebases;
  unsigned long long file_sorted_rebases;
  unsigned long long directory_bytes;
  unsigned long long total_bytes;
  unsigned long long peak_bytes;
  unsigned long long compactions;
  unsigned long long bytes_reclaimed;
  BOOL sharing_profile_enabled;
  unsigned long long profile_term_occurrences;
  unsigned long long profile_unique_terms;
  unsigned long long profile_child_references;
  unsigned long long profile_atom_roots;
  unsigned long long profile_dag_payload_bytes;
  unsigned long long profile_table_bytes;
};

Compact_term_pool compact_term_pool_init(void);

Compact_term_rebase_map compact_term_rebase_map_init(void);

/* Copy one complete clause serialization at most once and remember how its
   old token interval maps into DESTINATION. */
BOOL compact_term_pool_copy_clause(Compact_term_pool destination,
                                   Compact_term_pool source,
                                   Compact_term_rebase_map map,
                                   unsigned long long proof_id);

/* Mark one source clause for an in-place compaction.  Repeated proof IDs are
   deduplicated by their source-directory slots without allocating another
   proof-ID hash table. */
BOOL compact_term_rebase_map_retain_clause(Compact_term_rebase_map map,
                                           Compact_term_pool source,
                                           unsigned long long proof_id);

/* Predict the resident allocation reduction of an in-place retained
   compaction without materializing its rebase vector or touching tokens. */
unsigned long long compact_term_pool_retained_reclaimable_bytes(
  Compact_term_pool pool, Compact_term_rebase_map map);

/* Move every retained source interval downward in the existing token array,
   rebuild the proof-ID directory, and finalize MAP for offset rebasing. */
void compact_term_pool_compact_retained(Compact_term_pool pool,
                                        Compact_term_rebase_map map);

void compact_term_rebase_map_finalize(Compact_term_rebase_map map);

Compact_term_slice compact_term_rebase_slice(Compact_term_rebase_map map,
                                             Compact_term_slice old_slice);

uint32_t compact_term_rebase_offset(Compact_term_rebase_map map,
                                    uint32_t old_offset);

void compact_term_rebase_map_free(Compact_term_rebase_map map);

void compact_term_pool_finish_compaction(Compact_term_pool destination,
                                         Compact_term_pool source);

/* Enable exact cross-clause subterm accounting.  This diagnostic allocates
   its own hash table and is deliberately off during ordinary measurements. */
void compact_term_pool_enable_sharing_profile(Compact_term_pool pool);

/* Return a stable 32-bit offset for TARGET's prefix-token representation.
   The first request for PROOF_ID serializes all clause atoms; later compact
   indexes reuse subterm slices from that immutable clause interval. */
uint32_t compact_term_pool_intern(Compact_term_pool pool,
                                  unsigned long long proof_id,
                                  Literals literals, Term target,
                                  uint32_t *length);

Compact_term_slice compact_term_pool_intern_slice(Compact_term_pool pool,
                                                  unsigned long long proof_id,
                                                  Literals literals,
                                                  Term target);

/* Append an already validated prefix-token sequence.  This is used when an
   owning compact index rebuilds into a fresh private pool. */
uint32_t compact_term_pool_append(Compact_term_pool pool,
                                  const int32_t *tokens, uint32_t length);

Compact_term_slice compact_term_pool_append_slice(Compact_term_pool pool,
                                                  const int32_t *tokens,
                                                  uint32_t length);

BOOL compact_term_slice_encode(unsigned long long offset, uint32_t length,
                               Compact_term_slice *slice);
unsigned long long compact_term_slice_offset(Compact_term_slice slice);
uint32_t compact_term_slice_length(Compact_term_slice slice);
BOOL compact_term_slice_subslice(Compact_term_slice slice, uint32_t start,
                                 uint32_t length,
                                 Compact_term_slice *subslice);

/* Resolve one packed logical slice to a contiguous local span. */
const int32_t *compact_term_pool_slice_tokens(Compact_term_pool pool,
                                             Compact_term_slice slice);

/* Assign the logical base of an unused pool.  Production pools use zero;
   accelerated numerical-limit tests use a high base with bounded storage. */
void compact_term_pool_set_logical_base(Compact_term_pool pool,
                                        unsigned long long logical_base);

const int32_t *compact_term_pool_tokens(Compact_term_pool pool);

size_t compact_term_pool_token_count(Compact_term_pool pool);

void compact_term_pool_get_stats(Compact_term_pool pool,
                                 struct compact_term_pool_stats *stats);

void compact_term_pool_free(Compact_term_pool pool);

#endif
