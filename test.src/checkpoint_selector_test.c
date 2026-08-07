/* Regression test for checkpoint bulk restoration of given selectors. */

#include "../ladr/ladr.h"
#include "../provers.src/giv_select.h"

static void fail(const char *message)
{
  fprintf(stderr, "FAIL: %s\n", message);
  exit(1);
}

int main(void)
{
  Plist rules = NULL;
  Clist sos;
  Topform initial, ordinary, given;
  char *type = NULL;

  init_standard_ladr();

  rules = plist_append(rules,
                       selector_rule_term("I", "high", "age",
                                          "initial", 1));
  rules = plist_append(rules,
                       selector_rule_term("A", "low", "age", "all", 1));
  init_giv_select(rules);
  zap_plist_of_terms(rules);

  sos = clist_init("checkpoint selector regression");
  initial = get_topform();
  ordinary = get_topform();
  initial->id = 1;
  initial->initial = TRUE;
  ordinary->id = 2;
  ordinary->initial = FALSE;
  clist_append(initial, sos);
  clist_append(ordinary, sos);

  /* Checkpoint restore owns a populated SOS list before rebuilding indexes. */
  bulk_insert_into_sos2(sos);

  given = get_given_clause2(sos, 0, NULL, &type);
  if (given != initial || strcmp(type, "I") != 0)
    fail("the restored high-priority clause was not selected first");

  given = get_given_clause2(sos, 1, NULL, &type);
  if (given != ordinary || strcmp(type, "A") != 0)
    fail("a high-priority clause remained in the low selector index");

  if (!clist_empty(sos) || givens_available())
    fail("selector indexes were not empty after both selections");

  clist_free(sos);
  zap_topform(initial);
  zap_topform(ordinary);
  zap_given_selectors();
  puts("checkpoint selector test: PASS");
  return 0;
}
