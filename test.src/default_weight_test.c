/* Regression checks for the exact-default clause-weight fast path. */

#include "../ladr/ladr.h"

static void require_weight(const char *text, double expected,
                           const char *message)
{
  Topform clause = parse_clause_from_string((char *) text);
  double actual = clause_weight(clause->literals);

  if (actual != expected) {
    fprintf(stderr, "default_weight_test: %s: got %.3f, expected %.3f\n",
            message, actual, expected);
    exit(1);
  }
  delete_clause(clause);
}

static Plist singleton_term_list(const char *text)
{
  return plist_append(NULL, parse_term_from_string((char *) text));
}

int main(void)
{
  Plist rules;

  init_standard_ladr();
  init_resonators(NULL);
  init_weight(NULL, 1, 1, 0, 0, 1, 1, 0, 0, 0, 0);
  require_weight("p(f(x),a) | -q(g(b,c)).", 8,
                 "default mode counts exactly the term nodes");
  require_weight("f(a) = g(x).", 5,
                 "default mode counts the equality root");

  init_weight(NULL, 2, 3, 5, 7, 3, 3, 0, 0, 0, 0);
  require_weight("p(f(x),a) | -q(g(b,c)).", 27,
                 "nondefault parameters use the general evaluator");

  rules = singleton_term_list("weight(f(x)) = 9.");
  init_weight(rules, 1, 1, 0, 0, 1, 1, 0, 0, 0, 0);
  zap_plist_of_terms(rules);
  require_weight("p(f(a)).", 10,
                 "ordinary rules bypass default symbol weighting");

  init_weight(NULL, 1, 1, 0, 0, 1, 1, 0, 0, 0, 0);
  rules = singleton_term_list("weight(p(_),4).");
  init_resonators(rules);
  zap_plist_of_terms(rules);
  require_weight("p(f(a)).", 4,
                 "resonators bypass default symbol weighting");

  printf("default_weight_test: PASS\n");
  return 0;
}
