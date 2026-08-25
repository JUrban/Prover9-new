/* Reverse target planning for the first, deliberately bounded unit-
   paramodulation experiment. */

#ifndef PROVERS_HASH_TARGET_INFERENCE_H
#define PROVERS_HASH_TARGET_INFERENCE_H

#include "hash_target_index.h"
#include "../ladr/mindex.h"

typedef struct hash_target_inference *Hash_target_inference;

struct hash_target_inference_stats {
  unsigned long long active_units;
  unsigned long long active_unit_peak;
  unsigned long long plans;
  unsigned long long from_sides;
  unsigned long long variable_from_sides;
  unsigned long long target_recipes;
  unsigned long long target_positions;
  unsigned long long compatible_positions;
  unsigned long long required_queries;
  unsigned long long required_query_answers;
  unsigned long long unique_partners;
  unsigned long long ordinary_hash_hits;
  unsigned long long covered_hash_hits;
  unsigned long long missed_hash_hits;
  unsigned long long duplicate_partners;
  unsigned long long requirements;
  unsigned long long requirement_peak;
  unsigned long long requirement_bytes;
  unsigned long long requirement_peak_bytes;
  unsigned long long requirement_queries;
  unsigned long long requirement_answers;
  unsigned long long requirement_budget_bytes;
  unsigned long long workspace_bytes;
  unsigned long long workspace_peak_bytes;
  double planning_seconds;
};

Hash_target_inference hash_target_inference_init(Hash_target_index targets,
                                                 int fpa_depth,
                                                 unsigned long long
                                                   requirement_budget_bytes);

void hash_target_inference_destroy(Hash_target_inference inference);

void hash_target_inference_update(Hash_target_inference inference,
                                  Topform clause, Indexop op);

/* Plan older active unit into-parents for GIVEN as the from parent. */
void hash_target_inference_plan_from(Hash_target_inference inference,
                                     Topform given);

unsigned hash_target_inference_partner_count(Hash_target_inference inference,
                                             BOOL given_is_from);

Topform hash_target_inference_partner(Hash_target_inference inference,
                                      BOOL given_is_from, unsigned index);

BOOL hash_target_inference_partner_planned(Hash_target_inference inference,
                                           Topform clause,
                                           BOOL given_is_from);

void hash_target_inference_note_hash_hit(Hash_target_inference inference,
                                         BOOL covered);

void hash_target_inference_get_stats(Hash_target_inference inference,
                                     struct hash_target_inference_stats *stats);

void fprint_hash_target_inference_stats(FILE *fp,
                                        Hash_target_inference inference);

#endif
