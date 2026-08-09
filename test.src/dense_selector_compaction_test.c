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

static size_t archive_clause(Topform c, unsigned *body_bytes,
                             unsigned *justification_bytes,
                             unsigned *logical_body_bytes)
{
  size_t position = Stored_count;
  if (position >= CLAUSES)
    fail("archive callback overflow");
  *body_bytes = (unsigned) c->id;
  *justification_bytes = (unsigned) c->id * 2;
  *logical_body_bytes = (unsigned) c->id * 3;
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

static unsigned long long range_sum(unsigned first, unsigned last)
{
  return ((unsigned long long) first + last) * (last - first + 1) / 2;
}

static void check_payload(unsigned first, unsigned last, char *where)
{
  unsigned long long body, justification, logical;
  unsigned long long expected = first > last ? 0 : range_sum(first, last);
  dense_passive_payload_memory(&body, &justification, &logical);
  if (body != expected || justification != expected * 2 ||
      logical != expected * 3)
    fail(where);
}

static void check_payload_value(unsigned long long expected, char *where)
{
  unsigned long long body, justification, logical;
  dense_passive_payload_memory(&body, &justification, &logical);
  if (body != expected || justification != expected * 2 ||
      logical != expected * 3)
    fail(where);
}

int main(void)
{
  Plist rules = NULL;
  Clist sos;
  int i;
  unsigned long long compactions, reclaimed;
  unsigned long long cursor_id;
  size_t restored_cursor;

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
    c->semantics = i % 4;
    insert_into_sos2(c, sos);
  }
  check_payload(1, CLAUSES, "payload totals after insertion");
  {
    struct dense_passive_view view;
    unsigned long long total = range_sum(1, CLAUSES);
    if (!dense_passive_deactivate_id(CLAUSES, &view))
      fail("payload test deactivation failed");
    if (view.semantics != (CLAUSES - 1) % 4)
      fail("packed semantics changed during direct deactivation");
    check_payload_value(total - CLAUSES,
                        "payload totals after direct deactivation");
    if (!dense_passive_reactivate_id(
          CLAUSES, view.simplifier_epoch, view.rewrite_epoch,
          view.delayed_demodulator, view.rewrite_rule_dirty))
      fail("payload test reactivation failed");
    check_payload_value(total, "payload totals after reactivation");
  }
  for (i = 0; i < SELECT_BEFORE_COMPACT; i++) {
    char *type = NULL;
    Topform c = get_given_clause2(sos, i, NULL, &type);
    if (c == NULL || c->id != (unsigned long long) i + 1 ||
        c->semantics != i % 4 || strcmp(type, "A") != 0)
      fail("age order changed before compaction");
  }
  check_payload(SELECT_BEFORE_COMPACT + 1, CLAUSES,
                "payload totals after deactivation");
  if (!dense_passive_compaction_needed())
    fail("expected compaction threshold was not reached");
  cursor_id = dense_passive_cursor_id(700);
  if (cursor_id != 701)
    fail("physical cursor did not resolve to expected active ID");
  dense_passive_compact(retain_position, NULL);
  restored_cursor = dense_passive_cursor_from_id(cursor_id);
  if (restored_cursor != 100 ||
      dense_passive_cursor_id(restored_cursor) != cursor_id)
    fail("stable cursor ID did not survive dense compaction");
  dense_passive_compaction_stats(&compactions, &reclaimed);
  if (compactions != 1 || reclaimed != SELECT_BEFORE_COMPACT)
    fail("incorrect compaction accounting");
  if (dense_passive_size() != CLAUSES - SELECT_BEFORE_COMPACT)
    fail("active count changed during compaction");
  check_payload(SELECT_BEFORE_COMPACT + 1, CLAUSES,
                "payload totals changed during compaction");

  for (i = SELECT_BEFORE_COMPACT; i < CLAUSES; i++) {
    char *type = NULL;
    Topform c = get_given_clause2(sos, i, NULL, &type);
    if (c == NULL || c->id != (unsigned long long) i + 1 ||
        c->semantics != i % 4 || strcmp(type, "A") != 0)
      fail("age order changed after compaction");
  }
  if (givens_available() || dense_passive_size() != 0)
    fail("dense selector was not empty after selection");
  check_payload(1, 0, "payload totals did not reach zero");

  clist_free(sos);
  zap_given_selectors();
  configure_dense_passive(FALSE, NULL, NULL);
  for (i = 0; i < CLAUSES; i++)
    zap_topform(Stored[i]);
  puts("dense_selector_compaction_test: PASS");
  return 0;
}
