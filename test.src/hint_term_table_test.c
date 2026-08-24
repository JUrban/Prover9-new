#include "../ladr/ladr.h"

static int Failures;

#define CHECK(test, message) do {                                    \
  if (!(test)) {                                                     \
    fprintf(stderr, "FAIL: %s (line %d)\n", (message), __LINE__); \
    Failures++;                                                      \
  }                                                                 \
} while (0)

static Topform normalized_clause(const char *text)
{
  Topform c = parse_clause_from_string((char *) text);
  renumber_variables(c, MAX_VARS);
  return c;
}

int main(void)
{
  Hint_term_table table;
  Topform first, renamed, shared, replacement;
  uint32_t first_root, renamed_root, shared_root, replacement_root;
  struct hint_term_node_view root_view, f_view;
  struct hint_term_table_stats before, frozen, after;

  init_standard_ladr();
  table = hint_term_table_init();
  first = normalized_clause("p(f(x,x)).");
  renamed = normalized_clause("p(f(y,y)).");
  shared = normalized_clause("p(f(g(a),g(a))).");

  CHECK(hint_term_table_add(table, 1, TRUE, first->literals->atom),
        "add first normalized unit atom");
  CHECK(hint_term_table_add(table, 7, FALSE, renamed->literals->atom),
        "add variable-renamed equivalent atom");
  CHECK(!hint_term_table_add(table, 7, TRUE, renamed->literals->atom),
        "reject duplicate active stable ID");
  first_root = hint_term_table_root(table, 1);
  renamed_root = hint_term_table_root(table, 7);
  CHECK(first_root != 0 && first_root == renamed_root,
        "normalized equivalent terms share their root handle");
  CHECK(hint_term_table_positive(table, 1) &&
        !hint_term_table_positive(table, 7),
        "sign is stored separately from the canonical root");

  CHECK(hint_term_table_add(table, 12, TRUE, shared->literals->atom),
        "add atom containing a repeated ground subterm");
  shared_root = hint_term_table_root(table, 12);
  CHECK(hint_term_table_node(table, shared_root, &root_view) &&
        !root_view.variable && root_view.arity == 1,
        "read immutable root node");
  CHECK(hint_term_table_node(table, root_view.children[0], &f_view) &&
        f_view.arity == 2 && f_view.children[0] == f_view.children[1],
        "repeated ground subterm has one canonical handle");
  hint_term_table_get_stats(table, &before);
  CHECK(before.base_intern_hits != 0 &&
        before.base_occurrences > before.base_nodes,
        "construction reports cross-hint and within-term sharing");
  CHECK(before.hash_bytes != 0 && before.scratch_bytes != 0,
        "construction workspace is visible before finalization");

  hint_term_table_finalize(table);
  hint_term_table_get_stats(table, &frozen);
  CHECK(frozen.finalized && frozen.hash_bytes == 0 &&
        frozen.scratch_bytes == 0,
        "finalization releases immutable construction workspace");
  CHECK(frozen.active_records == 3 && frozen.total_bytes != 0,
        "finalized table retains compact roots and nodes");

  CHECK(hint_term_table_remove(table, 1),
        "remove stable ID after finalization");
  CHECK(hint_term_table_root(table, 1) == 0,
        "removed ID has no active root");
  replacement = normalized_clause("p(f(g(a),g(a))).");
  CHECK(hint_term_table_add(table, 1, TRUE, replacement->literals->atom),
        "reinsert stable ID into mutable delta");
  replacement_root = hint_term_table_root(table, 1);
  CHECK((replacement_root & UINT32_C(0x80000000)) != 0,
        "post-finalization root is tagged as delta storage");
  CHECK(hint_term_table_node(table, replacement_root, &root_view) &&
        (root_view.children[0] & UINT32_C(0x80000000)) != 0,
        "delta child handles retain their arena tag");
  hint_term_table_get_stats(table, &after);
  CHECK(after.removals == 1 && after.reinsertions == 1 &&
        after.delta_nodes != 0,
        "lifecycle and delta growth are reported");

  hint_term_table_destroy(table);
  delete_clause(first);
  delete_clause(renamed);
  delete_clause(shared);
  delete_clause(replacement);

  if (Failures != 0) {
    fprintf(stderr, "hint_term_table_test: %d failure(s)\n", Failures);
    return 1;
  }
  printf("hint_term_table_test: PASS\n");
  return 0;
}
