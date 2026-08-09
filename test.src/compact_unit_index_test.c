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

  CHECK(compact_unit_index_contains(index, exact->id), "contains live ID");
  CHECK(compact_unit_index_remove(index, exact->id), "remove live ID");
  CHECK(!compact_unit_index_contains(index, exact->id), "removed ID absent");
  CHECK(!compact_unit_index_remove(index, exact->id), "reject double remove");

  compact_unit_index_get_stats(index, &stats);
  CHECK(stats.active == 3 && stats.retired == 1 && stats.physical == 4,
        "lifecycle counters are exact");
  CHECK(stats.total_bytes > 0 && stats.peak_bytes >= stats.total_bytes,
        "resident byte accounting is present");

  compact_unit_index_free(index);
  delete_clause(general);
  delete_clause(exact);
  delete_clause(negative);
  delete_clause(repeated);

  if (Failures != 0) {
    fprintf(stderr, "compact_unit_index_test: %d failure(s)\n", Failures);
    return 1;
  }
  printf("compact_unit_index_test: PASS\n");
  return 0;
}
