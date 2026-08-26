#include "../provers.src/proof_recipe_replay.h"

#include <stdio.h>
#include <string.h>

static int Failures;
static int Node_attribute;
static int Recipe_attribute;

#define CHECK(test, message) do {                                    \
  if (!(test)) {                                                     \
    fprintf(stderr, "FAIL: %s (line %d)\n", (message), __LINE__); \
    Failures++;                                                      \
  }                                                                 \
} while (0)

struct guide_input {
  const char *body;
  const char *recipe;
};

static const struct guide_input Guide_inputs[] = {
  {"f(x) = x.",             "AQEAAA"},
  {"p(f(a)).",              "AQEAAA"},
  {"p(a).",                 "AQYAAQICAgICAgIA"},
  {"g(x) = x.",             "AQEAAA"},
  {"g(a) = a.",             "AQEAAA"},
  {"a = a.",                "AQQABQEBBAIB"},
  {"a = a.",                "AQQABgECAg"},
  {"p(a).",                 "AQEAAA"},
  {"q(a).",                 "AQEAAA"},
  {"-p(x) | -q(x).",        "AQEAAA"},
  {"$F.",                   "AQcACgICCAIECQIA"},
  {"wrong(a).",             "AQQAAwA"},
  {"p(a).",                 "AQYAAQICAgICAgQA"},
};

static Topform guide_clause(const struct guide_input *input, unsigned node)
{
  Topform clause = parse_clause_from_string((char *) input->body);
  clause->attributes = set_int_attribute(
    clause->attributes, Node_attribute, (int) node);
  clause->attributes = set_string_attribute(
    clause->attributes, Recipe_attribute, (char *) input->recipe);
  return clause;
}

static Topform runtime_clause(const char *body, unsigned long long id)
{
  Topform clause = parse_clause_from_string((char *) body);
  renumber_variables(clause, MAX_VARS);
  clause->id = id;
  return clause;
}

static enum proof_recipe_replay_result compute(
  Proof_parent_guide guide, unsigned node, Topform *verified,
  Topform *result, struct proof_recipe_replay_diagnostic *diagnostic)
{
  return proof_recipe_replay_compute(
    guide, node, (Topform const *) verified, result, diagnostic);
}

int main(void)
{
  Plist clauses = NULL, p;
  Proof_parent_guide guide;
  Topform verified[sizeof(Guide_inputs) / sizeof(Guide_inputs[0])] = {0};
  Topform result = NULL;
  struct proof_recipe_replay_diagnostic diagnostic;
  unsigned i;

  init_standard_ladr();
  (void) register_attribute("label", STRING_ATTRIBUTE);
  Node_attribute = register_attribute("proof_parent_node", INT_ATTRIBUTE);
  (void) register_attribute("proof_parent_para", INT_ATTRIBUTE);
  (void) register_attribute("proof_parent_hyper", INT_ATTRIBUTE);
  (void) register_attribute("proof_parent_rewrite", INT_ATTRIBUTE);
  Recipe_attribute = register_attribute(
    "proof_parent_recipe", STRING_ATTRIBUTE);
  for (i = 0; i < sizeof(Guide_inputs) / sizeof(Guide_inputs[0]); i++)
    clauses = plist_append(clauses, guide_clause(&Guide_inputs[i], i + 1));
  guide = proof_parent_guide_build(
    clauses, PROOF_PARENT_GUIDE_AUTHORITATIVE);

  verified[0] = runtime_clause("f(y) = y.", 101);
  verified[1] = runtime_clause("p(f(a)).", 102);
  verified[3] = runtime_clause("g(y) = y.", 104);
  verified[4] = runtime_clause("g(a) = a.", 105);
  verified[7] = runtime_clause("p(a).", 108);
  verified[8] = runtime_clause("q(a).", 109);
  verified[9] = runtime_clause("-p(z) | -q(z).", 110);

  CHECK(proof_recipe_replay_anchor_matches(guide, 1, verified[0]),
        "renamed input assumption anchors an exact guide root");
  CHECK(!proof_recipe_replay_anchor_matches(guide, 1, verified[1]),
        "a different input body cannot anchor a guide root");
  CHECK(compute(guide, 1, verified, &result, &diagnostic) ==
          PROOF_REPLAY_NEEDS_ASSUMPTION && result == NULL,
        "assumption recipe requires an externally checked root");

  CHECK(compute(guide, 3, verified, &result, &diagnostic) ==
          PROOF_REPLAY_VERIFIED,
        "exact-position paramodulation replays");
  CHECK(result != NULL && proof_recipe_replay_anchor_matches(guide, 3, result),
        "paramodulation result is checked against the expected body");
  result->id = 103;
  verified[2] = result;
  result = NULL;

  CHECK(compute(guide, 6, verified, &result, &diagnostic) ==
          PROOF_REPLAY_VERIFIED,
        "copy followed by a recorded rewrite replays");
  result->id = 106;
  verified[5] = result;
  result = NULL;
  CHECK(compute(guide, 7, verified, &result, &diagnostic) ==
          PROOF_REPLAY_VERIFIED,
        "recorded equality flip replays after a copy");
  result->id = 107;
  verified[6] = result;
  result = NULL;

  CHECK(compute(guide, 11, verified, &result, &diagnostic) ==
          PROOF_REPLAY_VERIFIED,
        "three-parent hyperresolution replays as exact resolutions");
  CHECK(result != NULL && result->literals == NULL,
        "hyperresolution produces the recorded empty clause");
  result->id = 111;
  verified[10] = result;
  result = NULL;

  CHECK(compute(guide, 12, verified, &result, &diagnostic) ==
          PROOF_REPLAY_FAILED &&
        diagnostic.stage == PROOF_REPLAY_STAGE_BODY && result == NULL,
        "a wrong expected body fails after computing the inference");
  CHECK(compute(guide, 13, verified, &result, &diagnostic) ==
          PROOF_REPLAY_FAILED &&
        diagnostic.stage == PROOF_REPLAY_STAGE_PRIMARY && result == NULL,
        "a wrong paramodulation position fails without a fatal error");

  proof_parent_guide_destroy(guide);
  for (i = 0; i < sizeof(verified) / sizeof(verified[0]); i++)
    if (verified[i] != NULL)
      delete_clause(verified[i]);
  for (p = clauses; p != NULL; p = p->next)
    delete_clause(p->v);
  zap_plist(clauses);

  if (Failures != 0)
    return 1;
  puts("proof_recipe_replay_test: PASS");
  return 0;
}
