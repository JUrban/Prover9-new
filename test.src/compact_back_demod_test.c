#include "../provers.src/compact_back_demod.h"

#include <stdint.h>

static int Failures;

#define CHECK(test, message) do {                                    \
  if (!(test)) {                                                     \
    fprintf(stderr, "FAIL: %s (line %d)\n", (message), __LINE__);  \
    Failures++;                                                      \
  }                                                                 \
} while (0)

static Topform indexed_clause(const char *text)
{
  Topform clause = parse_clause_from_string((char *) text);
  assign_clause_id(clause);
  return clause;
}

int main(void)
{
  Compact_back_demod_index index;
  struct compact_back_demod_stats stats;
  Topform first, second, irrelevant, repeated_good, repeated_bad;
  Topform demod, path_demod, bidirectional, repeated_demod;
  unsigned long long *ids;
  size_t count;

  init_standard_ladr();
  index = compact_back_demod_init();
  first = indexed_clause("p(f(a),g(b)).");
  second = indexed_clause("q(h(f(c))).");
  irrelevant = indexed_clause("r(k(d)).");
  repeated_good = indexed_clause("s(m(a,a)).");
  repeated_bad = indexed_clause("s(m(a,b)).");
  CHECK(compact_back_demod_add(index, first), "add first clause");
  CHECK(compact_back_demod_add(index, second), "add second clause");
  CHECK(compact_back_demod_add(index, irrelevant), "add irrelevant clause");
  CHECK(compact_back_demod_add(index, repeated_good),
        "add repeated-variable match");
  CHECK(compact_back_demod_add(index, repeated_bad),
        "add repeated-variable nonmatch");
  CHECK(!compact_back_demod_add(index, first), "reject duplicate proof ID");

  demod = indexed_clause("f(x) = x.");
  ids = compact_back_demod_candidate_ids(index, demod, ORIENTED, &count);
  CHECK(count == 2, "root-symbol posting finds both possible redex clauses");
  CHECK(ids != NULL && ids[0] == second->id && ids[1] == first->id,
        "candidate IDs are in decreasing proof-ID order");
  compact_back_demod_note_exact_query(index, count, count, 0);
  safe_free(ids);

  path_demod = indexed_clause("f(a) = a.");
  ids = compact_back_demod_candidate_ids(index, path_demod,
                                         ORIENTED, &count);
  CHECK(count == 1 && ids[0] == first->id,
        "path signature rejects a different fixed child");
  compact_back_demod_note_exact_query(index, count, count, 0);
  safe_free(ids);

  repeated_demod = indexed_clause("m(x,x) = x.");
  ids = compact_back_demod_candidate_ids(index, repeated_demod,
                                         ORIENTED, &count);
  CHECK(count == 1 && ids[0] == repeated_good->id,
        "structural filter enforces repeated pattern variables");
  compact_back_demod_note_exact_query(index, count, count, 0);
  safe_free(ids);

  bidirectional = indexed_clause("k(x) = g(x).");
  ids = compact_back_demod_candidate_ids(index, bidirectional,
                                         LEX_DEP_BOTH, &count);
  CHECK(count == 2, "bidirectional query merges both source symbols");
  CHECK(ids != NULL && ids[0] == irrelevant->id && ids[1] == first->id,
        "merged candidates remain unique and decreasing");
  compact_back_demod_note_exact_query(index, count, count, 0);
  safe_free(ids);

  CHECK(compact_back_demod_remove(index, second->id), "remove live clause");
  CHECK(!compact_back_demod_remove(index, second->id), "reject double remove");
  ids = compact_back_demod_candidate_ids(index, demod, ORIENTED, &count);
  CHECK(count == 1 && ids[0] == first->id,
        "retired postings are ignored");
  compact_back_demod_note_exact_query(index, count, count, 0);
  safe_free(ids);

  compact_back_demod_get_stats(index, &stats);
  CHECK(stats.active == 4 && stats.retired == 1 && stats.physical == 5,
        "lifecycle counters are exact");
  CHECK(stats.queries == 5 && stats.exact_tests == 7,
        "query accounting is exact");
  CHECK(stats.query_profile.queries == stats.queries &&
        stats.query_profile.exact_tests == stats.exact_tests &&
        stats.query_profile.candidate_max == 2 &&
        stats.query_profile.bytes_decoded > 0,
        "back-demod profile records candidates, exact work, and bytes");
  CHECK(stats.path_filter_checks > 0 && stats.path_filter_rejects > 0,
        "path filter accounts for a safe fixed-symbol rejection");
  CHECK(stats.posting_groups == 14 && stats.symbol_occurrences == 15,
        "repeated clause symbols share one posting group");
  CHECK(stats.path_buckets > 0,
        "root/path posting directory contains sparse buckets");
  CHECK(stats.occurrence_stream_bytes > 0 &&
        stats.occurrence_stream_bytes <
          stats.symbol_occurrences * (sizeof(uint32_t) + sizeof(uint32_t)),
        "delta offsets plus path masks beat fixed offset/mask records");
  CHECK(stats.posting_stream_used > 0 &&
        stats.posting_stream_used < stats.posting_groups * 12 &&
        stats.posting_stream_bytes >= stats.posting_stream_used,
        "delta posting blocks are smaller than fixed linked postings");
  CHECK(stats.total_bytes > 0 && stats.peak_bytes >= stats.total_bytes,
        "resident byte accounting is present");

  compact_back_demod_free(index);

  {
    Compact_back_demod_index gap_index = compact_back_demod_init();
    struct compact_back_demod_stats gap_stats;
    Topform gap_clause, gap_demod, duplicate_clause, duplicate_demod;
    char text[1024];
    size_t used = 0;
    int j;
    used += (size_t) snprintf(text + used, sizeof(text) - used, "t(m(a),");
    for (j = 0; j < 140; j++)
      used += (size_t) snprintf(text + used, sizeof(text) - used, "g(");
    used += (size_t) snprintf(text + used, sizeof(text) - used, "m(b)");
    for (j = 0; j < 140; j++)
      used += (size_t) snprintf(text + used, sizeof(text) - used, ")");
    (void) snprintf(text + used, sizeof(text) - used, ").");
    gap_clause = indexed_clause(text);
    gap_demod = indexed_clause("m(x) = x.");
    CHECK(compact_back_demod_add(gap_index, gap_clause),
          "add clause with distant repeated symbol");
    ids = compact_back_demod_candidate_ids(
      gap_index, gap_demod, ORIENTED, &count);
    CHECK(count == 1 && ids[0] == gap_clause->id,
          "multi-byte occurrence delta retrieves distant occurrence");
    compact_back_demod_note_exact_query(gap_index, count, count, 0);
    safe_free(ids);
    compact_back_demod_get_stats(gap_index, &gap_stats);
    CHECK(gap_stats.posting_groups >= 4 &&
          gap_stats.posting_groups < gap_stats.symbol_occurrences &&
          gap_stats.symbol_occurrences == 144 &&
          gap_stats.occurrence_stream_bytes > gap_stats.symbol_occurrences &&
          gap_stats.occurrence_stream_bytes <
            gap_stats.symbol_occurrences *
              (sizeof(uint32_t) + sizeof(uint32_t)),
          "bucketed multi-byte deltas beat fixed offset records");
    duplicate_clause = indexed_clause("d(n(a),n(a)).");
    duplicate_demod = indexed_clause("n(x) = x.");
    CHECK(compact_back_demod_add(gap_index, duplicate_clause),
          "add clause whose arguments share one pooled token slice");
    ids = compact_back_demod_candidate_ids(
      gap_index, duplicate_demod, ORIENTED, &count);
    CHECK(count == 1 && ids[0] == duplicate_clause->id,
          "deduplicated pooled offsets retain the clause candidate");
    compact_back_demod_note_exact_query(gap_index, count, count, 0);
    safe_free(ids);
    compact_back_demod_free(gap_index);
    delete_clause(gap_clause);
    delete_clause(gap_demod);
    delete_clause(duplicate_clause);
    delete_clause(duplicate_demod);
  }

  {
    Compact_back_demod_index block_index = compact_back_demod_init();
    struct compact_back_demod_stats block_stats;
    Topform block_clauses[100];
    Topform block_demod;
    char text[64];
    int j;
    for (j = 0; j < 100; j++) {
      (void) snprintf(text, sizeof(text), "u(f(c%d)).", j);
      block_clauses[j] = indexed_clause(text);
      CHECK(compact_back_demod_add(block_index, block_clauses[j]),
            "append posting across a block boundary");
    }
    block_demod = indexed_clause("f(x) = x.");
    ids = compact_back_demod_candidate_ids(
      block_index, block_demod, ORIENTED, &count);
    CHECK(count == 100,
          "multi-block symbol stream returns every exact candidate");
    CHECK(ids != NULL && ids[0] == block_clauses[99]->id &&
          ids[99] == block_clauses[0]->id,
          "multi-block candidates retain decreasing proof-ID order");
    compact_back_demod_note_exact_query(block_index, count, count, 0);
    safe_free(ids);
    compact_back_demod_get_stats(block_index, &block_stats);
    CHECK(block_stats.posting_stream_used > 0 &&
          block_stats.posting_stream_bytes >=
            block_stats.posting_stream_used,
          "multi-block posting byte accounting is exact");
    compact_back_demod_free(block_index);
    for (j = 0; j < 100; j++)
      delete_clause(block_clauses[j]);
    delete_clause(block_demod);
  }

  {
    enum { DEEP_FAMILY = 128, DEEP_TARGET = 73 };
    Compact_back_demod_index mask_index, signature_index;
    struct compact_back_demod_stats mask_stats, signature_stats;
    Topform deep_clauses[DEEP_FAMILY];
    Topform deep_demod;
    unsigned long long *mask_ids, *signature_ids;
    size_t mask_count, signature_count;
    char text[128];
    int j;

    compact_back_demod_set_strategy(COMPACT_BACK_DEMOD_MASK8);
    mask_index = compact_back_demod_init();
    compact_back_demod_set_strategy(COMPACT_BACK_DEMOD_SIGNATURE32);
    signature_index = compact_back_demod_init();
    for (j = 0; j < DEEP_FAMILY; j++) {
      (void) snprintf(text, sizeof(text),
                      "u(f(a,g(h(j(c%d))))).", j);
      deep_clauses[j] = indexed_clause(text);
      CHECK(compact_back_demod_add(mask_index, deep_clauses[j]),
            "add deep family clause to mask8 index");
      CHECK(compact_back_demod_add(signature_index, deep_clauses[j]),
            "add deep family clause to signature32 index");
    }
    (void) snprintf(text, sizeof(text),
                    "f(a,g(h(j(c%d)))) = a.", DEEP_TARGET);
    deep_demod = indexed_clause(text);
    mask_ids = compact_back_demod_candidate_ids(
      mask_index, deep_demod, ORIENTED, &mask_count);
    signature_ids = compact_back_demod_candidate_ids(
      signature_index, deep_demod, ORIENTED, &signature_count);
    CHECK(mask_count == 1 && signature_count == mask_count &&
          mask_ids[0] == deep_clauses[DEEP_TARGET]->id &&
          signature_ids[0] == mask_ids[0],
          "unbounded signature preserves the exact deep candidate");
    compact_back_demod_note_exact_query(mask_index, mask_count, mask_count, 0);
    compact_back_demod_note_exact_query(
      signature_index, signature_count, signature_count, 0);
    compact_back_demod_get_stats(mask_index, &mask_stats);
    compact_back_demod_get_stats(signature_index, &signature_stats);
    CHECK(mask_stats.strategy == COMPACT_BACK_DEMOD_MASK8 &&
          signature_stats.strategy == COMPACT_BACK_DEMOD_SIGNATURE32,
          "back-demod indexes retain their configured strategies");
    CHECK(mask_stats.posting_groups_examined >= DEEP_FAMILY &&
          signature_stats.posting_groups_examined <= 8 &&
          signature_stats.posting_groups_examined * 16 <
            mask_stats.posting_groups_examined,
          "unbounded signature separates deep same-shape occurrences");
    CHECK(compact_back_demod_remove(
            signature_index, deep_clauses[DEEP_TARGET]->id),
          "remove deep signature answer");
    compact_back_demod_compact_all_stale(signature_index);
    safe_free(signature_ids);
    signature_ids = compact_back_demod_candidate_ids(
      signature_index, deep_demod, ORIENTED, &signature_count);
    CHECK(signature_count == 0 && signature_ids == NULL,
          "signature strategy survives deletion and forced rebuild");

    safe_free(mask_ids);
    compact_back_demod_free(mask_index);
    compact_back_demod_free(signature_index);
    delete_clause(deep_demod);
    for (j = 0; j < DEEP_FAMILY; j++)
      delete_clause(deep_clauses[j]);
    compact_back_demod_set_strategy(COMPACT_BACK_DEMOD_MASK8);
  }

  delete_clause(first);
  delete_clause(second);
  delete_clause(irrelevant);
  delete_clause(repeated_good);
  delete_clause(repeated_bad);
  delete_clause(demod);
  delete_clause(path_demod);
  delete_clause(bidirectional);
  delete_clause(repeated_demod);

  if (Failures != 0) {
    fprintf(stderr, "compact_back_demod_test: %d failure(s)\n", Failures);
    return 1;
  }
  printf("compact_back_demod_test: PASS\n");
  return 0;
}
