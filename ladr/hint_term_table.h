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
  unsigned long long base_rehashes;
  unsigned long long delta_rehashes;
  unsigned long long node_bytes;
  unsigned long long child_bytes;
  unsigned long long record_bytes;
  unsigned long long hash_bytes;
  unsigned long long hash_peak_bytes;
  unsigned long long scratch_bytes;
  unsigned long long total_bytes;
  unsigned long long match_attempts;
  unsigned long long match_successes;
  unsigned long long match_nodes;
  unsigned long long match_rigid_tests;
  unsigned long long match_rigid_rejects;
  unsigned long long match_first_bindings;
  unsigned long long match_repeated_tests;
  unsigned long long match_repeated_rejects;
  BOOL finalized;
};

Hint_term_table hint_term_table_init(void);

void hint_term_table_destroy(Hint_term_table table);

/* Add an ordinary unit-hint atom under its stable ID.  The table borrows the
   term only for this call.  FALSE reports an invalid/duplicate ID or 32-bit
   capacity exhaustion. */
BOOL hint_term_table_add(Hint_term_table table, unsigned id,
                         BOOL positive, Term atom);

/* Add a supported compressed unit clause by streaming its preorder bytes
   directly into the canonical arena, without materializing a Term tree. */
BOOL hint_term_table_add_compressed(Hint_term_table table, unsigned id,
                                    Topform compressed);

/* Deactivate ID without reclaiming immutable nodes. */
BOOL hint_term_table_remove(Hint_term_table table, unsigned id);

/* Freeze the initial arena and release its construction hash/scratch memory.
   Later additions are interned in the separate mutable delta arena. */
void hint_term_table_finalize(Hint_term_table table);

uint32_t hint_term_table_root(Hint_term_table table, unsigned id);

BOOL hint_term_table_positive(Hint_term_table table, unsigned id);

BOOL hint_term_table_node(Hint_term_table table, uint32_t handle,
                          struct hint_term_node_view *view);

/* Compare two target subterms while sharing their common path prefix.
   Return 1 for equal canonical handles, 0 for unequal handles, and -1 when
   either path is invalid for the target root. */
int hint_term_table_compare_paths(Hint_term_table table, uint32_t root,
                                  const unsigned *first,
                                  unsigned first_length,
                                  const unsigned *second,
                                  unsigned second_length);

/* Return 1 when the target subterm at PATH has SYMBOL, 0 when the exact
   necessary condition fails, and -1 for invalid table/input state. */
int hint_term_table_path_symbol(Hint_term_table table, uint32_t root,
                                const unsigned *path,
                                unsigned path_length,
                                unsigned symbol);

/* Allocation-free one-way match: PATTERN variables may bind canonical
   subterm handles from retained unit hint ID.  TRUE means ID was supported
   and *MATCHED is authoritative; FALSE requests the compressed fallback. */
BOOL hint_term_table_matches(Hint_term_table table, unsigned id,
                             BOOL positive, Term pattern, BOOL *matched);

/* Same exact operation without charging authoritative lookup counters. */
BOOL hint_term_table_matches_readonly(Hint_term_table table, unsigned id,
                                      BOOL positive, Term pattern,
                                      BOOL *matched);

void hint_term_table_get_stats(Hint_term_table table,
                               struct hint_term_table_stats *stats);

#endif
