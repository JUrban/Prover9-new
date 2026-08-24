#include "../ladr/ladr.h"

static int Failures;

#define CHECK(test, message) do {                                    \
  if (!(test)) {                                                     \
    fprintf(stderr, "FAIL: %s (line %d)\n", (message), __LINE__); \
    Failures++;                                                      \
  }                                                                 \
} while (0)

static Topform clause(const char *text)
{
  Topform c = parse_clause_from_string((char *) text);
  renumber_variables(c, MAX_VARS);
  return c;
}

static unsigned lookup(Hint_generalization_hash table, const char *text)
{
  Topform c = clause(text);
  unsigned id = hint_generalization_hash_lookup(table, c);
  delete_clause(c);
  return id;
}

int main(void)
{
  Hint_generalization_hash complete, partial;
  Topform h1, h2, h3, h4;
  struct hint_generalization_hash_stats stats;

  init_standard_ladr();

  h1 = clause("p(f(a,b)).");
  h2 = clause("p(f(a,a)).");
  h3 = clause("r(a) | s(b).");
  complete = hint_generalization_hash_init(12, 0, 100000, 3);
  CHECK(hint_generalization_hash_add_exact(complete, 1, h1),
        "add first exact hint");
  CHECK(hint_generalization_hash_add_exact(complete, 2, h2),
        "add repeated-target exact hint");
  CHECK(hint_generalization_hash_add_exact(complete, 3, h3),
        "add nonunit exact hint");
  CHECK(hint_generalization_hash_add_complete(complete, 1, h1),
        "fully generalize first unit hint");
  CHECK(hint_generalization_hash_add_complete(complete, 2, h2),
        "fully generalize repeated-target unit hint");
  CHECK(hint_generalization_hash_add_complete(complete, 3, h3),
        "fully generalize short nonunit hint");
  hint_generalization_hash_finalize(complete);

  CHECK(lookup(complete, "p(f(a,b)).") == 1,
        "exact unit key returns its hint");
  CHECK(lookup(complete, "p(x).") == 1,
        "root-argument abstraction is present");
  CHECK(lookup(complete, "p(f(x,y)).") == 1,
        "distinct-variable generalization is present");
  CHECK(lookup(complete, "p(f(x,x)).") == 2,
        "shared-variable generalization requires equal target subterms");
  CHECK(lookup(complete, "q(x).") == 0,
        "unrelated predicate misses without fallback");
  CHECK(lookup(complete, "r(x).") == 3,
        "literal-deletion generalization is present");
  CHECK(lookup(complete, "s(y) | r(x).") == 3,
        "nonunit literal order and variables are canonical");
  hint_generalization_hash_get_stats(complete, &stats);
  CHECK(stats.complete_hints == 3 && stats.queries == 7 && stats.hits == 6,
        "complete-table statistics report construction and lookup");

  h4 = clause("t(f(g(a),h(b))).");
  partial = hint_generalization_hash_init(2, 3, 100000, 1);
  CHECK(hint_generalization_hash_add_exact(partial, 1, h4),
        "add long exact hint");
  CHECK(hint_generalization_hash_add_partial(partial, 1, h4),
        "add bounded one-hole generalizations");
  hint_generalization_hash_finalize(partial);
  CHECK(lookup(partial, "t(f(g(a),h(b))).") == 1,
        "long hint retains exact matching");
  CHECK(lookup(partial, "t(f(x,h(b))).") == 1,
        "long hint includes one shallow abstraction");
  CHECK(lookup(partial, "t(f(g(x),h(y))).") == 0,
        "unrecorded two-hole generalization misses without fallback");

  hint_generalization_hash_destroy(complete);
  hint_generalization_hash_destroy(partial);
  delete_clause(h1);
  delete_clause(h2);
  delete_clause(h3);
  delete_clause(h4);

  if (Failures != 0) {
    fprintf(stderr, "hint_generalization_hash_test: %d failure(s)\n", Failures);
    return 1;
  }
  printf("hint_generalization_hash_test: PASS\n");
  return 0;
}
