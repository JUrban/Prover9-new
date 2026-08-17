/* Phase 3 regression and scale test for compact retained-clause metadata. */

#include "../ladr/ladr.h"
#include <errno.h>
#include <limits.h>

static int Failures;

#define CHECK(test, message) do {                                         \
  if (!(test)) {                                                          \
    fprintf(stderr, "FAIL: %s (line %d)\n", (message), __LINE__);        \
    Failures++;                                                           \
  }                                                                       \
} while (0)

static size_t record_count(int argc, char **argv)
{
  unsigned long long n = 200000;
  char *end = NULL;
  if (argc > 1) {
    errno = 0;
    n = strtoull(argv[1], &end, 10);
    if (errno != 0 || end == argv[1] || *end != '\0' || n < 100 ||
        n > (unsigned long long) INT_MAX) {
      fprintf(stderr, "usage: %s [record_count: 100..%d]\n", argv[0],
              INT_MAX);
      exit(2);
    }
  }
  return (size_t) n;
}

static void ordering_test(void)
{
  Clause_store store = clause_store_init("ordering-test");
  Topform a = get_topform();
  Topform b = get_topform();
  Topform c = get_topform();

  a->id = 90;
  b->id = 4;
  c->id = 12;
  clause_store_append(store, a);
  clause_store_append(store, b);
  clause_store_append(store, c);
  CHECK(clause_store_member(store, a) && clause_store_member(store, b) &&
        clause_store_member(store, c), "disabled membership is O(1)");
  clause_store_sort_by_id(store);
  CHECK(clause_store_get(store, 0) == b &&
        clause_store_get(store, 1) == c &&
        clause_store_get(store, 2) == a,
        "disabled store sorts by stable clause ID");
  clause_store_free(store);
  CHECK(!a->disabled && !b->disabled && !c->disabled,
        "freeing a store clears membership flags");
  zap_topform(a);
  zap_topform(b);
  zap_topform(c);
}

int main(int argc, char **argv)
{
  size_t n = record_count(argc, argv);
  Clause_store store;
  struct clause_id_table_stats dense_stats, sparse_stats, final_stats;
  unsigned long long current_bytes, legacy_bytes;
  unsigned long long sparse_id = (1ULL << 50) + 7;
  Topform sparse, fresh;
  Plist formulas, p;
  size_t i;
  int formula_count = 0;
  unsigned long long previous_formula_id = 0;

  init_standard_ladr();
  clear_clause_id_tab();
  set_clause_id_count(0);
  ordering_test();

  store = clause_store_init("disabled");
  for (i = 0; i < n; i++) {
    Topform c = get_topform();
    if (i == 0 || i == n / 2 || i == n - 1) {
      Term t = parse_term_from_string("proof_formula(a).");
      CHECK(t != NULL, "proof-formula fixture parses");
      c->is_formula = 1;
      c->formula = term_to_formula(t);
      zap_term(t);
    }
    assign_clause_id(c);
    clause_store_append(store, c);
  }

  CHECK(clause_store_length(store) == n, "every retained clause is stored");
  for (i = 0; i < n; i += n / 997 + 1) {
    Topform c = clause_store_get(store, i);
    CHECK(c->id == i + 1, "append order preserves stable IDs");
    CHECK(find_clause_by_id(i + 1) == c,
          "paged ID lookup returns the exact clause pointer");
    CHECK(clause_store_member(store, c), "retained clause membership holds");
  }

  dense_stats = clause_id_table_get_stats();
  CHECK(dense_stats.entries == n, "ID table live-entry count is exact");
  CHECK(dense_stats.pages == n / 64 + 1,
        "dense IDs occupy the expected number of 64-ID pages");

  formulas = collect_formulas_from_id_tab();
  for (p = formulas; p != NULL; p = p->next) {
    Topform c = p->v;
    CHECK(c->id > previous_formula_id,
          "formula collection is deterministic and ID ordered");
    previous_formula_id = c->id;
    formula_count++;
  }
  CHECK(formula_count == 3, "proof-formula entries remain discoverable");
  for (p = formulas; p != NULL; p = p->next) {
    Topform c = p->v;
    BOOL known = FALSE;
    CHECK(!negative_clause_possibly_compressed(c),
          "formula Topforms are never classified as negative clauses");
    CHECK(!clause_negative_by_id(c->id, &known) && known,
          "resident formula IDs have a known non-clause sign");
  }
  zap_plist(formulas);

  sparse = get_topform();
  sparse->id = sparse_id;
  register_clause_with_id(sparse);
  sparse_stats = clause_id_table_get_stats();
  CHECK(find_clause_by_id(sparse_id) == sparse,
        "sparse high ID is found without a max-ID-sized array");
  CHECK(sparse_stats.pages == dense_stats.pages + 1,
        "one sparse ID allocates one additional page");
  delete_clause(sparse);
  CHECK(find_clause_by_id(sparse_id) == NULL,
        "unassigned sparse ID leaves no stale lookup");

  fresh = get_topform();
  assign_clause_id(fresh);
  CHECK(fresh->id == n + 1 && find_clause_by_id(fresh->id) == fresh,
        "registering a sparse saved ID does not disturb the next stable ID");
  delete_clause(fresh);

  current_bytes = clause_store_allocated_bytes(store) +
                  dense_stats.allocated_bytes;
  legacy_bytes = clause_store_legacy_clist_bytes(store) +
                 dense_stats.legacy_bytes;
  CHECK(current_bytes * 2 < legacy_bytes,
        "compact metadata uses less than half the legacy bookkeeping bytes");

  printf("bookkeeping_lifecycle_test: records=%llu topform_bytes=%llu "
         "store_bytes=%llu id_bytes=%llu combined_bytes=%llu "
         "legacy_bytes=%llu bytes_per_record=%.3f legacy_per_record=%.3f\n",
         (unsigned long long) n,
         (unsigned long long) sizeof(struct topform),
         clause_store_allocated_bytes(store), dense_stats.allocated_bytes,
         current_bytes, legacy_bytes,
         (double) current_bytes / n, (double) legacy_bytes / n);

  clause_store_delete_clauses(store);
  final_stats = clause_id_table_get_stats();
  CHECK(final_stats.entries == 0 && final_stats.pages == 0 &&
        final_stats.allocated_bytes == 0,
        "full teardown releases every ID page and table slot");
  set_clause_id_count(0);

  if (Failures != 0) {
    fprintf(stderr, "bookkeeping_lifecycle_test: %d failure(s)\n", Failures);
    return 1;
  }
  printf("bookkeeping_lifecycle_test: PASS\n");
  return 0;
}
