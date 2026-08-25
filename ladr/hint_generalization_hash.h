/* Experimental precomputed hash-only hint matcher. */

#ifndef TP_HINT_GENERALIZATION_HASH_H
#define TP_HINT_GENERALIZATION_HASH_H

#include "topform.h"
#include "unify.h"
#include <stdint.h>

typedef struct hint_generalization_hash *Hint_generalization_hash;

struct hint_generalization_hash_stats {
  unsigned long long entries;
  unsigned long long capacity;
  unsigned long long bytes;
  unsigned long long exact_hints;
  unsigned long long exact_new_entries;
  unsigned long long complete_hints;
  unsigned long long partial_hints;
  unsigned long long generated_attempts;
  unsigned long long generated_new_entries;
  unsigned long long duplicates;
  unsigned long long partial_cap_skips;
  unsigned long long rehashes;
  unsigned long long queries;
  unsigned long long hits;
  unsigned long long probes;
  unsigned long long maximum_probe;
  unsigned complete_nodes;
  unsigned partial_per_hint;
  unsigned long long maximum_entries;
  BOOL finalized;
  unsigned long long target_recipes;
  unsigned long long target_exact_recipes;
  unsigned long long target_partial_recipes;
  unsigned long long target_complete_recipes;
  unsigned long long target_position_records;
  unsigned long long target_variable_positions;
  unsigned long long target_rigid_positions;
  unsigned long long target_recipe_bytes;
  unsigned long long target_complete_token_bytes;
  unsigned target_max_nodes;
  unsigned target_max_depth;
  unsigned long long target_budget_bytes;
};

Hint_generalization_hash hint_generalization_hash_init(
  unsigned complete_nodes, unsigned partial_per_hint,
  unsigned long long maximum_entries, unsigned expected_hints);

/* Retain compact reconstruction recipes for unique positive unit-equation
   targets while they are generated.  A zero budget leaves the sidecar off.
   Construction stops rather than silently exceeding a nonzero budget. */
void hint_generalization_hash_enable_target_recipes(
  Hint_generalization_hash table, unsigned long long budget_bytes);

void hint_generalization_hash_destroy(Hint_generalization_hash table);

/* Add the exact variant key and remember the hint length for the later,
   population-ordered generalization build. */
BOOL hint_generalization_hash_add_exact(Hint_generalization_hash table,
                                        unsigned id, Topform hint);

BOOL hint_generalization_hash_is_complete_hint(
  Hint_generalization_hash table, unsigned id);

/* Exhaustive first-order clause generalizations for a short hint. */
BOOL hint_generalization_hash_add_complete(Hint_generalization_hash table,
                                           unsigned id, Topform hint);

/* Exact plus bounded one-subterm abstractions for a longer hint. */
/* FALSE means the global entry cap was reached; later large hints can be
   skipped without further materialization. */
BOOL hint_generalization_hash_add_partial(Hint_generalization_hash table,
                                          unsigned id, Topform hint);

void hint_generalization_hash_finalize(Hint_generalization_hash table);

/* Return the selected stable hint ID, or zero on a hash miss. */
unsigned hint_generalization_hash_lookup(Hint_generalization_hash table,
                                         Topform clause);

/* Hash a positive unit paramodulant directly from the live substitutions,
   without allocating its Literal, Term, Topform, or justification.  Both
   equality orientations are queried because later equality orientation can
   swap the sides.  TRUE reports that this candidate shape was supported;
   IDs are zero on misses.  These preview probes are accounted by the caller,
   not in the authoritative query counters above. */
BOOL hint_generalization_hash_lookup_unit_paramod(
  Hint_generalization_hash table,
  Literals from_lit, int from_side, Context from_subst,
  Literals into_lit, Ilist into_pos, Context into_subst,
  unsigned *normal_id, unsigned *flipped_id,
  unsigned *term_nodes, BOOL *reflexive,
  unsigned long long *probes);

/* Allocation-free, non-accounting lookup of an already materialized unit
   equality in both side orders.  Used to distinguish virtual-walker errors
   from later simplification changes. */
BOOL hint_generalization_hash_preview_unit_equality(
  Hint_generalization_hash table, Literals literal,
  unsigned *normal_id, unsigned *flipped_id,
  unsigned long long *probes);

void hint_generalization_hash_get_stats(
  Hint_generalization_hash table, struct hint_generalization_hash_stats *stats);

#endif
