#include "../provers.src/proof_parent_guide.h"

#include <stdio.h>

static int Failures;
static int Node_attribute;
static int Para_attribute;
static int Hyper_attribute;
static int Rewrite_attribute;
static int Recipe_attribute;

#define CHECK(test, message) do {                                    \
  if (!(test)) {                                                     \
    fprintf(stderr, "FAIL: %s (line %d)\n", (message), __LINE__); \
    Failures++;                                                      \
  }                                                                 \
} while (0)

static Topform labeled_clause(const char *body, int node,
                              int primary_attribute, int first, int second,
                              const char *recipe)
{
  Topform clause = parse_clause_from_string((char *) body);
  clause->attributes = set_int_attribute(
    clause->attributes, Node_attribute, node);
  if (primary_attribute >= 0) {
    clause->attributes = set_int_attribute(
      clause->attributes, primary_attribute, first);
    clause->attributes = set_int_attribute(
      clause->attributes, primary_attribute, second);
  }
  if (recipe != NULL)
    clause->attributes = set_string_attribute(
      clause->attributes, Recipe_attribute, (char *) recipe);
  return clause;
}

static Topform runtime_clause(const char *body, unsigned long long id)
{
  Topform clause = parse_clause_from_string((char *) body);
  renumber_variables(clause, MAX_VARS);
  clause->id = id;
  return clause;
}

int main(void)
{
  Plist clauses = NULL, p;
  Proof_parent_guide guide;
  Topform a, b1, b2, c, outside;
  Topform *partners;
  unsigned count;

  init_standard_ladr();
  (void) register_attribute("label", STRING_ATTRIBUTE);
  Node_attribute = register_attribute("proof_parent_node", INT_ATTRIBUTE);
  Para_attribute = register_attribute("proof_parent_para", INT_ATTRIBUTE);
  Hyper_attribute = register_attribute("proof_parent_hyper", INT_ATTRIBUTE);
  Rewrite_attribute = register_attribute("proof_parent_rewrite", INT_ATTRIBUTE);
  Recipe_attribute = register_attribute(
    "proof_parent_recipe", STRING_ATTRIBUTE);
  (void) Rewrite_attribute;
  clauses = plist_append(clauses, labeled_clause(
    "p(x).", 1, -1, 0, 0, "AQEAAA"));
  clauses = plist_append(clauses, labeled_clause(
    "q(x).", 2, -1, 0, 0, "AQEAAA"));
  clauses = plist_append(clauses, labeled_clause(
    "result(x).", 3, Para_attribute, 1, 2,
    "AQYAAQICAgIDAgICAA"));
  /* A repeated body must share its exact-body directory entry. */
  clauses = plist_append(clauses, labeled_clause(
    "p(y).", 4, -1, 0, 0, "AQEAAA"));
  clauses = plist_append(clauses, labeled_clause(
    "hyper_result(x).", 5, Hyper_attribute, 2, 3,
    "AQcAAgECAwIA"));
  clauses = plist_append(clauses, labeled_clause(
    "rewritten(x) = x.", 6, -1, 0, 0,
    "AQQABAIBAQICAgI"));

  guide = proof_parent_guide_build(
    clauses, PROOF_PARENT_GUIDE_AUTHORITATIVE);
  CHECK(proof_parent_guide_has_complete_recipes(guide),
        "all guide recipes are decoded");
  {
    struct proof_recipe_decoded recipe;
    const char *error = NULL;
    CHECK(proof_parent_guide_decode_recipe(guide, 3, &recipe, &error),
          "paramodulation recipe can be decoded on demand");
    CHECK(error == NULL && recipe.rule == PROOF_RECIPE_PARAMOD,
          "decoded recipe preserves its primary rule");
    CHECK(recipe.paramod[0].node == 1 &&
          recipe.paramod[0].position.count == 2 &&
          recipe.paramod[1].node == 2 &&
          recipe.paramod[1].position.count == 3,
          "decoded recipe preserves parents and exact positions");
    proof_recipe_decoded_destroy(&recipe);
    CHECK(proof_parent_guide_decode_recipe(guide, 6, &recipe, &error),
          "copy/rewrite/flip recipe can be decoded on demand");
    CHECK(recipe.rule == PROOF_RECIPE_COPY && recipe.unary_parent == 4 &&
          recipe.secondary_count == 2 &&
          recipe.secondary[0].rule == PROOF_RECIPE_REWRITE &&
          recipe.secondary[0].parent_node == 1 &&
          recipe.secondary[0].target == 2 &&
          recipe.secondary[0].direction == 2 &&
          recipe.secondary[1].rule == PROOF_RECIPE_FLIP &&
          recipe.secondary[1].literal == 1,
          "decoded recipe preserves ordered secondary operations");
    proof_recipe_decoded_destroy(&recipe);
  }
  a = runtime_clause("p(z).", 10);
  b1 = runtime_clause("q(z).", 11);
  b2 = runtime_clause("q(w).", 14);
  c = runtime_clause("result(z).", 12);
  outside = runtime_clause("outside(z).", 13);
  proof_parent_guide_activate(guide, a);
  proof_parent_guide_activate(guide, b1);
  proof_parent_guide_activate(guide, b2);
  proof_parent_guide_activate(guide, c);
  proof_parent_guide_activate(guide, outside);

  CHECK(proof_parent_guide_pair_test(
          guide, a, b1, PROOF_PARENT_PARAMOD),
        "recorded paramodulation parents are accepted");
  CHECK(!proof_parent_guide_pair_test(
          guide, a, c, PROOF_PARENT_PARAMOD),
        "unrecorded paramodulation parents are rejected");
  CHECK(proof_parent_guide_pair_test(
          guide, b1, c, PROOF_PARENT_HYPER),
        "recorded hyperresolution co-parents are accepted");
  CHECK(!proof_parent_guide_pair_test(
          guide, b1, outside, PROOF_PARENT_HYPER),
        "unmapped hyperresolution parent is rejected");

  partners = proof_parent_guide_paramod_partners(guide, a, &count);
  CHECK(count == 2, "both active runtime copies of a proof body are returned");
  CHECK(partners[0]->id == 11 && partners[1]->id == 14,
        "sparse partners are unique and ordered by runtime ID");
  proof_parent_guide_deactivate(guide, b1);
  partners = proof_parent_guide_paramod_partners(guide, a, &count);
  CHECK(count == 1 && partners[0]->id == 14,
        "disabled runtime parents leave sparse enumeration");

  proof_parent_guide_destroy(guide);
  delete_clause(a);
  delete_clause(b1);
  delete_clause(b2);
  delete_clause(c);
  delete_clause(outside);
  for (p = clauses; p != NULL; p = p->next)
    delete_clause(p->v);
  zap_plist(clauses);

  if (Failures != 0)
    return 1;
  puts("proof_parent_guide_test: PASS");
  return 0;
}
