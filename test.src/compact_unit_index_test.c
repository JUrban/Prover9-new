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
  CHECK(stats.total_bytes > 0 && stats.peak_bytes >= stats.total_bytes,
        "resident byte accounting is present");
  CHECK(stats.generalization_profile.queries ==
          stats.generalization_queries &&
        stats.instance_profile.queries == stats.instance_queries &&
        stats.unifier_profile.queries == stats.unifier_queries,
        "operation profiles account for every unit query");
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
    Compact_unit_index root_index, position_index;
    struct compact_unit_index_stats root_stats, position_stats;
    Topform *family = safe_malloc(FAMILY * sizeof(*family));
    Topform broad;
    Topform query;
    unsigned long long *root_ids, *position_ids;
    size_t root_count, position_count;
    char text[128];
    int i;

    compact_unit_index_set_strategy(COMPACT_UNIT_ROOT_SCAN);
    root_index = compact_unit_index_init();
    compact_unit_index_set_strategy(COMPACT_UNIT_POSITION);
    position_index = compact_unit_index_init();
    for (i = 0; i < FAMILY; i++) {
      (void) snprintf(text, sizeof(text),
                      "u(f(c%d,g(h(c%d)))).", i, i);
      family[i] = indexed_unit(text);
      CHECK(compact_unit_index_add(root_index, family[i]),
            "add same-root family to root scan");
      CHECK(compact_unit_index_add(position_index, family[i]),
            "add same-root family to position index");
    }
    broad = indexed_unit("u(x).");
    CHECK(compact_unit_index_add(root_index, broad),
          "add variable-cover unit to root scan");
    CHECK(compact_unit_index_add(position_index, broad),
          "add variable-cover unit to position index");
    query = parse_clause_from_string("u(f(c137,g(h(c137)))).");
    root_ids = compact_unit_unifier_ids(
      root_index, query->literals->atom, TRUE, 0, &root_count);
    position_ids = compact_unit_unifier_ids(
      position_index, query->literals->atom, TRUE, 0, &position_count);
    CHECK(root_count == 2 && position_count == root_count,
          "position retrieval retains exact and variable-cover answers");
    CHECK(root_ids != NULL && position_ids != NULL &&
          memcmp(root_ids, position_ids,
                 root_count * sizeof(*root_ids)) == 0,
          "position retrieval preserves canonical answer order");
    compact_unit_index_get_stats(root_index, &root_stats);
    compact_unit_index_get_stats(position_index, &position_stats);
    CHECK(root_stats.unifier_exact_tests == FAMILY + 1 &&
          position_stats.unifier_exact_tests <= 2,
          "deep position posting avoids the same-root exact-test scan");
    CHECK(position_stats.feature_items > 0 &&
          position_stats.feature_posting_items > FAMILY &&
          position_stats.position_fallback_queries == 0,
          "position index records unbounded-depth rigid and variable features");
    CHECK(compact_unit_index_remove(position_index, family[137]->id),
          "remove a position-index answer");
    compact_unit_index_compact_all_stale(position_index);
    safe_free(position_ids);
    position_ids = compact_unit_unifier_ids(
      position_index, query->literals->atom, TRUE, 0, &position_count);
    CHECK(position_count == 1 && position_ids[0] == broad->id,
          "position features survive deletion and forced rebuilding");

    safe_free(root_ids);
    safe_free(position_ids);
    delete_clause(query);
    compact_unit_index_free(root_index);
    compact_unit_index_free(position_index);
    for (i = 0; i < FAMILY; i++)
      delete_clause(family[i]);
    safe_free(family);
    delete_clause(broad);
    compact_unit_index_set_strategy(COMPACT_UNIT_ROOT_SCAN);
  }

  if (Failures != 0) {
    fprintf(stderr, "compact_unit_index_test: %d failure(s)\n", Failures);
    return 1;
  }
  printf("compact_unit_index_test: PASS\n");
  return 0;
}
