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
    CHECK(compact_term_pool_retained_reclaimable_bytes(source, map) > 0,
          "retained compaction predicts an allocation reduction");
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
          compacted.rebase_growths > 0 &&
#if defined(__linux__) && !defined(__EMSCRIPTEN__)
          compacted.rebase_copy_bytes == 0 &&
          compacted.streamed_rebases == 1 &&
          compacted.file_sorted_rebases == 0 &&
#endif
          compacted.bytes_reclaimed > 0 &&
          compacted.total_bytes < before.total_bytes,
          "in-place compaction drops stale tokens and directory entries");
    compact_term_rebase_map_free(map);
    compact_term_pool_free(source);
    delete_clause(first);
    delete_clause(stale);
    delete_clause(last);
  }

  {
    Compact_term_pool source = compact_term_pool_init();
    Compact_term_rebase_map map = compact_term_rebase_map_init();
    struct compact_term_pool_stats compacted;
    Topform earlier = parse_clause_from_string("s(f(d)).");
    Topform later = parse_clause_from_string("t(g(e)).");
    uint32_t earlier_offset, later_offset, earlier_length, later_length;
    earlier->id = 900;
    later->id = 100;
    earlier_offset = compact_term_pool_intern(
      source, earlier->id, earlier->literals, earlier->literals->atom,
      &earlier_length);
    later_offset = compact_term_pool_intern(
      source, later->id, later->literals, later->literals->atom,
      &later_length);
    CHECK(compact_term_rebase_map_retain_clause(
            map, source, earlier->id) &&
          compact_term_rebase_map_retain_clause(
            map, source, later->id),
          "retained compaction accepts non-monotone proof-ID histories");
    compact_term_pool_compact_retained(source, map);
    compact_term_pool_get_stats(source, &compacted);
    CHECK(compact_term_rebase_offset(map, earlier_offset) == 0 &&
          compact_term_rebase_offset(map, later_offset) == earlier_length &&
          compact_term_pool_token_count(source) ==
            earlier_length + later_length &&
#if defined(__linux__) && !defined(__EMSCRIPTEN__)
          compacted.streamed_rebases == 1 &&
          compacted.file_sorted_rebases == 1 &&
#endif
          compacted.compactions == 1,
          "non-monotone proof IDs use the bounded file-sort path");
    compact_term_rebase_map_free(map);
    compact_term_pool_free(source);
    delete_clause(earlier);
    delete_clause(later);
  }

  {
    const unsigned long long high_base =
      (unsigned long long) UINT32_MAX + 123ULL;
    Compact_term_pool source = compact_term_pool_init();
    Compact_term_pool destination = compact_term_pool_init();
    Compact_term_rebase_map copy_map = compact_term_rebase_map_init();
    Compact_term_rebase_map retained_map = compact_term_rebase_map_init();
    Topform first = parse_clause_from_string("u(f(k)).");
    Topform stale = parse_clause_from_string("v(g(l)).");
    Topform last = parse_clause_from_string("w(h(m)).");
    Compact_term_slice first_slice, stale_slice, last_slice;
    Compact_term_slice interior, translated_interior;
    Compact_term_slice translated_first, translated_last;
    int32_t first_root, last_root;
    first->id = 1001;
    stale->id = 1002;
    last->id = 1003;
    compact_term_pool_set_logical_base(source, high_base);
    first_slice = compact_term_pool_intern_slice(
      source, first->id, first->literals, first->literals->atom);
    stale_slice = compact_term_pool_intern_slice(
      source, stale->id, stale->literals, stale->literals->atom);
    last_slice = compact_term_pool_intern_slice(
      source, last->id, last->literals, last->literals->atom);
    CHECK(compact_term_slice_offset(first_slice) == high_base &&
          compact_term_slice_offset(stale_slice) > UINT32_MAX &&
          compact_term_slice_offset(last_slice) >
            compact_term_slice_offset(stale_slice) &&
          compact_term_slice_length(first_slice) == 3,
          "packed slices address bounded storage above UINT32_MAX");
    first_root = compact_term_pool_slice_tokens(source, first_slice)[0];
    last_root = compact_term_pool_slice_tokens(source, last_slice)[0];
    CHECK(compact_term_pool_copy_clause(
            destination, source, copy_map, last->id) &&
          compact_term_pool_copy_clause(
            destination, source, copy_map, first->id),
          "high-base clause copying uses packed term slices");
    compact_term_rebase_map_finalize(copy_map);
    CHECK(compact_term_slice_subslice(first_slice, 1, 1, &interior),
          "high-base interior subslice is representable");
    translated_interior = compact_term_rebase_slice(copy_map, interior);
    CHECK(compact_term_slice_offset(translated_interior) > UINT32_MAX &&
          compact_term_pool_slice_tokens(
            destination, translated_interior)[0] ==
          compact_term_pool_slice_tokens(source, interior)[0],
          "copy rebase translates an interior token above UINT32_MAX");

    CHECK(compact_term_rebase_map_retain_clause(
            retained_map, source, last->id) &&
          compact_term_rebase_map_retain_clause(
            retained_map, source, first->id),
          "high-base retained compaction accepts packed directory slices");
    compact_term_pool_compact_retained(source, retained_map);
    translated_first = compact_term_rebase_slice(retained_map, first_slice);
    translated_last = compact_term_rebase_slice(retained_map, last_slice);
    CHECK(compact_term_slice_offset(translated_first) == high_base &&
          compact_term_slice_offset(translated_last) ==
            high_base + compact_term_slice_length(first_slice) &&
          compact_term_pool_slice_tokens(source, translated_first)[0] ==
            first_root &&
          compact_term_pool_slice_tokens(source, translated_last)[0] ==
            last_root,
          "retained compaction preserves high-base slices and token order");
    compact_term_rebase_map_free(copy_map);
    compact_term_rebase_map_free(retained_map);
    compact_term_pool_free(destination);
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
