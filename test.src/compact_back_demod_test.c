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
  Topform demod, bidirectional, repeated_demod;
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
  safe_free(ids);

  repeated_demod = indexed_clause("m(x,x) = x.");
  ids = compact_back_demod_candidate_ids(index, repeated_demod,
                                         ORIENTED, &count);
  CHECK(count == 1 && ids[0] == repeated_good->id,
        "structural filter enforces repeated pattern variables");
  safe_free(ids);

  bidirectional = indexed_clause("k(x) = g(x).");
  ids = compact_back_demod_candidate_ids(index, bidirectional,
                                         LEX_DEP_BOTH, &count);
  CHECK(count == 2, "bidirectional query merges both source symbols");
  CHECK(ids != NULL && ids[0] == irrelevant->id && ids[1] == first->id,
        "merged candidates remain unique and decreasing");
  safe_free(ids);

  CHECK(compact_back_demod_remove(index, second->id), "remove live clause");
  CHECK(!compact_back_demod_remove(index, second->id), "reject double remove");
  ids = compact_back_demod_candidate_ids(index, demod, ORIENTED, &count);
  CHECK(count == 1 && ids[0] == first->id,
        "retired postings are ignored");
  safe_free(ids);

  compact_back_demod_note_exact_tests(index, 3);
  compact_back_demod_get_stats(index, &stats);
  CHECK(stats.active == 4 && stats.retired == 1 && stats.physical == 5,
        "lifecycle counters are exact");
  CHECK(stats.queries == 4 && stats.exact_tests == 3,
        "query accounting is exact");
  CHECK(stats.posting_groups == 14 && stats.symbol_occurrences == 15,
        "repeated clause symbols share one posting group");
  CHECK(stats.occurrence_stream_bytes > 0 &&
        stats.occurrence_stream_bytes <
          stats.symbol_occurrences * sizeof(uint32_t),
        "delta occurrence stream is smaller than raw offsets");
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
    safe_free(ids);
    compact_back_demod_get_stats(gap_index, &gap_stats);
    CHECK(gap_stats.posting_groups == 4 &&
          gap_stats.symbol_occurrences == 144 &&
          gap_stats.occurrence_stream_bytes > gap_stats.symbol_occurrences &&
          gap_stats.occurrence_stream_bytes <
            gap_stats.symbol_occurrences * sizeof(uint32_t),
          "multi-byte deltas remain smaller than raw offsets");
    duplicate_clause = indexed_clause("d(n(a),n(a)).");
    duplicate_demod = indexed_clause("n(x) = x.");
    CHECK(compact_back_demod_add(gap_index, duplicate_clause),
          "add clause whose arguments share one pooled token slice");
    ids = compact_back_demod_candidate_ids(
      gap_index, duplicate_demod, ORIENTED, &count);
    CHECK(count == 1 && ids[0] == duplicate_clause->id,
          "deduplicated pooled offsets retain the clause candidate");
    safe_free(ids);
    compact_back_demod_free(gap_index);
    delete_clause(gap_clause);
    delete_clause(gap_demod);
    delete_clause(duplicate_clause);
    delete_clause(duplicate_demod);
  }

  delete_clause(first);
  delete_clause(second);
  delete_clause(irrelevant);
  delete_clause(repeated_good);
  delete_clause(repeated_bad);
  delete_clause(demod);
  delete_clause(bidirectional);
  delete_clause(repeated_demod);

  if (Failures != 0) {
    fprintf(stderr, "compact_back_demod_test: %d failure(s)\n", Failures);
    return 1;
  }
  printf("compact_back_demod_test: PASS\n");
  return 0;
}
