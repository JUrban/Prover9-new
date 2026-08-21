/* Regression checks for incremental variable-map sentinel maintenance. */

#include "../ladr/ladr.h"

static void require(BOOL condition, const char *message)
{
  if (!condition) {
    fprintf(stderr, "renumber_variables_test: %s\n", message);
    exit(1);
  }
}

static Term wide_term(void)
{
  Term t = get_rigid_term("renumber_variables_test_wide", MAX_VARS);
  int i;
  for (i = 0; i < MAX_VARS; i++)
    ARG(t, i) = get_variable_term(1000 + 3 * i);
  return t;
}

int main(void)
{
  Term wide;
  Topform clause;
  Literals first, second;
  Plist pairs;
  int i;

  init_standard_ladr();

  /* Filling the final legal slot must not require a sentinel past the map. */
  wide = wide_term();
  term_renumber_variables(wide, MAX_VARS);
  for (i = 0; i < MAX_VARS; i++)
    require(VARIABLE(ARG(wide, i)) && VARNUM(ARG(wide, i)) == i,
            "100-variable boundary was not numbered in occurrence order");
  zap_term(wide);

  /* The sentinel is shared across literals, including repeated variables. */
  clause = get_topform();
  first = get_literals();
  second = get_literals();
  first->sign = second->sign = TRUE;
  first->atom = get_rigid_term("renumber_variables_test_first", 2);
  second->atom = get_rigid_term("renumber_variables_test_second", 2);
  ARG(first->atom, 0) = get_variable_term(450);
  ARG(first->atom, 1) = get_variable_term(150);
  ARG(second->atom, 0) = get_variable_term(450);
  ARG(second->atom, 1) = get_variable_term(900);
  first->next = second;
  clause->literals = first;
  upward_clause_links(clause);
  renumber_variables(clause, MAX_VARS);
  require(VARNUM(ARG(first->atom, 0)) == 0 &&
          VARNUM(ARG(first->atom, 1)) == 1 &&
          VARNUM(ARG(second->atom, 0)) == 0 &&
          VARNUM(ARG(second->atom, 1)) == 2,
          "clause-wide map did not preserve first occurrence or reuse");
  delete_clause(clause);

  /* renum_vars_map() now stops at the maintained sentinel, not stale tail. */
  clause = get_topform();
  first = get_literals();
  first->sign = TRUE;
  first->atom = get_rigid_term("renumber_variables_test_map", 2);
  ARG(first->atom, 0) = get_variable_term(9);
  ARG(first->atom, 1) = get_variable_term(4);
  clause->literals = first;
  upward_clause_links(clause);
  pairs = renum_vars_map(clause);
  require(plist_count(pairs) == 2,
          "renum_vars_map omitted or invented a populated mapping");
  zap_plist_of_terms(pairs);
  delete_clause(clause);

  printf("renumber_variables_test: PASS\n");
  return 0;
}
