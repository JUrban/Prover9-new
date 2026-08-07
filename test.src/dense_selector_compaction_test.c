/* Regression test for dense passive record/heap compaction. */

#include "../ladr/ladr.h"
#include "../provers.src/giv_select.h"

#define CLAUSES 1500
#define SELECT_BEFORE_COMPACT 600

static Topform Stored[CLAUSES];
static size_t Stored_count = 0;

static void fail(char *message)
{
  fprintf(stderr, "FAIL: %s\n", message);
  exit(1);
}

static size_t archive_clause(Topform c)
{
  size_t position = Stored_count;
  if (position >= CLAUSES)
    fail("archive callback overflow");
  Stored[Stored_count++] = c;
  return position;
}

static Topform activate_clause(size_t position, unsigned long long id,
                               unsigned long long hint_id)
{
  Topform c;
  (void) hint_id;
  if (position >= Stored_count)
    fail("activation position out of range");
  c = Stored[position];
  if (c->id != id)
    fail("activation ID mismatch");
  return c;
}

static size_t retain_position(size_t old_position, void *context)
{
  (void) context;
  return old_position;
}

int main(void)
{
  Plist rules = NULL;
  Clist sos;
  int i;
  unsigned long long compactions, reclaimed;

  init_standard_ladr();
  configure_dense_passive(TRUE, archive_clause, activate_clause);
  rules = plist_append(rules,
                       selector_rule_term("A", "low", "age", "all", 1));
  init_giv_select(rules);
  zap_plist_of_terms(rules);
  sos = clist_init("dense selector compaction regression");

  for (i = 0; i < CLAUSES; i++) {
    Topform c = get_topform();
    c->id = (unsigned long long) i + 1;
    insert_into_sos2(c, sos);
  }
  for (i = 0; i < SELECT_BEFORE_COMPACT; i++) {
    char *type = NULL;
    Topform c = get_given_clause2(sos, i, NULL, &type);
    if (c == NULL || c->id != (unsigned long long) i + 1 ||
        strcmp(type, "A") != 0)
      fail("age order changed before compaction");
  }
  if (!dense_passive_compaction_needed())
    fail("expected compaction threshold was not reached");
  dense_passive_compact(retain_position, NULL);
  dense_passive_compaction_stats(&compactions, &reclaimed);
  if (compactions != 1 || reclaimed != SELECT_BEFORE_COMPACT)
    fail("incorrect compaction accounting");
  if (dense_passive_size() != CLAUSES - SELECT_BEFORE_COMPACT)
    fail("active count changed during compaction");

  for (i = SELECT_BEFORE_COMPACT; i < CLAUSES; i++) {
    char *type = NULL;
    Topform c = get_given_clause2(sos, i, NULL, &type);
    if (c == NULL || c->id != (unsigned long long) i + 1 ||
        strcmp(type, "A") != 0)
      fail("age order changed after compaction");
  }
  if (givens_available() || dense_passive_size() != 0)
    fail("dense selector was not empty after selection");

  clist_free(sos);
  zap_given_selectors();
  configure_dense_passive(FALSE, NULL, NULL);
  for (i = 0; i < CLAUSES; i++)
    zap_topform(Stored[i]);
  puts("dense_selector_compaction_test: PASS");
  return 0;
}
