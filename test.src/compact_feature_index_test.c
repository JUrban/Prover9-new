#include "../provers.src/compact_feature_index.h"

static int Failures;

#define CHECK(test, message) do {                                    \
  if (!(test)) {                                                     \
    fprintf(stderr, "FAIL: %s (line %d)\n", (message), __LINE__);  \
    Failures++;                                                      \
  }                                                                 \
} while (0)

int main(void)
{
  Compact_feature_index index;
  struct compact_feature_index_stats stats;
  struct compact_feature_structural_summary none = {0, 0, 0};
  int a[] = {1, 2}, b[] = {1, 2}, c[] = {0, 3}, d[] = {2, 2},
      e[] = {1, 1};
  int forward[] = {1, 3}, back[] = {1, 2};
  unsigned long long *ids;
  size_t count;

  init_standard_ladr();
  index = compact_feature_index_init(2, FALSE);
  CHECK(compact_feature_index_add(index, 10, a, none), "add first vector");
  CHECK(compact_feature_index_add(index, 20, b, none), "add duplicate vector");
  CHECK(compact_feature_index_add(index, 30, c, none), "add lower vector");
  CHECK(compact_feature_index_add(index, 40, d, none), "add higher vector");
  CHECK(compact_feature_index_add(index, 50, e, none),
        "split a radix edge at the second feature");
  CHECK(!compact_feature_index_add(index, 20, b, none), "reject duplicate ID");

  ids = compact_feature_forward_candidates(index, forward, none, &count);
  CHECK(count == 4, "forward componentwise filter returns four vectors");
  CHECK(ids != NULL && ids[0] == 30 && ids[1] == 50 &&
        ids[2] == 20 && ids[3] == 10,
        "forward order is ascending trie traversal then newest leaf first");
  compact_feature_note_exact_query(index, TRUE, 3, 1, 2);
  safe_free(ids);

  ids = compact_feature_back_candidates(index, back, none, &count);
  CHECK(count == 3, "back componentwise filter returns three vectors");
  CHECK(ids != NULL && ids[0] == 20 && ids[1] == 10 && ids[2] == 40,
        "back traversal order matches the legacy trie before prepending");
  compact_feature_note_exact_query(index, FALSE, count, 2, 1);
  safe_free(ids);

  CHECK(compact_feature_index_remove(index, 20), "remove live ID");
  CHECK(!compact_feature_index_remove(index, 20), "reject double remove");
  ids = compact_feature_forward_candidates(index, forward, none, &count);
  CHECK(count == 3 && ids[0] == 30 && ids[1] == 50 && ids[2] == 10,
        "retired leaf postings are ignored");
  compact_feature_note_exact_query(index, TRUE, count, 0, 3);
  safe_free(ids);

  compact_feature_index_get_stats(index, &stats);
  CHECK(stats.active == 4 && stats.retired == 1 && stats.physical == 5,
        "lifecycle counters are exact");
  CHECK(stats.forward_queries == 2 && stats.back_queries == 1,
        "query counters are exact");
  CHECK(stats.forward_profile.queries == 2 &&
        stats.forward_profile.exact_tests == 6 &&
        stats.forward_profile.materializations == 5 &&
        stats.back_profile.queries == 1 &&
        stats.back_profile.exact_tests == 3,
        "feature profiles separate lookup, exact, and materialization work");
  CHECK(stats.label_bytes > 0 && stats.total_bytes > 0 &&
        stats.peak_bytes >= stats.total_bytes,
        "resident byte accounting is present");
  compact_feature_index_free(index);

  {
    int same[] = {1, 1};
    const uint64_t bit_a = UINT64_C(1) << 1;
    const uint64_t bit_b = UINT64_C(1) << 7;
    struct compact_feature_structural_summary only_a = {bit_a, 0, 0};
    struct compact_feature_structural_summary only_b = {bit_b, 0, 0};
    struct compact_feature_structural_summary both = {
      bit_a | bit_b, 0, 0
    };
    index = compact_feature_index_init(2, TRUE);
    CHECK(compact_feature_index_add(index, 101, same, only_a),
          "add structurally general posting");
    CHECK(compact_feature_index_add(index, 102, same, both),
          "add structurally specific posting");
    CHECK(compact_feature_index_add(index, 103, same, only_b),
          "add structurally incompatible posting");
    ids = compact_feature_forward_candidates(index, same, only_a, &count);
    CHECK(count == 1 && ids[0] == 101,
          "forward structural subset rejects impossible subsumers");
    safe_free(ids);
    ids = compact_feature_back_candidates(index, same, only_a, &count);
    CHECK(count == 2 && ids[0] == 102 && ids[1] == 101,
          "back structural subset retains possible subsumees in trie order");
    safe_free(ids);
    compact_feature_index_get_stats(index, &stats);
    CHECK(stats.forward_structural_rejects == 2 &&
          stats.back_structural_rejects == 1,
          "structural rejects are attributed by operation");
    CHECK(stats.structural_bytes > 0,
          "enabled structural masks are included in byte accounting");
    compact_feature_index_free(index);
  }

  {
    const char *clauses[] = {
      "p(x) | -q(g(x)).",
      "p(f(a)) | -q(g(a)).",
      "-q(g(a)) | p(f(a)).",
      "p(f(b)) | -q(g(b)).",
      "f(x) = g(x) | r(x).",
      "f(a) = g(a) | r(a).",
      "g(a) = f(a) | r(a).",
      "s(x,x) | t(x).",
      "s(a,a) | t(a).",
      "s(a,b) | t(a)."
    };
    Topform parsed[sizeof(clauses) / sizeof(clauses[0])];
    size_t i, j;
    for (i = 0; i < sizeof(clauses) / sizeof(clauses[0]); i++)
      parsed[i] = parse_clause_from_string((char *) clauses[i]);
    for (i = 0; i < sizeof(clauses) / sizeof(clauses[0]); i++)
      for (j = 0; j < sizeof(clauses) / sizeof(clauses[0]); j++)
        if (feature_subsumes_raw(parsed[i], parsed[j])) {
          struct compact_feature_structural_summary candidate =
            compact_feature_clause_summary(parsed[i]);
          struct compact_feature_structural_summary target =
            compact_feature_clause_summary(parsed[j]);
          CHECK((candidate.rigid & ~target.rigid) == 0 &&
                (candidate.variable_constraints &
                 ~target.equal_positions) == 0,
                "structural summary never rejects exact subsumption");
        }
    for (i = 0; i < sizeof(clauses) / sizeof(clauses[0]); i++)
      delete_clause(parsed[i]);
  }

  {
    Topform repeated = parse_clause_from_string("u(x,x) | v(x).");
    Topform consistent = parse_clause_from_string("u(a,a) | v(a).");
    Topform inconsistent = parse_clause_from_string("u(a,b) | v(a).");
    struct compact_feature_structural_summary required =
      compact_feature_clause_summary(repeated);
    struct compact_feature_structural_summary available =
      compact_feature_clause_summary(consistent);
    struct compact_feature_structural_summary missing =
      compact_feature_clause_summary(inconsistent);
    CHECK(required.variable_constraints != 0,
          "repeated variables produce equality constraints");
    CHECK((required.variable_constraints & ~available.equal_positions) == 0,
          "consistent target satisfies repeated-variable paths");
    CHECK((required.variable_constraints & ~missing.equal_positions) != 0,
          "inconsistent target is rejected before exact matching");
    delete_clause(repeated);
    delete_clause(consistent);
    delete_clause(inconsistent);
  }

  if (Failures != 0) {
    fprintf(stderr, "compact_feature_index_test: %d failure(s)\n", Failures);
    return 1;
  }
  printf("compact_feature_index_test: PASS\n");
  return 0;
}
