#include "../provers.src/compact_term_pool.h"

#include <stdio.h>

static int Failures;

#define CHECK(test, message) do {                                    \
  if (!(test)) {                                                     \
    fprintf(stderr, "FAIL: %s (line %d)\n", (message), __LINE__);  \
    Failures++;                                                      \
  }                                                                 \
} while (0)

int main(void)
{
  Compact_term_pool pool;
  struct compact_term_pool_stats stats;
  Topform clause, changed;
  uint32_t atom_offset, left_offset, right_offset, length;
  const int32_t *tokens;

  init_standard_ladr();
  pool = compact_term_pool_init();
  compact_term_pool_enable_sharing_profile(pool);
  clause = parse_clause_from_string("f(a) = g(a).");
  clause->id = 101;
  atom_offset = compact_term_pool_intern(
    pool, clause->id, clause->literals, clause->literals->atom, &length);
  CHECK(atom_offset == 0 && length == 5,
        "first request serializes the complete equality atom");
  left_offset = compact_term_pool_intern(
    pool, clause->id, clause->literals, ARG(clause->literals->atom, 0),
    &length);
  CHECK(left_offset == 1 && length == 2,
        "left side reuses a clause subterm slice");
  right_offset = compact_term_pool_intern(
    pool, clause->id, clause->literals, ARG(clause->literals->atom, 1),
    &length);
  CHECK(right_offset == 3 && length == 2,
        "right side reuses a clause subterm slice");
  tokens = compact_term_pool_tokens(pool);
  CHECK(tokens[left_offset + 1] == tokens[right_offset + 1],
        "shared slices retain identical constant codes");

  changed = parse_clause_from_string("h(b) = b.");
  changed->id = clause->id;
  left_offset = compact_term_pool_intern(
    pool, changed->id, changed->literals, ARG(changed->literals->atom, 0),
    &length);
  CHECK(left_offset == 6 && length == 2,
        "changed content under one proof ID starts a new immutable version");

  compact_term_pool_get_stats(pool, &stats);
  CHECK(stats.clause_entries == 1 && stats.serializations == 2,
        "directory keeps the latest version without reclaiming old tokens");
  CHECK(stats.lookups == 4 && stats.hits == 2 && stats.reused_tokens == 4,
        "subterm reuse accounting is exact");
  CHECK(stats.logical_tokens == 9 && stats.total_bytes > 0 &&
        stats.peak_bytes >= stats.total_bytes,
        "pool byte accounting includes tokens and proof directory");
  CHECK(stats.sharing_profile_enabled &&
        stats.profile_term_occurrences == 9 &&
        stats.profile_unique_terms == 7 &&
        stats.profile_child_references == 7 &&
        stats.profile_atom_roots == 2 &&
        stats.profile_dag_payload_bytes == 64 &&
        stats.profile_table_bytes > 0,
        "sharing profile exactly counts canonical subterms and DAG payload");

  compact_term_pool_free(pool);
  delete_clause(clause);
  delete_clause(changed);

  if (Failures != 0) {
    fprintf(stderr, "compact_term_pool_test: %d failure(s)\n", Failures);
    return 1;
  }
  printf("compact_term_pool_test: PASS\n");
  return 0;
}
