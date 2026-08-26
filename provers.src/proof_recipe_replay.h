/* Strict checked replay of recipes stored in a proof-parent guide. */

#ifndef PROVERS_PROOF_RECIPE_REPLAY_H
#define PROVERS_PROOF_RECIPE_REPLAY_H

#include "proof_parent_guide.h"

enum proof_recipe_replay_result {
  PROOF_REPLAY_VERIFIED,
  PROOF_REPLAY_NEEDS_ASSUMPTION,
  PROOF_REPLAY_NEEDS_GOAL,
  PROOF_REPLAY_NEEDS_DENIAL,
  PROOF_REPLAY_FAILED
};

enum proof_recipe_replay_stage {
  PROOF_REPLAY_STAGE_NONE,
  PROOF_REPLAY_STAGE_DECODE,
  PROOF_REPLAY_STAGE_PARENT,
  PROOF_REPLAY_STAGE_PRIMARY,
  PROOF_REPLAY_STAGE_REWRITE,
  PROOF_REPLAY_STAGE_FLIP,
  PROOF_REPLAY_STAGE_BODY
};

struct proof_recipe_replay_diagnostic {
  unsigned node;
  enum proof_recipe_replay_stage stage;
  unsigned secondary;
  const char *message;
};

/* VERIFIED is indexed by dense node ID minus one.  On success, *RESULT is a
   newly allocated, structurally checked clause whose parent IDs refer to
   VERIFIED.  Root recipes return NEEDS_* and leave *RESULT NULL. */
enum proof_recipe_replay_result proof_recipe_replay_compute(
  Proof_parent_guide guide, unsigned node, Topform const *verified,
  Topform *result, struct proof_recipe_replay_diagnostic *diagnostic);

/* Check that ACTUAL is structurally identical to NODE's expected body.  This
   is used only to anchor roots in the supplied problem; it never derives a
   non-root node from the guide body. */
BOOL proof_recipe_replay_anchor_matches(Proof_parent_guide guide,
                                        unsigned node, Topform actual);

#endif
