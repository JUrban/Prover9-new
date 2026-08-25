/* Opt-in proof-parent guidance for incomplete proof-confirmation searches. */

#ifndef PROVERS_PROOF_PARENT_GUIDE_H
#define PROVERS_PROOF_PARENT_GUIDE_H

#include "../ladr/ladr.h"

typedef struct proof_parent_guide *Proof_parent_guide;

enum proof_parent_guide_mode {
  PROOF_PARENT_GUIDE_OFF,
  PROOF_PARENT_GUIDE_SHADOW,
  PROOF_PARENT_GUIDE_AUTHORITATIVE
};

enum proof_parent_rule {
  PROOF_PARENT_PARAMOD,
  PROOF_PARENT_HYPER
};

Proof_parent_guide proof_parent_guide_build(
  Plist clauses, enum proof_parent_guide_mode mode);

void proof_parent_guide_destroy(Proof_parent_guide guide);

void proof_parent_guide_activate(Proof_parent_guide guide, Topform clause);

void proof_parent_guide_deactivate(Proof_parent_guide guide, Topform clause);

void proof_parent_guide_note_given(Proof_parent_guide guide, Topform clause);

BOOL proof_parent_guide_pair_test(Proof_parent_guide guide,
                                  Topform first, Topform second,
                                  enum proof_parent_rule rule);

/* The returned array belongs to GUIDE and remains valid until the next call. */
Topform *proof_parent_guide_paramod_partners(Proof_parent_guide guide,
                                             Topform given,
                                             unsigned *count);

void fprint_proof_parent_guide_stats(FILE *fp, Proof_parent_guide guide);

#endif
