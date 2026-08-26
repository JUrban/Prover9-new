/* Opt-in proof-parent guidance for incomplete proof-confirmation searches. */

#ifndef PROVERS_PROOF_PARENT_GUIDE_H
#define PROVERS_PROOF_PARENT_GUIDE_H

#include "../ladr/ladr.h"

typedef struct proof_parent_guide *Proof_parent_guide;

enum proof_parent_guide_mode {
  PROOF_PARENT_GUIDE_OFF,
  PROOF_PARENT_GUIDE_SHADOW,
  PROOF_PARENT_GUIDE_AUTHORITATIVE,
  PROOF_PARENT_GUIDE_RECIPE_REPLAY
};

enum proof_parent_rule {
  PROOF_PARENT_PARAMOD,
  PROOF_PARENT_HYPER
};

enum proof_recipe_rule {
  PROOF_RECIPE_ASSUMPTION = 1,
  PROOF_RECIPE_GOAL = 2,
  PROOF_RECIPE_DENY = 3,
  PROOF_RECIPE_COPY = 4,
  PROOF_RECIPE_BACK_REWRITE = 5,
  PROOF_RECIPE_PARAMOD = 6,
  PROOF_RECIPE_HYPER = 7,
  PROOF_RECIPE_RESOLVE = 8
};

enum proof_recipe_secondary_rule {
  PROOF_RECIPE_REWRITE = 1,
  PROOF_RECIPE_FLIP = 2
};

struct proof_recipe_position {
  int *items;
  unsigned count;
};

struct proof_recipe_positioned_parent {
  unsigned node;  /* one-based dense guide node */
  struct proof_recipe_position position;
};

struct proof_recipe_clash {
  int nucleus_literal;
  unsigned satellite_node;
  int satellite_literal;
};

struct proof_recipe_secondary {
  enum proof_recipe_secondary_rule rule;
  unsigned parent_node;  /* rewrite only */
  unsigned target;       /* rewrite only */
  unsigned direction;    /* rewrite only: 1=L->R, 2=R->L */
  int literal;           /* flip only */
};

struct proof_recipe_decoded {
  enum proof_recipe_rule rule;
  BOOL source_given;
  unsigned unary_parent;
  struct proof_recipe_positioned_parent paramod[2];
  unsigned nucleus_node;
  struct proof_recipe_clash *clashes;
  unsigned clash_count;
  struct proof_recipe_secondary *secondary;
  unsigned secondary_count;
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

unsigned proof_parent_guide_node_count(Proof_parent_guide guide);

Topform proof_parent_guide_node_body(Proof_parent_guide guide, unsigned node);

BOOL proof_parent_guide_has_complete_recipes(Proof_parent_guide guide);

BOOL proof_parent_guide_decode_recipe(Proof_parent_guide guide,
                                      unsigned node,
                                      struct proof_recipe_decoded *recipe,
                                      const char **error);

BOOL proof_parent_guide_recipe_info(Proof_parent_guide guide, unsigned node,
                                    enum proof_recipe_rule *rule,
                                    BOOL *source_given,
                                    unsigned *rewrites,
                                    unsigned *flips);

void proof_recipe_decoded_destroy(struct proof_recipe_decoded *recipe);

#endif
