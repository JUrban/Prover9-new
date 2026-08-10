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
  int a[] = {1, 2}, b[] = {1, 2}, c[] = {0, 3}, d[] = {2, 2};
  int forward[] = {1, 3}, back[] = {1, 2};
  unsigned long long *ids;
  size_t count;

  init_standard_ladr();
  index = compact_feature_index_init(2);
  CHECK(compact_feature_index_add(index, 10, a), "add first vector");
  CHECK(compact_feature_index_add(index, 20, b), "add duplicate vector");
  CHECK(compact_feature_index_add(index, 30, c), "add lower vector");
  CHECK(compact_feature_index_add(index, 40, d), "add higher vector");
  CHECK(!compact_feature_index_add(index, 20, b), "reject duplicate ID");

  ids = compact_feature_forward_candidates(index, forward, &count);
  CHECK(count == 3, "forward componentwise filter returns three vectors");
  CHECK(ids != NULL && ids[0] == 30 && ids[1] == 20 && ids[2] == 10,
        "forward order is ascending trie traversal then newest leaf first");
  safe_free(ids);

  ids = compact_feature_back_candidates(index, back, &count);
  CHECK(count == 3, "back componentwise filter returns three vectors");
  CHECK(ids != NULL && ids[0] == 20 && ids[1] == 10 && ids[2] == 40,
        "back traversal order matches the legacy trie before prepending");
  safe_free(ids);

  CHECK(compact_feature_index_remove(index, 20), "remove live ID");
  CHECK(!compact_feature_index_remove(index, 20), "reject double remove");
  ids = compact_feature_forward_candidates(index, forward, &count);
  CHECK(count == 2 && ids[0] == 30 && ids[1] == 10,
        "retired leaf postings are ignored");
  safe_free(ids);

  compact_feature_index_get_stats(index, &stats);
  CHECK(stats.active == 3 && stats.retired == 1 && stats.physical == 4,
        "lifecycle counters are exact");
  CHECK(stats.forward_queries == 2 && stats.back_queries == 1,
        "query counters are exact");
  CHECK(stats.total_bytes > 0 && stats.peak_bytes >= stats.total_bytes,
        "resident byte accounting is present");
  compact_feature_index_free(index);

  if (Failures != 0) {
    fprintf(stderr, "compact_feature_index_test: %d failure(s)\n", Failures);
    return 1;
  }
  printf("compact_feature_index_test: PASS\n");
  return 0;
}
