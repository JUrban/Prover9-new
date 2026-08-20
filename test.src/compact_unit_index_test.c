#include "../provers.src/compact_unit_index.h"

static int Failures;

#define CHECK(test, message) do {                                    \
  if (!(test)) {                                                     \
    fprintf(stderr, "FAIL: %s (line %d)\n", (message), __LINE__);  \
    Failures++;                                                      \
  }                                                                 \
} while (0)

static Topform indexed_unit(const char *text)
{
  Topform c = parse_clause_from_string((char *) text);
  assign_clause_id(c);
  return c;
}

static void check_high_variable_generalization(void)
{
  Compact_unit_index index = compact_unit_index_init();
  Topform repeated = indexed_unit("high_var(f(x,x)).");
  Topform fallback = indexed_unit("high_var(f(x,b)).");
  Topform target = parse_clause_from_string("high_var(f(a,b)).");
  Term repeated_f = ARG(repeated->literals->atom, 0);
  Term fallback_f = ARG(fallback->literals->atom, 0);

  ARG(repeated_f, 0) = get_variable_term(64);
  ARG(repeated_f, 1) = get_variable_term(64);
  ARG(fallback_f, 0) = get_variable_term(64);
  CHECK(compact_unit_index_add(index, repeated) &&
        compact_unit_index_add(index, fallback),
        "index patterns whose variable uses the upper undo word");
  CHECK(compact_unit_generalization_first(
          index, target->literals->atom, TRUE, 0) == fallback->id,
        "a failed repeated high-variable edge is undone before its sibling");

  compact_unit_index_free(index);
  delete_clause(repeated);
  delete_clause(fallback);
  delete_clause(target);
}

static void check_high_base_strategy(Compact_unit_strategy strategy)
{
  Compact_term_pool pool = compact_term_pool_init();
  Compact_term_rebase_map map = compact_term_rebase_map_init();
  Compact_unit_index index;
  Topform general = indexed_unit("hp(x).");
  Topform exact = indexed_unit("hp(a).");
  Topform unifier = indexed_unit("hr(f(x),x).");
  Topform occurs = indexed_unit("hr(y,f(y)).");
  Topform query;
  unsigned long long *ids;
  size_t count;
  int pass;

  compact_term_pool_set_logical_base(
    pool, (unsigned long long) UINT32_MAX + 417ULL);
  compact_unit_index_set_strategy(strategy);
  index = compact_unit_index_init_with_pool(pool);
  CHECK(compact_unit_index_add(index, general) &&
        compact_unit_index_add(index, exact) &&
        compact_unit_index_add(index, unifier) &&
        compact_unit_index_add(index, occurs),
        "all unit strategies index terms above the 32-bit boundary");

  for (pass = 0; pass < 2; pass++) {
    query = parse_clause_from_string("hp(a).");
    CHECK(compact_unit_generalization_first(
            index, query->literals->atom, TRUE, 0) == general->id,
          "high-base generalization follows discrimination-tree order");
    ids = compact_unit_instance_ids(
      index, query->literals->atom, TRUE, 0, &count);
    CHECK(count == 1 && ids != NULL && ids[0] == exact->id,
          "high-base instance retrieval is exact");
    safe_free(ids);
    delete_clause(query);

    query = parse_clause_from_string("hr(f(a),a).");
    ids = compact_unit_unifier_ids(
      index, query->literals->atom, TRUE, 0, &count);
    CHECK(count == 1 && ids != NULL && ids[0] == unifier->id,
          "high-base unification preserves bindings and the occurs check");
    safe_free(ids);
    delete_clause(query);

    if (pass == 0) {
      compact_unit_index_retain_live_clauses(index, map);
      compact_term_pool_compact_retained(pool, map);
      compact_unit_index_rebase_term_pool(index, pool, map);
    }
  }

  compact_unit_index_free(index);
  compact_term_rebase_map_free(map);
  compact_term_pool_free(pool);
  delete_clause(general);
  delete_clause(exact);
  delete_clause(unifier);
  delete_clause(occurs);
}

int main(void)
{
  Compact_unit_index index;
  struct compact_unit_index_stats stats;
  Topform general = NULL, exact = NULL, negative = NULL, repeated = NULL;
  Topform target = NULL, pattern = NULL;
  Topform unifier = NULL, occurs = NULL;
  unsigned long long *ids;
  size_t count;

  init_standard_ladr();
  check_high_variable_generalization();
  index = compact_unit_index_init();
  general = indexed_unit("p(x).");
  exact = indexed_unit("p(a).");
  negative = indexed_unit("-p(a).");
  repeated = indexed_unit("q(f(x,x)).");
  CHECK(compact_unit_index_add(index, general), "add general unit");
  CHECK(compact_unit_index_add(index, exact), "add exact unit");
  CHECK(compact_unit_index_add(index, negative), "add negative unit");
  CHECK(compact_unit_index_add(index, repeated), "add repeated-variable unit");
  CHECK(!compact_unit_index_add(index, exact), "reject duplicate proof ID");

  target = parse_clause_from_string("p(a).");
  CHECK(compact_unit_generalization_first(index, target->literals->atom,
                                          TRUE, 0) == general->id,
        "variable pattern precedes rigid pattern as in DISCRIM_BIND");
  CHECK(compact_unit_generalization_first(index, target->literals->atom,
                                          TRUE, general->id) == exact->id,
        "generalization exclusion advances within trie order");
  CHECK(compact_unit_generalization_first(index, target->literals->atom,
                                          FALSE, 0) == negative->id,
        "literal signs use separate roots");
  delete_clause(target);

  target = parse_clause_from_string("q(f(a,a)).");
  CHECK(compact_unit_generalization_first(index, target->literals->atom,
                                          TRUE, 0) == repeated->id,
        "repeated pattern variable accepts identical target terms");
  delete_clause(target);
  target = parse_clause_from_string("q(f(a,b)).");
  CHECK(compact_unit_generalization_first(index, target->literals->atom,
                                          TRUE, 0) == 0,
        "repeated pattern variable rejects different target terms");
  delete_clause(target);

  pattern = parse_clause_from_string("p(x).");
  ids = compact_unit_instance_ids(index, pattern->literals->atom, TRUE,
                                  0, &count);
  CHECK(count == 2, "instance query finds both positive p units");
  CHECK(ids != NULL && ids[0] == exact->id && ids[1] == general->id,
        "instance IDs are returned in decreasing proof-ID order");
  safe_free(ids);
  delete_clause(pattern);

  unifier = indexed_unit("r(f(x),x).");
  occurs = indexed_unit("r(y,f(y)).");
  CHECK(compact_unit_index_add(index, unifier), "add unification unit");
  CHECK(compact_unit_index_add(index, occurs), "add occurs-check unit");
  target = parse_clause_from_string("r(f(a),a).");
  ids = compact_unit_unifier_ids(index, target->literals->atom, TRUE,
                                 0, &count);
  CHECK(count == 1 && ids[0] == unifier->id,
        "token unification accepts a consistent binding and rejects occurs cycle");
  safe_free(ids);
  delete_clause(target);
  target = parse_clause_from_string("r(x,x).");
  ids = compact_unit_unifier_ids(index, target->literals->atom, TRUE,
                                 0, &count);
  CHECK(count == 0 && ids == NULL,
        "cross-namespace unification applies the occurs check");
  delete_clause(target);

  CHECK(compact_unit_index_contains(index, exact->id), "contains live ID");
  CHECK(compact_unit_index_remove(index, exact->id), "remove live ID");
  CHECK(!compact_unit_index_contains(index, exact->id), "removed ID absent");
  CHECK(!compact_unit_index_remove(index, exact->id), "reject double remove");

  compact_unit_index_get_stats(index, &stats);
  CHECK(stats.active == 5 && stats.retired == 1 && stats.physical == 6,
        "lifecycle counters are exact");
  CHECK(compact_unit_index_active_records(index) == stats.active &&
        compact_unit_index_physical_records(index) == stats.physical,
        "constant-time unit populations agree with full statistics");
  CHECK(stats.total_bytes > 0 && stats.peak_bytes >= stats.total_bytes,
        "resident byte accounting is present");
  CHECK(stats.generalization_profile.queries ==
          stats.generalization_queries &&
        stats.instance_profile.queries == stats.instance_queries &&
        stats.unifier_profile.queries == stats.unifier_queries,
        "operation profiles account for every unit query");
  CHECK(stats.generalization_timing_eligible ==
          stats.generalization_queries &&
        stats.instance_timing_eligible == stats.instance_queries &&
        stats.unifier_timing_eligible == stats.unifier_queries &&
        stats.timing_sample_rate == COMPACT_TIMING_SAMPLE_RATE,
        "unit timing samples each operation class without losing counts");
  CHECK(stats.instance_profile.exact_tests ==
          stats.instance_exact_tests &&
        stats.unifier_profile.exact_tests == stats.unifier_exact_tests &&
        stats.unifier_profile.candidate_max >= 1,
        "unit profiles retain exact-test totals and query tails");
  {
    unsigned long long bloated_bytes = stats.total_bytes;
    compact_unit_index_compact_all_stale(index);
    compact_unit_index_get_stats(index, &stats);
    CHECK(stats.active == 5 && stats.physical == 5 &&
          stats.compactions == 1 && stats.total_bytes < bloated_bytes,
          "forced compaction reuses only live records");
    CHECK(compact_unit_index_contains(index, general->id) &&
          !compact_unit_index_contains(index, exact->id),
          "forced compaction preserves live ID membership");
    CHECK(stats.generalization_profile.queries == 5 &&
          stats.instance_profile.queries == 1 &&
          stats.unifier_profile.queries == 2,
          "forced compaction preserves unit query distributions");
    CHECK(stats.generalization_timing_eligible == 5 &&
          stats.instance_timing_eligible == 1 &&
          stats.unifier_timing_eligible == 2,
          "forced compaction preserves unit timing populations");
  }

  compact_unit_index_free(index);
  delete_clause(general);
  delete_clause(exact);
  delete_clause(negative);
  delete_clause(repeated);
  delete_clause(unifier);
  delete_clause(occurs);

  {
    enum { FAMILY = 256 };
    Compact_unit_index root_index, position_index, tree_index, adaptive_index;
    struct compact_unit_index_stats root_stats, position_stats, tree_stats;
    struct compact_unit_index_stats tree_variable_stats;
    struct compact_unit_index_stats adaptive_stats;
    Topform *family = safe_malloc(FAMILY * sizeof(*family));
    Topform broad;
    Topform query;
    Topform variable_query;
    unsigned long long *root_ids, *position_ids, *tree_ids;
    unsigned long long *adaptive_ids, *adaptive_repeat_ids;
    unsigned long long *root_instances, *tree_instances;
    unsigned long long *adaptive_instances;
    unsigned long long *tree_variable_ids;
    size_t root_count, position_count, tree_count;
    size_t adaptive_count, adaptive_repeat_count;
    size_t root_instance_count, tree_instance_count;
    size_t adaptive_instance_count;
    size_t tree_variable_count;
    char text[128];
    int i;

    compact_unit_index_set_strategy(COMPACT_UNIT_ROOT_SCAN);
    root_index = compact_unit_index_init();
    compact_unit_index_set_strategy(COMPACT_UNIT_POSITION);
    position_index = compact_unit_index_init();
    compact_unit_index_set_strategy(COMPACT_UNIT_CODE_TREE);
    tree_index = compact_unit_index_init();
    compact_unit_index_set_strategy(COMPACT_UNIT_ADAPTIVE);
    adaptive_index = compact_unit_index_init();
    for (i = 0; i < FAMILY; i++) {
      (void) snprintf(text, sizeof(text),
                      "u(f(c%d,g(h(c%d)))).", i, i);
      family[i] = indexed_unit(text);
      CHECK(compact_unit_index_add(root_index, family[i]),
            "add same-root family to root scan");
      CHECK(compact_unit_index_add(position_index, family[i]),
            "add same-root family to position index");
      CHECK(compact_unit_index_add(tree_index, family[i]),
            "add same-root family to code-tree index");
      CHECK(compact_unit_index_add(adaptive_index, family[i]),
            "add same-root family to adaptive index");
    }
    broad = indexed_unit("u(x).");
    CHECK(compact_unit_index_add(root_index, broad),
          "add variable-cover unit to root scan");
    CHECK(compact_unit_index_add(position_index, broad),
          "add variable-cover unit to position index");
    CHECK(compact_unit_index_add(tree_index, broad),
          "add variable-cover unit to code-tree index");
    CHECK(compact_unit_index_add(adaptive_index, broad),
          "add variable-cover unit to adaptive index");
    query = parse_clause_from_string("u(f(c137,g(h(c137)))).");
    root_ids = compact_unit_unifier_ids(
      root_index, query->literals->atom, TRUE, 0, &root_count);
    position_ids = compact_unit_unifier_ids(
      position_index, query->literals->atom, TRUE, 0, &position_count);
    tree_ids = compact_unit_unifier_ids(
      tree_index, query->literals->atom, TRUE, 0, &tree_count);
    adaptive_ids = compact_unit_unifier_ids(
      adaptive_index, query->literals->atom, TRUE, 0, &adaptive_count);
    adaptive_repeat_ids = compact_unit_unifier_ids(
      adaptive_index, query->literals->atom, TRUE, 0,
      &adaptive_repeat_count);
    CHECK(compact_unit_generalization_first(
            tree_index, query->literals->atom, TRUE, broad->id) ==
          family[137]->id,
          "ordered generalization finds the matching rigid sibling");
    CHECK(root_count == 2 && position_count == root_count &&
          tree_count == root_count,
          "selective retrieval retains exact and variable-cover answers");
    CHECK(root_ids != NULL && position_ids != NULL &&
          memcmp(root_ids, position_ids,
                 root_count * sizeof(*root_ids)) == 0 &&
          tree_ids != NULL &&
          memcmp(root_ids, tree_ids,
                 root_count * sizeof(*root_ids)) == 0 &&
          adaptive_count == root_count &&
          adaptive_repeat_count == root_count &&
          adaptive_ids != NULL && adaptive_repeat_ids != NULL &&
          memcmp(root_ids, adaptive_ids,
                 root_count * sizeof(*root_ids)) == 0 &&
          memcmp(root_ids, adaptive_repeat_ids,
                 root_count * sizeof(*root_ids)) == 0,
          "selective retrieval preserves canonical answer order");
    root_instances = compact_unit_instance_ids(
      root_index, query->literals->atom, TRUE, 0, &root_instance_count);
    tree_instances = compact_unit_instance_ids(
      tree_index, query->literals->atom, TRUE, 0, &tree_instance_count);
    adaptive_instances = compact_unit_instance_ids(
      adaptive_index, query->literals->atom, TRUE, 0,
      &adaptive_instance_count);
    CHECK(root_instance_count == 1 &&
          tree_instance_count == root_instance_count &&
          memcmp(root_instances, tree_instances,
                 root_instance_count * sizeof(*root_instances)) == 0 &&
          adaptive_instance_count == root_instance_count &&
          memcmp(root_instances, adaptive_instances,
                 root_instance_count * sizeof(*root_instances)) == 0,
          "tree-backed strategies preserve the exact rigid instance answer");
    compact_unit_index_get_stats(root_index, &root_stats);
    compact_unit_index_get_stats(position_index, &position_stats);
    compact_unit_index_get_stats(tree_index, &tree_stats);
    compact_unit_index_get_stats(adaptive_index, &adaptive_stats);
    CHECK(root_stats.unifier_exact_tests == FAMILY + 1 &&
          position_stats.unifier_exact_tests <= 2 &&
          tree_stats.unifier_exact_tests <= 2,
          "deep selective retrieval avoids the same-root exact-test scan");
    CHECK(position_stats.feature_items > 0 &&
          position_stats.feature_posting_items > FAMILY &&
          position_stats.position_fallback_queries == 0,
          "position index records unbounded-depth rigid and variable features");
    CHECK(tree_stats.code_tree_queries == 1 &&
          tree_stats.code_tree_nodes_examined < FAMILY / 4 &&
          tree_stats.code_tree_postings_examined <= 2 &&
          tree_stats.feature_bytes == 0,
          "code-tree retrieval prunes incompatible siblings without features");
    CHECK(tree_stats.generalization_profile.queries == 1 &&
          tree_stats.generalization_profile.successes == 1 &&
          tree_stats.generalization_profile.work < FAMILY / 4,
          "generalization prunes ordered incompatible rigid siblings");
    CHECK(tree_stats.instance_tree_queries == 1 &&
          tree_stats.instance_exact_tests == 1 &&
          tree_stats.instance_tree_postings_examined == 1,
          "instance-tree retrieval reaches only the compatible posting");
    CHECK(adaptive_stats.instance_tree_queries == 1 &&
          adaptive_stats.instance_exact_tests == 1 &&
          adaptive_stats.instance_tree_postings_examined == 1,
          "adaptive instance retrieval uses its code tree");
    variable_query = parse_clause_from_string("u(x).");
    tree_variable_ids = compact_unit_unifier_ids(
      tree_index, variable_query->literals->atom, TRUE, 0,
      &tree_variable_count);
    compact_unit_index_get_stats(tree_index, &tree_variable_stats);
    CHECK(tree_variable_count == FAMILY + 1 && tree_variable_ids != NULL,
          "code-tree variable expansion preserves every unifier");
    CHECK(tree_variable_stats.code_tree_variable_parents > 0 &&
          tree_variable_stats.code_tree_variable_children > 0 &&
          tree_variable_stats.code_tree_rigid_parents > 0 &&
          tree_variable_stats.code_tree_rigid_sibling_checks >=
            tree_variable_stats.code_tree_rigid_children,
          "code-tree fanout counters distinguish variable and rigid work");
    CHECK(adaptive_stats.adaptive_queries == 2 &&
          adaptive_stats.adaptive_tree_choices >= 1 &&
          adaptive_stats.adaptive_route_misses == 1 &&
          adaptive_stats.adaptive_route_hits == 1 &&
          adaptive_stats.adaptive_route_bytes > 0,
          "adaptive unit routing learns without changing repeated answers");
    CHECK(compact_unit_index_remove(position_index, family[137]->id),
          "remove a position-index answer");
    compact_unit_index_compact_all_stale(position_index);
    safe_free(position_ids);
    position_ids = compact_unit_unifier_ids(
      position_index, query->literals->atom, TRUE, 0, &position_count);
    CHECK(position_count == 1 && position_ids[0] == broad->id,
          "position features survive deletion and forced rebuilding");
    CHECK(compact_unit_index_remove(tree_index, family[137]->id),
          "remove a code-tree answer");
    compact_unit_index_compact_all_stale(tree_index);
    safe_free(tree_ids);
    safe_free(adaptive_ids);
    safe_free(adaptive_repeat_ids);
    tree_ids = compact_unit_unifier_ids(
      tree_index, query->literals->atom, TRUE, 0, &tree_count);
    CHECK(tree_count == 1 && tree_ids[0] == broad->id,
          "code-tree retrieval survives deletion and forced rebuilding");

    safe_free(root_ids);
    safe_free(position_ids);
    safe_free(tree_ids);
    safe_free(root_instances);
    safe_free(tree_instances);
    safe_free(adaptive_instances);
    safe_free(tree_variable_ids);
    delete_clause(query);
    delete_clause(variable_query);
    compact_unit_index_free(root_index);
    compact_unit_index_free(position_index);
    compact_unit_index_free(tree_index);
    compact_unit_index_free(adaptive_index);
    for (i = 0; i < FAMILY; i++)
      delete_clause(family[i]);
    safe_free(family);
    delete_clause(broad);
    compact_unit_index_set_strategy(COMPACT_UNIT_ROOT_SCAN);
  }

  {
    Compact_unit_index position_index;
    struct compact_unit_index_stats stats;
    Topform exact, near_left, near_right, query;
    unsigned long long *ids;
    size_t count;

    compact_unit_index_set_strategy(COMPACT_UNIT_POSITION);
    position_index = compact_unit_index_init();
    exact = indexed_unit("u(f(f(c137,a),a)).");
    near_left = indexed_unit("u(f(f(c138,a),b)).");
    near_right = indexed_unit("u(f(f(c139,b),a)).");
    CHECK(compact_unit_index_add(position_index, exact) &&
          compact_unit_index_add(position_index, near_left) &&
          compact_unit_index_add(position_index, near_right),
          "add two-position refinement family");
    query = parse_clause_from_string("u(f(f(x,a),a)).");
    ids = compact_unit_unifier_ids(
      position_index, query->literals->atom, TRUE, 0, &count);
    compact_unit_index_get_stats(position_index, &stats);
    CHECK(count == 1 && ids != NULL && ids[0] == exact->id,
          "direct two-position refinement preserves the exact answer");
    CHECK(stats.position_refinement_queries == 1 &&
          stats.position_refinement_checks == 3 &&
          stats.position_refinement_rejects == 1 &&
          stats.position_tertiary_queries == 1 &&
          stats.position_tertiary_checks == 1 &&
          stats.position_tertiary_rejects == 0,
          "second rigid feature rejects one first-position near miss");
    safe_free(ids);
    delete_clause(exact);
    delete_clause(near_left);
    delete_clause(near_right);
    delete_clause(query);
    compact_unit_index_free(position_index);
    compact_unit_index_set_strategy(COMPACT_UNIT_ROOT_SCAN);
  }

  {
    Compact_unit_index position_index;
    struct compact_unit_index_stats stats;
    Topform exact, near_third, decoy, near_a1, near_a2, near_b1, near_b2;
    Topform query;
    unsigned long long *ids;
    size_t count;

    compact_unit_index_set_strategy(COMPACT_UNIT_POSITION);
    position_index = compact_unit_index_init();
    exact = indexed_unit("v(g(c137,a,a,a)).");
    near_third = indexed_unit("v(g(c138,a,a,b)).");
    decoy = indexed_unit("v(g(c139,b,b,a)).");
    near_a1 = indexed_unit("v(g(c140,a,b,a)).");
    near_a2 = indexed_unit("v(g(c141,a,b,a)).");
    near_b1 = indexed_unit("v(g(c142,b,a,a)).");
    near_b2 = indexed_unit("v(g(c143,b,a,a)).");
    CHECK(compact_unit_index_add(position_index, exact) &&
          compact_unit_index_add(position_index, near_third) &&
          compact_unit_index_add(position_index, decoy) &&
          compact_unit_index_add(position_index, near_a1) &&
          compact_unit_index_add(position_index, near_a2) &&
          compact_unit_index_add(position_index, near_b1) &&
          compact_unit_index_add(position_index, near_b2),
          "add three-position refinement family");
    query = parse_clause_from_string("v(g(x,a,a,a)).");
    ids = compact_unit_unifier_ids(
      position_index, query->literals->atom, TRUE, 0, &count);
    compact_unit_index_get_stats(position_index, &stats);
    CHECK(count == 1 && ids != NULL && ids[0] == exact->id,
          "direct three-position refinement preserves the exact answer");
    CHECK(stats.position_refinement_queries == 1 &&
          stats.position_refinement_checks == 6 &&
          stats.position_refinement_rejects == 3 &&
          stats.position_tertiary_queries == 1 &&
          stats.position_tertiary_checks == 2 &&
          stats.position_tertiary_rejects == 1,
          "third rigid feature rejects a two-position near miss");
    safe_free(ids);
    delete_clause(exact);
    delete_clause(near_third);
    delete_clause(decoy);
    delete_clause(near_a1);
    delete_clause(near_a2);
    delete_clause(near_b1);
    delete_clause(near_b2);
    delete_clause(query);
    compact_unit_index_free(position_index);
    compact_unit_index_set_strategy(COMPACT_UNIT_ROOT_SCAN);
  }

  {
    Compact_unit_index shallow_index;
    struct compact_unit_index_stats shallow_stats;
    Topform shallow, deep, broad, query;
    unsigned long long *ids;
    size_t count;

    compact_unit_index_set_feature_depth(2);
    compact_unit_index_set_strategy(COMPACT_UNIT_ADAPTIVE);
    shallow_index = compact_unit_index_init();
    shallow = indexed_unit("u(f(a,g(c))).");
    deep = indexed_unit("u(f(a,g(d))).");
    broad = indexed_unit("u(x).");
    CHECK(compact_unit_index_add(shallow_index, shallow) &&
          compact_unit_index_add(shallow_index, deep) &&
          compact_unit_index_add(shallow_index, broad),
          "add shallow-feature differential units");
    query = parse_clause_from_string("u(f(a,g(c))).");
    ids = compact_unit_unifier_ids(
      shallow_index, query->literals->atom, TRUE, 0, &count);
    compact_unit_index_get_stats(shallow_index, &shallow_stats);
    CHECK(count == 2 && ids != NULL && ids[0] == broad->id &&
          ids[1] == shallow->id,
          "depth-limited adaptive features preserve every unifier");
    CHECK(shallow_stats.feature_depth == 2 &&
          shallow_stats.feature_items == 4 &&
          shallow_stats.feature_posting_items == 7,
          "depth-limited adaptive index stores no deeper features");
    safe_free(ids);
    delete_clause(query);
    delete_clause(shallow);
    delete_clause(deep);
    delete_clause(broad);
    compact_unit_index_free(shallow_index);
    compact_unit_index_set_feature_depth(0);
    compact_unit_index_set_strategy(COMPACT_UNIT_ROOT_SCAN);
  }

  {
    enum { FANOUT = 32 };
    Compact_unit_index adaptive_index;
    struct compact_unit_index_stats adaptive_stats;
    Topform units[FANOUT];
    Topform query;
    unsigned long long *ids;
    size_t count;
    char text[96];
    int i;

    compact_unit_index_set_strategy(COMPACT_UNIT_ADAPTIVE);
    adaptive_index = compact_unit_index_init();
    for (i = 0; i < FANOUT; i++) {
      (void) snprintf(text, sizeof(text), "w(f(c%d),a).", i);
      units[i] = indexed_unit(text);
      CHECK(compact_unit_index_add(adaptive_index, units[i]),
            "add empty-route fanout unit");
    }
    query = parse_clause_from_string("w(x,b).");
    ids = compact_unit_unifier_ids(
      adaptive_index, query->literals->atom, TRUE, 0, &count);
    CHECK(count == 0 && ids == NULL,
          "measured empty route preserves its first negative answer");
    ids = compact_unit_unifier_ids(
      adaptive_index, query->literals->atom, TRUE, 0, &count);
    CHECK(count == 0 && ids == NULL,
          "learned empty route preserves its repeated negative answer");
    compact_unit_index_get_stats(adaptive_index, &adaptive_stats);
    CHECK(adaptive_stats.adaptive_tree_choices == 1 &&
          adaptive_stats.adaptive_position_choices == 1 &&
          adaptive_stats.adaptive_position_empty_choices == 1 &&
          adaptive_stats.adaptive_route_misses == 1 &&
          adaptive_stats.adaptive_route_hits == 1,
          "costly empty route measures tree once before using position");
    delete_clause(query);
    compact_unit_index_free(adaptive_index);
    for (i = 0; i < FANOUT; i++)
      delete_clause(units[i]);
    compact_unit_index_set_strategy(COMPACT_UNIT_ROOT_SCAN);
  }

  {
    const char *unit_text[] = {
      "p(x).", "p(a).", "p(f(x)).", "p(f(a)).",
      "p(f(x,x)).", "p(f(a,b)).", "p(g(f(a),y)).",
      "p(g(z,z)).", "p(k(g(a,b),f(c))).", "-p(f(a))."
    };
    const char *query_text[] = {
      "p(x).", "p(a).", "p(b).", "p(f(y)).", "p(f(a)).",
      "p(f(a,a)).", "p(f(a,b)).", "p(g(w,w)).",
      "p(g(f(a),b)).", "p(k(g(a,b),f(c))).", "p(k(q,f(c))).",
      "-p(f(a))."
    };
    enum {
      UNIT_COUNT = sizeof(unit_text) / sizeof(unit_text[0]),
      QUERY_COUNT = sizeof(query_text) / sizeof(query_text[0])
    };
    Compact_unit_index root_index, tree_index;
    Topform units[UNIT_COUNT];
    int i, pass;

    compact_unit_index_set_strategy(COMPACT_UNIT_ROOT_SCAN);
    root_index = compact_unit_index_init();
    compact_unit_index_set_strategy(COMPACT_UNIT_CODE_TREE);
    tree_index = compact_unit_index_init();
    for (i = 0; i < UNIT_COUNT; i++) {
      units[i] = indexed_unit(unit_text[i]);
      CHECK(compact_unit_index_add(root_index, units[i]),
            "add variable-rich differential unit to root scan");
      CHECK(compact_unit_index_add(tree_index, units[i]),
            "add variable-rich differential unit to code tree");
    }
    for (pass = 0; pass < 2; pass++) {
      for (i = 0; i < QUERY_COUNT; i++) {
        Topform query = parse_clause_from_string((char *) query_text[i]);
        BOOL sign = query->literals->sign;
        unsigned long long exclude = (i % 3 == 0) ? units[1]->id : 0;
        unsigned long long *root_ids, *tree_ids;
        unsigned long long *root_instances, *tree_instances;
        size_t root_count, tree_count;
        size_t root_instance_count, tree_instance_count;
        root_ids = compact_unit_unifier_ids(
          root_index, query->literals->atom, sign, exclude, &root_count);
        tree_ids = compact_unit_unifier_ids(
          tree_index, query->literals->atom, sign, exclude, &tree_count);
        CHECK(root_count == tree_count,
              "code-tree variable-rich differential count");
        CHECK(root_count == 0 ||
              (root_ids != NULL && tree_ids != NULL &&
               memcmp(root_ids, tree_ids,
                      root_count * sizeof(*root_ids)) == 0),
              "code-tree variable-rich differential order");
        root_instances = compact_unit_instance_ids(
          root_index, query->literals->atom, sign, exclude,
          &root_instance_count);
        tree_instances = compact_unit_instance_ids(
          tree_index, query->literals->atom, sign, exclude,
          &tree_instance_count);
        CHECK(root_instance_count == tree_instance_count,
              "instance-tree variable-rich differential count");
        CHECK(root_instance_count == 0 ||
              (root_instances != NULL && tree_instances != NULL &&
               memcmp(root_instances, tree_instances,
                      root_instance_count * sizeof(*root_instances)) == 0),
              "instance-tree variable-rich differential order");
        safe_free(root_ids);
        safe_free(tree_ids);
        safe_free(root_instances);
        safe_free(tree_instances);
        delete_clause(query);
      }
      if (pass == 0) {
        CHECK(compact_unit_index_remove(root_index, units[3]->id) &&
              compact_unit_index_remove(tree_index, units[3]->id),
              "remove differential unit from both strategies");
        CHECK(compact_unit_index_remove(root_index, units[7]->id) &&
              compact_unit_index_remove(tree_index, units[7]->id),
              "remove repeated-variable differential unit");
        compact_unit_index_compact_all_stale(root_index);
        compact_unit_index_compact_all_stale(tree_index);
      }
    }
    compact_unit_index_free(root_index);
    compact_unit_index_free(tree_index);
    for (i = 0; i < UNIT_COUNT; i++)
      delete_clause(units[i]);
    compact_unit_index_set_strategy(COMPACT_UNIT_ROOT_SCAN);
  }

  check_high_base_strategy(COMPACT_UNIT_ROOT_SCAN);
  check_high_base_strategy(COMPACT_UNIT_POSITION);
  check_high_base_strategy(COMPACT_UNIT_CODE_TREE);
  compact_unit_index_set_strategy(COMPACT_UNIT_ROOT_SCAN);

  if (Failures != 0) {
    fprintf(stderr, "compact_unit_index_test: %d failure(s)\n", Failures);
    return 1;
  }
  printf("compact_unit_index_test: PASS\n");
  return 0;
}
