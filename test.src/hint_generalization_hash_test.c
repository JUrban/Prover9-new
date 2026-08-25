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

static void virtual_case(const char *from_text, int from_side,
                         const char *into_text,
                         const int *coordinates, unsigned coordinate_count,
                         const char *expected_text, BOOL expected_reflexive,
                         const char *message)
{
  Hint_generalization_hash table;
  Topform from = clause(from_text);
  Topform into = clause(into_text);
  Topform expected = clause(expected_text);
  Topform result;
  Literals from_lit = from->literals;
  Literals into_lit = into->literals;
  Context cf = get_context();
  Context ci = get_context();
  Trail trail = NULL;
  Ilist position = NULL, p;
  Term target = into_lit->atom;
  unsigned i, normal_id, flipped_id, nodes;
  unsigned long long probes;
  BOOL reflexive;

  for (i = 0; i < coordinate_count; i++)
    position = ilist_append(position, coordinates[i]);
  for (p = position->next; p != NULL; p = p->next)
    target = ARG(target, p->i - 1);
  table = hint_generalization_hash_init(1, 0, 1000, 1);
  CHECK(hint_generalization_hash_add_exact(table, 1, expected), message);
  hint_generalization_hash_finalize(table);
  CHECK(unify(ARG(from_lit->atom, from_side), cf, target, ci, &trail),
        message);
  CHECK(hint_generalization_hash_lookup_unit_paramod(
          table, from_lit, from_side, cf, into_lit, position, ci,
          &normal_id, &flipped_id, &nodes, &reflexive, &probes), message);
  result = paramodulate(from_lit, from_side, cf, into, position, ci);
  renumber_variables(result, MAX_VARS);
  CHECK(normal_id == 1, message);
  CHECK(reflexive == expected_reflexive, message);
  CHECK(nodes == (unsigned) clause_symbol_count(result->literals), message);
  CHECK(hint_generalization_hash_lookup(table, result) == normal_id, message);
  CHECK(clause_ident(result->literals, expected->literals), message);
  delete_clause(result);
  undo_subst(trail);
  free_context(cf);
  free_context(ci);
  zap_ilist(position);
  hint_generalization_hash_destroy(table);
  delete_clause(from);
  delete_clause(into);
  delete_clause(expected);
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

  {
    Hint_generalization_hash targets =
      hint_generalization_hash_init(12, 0, 100000, 1);
    Topform equality = clause("f(a)=g(b).");
    hint_generalization_hash_enable_target_recipes(targets, 1024 * 1024);
    CHECK(hint_generalization_hash_add_exact(targets, 1, equality),
          "target sidecar adds an exact unit equality");
    CHECK(hint_generalization_hash_add_complete(targets, 1, equality),
          "target sidecar captures exhaustive unit generalizations");
    hint_generalization_hash_finalize(targets);
    hint_generalization_hash_get_stats(targets, &stats);
    CHECK(stats.target_exact_recipes == 1 &&
          stats.target_complete_recipes > 0,
          "target sidecar distinguishes exact and exhaustive recipes");
    CHECK(stats.target_recipes == stats.target_exact_recipes +
          stats.target_complete_recipes &&
          stats.target_position_records > stats.target_recipes,
          "target sidecar reports unique recipes and rewrite positions");
    CHECK(stats.target_variable_positions > 0 &&
          stats.target_rigid_positions > 0 &&
          stats.target_recipe_bytes > 0 &&
          stats.target_complete_token_bytes > 0,
          "target census accounts structure and allocated sidecar bytes");
    hint_generalization_hash_destroy(targets);
    delete_clause(equality);
  }

  {
    Hint_generalization_hash targets =
      hint_generalization_hash_init(2, 3, 100000, 1);
    Topform equality = clause("f(g(a))=k(h(b)).");
    hint_generalization_hash_enable_target_recipes(targets, 1024 * 1024);
    CHECK(hint_generalization_hash_add_exact(targets, 1, equality),
          "partial target sidecar adds its exact equation");
    CHECK(hint_generalization_hash_add_partial(targets, 1, equality),
          "partial target sidecar adds bounded one-hole equations");
    hint_generalization_hash_finalize(targets);
    hint_generalization_hash_get_stats(targets, &stats);
    CHECK(stats.target_exact_recipes == 1 &&
          stats.target_partial_recipes == 3,
          "partial target recipes retain the admitted hole ordinals");
    CHECK(stats.target_max_nodes >= 4 && stats.target_max_depth >= 2,
          "partial target census retains size and depth bounds");
    hint_generalization_hash_destroy(targets);
    delete_clause(equality);
  }

  {
    static const int deep_left[] = {1, 1, 1};
    static const int deep_right[] = {1, 2, 1};
    static const int root_left[] = {1, 1};
    virtual_case("f(x)=g(x).", 0, "h(f(a))=k(y).",
                 deep_left, 3, "h(g(a))=k(y).", FALSE,
                 "deep left-side virtual rewrite");
    virtual_case("f(x)=g(x).", 0, "k(y)=h(f(a)).",
                 deep_right, 3, "k(y)=h(g(a)).", FALSE,
                 "deep right-side virtual rewrite");
    virtual_case("f(x)=g(x).", 0, "f(a)=k(y).",
                 root_left, 2, "g(a)=k(y).", FALSE,
                 "root-side virtual rewrite");
    virtual_case("f(x)=x.", 0, "h(f(a),a)=k(a).",
                 deep_left, 3, "h(a,a)=k(a).", FALSE,
                 "variable replacement side");
    virtual_case("f(x)=c.", 0, "h(f(a))=k(a).",
                 deep_left, 3, "h(c)=k(a).", FALSE,
                 "constant replacement side");
    virtual_case("f(x)=g(x).", 0, "h(f(a))=h(g(a)).",
                 deep_left, 3, "h(g(a))=h(g(a)).", TRUE,
                 "reflexive virtual conclusion");
    virtual_case("f(x)=g(x).", 0, "h(f(z),z)=k(z).",
                 deep_left, 3, "h(g(z),z)=k(z).", FALSE,
                 "shared variable across conclusion sides");
    virtual_case("f(x,x)=g(x).", 0, "h(f(a,y),y)=k(y).",
                 deep_left, 3, "h(g(a),a)=k(a).", FALSE,
                 "unification merges variables from both parents");
    virtual_case("f(x)=g(u).", 0, "h(f(a),y)=k(y).",
                 deep_left, 3, "h(g(u),y)=k(y).", FALSE,
                 "independent unbound parent variables");
    virtual_case("g(x)=f(x).", 1, "h(f(a))=k(a).",
                 deep_left, 3, "h(g(a))=k(a).", FALSE,
                 "right source side virtual rewrite");
  }

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
