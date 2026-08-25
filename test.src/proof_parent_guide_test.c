#include "../provers.src/proof_parent_guide.h"

#include <stdio.h>

static int Failures;

#define CHECK(test, message) do {                                    \
  if (!(test)) {                                                     \
    fprintf(stderr, "FAIL: %s (line %d)\n", (message), __LINE__); \
    Failures++;                                                      \
  }                                                                 \
} while (0)

static Topform labeled_clause(const char *body, const char *node,
                              const char *primary)
{
  Topform clause = parse_clause_from_string((char *) body);
  clause->attributes = set_string_attribute(
    clause->attributes, label_att(), (char *) node);
  if (primary != NULL)
    clause->attributes = set_string_attribute(
      clause->attributes, label_att(), (char *) primary);
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
  clauses = plist_append(clauses, labeled_clause(
    "p(x).", "proof_parent_node=1", NULL));
  clauses = plist_append(clauses, labeled_clause(
    "q(x).", "proof_parent_node=2", NULL));
  clauses = plist_append(clauses, labeled_clause(
    "result(x).", "proof_parent_node=3", "proof_parent_para=1,2"));
  /* A repeated body must share its exact-body directory entry. */
  clauses = plist_append(clauses, labeled_clause(
    "p(y).", "proof_parent_node=4", NULL));
  clauses = plist_append(clauses, labeled_clause(
    "hyper_result(x).", "proof_parent_node=5", "proof_parent_hyper=2,3"));

  guide = proof_parent_guide_build(
    clauses, PROOF_PARENT_GUIDE_AUTHORITATIVE);
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
