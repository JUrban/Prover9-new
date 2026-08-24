/* Compact canonical unit-hint term storage for compiled matching. */

#ifndef TP_HINT_TERM_TABLE_H
#define TP_HINT_TERM_TABLE_H

#include "topform.h"
#include <stdint.h>

typedef struct hint_term_table *Hint_term_table;

struct hint_term_node_view {
  BOOL variable;
  unsigned symbol_or_variable;
  unsigned arity;
  const uint32_t *children;
};

struct hint_term_table_stats {
  unsigned long long active_records;
  unsigned long long additions;
  unsigned long long removals;
  unsigned long long reinsertions;
  unsigned long long base_nodes;
  unsigned long long base_children;
  unsigned long long base_occurrences;
  unsigned long long base_intern_hits;
  unsigned long long delta_nodes;
  unsigned long long delta_children;
  unsigned long long delta_occurrences;
  unsigned long long delta_intern_hits;
  unsigned long long node_bytes;
  unsigned long long child_bytes;
  unsigned long long record_bytes;
  unsigned long long hash_bytes;
  unsigned long long hash_peak_bytes;
  unsigned long long scratch_bytes;
  unsigned long long total_bytes;
  BOOL finalized;
};

Hint_term_table hint_term_table_init(void);

void hint_term_table_destroy(Hint_term_table table);

/* Add an ordinary unit-hint atom under its stable ID.  The table borrows the
   term only for this call.  FALSE reports an invalid/duplicate ID or 32-bit
   capacity exhaustion. */
BOOL hint_term_table_add(Hint_term_table table, unsigned id,
                         BOOL positive, Term atom);

/* Deactivate ID without reclaiming immutable nodes. */
BOOL hint_term_table_remove(Hint_term_table table, unsigned id);

/* Freeze the initial arena and release its construction hash/scratch memory.
   Later additions are interned in the separate mutable delta arena. */
void hint_term_table_finalize(Hint_term_table table);

uint32_t hint_term_table_root(Hint_term_table table, unsigned id);

BOOL hint_term_table_positive(Hint_term_table table, unsigned id);

BOOL hint_term_table_node(Hint_term_table table, uint32_t handle,
                          struct hint_term_node_view *view);

void hint_term_table_get_stats(Hint_term_table table,
                               struct hint_term_table_stats *stats);

#endif
