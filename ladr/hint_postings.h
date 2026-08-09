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
  unsigned long long maximum_posting;
  unsigned long long dense_keys;
  unsigned long long dense_bit_bytes;
  unsigned long long dense_summary_bytes;
};

struct hint_dense_view {
  const unsigned long long *bits;
  const unsigned long long *summary;
  unsigned words;
  unsigned summary_words;
};

Hint_postings hint_postings_init(void);

void hint_postings_destroy(Hint_postings index);

void hint_postings_add(Hint_postings index, unsigned long long key,
                       unsigned id);

const unsigned *hint_postings_get(Hint_postings index,
                                  unsigned long long key,
                                  unsigned *count);

unsigned long long hint_postings_generation(Hint_postings index,
                                             unsigned long long key);

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
