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
  Hint_generalization_hash complete, partial, virtual;
  Topform h1, h2, h3, h4, h5, from, into, result;
  struct hint_generalization_hash_stats stats;
  Context cf, ci;
  Trail trail = NULL;
  Ilist position = NULL;
  unsigned normal_id, flipped_id, term_nodes;
  BOOL reflexive;
  unsigned long long probes;

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

  h5 = clause("h(g(a)) = k(y).");
  from = clause("f(x) = g(x).");
  into = clause("h(f(a)) = k(y).");
  virtual = hint_generalization_hash_init(2, 0, 100000, 1);
  CHECK(hint_generalization_hash_add_exact(virtual, 1, h5),
        "add virtual-paramodulant exact hint");
  hint_generalization_hash_finalize(virtual);
  cf = get_context();
  ci = get_context();
  position = ilist_append(position, 1);  /* literal */
  position = ilist_append(position, 1);  /* equality left side */
  position = ilist_append(position, 1);  /* h argument */
  CHECK(unify(ARG(from->literals->atom, 0), cf,
              ARG(ARG(into->literals->atom, 0), 0), ci, &trail),
        "prepare virtual paramodulation substitution");
  CHECK(hint_generalization_hash_lookup_unit_paramod(
          virtual, from->literals, 0, cf, into->literals, position, ci,
          &normal_id, &flipped_id, &term_nodes, &reflexive, &probes),
        "virtual unit-paramodulation shape is supported");
  CHECK(normal_id == 1 && flipped_id == 0,
        "virtual lookup finds normal equality orientation only");
  CHECK(probes >= 2, "virtual lookup reports both table probes");
  result = paramodulate(from->literals, 0, cf, into, position, ci);
  renumber_variables(result, MAX_VARS);
  CHECK(hint_generalization_hash_lookup(virtual, result) == normal_id,
        "virtual and materialized paramodulants have identical keys");
  CHECK(term_nodes == (unsigned) clause_symbol_count(result->literals),
        "virtual node count equals materialized default weight");
  CHECK(!reflexive, "virtual result is correctly non-reflexive");
  delete_clause(result);
  undo_subst(trail);
  free_context(cf);
  free_context(ci);
  zap_ilist(position);

  hint_generalization_hash_destroy(complete);
  hint_generalization_hash_destroy(partial);
  hint_generalization_hash_destroy(virtual);
  delete_clause(h1);
  delete_clause(h2);
  delete_clause(h3);
  delete_clause(h4);
  delete_clause(h5);
  delete_clause(from);
  delete_clause(into);

  if (Failures != 0) {
    fprintf(stderr, "hint_generalization_hash_test: %d failure(s)\n", Failures);
    return 1;
  }
  printf("hint_generalization_hash_test: PASS\n");
  return 0;
}
