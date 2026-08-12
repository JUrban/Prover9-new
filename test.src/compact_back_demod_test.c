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
    Compact_back_demod_index mask_index, signature_index, tree_index,
      hybrid_index;
    struct compact_back_demod_stats mask_stats, signature_stats, tree_stats,
      hybrid_stats;
    Topform deep_clauses[DEEP_FAMILY];
    Topform deep_demod, cache_demod;
    unsigned long long *mask_ids, *signature_ids, *tree_ids, *hybrid_ids;
    size_t mask_count, signature_count, tree_count, hybrid_count;
    char text[128];
    int j;

    compact_back_demod_set_strategy(COMPACT_BACK_DEMOD_MASK8);
    mask_index = compact_back_demod_init();
    compact_back_demod_set_strategy(COMPACT_BACK_DEMOD_SIGNATURE32);
    signature_index = compact_back_demod_init();
    compact_back_demod_set_strategy(COMPACT_BACK_DEMOD_CODE_TREE);
    tree_index = compact_back_demod_init();
    compact_back_demod_set_tree_min_tokens(6);
    compact_back_demod_set_tree_budget_kb(65536);
    compact_back_demod_set_strategy(COMPACT_BACK_DEMOD_HYBRID_TREE);
    hybrid_index = compact_back_demod_init();
    for (j = 0; j < DEEP_FAMILY; j++) {
      (void) snprintf(text, sizeof(text),
                      "u(f(a,g(h(j(c%d))))).", j);
      deep_clauses[j] = indexed_clause(text);
      CHECK(compact_back_demod_add(mask_index, deep_clauses[j]),
            "add deep family clause to mask8 index");
      CHECK(compact_back_demod_add(signature_index, deep_clauses[j]),
            "add deep family clause to signature32 index");
      CHECK(compact_back_demod_add(tree_index, deep_clauses[j]),
            "add deep family clause to code-tree index");
      CHECK(compact_back_demod_add(hybrid_index, deep_clauses[j]),
            "add deep family clause to hybrid index");
    }
    (void) snprintf(text, sizeof(text),
                    "f(a,g(h(j(c%d)))) = a.", DEEP_TARGET);
    deep_demod = indexed_clause(text);
    mask_ids = compact_back_demod_candidate_ids(
      mask_index, deep_demod, ORIENTED, &mask_count);
    signature_ids = compact_back_demod_candidate_ids(
      signature_index, deep_demod, ORIENTED, &signature_count);
    tree_ids = compact_back_demod_candidate_ids(
      tree_index, deep_demod, ORIENTED, &tree_count);
    hybrid_ids = compact_back_demod_candidate_ids(
      hybrid_index, deep_demod, ORIENTED, &hybrid_count);
    CHECK(mask_count == 1 && signature_count == mask_count &&
          tree_count == mask_count && hybrid_count == mask_count &&
          mask_ids[0] == deep_clauses[DEEP_TARGET]->id &&
          signature_ids[0] == mask_ids[0] && tree_ids[0] == mask_ids[0] &&
          hybrid_ids[0] == mask_ids[0],
          "arbitrary-depth indexes preserve the exact deep candidate");
    compact_back_demod_note_exact_query(mask_index, mask_count, mask_count, 0);
    compact_back_demod_note_exact_query(
      signature_index, signature_count, signature_count, 0);
    compact_back_demod_note_exact_query(
      tree_index, tree_count, tree_count, 0);
    compact_back_demod_note_exact_query(
      hybrid_index, hybrid_count, hybrid_count, 0);
    compact_back_demod_get_stats(mask_index, &mask_stats);
    compact_back_demod_get_stats(signature_index, &signature_stats);
    compact_back_demod_get_stats(tree_index, &tree_stats);
    compact_back_demod_get_stats(hybrid_index, &hybrid_stats);
    CHECK(mask_stats.strategy == COMPACT_BACK_DEMOD_MASK8 &&
          signature_stats.strategy == COMPACT_BACK_DEMOD_SIGNATURE32 &&
          tree_stats.strategy == COMPACT_BACK_DEMOD_CODE_TREE,
          "back-demod indexes retain their configured strategies");
    CHECK(hybrid_stats.strategy == COMPACT_BACK_DEMOD_HYBRID_TREE &&
          hybrid_stats.tree_complete && hybrid_stats.tree_queries == 1 &&
          hybrid_stats.path_buckets > 0 && hybrid_stats.tree_nodes > 0,
          "hybrid keeps a complete fallback and selective tree partition");
    CHECK(mask_stats.posting_groups_examined >= DEEP_FAMILY &&
          signature_stats.posting_groups_examined <= 8 &&
          signature_stats.posting_groups_examined * 16 <
            mask_stats.posting_groups_examined,
          "unbounded signature separates deep same-shape occurrences");
    CHECK(tree_stats.tree_nodes > 0 && tree_stats.tree_terminals > 0 &&
          tree_stats.tree_queries == 1 &&
          tree_stats.posting_groups_examined == 1 &&
          tree_stats.occurrences_examined == 1 &&
          tree_stats.occurrence_stream_bytes == 0,
          "code tree matches one terminal without occurrence storage");
    for (j = 0; j < DEEP_FAMILY; j++) {
      unsigned long long *cache_ids;
      size_t cache_count;
      (void) snprintf(text, sizeof(text),
                      "f(a,g(h(j(c%d)))) = a.", j);
      cache_demod = indexed_clause(text);
      cache_ids = compact_back_demod_candidate_ids(
        tree_index, cache_demod, ORIENTED, &cache_count);
      CHECK(cache_count == 1 && cache_ids[0] == deep_clauses[j]->id,
            "child-cache collision fill preserves exact candidate");
      safe_free(cache_ids);
      delete_clause(cache_demod);
    }
    for (j = DEEP_FAMILY - 1; j >= 0; j--) {
      unsigned long long *cache_ids;
      size_t cache_count;
      (void) snprintf(text, sizeof(text),
                      "f(a,g(h(j(c%d)))) = a.", j);
      cache_demod = indexed_clause(text);
      cache_ids = compact_back_demod_candidate_ids(
        tree_index, cache_demod, ORIENTED, &cache_count);
      CHECK(cache_count == 1 && cache_ids[0] == deep_clauses[j]->id,
            "child-cache collision hit preserves exact candidate");
      safe_free(cache_ids);
      delete_clause(cache_demod);
    }
    compact_back_demod_get_stats(tree_index, &tree_stats);
    CHECK(tree_stats.tree_child_cache_parents > 0 &&
          tree_stats.tree_child_cache_lookups > 0 &&
          tree_stats.tree_child_cache_hits > 0 &&
          tree_stats.tree_child_cache_misses > 0 &&
          tree_stats.tree_child_cache_replacements > 0 &&
          tree_stats.tree_child_cache_bytes <= 8 * 1024 * 1024,
          "broad-parent cache is bounded and exercises hits and collisions");
    CHECK(compact_back_demod_remove(
            signature_index, deep_clauses[DEEP_TARGET]->id),
          "remove deep signature answer");
    compact_back_demod_compact_all_stale(signature_index);
    safe_free(signature_ids);
    signature_ids = compact_back_demod_candidate_ids(
      signature_index, deep_demod, ORIENTED, &signature_count);
    CHECK(signature_count == 0 && signature_ids == NULL,
          "signature strategy survives deletion and forced rebuild");
    CHECK(compact_back_demod_remove(
            tree_index, deep_clauses[DEEP_TARGET]->id),
          "remove deep code-tree answer");
    compact_back_demod_compact_all_stale(tree_index);
    safe_free(tree_ids);
    tree_ids = compact_back_demod_candidate_ids(
      tree_index, deep_demod, ORIENTED, &tree_count);
    CHECK(tree_count == 0 && tree_ids == NULL,
          "code-tree strategy survives deletion and forced rebuild");

    safe_free(mask_ids);
    compact_back_demod_free(mask_index);
    compact_back_demod_free(signature_index);
    compact_back_demod_free(tree_index);
    compact_back_demod_free(hybrid_index);
    safe_free(hybrid_ids);
    delete_clause(deep_demod);
    for (j = 0; j < DEEP_FAMILY; j++)
      delete_clause(deep_clauses[j]);
    compact_back_demod_set_strategy(COMPACT_BACK_DEMOD_MASK8);
  }

  {
    Compact_back_demod_index bounded;
    struct compact_back_demod_stats bounded_stats;
    Topform clause = indexed_clause("v(f(a,g(h(j(c))))).");
    Topform rule = indexed_clause("f(x,y) = x.");
    compact_back_demod_set_tree_min_tokens(1);
    compact_back_demod_set_tree_budget_kb(1);
    compact_back_demod_set_strategy(COMPACT_BACK_DEMOD_HYBRID_TREE);
    bounded = compact_back_demod_init();
    CHECK(compact_back_demod_add(bounded, clause),
          "add clause after structural budget exhaustion");
    ids = compact_back_demod_candidate_ids(
      bounded, rule, ORIENTED, &count);
    CHECK(count == 1 && ids[0] == clause->id,
          "budget exhaustion falls back without losing candidates");
    compact_back_demod_get_stats(bounded, &bounded_stats);
    CHECK(!bounded_stats.tree_complete &&
          bounded_stats.tree_budget_exhaustions == 1 &&
          bounded_stats.path_buckets > 0,
          "hybrid reports deterministic budget fallback");
    safe_free(ids);
    compact_back_demod_free(bounded);
    delete_clause(clause);
    delete_clause(rule);
    compact_back_demod_set_tree_min_tokens(8);
    compact_back_demod_set_tree_budget_kb(65536);
    compact_back_demod_set_strategy(COMPACT_BACK_DEMOD_MASK8);
  }

  {
    enum { HOT_FAMILY = 64, HOT_TARGET = 31 };
    Compact_back_demod_index hot;
    struct compact_back_demod_stats hot_stats;
    Topform clauses[HOT_FAMILY], rule, later;
    char text[128];
    int j;
    compact_back_demod_set_tree_budget_kb(65536);
    compact_back_demod_set_tree_admit_work(1);
    compact_back_demod_set_tree_build_factor(1);
    compact_back_demod_set_strategy(COMPACT_BACK_DEMOD_HOT_ROOT_TREE);
    hot = compact_back_demod_init();
    for (j = 0; j < HOT_FAMILY; j++) {
      (void) snprintf(text, sizeof(text), "w(f(a,g(h(j(c%d))))).", j);
      clauses[j] = indexed_clause(text);
      CHECK(compact_back_demod_add(hot, clauses[j]),
            "add hot-root backfill family");
    }
    (void) snprintf(text, sizeof(text),
                    "f(a,g(h(j(c%d)))) = a.", HOT_TARGET);
    rule = indexed_clause(text);
    ids = compact_back_demod_candidate_ids(hot, rule, ORIENTED, &count);
    CHECK(count == 1 && ids[0] == clauses[HOT_TARGET]->id,
          "admission query returns the complete fallback answer");
    safe_free(ids);
    compact_back_demod_get_stats(hot, &hot_stats);
    CHECK(hot_stats.tree_root_admissions == 1 &&
          hot_stats.tree_queries == 0 && hot_stats.tree_complete,
          "fallback work admits and backfills one complete root");
    ids = compact_back_demod_candidate_ids(hot, rule, ORIENTED, &count);
    CHECK(count == 1 && ids[0] == clauses[HOT_TARGET]->id,
          "admitted root uses the same exact tree answer");
    safe_free(ids);
    later = indexed_clause("w(f(a,g(h(j(c31))))).");
    CHECK(compact_back_demod_add(hot, later),
          "index later occurrence for an admitted root");
    ids = compact_back_demod_candidate_ids(hot, rule, ORIENTED, &count);
    CHECK(count == 2 && ids[0] == later->id &&
          ids[1] == clauses[HOT_TARGET]->id,
          "admitted root remains complete after incremental insertion");
    safe_free(ids);
    CHECK(compact_back_demod_remove(hot, later->id),
          "remove later admitted-root occurrence");
    compact_back_demod_compact_all_stale(hot);
    ids = compact_back_demod_candidate_ids(hot, rule, ORIENTED, &count);
    CHECK(count == 1 && ids[0] == clauses[HOT_TARGET]->id,
          "hot-root admission survives forced compaction");
    safe_free(ids);
    compact_back_demod_free(hot);
    delete_clause(rule);
    delete_clause(later);
    for (j = 0; j < HOT_FAMILY; j++)
      delete_clause(clauses[j]);
    compact_back_demod_set_tree_admit_work(4096);
    compact_back_demod_set_tree_build_factor(8);
    compact_back_demod_set_strategy(COMPACT_BACK_DEMOD_MASK8);
  }

  {
    enum { COST_FAMILY = 64 };
    Compact_back_demod_index cost_aware;
    struct compact_back_demod_stats cost_stats;
    Topform clauses[COST_FAMILY], rule;
    char text[128];
    int j;
    compact_back_demod_set_tree_budget_kb(65536);
    compact_back_demod_set_tree_admit_work(1);
    compact_back_demod_set_tree_build_factor(8);
    compact_back_demod_set_strategy(COMPACT_BACK_DEMOD_HOT_ROOT_TREE);
    cost_aware = compact_back_demod_init();
    for (j = 0; j < COST_FAMILY; j++) {
      (void) snprintf(text, sizeof(text), "w(f(a,g(h(j(d%d))))).", j);
      clauses[j] = indexed_clause(text);
      CHECK(compact_back_demod_add(cost_aware, clauses[j]),
            "add cost-aware hot-root family");
    }
    rule = indexed_clause("f(a,g(h(j(d31)))) = a.");
    ids = compact_back_demod_candidate_ids(
      cost_aware, rule, ORIENTED, &count);
    safe_free(ids);
    compact_back_demod_get_stats(cost_aware, &cost_stats);
    CHECK(cost_stats.tree_root_admissions == 0 &&
          cost_stats.tree_root_cost_deferrals == 1 &&
          cost_stats.tree_root_censuses == 1,
          "hot root waits until fallback work repays construction");
    for (j = 1; j < 8; j++) {
      ids = compact_back_demod_candidate_ids(
        cost_aware, rule, ORIENTED, &count);
      safe_free(ids);
    }
    compact_back_demod_get_stats(cost_aware, &cost_stats);
    CHECK(cost_stats.tree_root_admissions == 1 &&
          cost_stats.tree_root_cost_deferrals == 1 &&
          cost_stats.tree_root_censuses == 2 &&
          cost_stats.tree_root_census_occurrences >= COST_FAMILY * 2,
          "accumulated fallback work deterministically admits hot root");
    compact_back_demod_free(cost_aware);
    delete_clause(rule);
    for (j = 0; j < COST_FAMILY; j++)
      delete_clause(clauses[j]);
    compact_back_demod_set_tree_admit_work(4096);
    compact_back_demod_set_tree_build_factor(8);
    compact_back_demod_set_strategy(COMPACT_BACK_DEMOD_MASK8);
  }

  {
    enum { DEMOTION_LIMIT = 512 };
    Compact_back_demod_index bounded_hot;
    struct compact_back_demod_stats before_s, after_s, bounded_stats;
    Topform clauses[DEMOTION_LIMIT + 2], r_rule, s_rule;
    char text[128];
    int added = 2, j;
    compact_back_demod_set_tree_budget_kb(16);
    compact_back_demod_set_tree_admit_work(1);
    compact_back_demod_set_tree_build_factor(1);
    compact_back_demod_set_strategy(COMPACT_BACK_DEMOD_HOT_ROOT_TREE);
    bounded_hot = compact_back_demod_init();
    clauses[0] = indexed_clause("w(r(a)).");
    clauses[1] = indexed_clause("w(s(a)).");
    CHECK(compact_back_demod_add(bounded_hot, clauses[0]) &&
          compact_back_demod_add(bounded_hot, clauses[1]),
          "add independent roots before budget demotion");
    r_rule = indexed_clause("r(a) = a.");
    s_rule = indexed_clause("s(a) = a.");
    ids = compact_back_demod_candidate_ids(
      bounded_hot, r_rule, ORIENTED, &count);
    safe_free(ids);
    ids = compact_back_demod_candidate_ids(
      bounded_hot, s_rule, ORIENTED, &count);
    safe_free(ids);
    for (j = 0; j < DEMOTION_LIMIT; j++) {
      (void) snprintf(text, sizeof(text), "w(r(q%d)).", j);
      clauses[added] = indexed_clause(text);
      CHECK(compact_back_demod_add(bounded_hot, clauses[added]),
            "grow one admitted root toward its tree budget");
      added++;
      compact_back_demod_get_stats(bounded_hot, &bounded_stats);
      if (bounded_stats.tree_root_demotions != 0)
        break;
    }
    CHECK(bounded_stats.tree_root_admissions == 2 &&
          bounded_stats.tree_root_demotions == 1 &&
          bounded_stats.tree_budget_exhaustions == 1 &&
          bounded_stats.tree_complete,
          "budget growth demotes only the affected hot root");
    compact_back_demod_get_stats(bounded_hot, &before_s);
    ids = compact_back_demod_candidate_ids(
      bounded_hot, s_rule, ORIENTED, &count);
    CHECK(count == 1 && ids[0] == clauses[1]->id,
          "unrelated admitted root remains complete after demotion");
    safe_free(ids);
    compact_back_demod_get_stats(bounded_hot, &after_s);
    CHECK(after_s.tree_queries == before_s.tree_queries + 1,
          "unrelated admitted root still uses its code tree");
    ids = compact_back_demod_candidate_ids(
      bounded_hot, r_rule, ORIENTED, &count);
    CHECK(count == 1 && ids[0] == clauses[0]->id,
          "demoted root falls back without losing its answer");
    safe_free(ids);
    CHECK(compact_back_demod_remove(bounded_hot, clauses[added - 1]->id),
          "retire one record before rebuilding demoted-root metadata");
    compact_back_demod_compact_all_stale(bounded_hot);
    compact_back_demod_get_stats(bounded_hot, &before_s);
    ids = compact_back_demod_candidate_ids(
      bounded_hot, s_rule, ORIENTED, &count);
    safe_free(ids);
    compact_back_demod_get_stats(bounded_hot, &after_s);
    CHECK(after_s.tree_queries == before_s.tree_queries + 1,
          "forced compaction retains only still-admitted root trees");
    compact_back_demod_free(bounded_hot);
    delete_clause(r_rule);
    delete_clause(s_rule);
    for (j = 0; j < added; j++)
      delete_clause(clauses[j]);
    compact_back_demod_set_tree_budget_kb(65536);
    compact_back_demod_set_tree_admit_work(4096);
    compact_back_demod_set_tree_build_factor(8);
    compact_back_demod_set_strategy(COMPACT_BACK_DEMOD_MASK8);
  }

  {
    enum { POSITION_FAMILY = 128, POSITION_TARGET = 73 };
    Compact_back_demod_index position;
    struct compact_back_demod_stats position_stats;
    Topform clauses[POSITION_FAMILY], rule, later, multi;
    char text[160];
    int j;
    compact_back_demod_set_position_options(1, 4, 1, 65536, 20, TRUE);
    compact_back_demod_set_strategy(COMPACT_BACK_DEMOD_POSITION);
    position = compact_back_demod_init();
    for (j = 0; j < POSITION_FAMILY; j++) {
      (void) snprintf(text, sizeof(text),
                      "w(f(a,g(h(j(c%d))))).", j);
      clauses[j] = indexed_clause(text);
      CHECK(compact_back_demod_add(position, clauses[j]),
            "add exact-position family");
    }
    (void) snprintf(text, sizeof(text),
                    "f(a,g(h(j(c%d)))) = a.", POSITION_TARGET);
    rule = indexed_clause(text);
    ids = compact_back_demod_candidate_ids(
      position, rule, ORIENTED, &count);
    CHECK(count == 1 && ids[0] == clauses[POSITION_TARGET]->id,
          "position probation query retains the fallback answer");
    safe_free(ids);
    ids = compact_back_demod_candidate_ids(
      position, rule, ORIENTED, &count);
    CHECK(count == 1 && ids[0] == clauses[POSITION_TARGET]->id,
          "position admission query retains the fallback answer");
    safe_free(ids);
    compact_back_demod_get_stats(position, &position_stats);
    CHECK(position_stats.position_admissions == 1 &&
          position_stats.position_features == 1 &&
          position_stats.position_queries == 0 &&
          position_stats.position_complete &&
          position_stats.position_postings == 1 &&
          position_stats.position_probation_updates > 0 &&
          position_stats.position_probation_bytes > 0,
          "broad fallback admits one rare complete position feature");
    ids = compact_back_demod_candidate_ids(
      position, rule, ORIENTED, &count);
    CHECK(count == 1 && ids[0] == clauses[POSITION_TARGET]->id,
          "admitted position feature returns the exact same answer");
    safe_free(ids);
    later = indexed_clause("w(f(a,g(h(j(c73))))).");
    CHECK(compact_back_demod_add(position, later),
          "add later clause to admitted position feature");
    ids = compact_back_demod_candidate_ids(
      position, rule, ORIENTED, &count);
    CHECK(count == 2 && ids[0] == later->id &&
          ids[1] == clauses[POSITION_TARGET]->id,
          "position feature remains complete after later insertion");
    safe_free(ids);
    multi = indexed_clause(
      "w(k(f(b,g(h(j(c73)))),f(a,g(h(j(c73)))))).");
    CHECK(compact_back_demod_add(position, multi),
          "add two same-feature occurrences to admitted position feature");
    ids = compact_back_demod_candidate_ids(
      position, rule, ORIENTED, &count);
    CHECK(count == 3 && ids[0] == multi->id && ids[1] == later->id &&
          ids[2] == clauses[POSITION_TARGET]->id,
          "position posting retains every matching root occurrence");
    safe_free(ids);
    CHECK(compact_back_demod_remove(position, later->id),
          "remove later exact-position clause");
    CHECK(compact_back_demod_remove(position, multi->id),
          "remove multi-occurrence exact-position clause");
    compact_back_demod_compact_all_stale(position);
    ids = compact_back_demod_candidate_ids(
      position, rule, ORIENTED, &count);
    CHECK(count == 1 && ids[0] == clauses[POSITION_TARGET]->id,
          "position feature remains complete after forced compaction");
    safe_free(ids);
    compact_back_demod_free(position);
    delete_clause(rule);
    delete_clause(later);
    delete_clause(multi);
    for (j = 0; j < POSITION_FAMILY; j++)
      delete_clause(clauses[j]);
    compact_back_demod_set_position_options(
      4096, 4, 8, 65536, 20, TRUE);
    compact_back_demod_set_strategy(COMPACT_BACK_DEMOD_MASK8);
  }

  {
    Compact_back_demod_index bounded;
    struct compact_back_demod_stats bounded_stats;
    Topform clause, rule;
    compact_back_demod_set_position_options(1, 1, 1, 8, 20, TRUE);
    compact_back_demod_set_strategy(COMPACT_BACK_DEMOD_POSITION);
    bounded = compact_back_demod_init();
    clause = indexed_clause("w(f(a,g(h(j(c))))).");
    rule = indexed_clause("f(a,g(h(j(c)))) = a.");
    CHECK(compact_back_demod_add(bounded, clause),
          "add exact-position byte-budget subject");
    ids = compact_back_demod_candidate_ids(
      bounded, rule, ORIENTED, &count);
    CHECK(count == 1 && ids[0] == clause->id,
          "position byte-budget rejection retains fallback answer");
    safe_free(ids);
    ids = compact_back_demod_candidate_ids(
      bounded, rule, ORIENTED, &count);
    CHECK(count == 1 && ids[0] == clause->id,
          "repeated budgeted query retains fallback answer");
    safe_free(ids);
    compact_back_demod_get_stats(bounded, &bounded_stats);
    CHECK(bounded_stats.position_features == 0 &&
          bounded_stats.position_rejections == 1 &&
          bounded_stats.position_budget_exhaustions == 1 &&
          bounded_stats.position_complete,
          "position admission is rejected before exceeding its byte cap");
    compact_back_demod_free(bounded);
    delete_clause(clause);
    delete_clause(rule);
    compact_back_demod_set_position_options(
      4096, 4, 8, 65536, 20, TRUE);
    compact_back_demod_set_strategy(COMPACT_BACK_DEMOD_MASK8);
  }

  {
    enum { INTERSECTION_FAMILY = 128 };
    Compact_back_demod_index intersected;
    struct compact_back_demod_stats before_compact, after_compact;
    Topform clauses[INTERSECTION_FAMILY], rule, later;
    char text[160];
    int j, round;
    compact_back_demod_set_position_options(1, 4, 1, 65536, 20, TRUE);
    compact_back_demod_set_strategy(COMPACT_BACK_DEMOD_POSITION);
    intersected = compact_back_demod_init();
    for (j = 0; j < INTERSECTION_FAMILY; j++) {
      (void) snprintf(text, sizeof(text),
                      "w(f(a,z(h(j(a%d)),h(j(b%d))))).",
                      j % 32, (j / 32) % 32);
      clauses[j] = indexed_clause(text);
      CHECK(compact_back_demod_add(intersected, clauses[j]),
            "add correlated position-intersection family");
    }
    rule = indexed_clause("f(a,z(h(j(a0)),h(j(b0)))) = a.");
    for (round = 0; round < 35; round++) {
      ids = compact_back_demod_candidate_ids(
        intersected, rule, ORIENTED, &count);
      CHECK(count == 1,
            "position intersection preserves every exact candidate");
      safe_free(ids);
    }
    compact_back_demod_get_stats(intersected, &before_compact);
    CHECK(before_compact.position_admissions == 2 &&
          before_compact.position_features == 2 &&
          before_compact.position_intersection_queries == 1 &&
          before_compact.position_dense_intersection_queries == 0 &&
          before_compact.position_intersection_scans == 4 &&
          before_compact.position_intersection_bit_checks == 4 &&
          before_compact.position_intersection_records == 1 &&
          before_compact.position_bitmap_bytes > 0,
          "two broad features use a bounded bitmap intersection");
    later = indexed_clause("w(f(a,z(h(j(a0)),h(j(b0))))).");
    CHECK(compact_back_demod_add(intersected, later),
          "add later clause to both intersected position features");
    ids = compact_back_demod_candidate_ids(
      intersected, rule, ORIENTED, &count);
    CHECK(count == 2 && ids[0] == later->id,
          "intersection membership stays complete after insertion");
    safe_free(ids);
    CHECK(compact_back_demod_remove(intersected, later->id),
          "retire later intersected-position clause");
    compact_back_demod_compact_all_stale(intersected);
    ids = compact_back_demod_candidate_ids(
      intersected, rule, ORIENTED, &count);
    CHECK(count == 1,
          "position intersection remains complete after compaction");
    safe_free(ids);
    compact_back_demod_get_stats(intersected, &after_compact);
    CHECK(after_compact.position_features == 2 &&
          after_compact.position_physical_features == 2 &&
          after_compact.position_intersection_queries ==
            before_compact.position_intersection_queries + 2 &&
          after_compact.position_bitmap_bytes > 0,
          "compaction rebuilds both intersection membership bitmaps");
    compact_back_demod_free(intersected);
    delete_clause(rule);
    delete_clause(later);
    for (j = 0; j < INTERSECTION_FAMILY; j++)
      delete_clause(clauses[j]);
    compact_back_demod_set_position_options(
      4096, 4, 8, 65536, 20, TRUE);
    compact_back_demod_set_strategy(COMPACT_BACK_DEMOD_MASK8);
  }

  {
    enum { DENSE_INTERSECTION_FAMILY = 512 };
    Compact_back_demod_index dense_intersection;
    struct compact_back_demod_stats dense_stats;
    Topform clauses[DENSE_INTERSECTION_FAMILY], rule;
    char text[160];
    int j, round;
    compact_back_demod_set_position_options(1, 4, 1, 65536, 20, TRUE);
    compact_back_demod_set_strategy(COMPACT_BACK_DEMOD_POSITION);
    dense_intersection = compact_back_demod_init();
    for (j = 0; j < DENSE_INTERSECTION_FAMILY; j++) {
      (void) snprintf(text, sizeof(text),
                      "w(d(a,z(h(j(da%d)),h(j(db%d))))).",
                      j % 4, (j / 4) % 4);
      clauses[j] = indexed_clause(text);
      CHECK(compact_back_demod_add(dense_intersection, clauses[j]),
            "add dense position-intersection family");
    }
    rule = indexed_clause("d(a,z(h(j(da0)),h(j(db0)))) = a.");
    for (round = 0; round < 8; round++) {
      ids = compact_back_demod_candidate_ids(
        dense_intersection, rule, ORIENTED, &count);
      CHECK(count == DENSE_INTERSECTION_FAMILY / 16,
            "dense bitmap intersection preserves exact candidates");
      safe_free(ids);
    }
    compact_back_demod_get_stats(dense_intersection, &dense_stats);
    CHECK(dense_stats.position_admissions == 2 &&
          dense_stats.position_features == 2 &&
          dense_stats.position_dense_intersection_queries == 2 &&
          dense_stats.position_intersection_scans == 0 &&
          dense_stats.position_bitmap_word_checks == 34 &&
          dense_stats.position_intersection_records ==
            2 * (DENSE_INTERSECTION_FAMILY / 16),
          "broad features select dense word-wise bitmap intersection");
    compact_back_demod_free(dense_intersection);
    delete_clause(rule);
    for (j = 0; j < DENSE_INTERSECTION_FAMILY; j++)
      delete_clause(clauses[j]);
    compact_back_demod_set_position_options(
      4096, 4, 8, 65536, 20, TRUE);
    compact_back_demod_set_strategy(COMPACT_BACK_DEMOD_MASK8);
  }

  {
    enum { POSITION_ROOT_FAMILY = 64, POSITION_DEMOTION_LIMIT = 4096 };
    Compact_back_demod_index bounded_position;
    struct compact_back_demod_stats before_s, after_s, bounded_stats;
    Topform clauses[POSITION_ROOT_FAMILY * 2 + POSITION_DEMOTION_LIMIT];
    Topform f_rule, s_rule;
    char text[160];
    int added = POSITION_ROOT_FAMILY * 2, j, rounds;
    compact_back_demod_set_position_options(1, 4, 1, 16, 0, TRUE);
    compact_back_demod_set_strategy(COMPACT_BACK_DEMOD_POSITION);
    bounded_position = compact_back_demod_init();
    for (j = 0; j < POSITION_ROOT_FAMILY; j++) {
      (void) snprintf(text, sizeof(text),
                      "w(f(a,g(h(j(fc%d))))).", j);
      clauses[j] = indexed_clause(text);
      CHECK(compact_back_demod_add(bounded_position, clauses[j]),
            "add first position-budget root family");
      (void) snprintf(text, sizeof(text),
                      "w(s(a,g(h(j(sc%d))))).", j);
      clauses[POSITION_ROOT_FAMILY + j] = indexed_clause(text);
      CHECK(compact_back_demod_add(
              bounded_position, clauses[POSITION_ROOT_FAMILY + j]),
            "add second position-budget root family");
    }
    f_rule = indexed_clause("f(a,g(h(j(fc31)))) = a.");
    s_rule = indexed_clause("s(a,g(h(j(sc31)))) = a.");
    for (rounds = 0; rounds < 4; rounds++) {
      ids = compact_back_demod_candidate_ids(
        bounded_position, f_rule, ORIENTED, &count);
      safe_free(ids);
      ids = compact_back_demod_candidate_ids(
        bounded_position, s_rule, ORIENTED, &count);
      safe_free(ids);
    }
    compact_back_demod_get_stats(bounded_position, &bounded_stats);
    CHECK(bounded_stats.position_admissions == 2 &&
          bounded_stats.position_features == 2,
          "admit two independent position features under hard budget");
    for (j = 0; j < POSITION_DEMOTION_LIMIT; j++) {
      clauses[added] = indexed_clause("w(f(a,g(h(j(fc31))))).");
      CHECK(compact_back_demod_add(bounded_position, clauses[added]),
            "grow one position feature toward its hard budget");
      added++;
      compact_back_demod_get_stats(bounded_position, &bounded_stats);
      if (bounded_stats.position_demotions != 0)
        break;
    }
    CHECK(bounded_stats.position_demotions == 1 &&
          bounded_stats.position_features == 1 &&
          bounded_stats.position_physical_features == 2 &&
          bounded_stats.position_budget_exhaustions == 1 &&
          bounded_stats.position_complete,
          "position growth demotes only its affected feature");
    compact_back_demod_get_stats(bounded_position, &before_s);
    ids = compact_back_demod_candidate_ids(
      bounded_position, s_rule, ORIENTED, &count);
    CHECK(count == 1 && ids[0] ==
          clauses[POSITION_ROOT_FAMILY + 31]->id,
          "unrelated position feature remains complete after demotion");
    safe_free(ids);
    compact_back_demod_get_stats(bounded_position, &after_s);
    CHECK(after_s.position_queries == before_s.position_queries + 1,
          "unrelated feature still uses position retrieval");
    ids = compact_back_demod_candidate_ids(
      bounded_position, f_rule, ORIENTED, &count);
    CHECK(count == (size_t) (added - POSITION_ROOT_FAMILY * 2 + 1),
          "demoted position feature falls back with every answer");
    safe_free(ids);
    CHECK(compact_back_demod_remove(
            bounded_position, clauses[added - 1]->id),
          "retire one position record before demotion compaction");
    compact_back_demod_compact_all_stale(bounded_position);
    compact_back_demod_get_stats(bounded_position, &bounded_stats);
    CHECK(bounded_stats.position_features == 1 &&
          bounded_stats.position_physical_features == 1 &&
          bounded_stats.position_complete,
          "forced compaction reclaims the demoted feature metadata");
    compact_back_demod_get_stats(bounded_position, &before_s);
    ids = compact_back_demod_candidate_ids(
      bounded_position, s_rule, ORIENTED, &count);
    safe_free(ids);
    compact_back_demod_get_stats(bounded_position, &after_s);
    CHECK(after_s.position_queries == before_s.position_queries + 1,
          "surviving feature remains indexed after demotion compaction");
    compact_back_demod_free(bounded_position);
    delete_clause(f_rule);
    delete_clause(s_rule);
    for (j = 0; j < added; j++)
      delete_clause(clauses[j]);
    compact_back_demod_set_position_options(
      4096, 4, 8, 65536, 20, TRUE);
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
