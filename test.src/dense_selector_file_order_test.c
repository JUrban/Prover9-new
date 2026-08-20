/* Differential ordering and lifecycle checks for file-backed selectors. */

#include "../ladr/ladr.h"
#include "../provers.src/giv_select.h"

#include <stdint.h>

#define CLAUSES 3000
#define SELECTORS 3

static unsigned long long Expected[CLAUSES];
static Topform Stored[CLAUSES];
static size_t Stored_count;

static void fail(const char *message)
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
    fail("archive overflow");
  *body_bytes = *justification_bytes = *logical_body_bytes = 0;
  Stored[Stored_count++] = c;
  return position;
}

static Topform activate_clause(size_t position, unsigned long long id,
                               unsigned long long hint_id)
{
  (void) hint_id;
  if (position >= Stored_count || Stored[position]->id != id)
    fail("activation mismatch");
  return Stored[position];
}

static void configure_rules(void)
{
  Plist rules = NULL;
  rules = plist_append(
    rules, selector_rule_term("W", "low", "weight", "all", 2));
  rules = plist_append(
    rules, selector_rule_term("A", "low", "age", "all", 1));
  rules = plist_append(
    rules, selector_rule_term("H", "low", "hint_age", "all", 3));
  init_giv_select(rules);
  zap_plist_of_terms(rules);
}

static void populate(Clist sos)
{
  unsigned i;
  for (i = 0; i < CLAUSES; i++) {
    Topform c = get_topform();
    c->id = i + 1;
    c->weight = (double) ((i * 97U) % 101U);
    if (i % 5 == 0) {
      c->matching_hint = get_topform();
      c->matching_hint->id = i == 2500 ? UINT32_MAX :
        10000 + ((i * 37U) % 113U);
    }
    insert_into_sos2(c, sos);
  }
}

static void release_stored(void)
{
  size_t i;
  for (i = 0; i < Stored_count; i++) {
    if (Stored[i]->matching_hint != NULL)
      zap_topform(Stored[i]->matching_hint);
    zap_topform(Stored[i]);
  }
  Stored_count = 0;
}

static size_t retain_position(size_t position, void *context)
{
  (void) context;
  return position;
}

static void run(Dense_passive_selector_mode mode, BOOL record_expected)
{
  Clist sos;
  unsigned i;
  configure_dense_passive_selectors(mode, 64);
  configure_dense_passive_directory(DENSE_DIRECTORY_MEMORY);
  configure_dense_passive(TRUE, archive_clause, activate_clause);
  configure_rules();
  sos = clist_init("file selector order regression");
  populate(sos);

  /* Exercise lazy removal and reinsertion before the selector reaches it. */
  {
    struct dense_passive_view view;
    if (!dense_passive_deactivate_id(2048, &view) ||
        !dense_passive_reactivate_id(
          2048, view.simplifier_epoch, view.rewrite_epoch,
          view.delayed_demodulator, view.rewrite_rule_dirty))
      fail("reactivation cycle failed");
  }

  for (i = 0; i < CLAUSES; i++) {
    char *type = NULL;
    Topform c = get_given_clause2(sos, i, NULL, &type);
    if (c == NULL || type == NULL)
      fail("premature selector exhaustion");
    if (record_expected)
      Expected[i] = c->id;
    else if (Expected[i] != c->id)
      fail("file selector changed the reference given order");
    if (i == 1199) {
      if (!dense_passive_compaction_needed())
        fail("expected differential compaction threshold");
      dense_passive_compact(retain_position, NULL);
    }
  }
  if (givens_available() || dense_passive_size() != 0)
    fail("selector not empty after full drain");
  if (mode == DENSE_SELECTOR_FILE) {
    struct dense_passive_selector_stats stats =
      dense_passive_selector_stats();
    if (stats.file_min_calls !=
        CLAUSES + stats.stale_entries_discarded)
      fail("file selector performed a redundant minimum search");
    if (stats.file_run_checks > stats.file_min_calls * 64ULL)
      fail("file selector run-head checks exceed the fixed level bound");
  }
  clist_free(sos);
  zap_given_selectors();
  configure_dense_passive(FALSE, NULL, NULL);
  release_stored();
}

int main(void)
{
  init_standard_ladr();
  run(DENSE_SELECTOR_HEAP, TRUE);
  run(DENSE_SELECTOR_FILE, FALSE);
  configure_dense_passive_selectors(DENSE_SELECTOR_HEAP, 65536);
  puts("dense_selector_file_order_test: PASS");
  return 0;
}
