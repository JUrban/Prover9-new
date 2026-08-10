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
  }

  compact_unit_index_free(index);
  delete_clause(general);
  delete_clause(exact);
  delete_clause(negative);
  delete_clause(repeated);
  delete_clause(unifier);
  delete_clause(occurs);

  if (Failures != 0) {
    fprintf(stderr, "compact_unit_index_test: %d failure(s)\n", Failures);
    return 1;
  }
  printf("compact_unit_index_test: PASS\n");
  return 0;
}
