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
};

Hint_postings hint_postings_init(void);

void hint_postings_destroy(Hint_postings index);

void hint_postings_add(Hint_postings index, unsigned long long key,
                       unsigned id, unsigned version);

const unsigned long long *hint_postings_get(Hint_postings index,
                                            unsigned long long key,
                                            unsigned *count);

unsigned hint_posting_id(unsigned long long reference);

unsigned hint_posting_version(unsigned long long reference);

void hint_postings_get_stats(Hint_postings index,
                             struct hint_postings_stats *stats);

#endif
