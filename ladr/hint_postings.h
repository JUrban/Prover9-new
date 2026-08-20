/* Compact stable-ID posting lists for compressed hint indexes. */

#ifndef TP_HINT_POSTINGS_H
#define TP_HINT_POSTINGS_H

#include "memory.h"

typedef struct hint_postings * Hint_postings;

struct hint_postings_stats {
  unsigned long long keys;
  unsigned long long references;
  unsigned long long table_bytes;
  unsigned long long reference_bytes;
  unsigned long long profile_bytes;
  unsigned long long profile_summary_bytes;
  unsigned long long profile_mask_words;
  unsigned long long profile_key_histogram[7];
  unsigned long long profile_reference_histogram[7];
  unsigned long long maximum_posting;
  unsigned long long dense_keys;
  unsigned long long dense_bit_bytes;
  unsigned long long dense_summary_bytes;
  unsigned long long dense_budget_bytes;
  unsigned long long dense_budget_denials;
};

struct hint_profile_view {
  const unsigned *ids;
  const unsigned long long *mask_planes;
  const unsigned *literal_counts;
  /* Two words per 64-reference block: mask OR, then packed max counts. */
  const unsigned long long *block_summaries;
  unsigned long long mask_union;
  unsigned count;
  unsigned mask_blocks;
  unsigned short maximum_positive;
  unsigned short maximum_negative;
};

struct hint_dense_view {
  const unsigned long long *bits;
  const unsigned long long *summary;
  unsigned words;
  unsigned summary_words;
};

Hint_postings hint_postings_init(void);

void hint_postings_destroy(Hint_postings index);

/* Enable/disable optional two-word summaries for each 64-reference profile
   block.  This must be selected before the first reference is inserted. */
void hint_postings_set_profile_block_summaries(Hint_postings index,
                                               BOOL enabled);

/* Release optional profile block summaries while retaining the complete base
   posting index.  Returns true iff any summary storage was removed. */
BOOL hint_postings_drop_profile_block_summaries(Hint_postings index);

void hint_postings_add(Hint_postings index, unsigned long long key,
                       unsigned id);

/* Add/get a posting whose references carry a 64-bit necessary-feature mask
   and packed positive/negative literal counts.  Profile and ordinary keys
   must not be mixed in one posting. */
void hint_postings_add_profile(Hint_postings index, unsigned long long key,
                               unsigned id, unsigned long long mask,
                               unsigned positive, unsigned negative);

BOOL hint_postings_get_profile(Hint_postings index,
                               unsigned long long key,
                               struct hint_profile_view *view);

/* Complete resident allocation for a profile-only table, maintained in O(1)
   so callers can enforce a hard construction budget without rescanning it. */
unsigned long long hint_postings_profile_allocated_bytes(
  Hint_postings index);

/* Resident bytes that a profile-only table with this exact hash-table,
   posting-capacity, and mask-block layout will allocate.  This lets a
   lightweight counter plan a complete index before any large sidecars are
   committed. */
unsigned long long hint_postings_profile_layout_bytes(
  unsigned table_capacity,
  unsigned long long reference_capacity,
  unsigned long long mask_blocks,
  BOOL block_summaries);

const unsigned *hint_postings_get(Hint_postings index,
                                  unsigned long long key,
                                  unsigned *count);

unsigned long long hint_postings_generation(Hint_postings index,
                                             unsigned long long key);

/* O(1) logical reference count for hot maintenance decisions.  Full stats
   intentionally scan the table to aggregate dense/profile layout details. */
unsigned long long hint_postings_reference_count(Hint_postings index);

/* Bound the combined dense bitset and summary allocation.  Zero disables
   dense views; sparse postings remain complete. */
void hint_postings_set_dense_budget(Hint_postings index,
                                    unsigned long long bytes);

/* Return an exact, stable-ID bitset for KEY.  If CREATE is false, this is a
   read-only probe and fails unless a view already exists at BIT_CAPACITY.
   Dense sets conservatively retain stale IDs just like the source posting;
   callers must still check current hint activity. */
BOOL hint_postings_dense_view(Hint_postings index,
                              unsigned long long key,
                              unsigned bit_capacity,
                              BOOL create,
                              struct hint_dense_view *view);

void hint_postings_get_stats(Hint_postings index,
                             struct hint_postings_stats *stats);

#endif
