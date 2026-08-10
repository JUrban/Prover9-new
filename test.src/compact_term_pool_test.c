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
  CHECK(stats.token_growths == 1 && stats.token_copy_bytes == 0,
        "small pool growth accounting has no copied predecessor capacity");
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

  {
    Compact_term_pool source = compact_term_pool_init();
    Compact_term_pool destination = compact_term_pool_init();
    Compact_term_rebase_map map = compact_term_rebase_map_init();
    struct compact_term_pool_stats compacted;
    Topform first = parse_clause_from_string("p(f(a)).");
    Topform stale = parse_clause_from_string("q(g(b)).");
    Topform last = parse_clause_from_string("r(h(c)).");
    uint32_t first_offset, last_offset, translated, first_length;
    const int32_t *source_tokens, *destination_tokens;
    first->id = 201;
    stale->id = 202;
    last->id = 203;
    first_offset = compact_term_pool_intern(
      source, first->id, first->literals, first->literals->atom,
      &first_length);
    (void) compact_term_pool_intern(
      source, stale->id, stale->literals, stale->literals->atom, &length);
    last_offset = compact_term_pool_intern(
      source, last->id, last->literals, last->literals->atom, &length);
    CHECK(compact_term_pool_copy_clause(destination, source, map, last->id) &&
          compact_term_pool_copy_clause(destination, source, map, first->id) &&
          compact_term_pool_copy_clause(destination, source, map, first->id),
          "rebase copy deduplicates retained clauses in arbitrary order");
    compact_term_rebase_map_finalize(map);
    translated = compact_term_rebase_offset(map, first_offset + 1);
    source_tokens = compact_term_pool_tokens(source);
    destination_tokens = compact_term_pool_tokens(destination);
    CHECK(destination_tokens[translated] == source_tokens[first_offset + 1] &&
          compact_term_rebase_offset(map, last_offset) != translated,
          "sorted rebase map translates root and interior token offsets");
    compact_term_pool_finish_compaction(destination, source);
    compact_term_pool_get_stats(destination, &compacted);
    CHECK(compacted.clause_entries == 2 && compacted.logical_tokens == 6 &&
          compacted.compactions == 1,
          "compacted pool drops an unretained clause and records reclamation");
    compact_term_rebase_map_free(map);
    compact_term_pool_free(destination);
    compact_term_pool_free(source);
    delete_clause(first);
    delete_clause(stale);
    delete_clause(last);
  }

  {
    Compact_term_pool source = compact_term_pool_init();
    Compact_term_rebase_map map = compact_term_rebase_map_init();
    struct compact_term_pool_stats before, compacted;
    Topform first = parse_clause_from_string("p(f(a)).");
    Topform stale = parse_clause_from_string("q(g(b)).");
    Topform last = parse_clause_from_string("r(h(c)).");
    uint32_t first_offset, last_offset, first_length, last_length;
    uint32_t translated_first, translated_last;
    int32_t first_root, last_root;
    int extra;
    first->id = 301;
    stale->id = 302;
    last->id = 303;
    first_offset = compact_term_pool_intern(
      source, first->id, first->literals, first->literals->atom,
      &first_length);
    (void) compact_term_pool_intern(
      source, stale->id, stale->literals, stale->literals->atom, &length);
    for (extra = 0; extra < 128; extra++) {
      stale->id = 400 + extra;
      (void) compact_term_pool_intern(
        source, stale->id, stale->literals, stale->literals->atom, &length);
    }
    last_offset = compact_term_pool_intern(
      source, last->id, last->literals, last->literals->atom, &last_length);
    first_root = compact_term_pool_tokens(source)[first_offset];
    last_root = compact_term_pool_tokens(source)[last_offset];
    compact_term_pool_get_stats(source, &before);
    CHECK(compact_term_rebase_map_retain_clause(map, source, last->id) &&
          compact_term_rebase_map_retain_clause(map, source, first->id) &&
          compact_term_rebase_map_retain_clause(map, source, first->id),
          "in-place retention deduplicates arbitrary proof-ID requests");
    compact_term_pool_compact_retained(source, map);
    translated_first = compact_term_rebase_offset(map, first_offset);
    translated_last = compact_term_rebase_offset(map, last_offset);
    compact_term_pool_get_stats(source, &compacted);
    CHECK(translated_first == 0 && translated_last == first_length &&
          compact_term_pool_tokens(source)[translated_first] == first_root &&
          compact_term_pool_tokens(source)[translated_last] == last_root,
          "in-place compaction preserves retained token intervals");
    CHECK(compacted.clause_entries == 2 &&
          compacted.logical_tokens == first_length + last_length &&
          compacted.compactions == 1 &&
          compacted.bytes_reclaimed > 0 &&
          compacted.total_bytes < before.total_bytes,
          "in-place compaction drops stale tokens and directory entries");
    compact_term_rebase_map_free(map);
    compact_term_pool_free(source);
    delete_clause(first);
    delete_clause(stale);
    delete_clause(last);
  }

  if (Failures != 0) {
    fprintf(stderr, "compact_term_pool_test: %d failure(s)\n", Failures);
    return 1;
  }
  printf("compact_term_pool_test: PASS\n");
  return 0;
}
