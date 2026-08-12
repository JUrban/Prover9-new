#include "../provers.src/compact_back_demod.h"

#include <stdint.h>
#include <sys/stat.h>
#include <unistd.h>

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

static void check_high_base_strategy(Compact_back_demod_strategy strategy)
{
  Compact_term_pool pool = compact_term_pool_init();
  Compact_term_rebase_map map = compact_term_rebase_map_init();
  Compact_back_demod_index index;
  Topform first = indexed_clause("hp(f(a,g(c)),m(a,a)).");
  Topform second = indexed_clause("hp(f(b,g(d)),m(a,b)).");
  Topform exact = indexed_clause("f(a,g(c)) = a.");
  Topform repeated = indexed_clause("m(x,x) = x.");
  unsigned long long *ids;
  size_t count;
  int pass;

  compact_term_pool_set_logical_base(
    pool, (unsigned long long) UINT32_MAX + 613ULL);
  compact_back_demod_set_strategy(strategy);
  index = compact_back_demod_init_with_pool(pool);
  CHECK(compact_back_demod_add(index, first) &&
        compact_back_demod_add(index, second),
        "all back-demod strategies index terms above the 32-bit boundary");

  for (pass = 0; pass < 2; pass++) {
    ids = compact_back_demod_candidate_ids(
      index, exact, ORIENTED, &count);
    CHECK(count == 1 && ids != NULL && ids[0] == first->id,
          "high-base rigid back-demod retrieval is exact");
    safe_free(ids);
    ids = compact_back_demod_candidate_ids(
      index, repeated, ORIENTED, &count);
    CHECK(count == 1 && ids != NULL && ids[0] == first->id,
          "high-base matching preserves repeated-variable equality");
    safe_free(ids);
    if (pass == 0) {
      compact_back_demod_retain_live_clauses(index, map);
      compact_term_pool_compact_retained(pool, map);
      compact_back_demod_rebase_term_pool(index, pool, map);
    }
  }

  compact_back_demod_free(index);
  compact_term_rebase_map_free(map);
  compact_term_pool_free(pool);
  delete_clause(first);
  delete_clause(second);
  delete_clause(exact);
  delete_clause(repeated);
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
  {
    struct compact_query_timer timer = {0};
    unsigned i;
    for (i = 0; i < 4096; i++) {
      compact_query_timer_start(&timer);
      compact_query_timer_stop(&timer);
    }
    CHECK(timer.eligible == 4096 && timer.samples > 0 &&
          timer.samples < timer.eligible / 4 && !timer.active &&
          timer.estimated_seconds >= 0.0,
          "compact query timing samples instead of timing every lookup");
  }
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
  CHECK(stats.lookup_timing_eligible == stats.queries &&
        stats.lookup_timing_samples <= stats.lookup_timing_eligible &&
        stats.timing_sample_rate == COMPACT_TIMING_SAMPLE_RATE,
        "back-demod timing reports bounded deterministic sampling");
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
    enum { ROOT_SIBLINGS = 24, ROOT_EXTENSIONS = 64 };
    Compact_back_demod_index roots;
    struct compact_back_demod_stats before_insert, before_root, after_first,
      after_second;
    Topform clauses[ROOT_SIBLINGS], rule;
    Topform extensions[ROOT_EXTENSIONS];
    char text[96];
    int j;
    compact_back_demod_set_strategy(COMPACT_BACK_DEMOD_CODE_TREE);
    roots = compact_back_demod_init();
    for (j = 0; j < ROOT_SIBLINGS; j++) {
      (void) snprintf(text, sizeof(text),
                      "w(root_sibling_%d(a)).", j);
      clauses[j] = indexed_clause(text);
      CHECK(compact_back_demod_add(roots, clauses[j]),
            "add top-level tree sibling family");
    }
    (void) snprintf(text, sizeof(text),
                    "root_sibling_%d(a) = a.", ROOT_SIBLINGS - 1);
    rule = indexed_clause(text);
    compact_back_demod_get_stats(roots, &before_root);
    ids = compact_back_demod_candidate_ids(roots, rule, ORIENTED, &count);
    CHECK(count == 1 && ids[0] == clauses[ROOT_SIBLINGS - 1]->id,
          "root sibling scan preserves the exact tree answer");
    safe_free(ids);
    compact_back_demod_get_stats(roots, &after_first);
    ids = compact_back_demod_candidate_ids(roots, rule, ORIENTED, &count);
    CHECK(count == 1 && ids[0] == clauses[ROOT_SIBLINGS - 1]->id,
          "cached root dispatch preserves the exact tree answer");
    safe_free(ids);
    compact_back_demod_get_stats(roots, &after_second);
    CHECK(after_first.tree_sibling_checks -
            before_root.tree_sibling_checks >= ROOT_SIBLINGS &&
          after_second.tree_sibling_checks ==
            after_first.tree_sibling_checks &&
          after_second.tree_child_cache_hits >
            after_first.tree_child_cache_hits,
          "route work counts cold root siblings and cached dispatch removes them");
    before_insert = after_second;
    for (j = 0; j < ROOT_EXTENSIONS; j++) {
      (void) snprintf(text, sizeof(text),
                      "w(root_sibling_23(root_branch_%d)).", j);
      extensions[j] = indexed_clause(text);
      CHECK(compact_back_demod_add(roots, extensions[j]),
            "extend one mature tree root through cached insertion dispatch");
    }
    compact_back_demod_get_stats(roots, &after_second);
    CHECK(after_second.tree_insert_cache_hits -
            before_insert.tree_insert_cache_hits >= ROOT_EXTENSIONS - 1 &&
          after_second.tree_insert_cache_lookups >
            before_insert.tree_insert_cache_lookups,
          "mature tree insertion reuses query-trained child dispatch");
    compact_back_demod_free(roots);
    delete_clause(rule);
    for (j = 0; j < ROOT_SIBLINGS; j++)
      delete_clause(clauses[j]);
    for (j = 0; j < ROOT_EXTENSIONS; j++)
      delete_clause(extensions[j]);
    compact_back_demod_set_strategy(COMPACT_BACK_DEMOD_MASK8);
  }

  {
    enum { REALLOC_FAMILY = 1024 };
    Compact_back_demod_index reallocating;
    struct compact_back_demod_stats realloc_stats;
    Topform clauses[REALLOC_FAMILY], rule;
    int j;
    compact_back_demod_set_tree_budget_kb(65536);
    compact_back_demod_set_tree_admit_work(1);
    compact_back_demod_set_tree_build_factor(1);
    compact_back_demod_set_strategy(COMPACT_BACK_DEMOD_HOT_ROOT_TREE);
    reallocating = compact_back_demod_init();
    for (j = 0; j < REALLOC_FAMILY; j++) {
      clauses[j] = indexed_clause("w(realloc_root(a)).");
      CHECK(compact_back_demod_add(reallocating, clauses[j]),
            "add duplicate root-backfill posting");
    }
    rule = indexed_clause("realloc_root(a) = a.");
    ids = compact_back_demod_candidate_ids(
      reallocating, rule, ORIENTED, &count);
    CHECK(count == REALLOC_FAMILY &&
          ids[0] == clauses[REALLOC_FAMILY - 1]->id &&
          ids[REALLOC_FAMILY - 1] == clauses[0]->id,
          "root backfill survives shared posting-array reallocation");
    safe_free(ids);
    compact_back_demod_get_stats(reallocating, &realloc_stats);
    CHECK(realloc_stats.tree_root_admissions == 1 &&
          realloc_stats.tree_posting_groups == REALLOC_FAMILY,
          "reallocated root backfill retains every tree posting");
    compact_back_demod_free(reallocating);
    delete_clause(rule);
    for (j = 0; j < REALLOC_FAMILY; j++)
      delete_clause(clauses[j]);
    compact_back_demod_set_tree_admit_work(4096);
    compact_back_demod_set_tree_build_factor(8);
    compact_back_demod_set_strategy(COMPACT_BACK_DEMOD_MASK8);
  }

  {
    enum { PROBE_FAMILY = 128, PROBE_QUERIES = 40 };
    Compact_back_demod_index bounded_probe;
    struct compact_back_demod_stats probe_stats;
    Topform clauses[PROBE_FAMILY], rule;
    char text[96];
    int j, round;
    compact_back_demod_set_tree_budget_kb(65536);
    compact_back_demod_set_tree_admit_work(1);
    compact_back_demod_set_tree_build_factor(1);
    compact_back_demod_set_position_options(
      4096, 4, 64, 16384, 20, FALSE);
    compact_back_demod_set_strategy(COMPACT_BACK_DEMOD_ADAPTIVE);
    bounded_probe = compact_back_demod_init();
    for (j = 0; j < PROBE_FAMILY; j++) {
      (void) snprintf(text, sizeof(text), "w(f(probe_%d,a)).", j);
      clauses[j] = indexed_clause(text);
      CHECK(compact_back_demod_add(bounded_probe, clauses[j]),
            "add variable-prefix calibration family");
    }
    rule = indexed_clause("f(x,a) = a.");
    for (round = 0; round < PROBE_QUERIES; round++) {
      ids = compact_back_demod_candidate_ids(
        bounded_probe, rule, ORIENTED, &count);
      CHECK(count == PROBE_FAMILY && ids[0] == clauses[PROBE_FAMILY - 1]->id &&
            ids[PROBE_FAMILY - 1] == clauses[0]->id,
            "bounded tree calibration falls back to the complete mask order");
      safe_free(ids);
    }
    compact_back_demod_get_stats(bounded_probe, &probe_stats);
    CHECK(probe_stats.tree_root_admissions == 1 &&
          probe_stats.route_tree_probes == 1 &&
          probe_stats.route_tree_probe_aborts == 1 &&
          probe_stats.route_tree_probe_budget > 0 &&
          probe_stats.route_tree_probe_discarded_candidates > 0 &&
          probe_stats.route_tree_candidates == 0,
          "variable-prefix calibration is stopped at its mask-derived budget");
    compact_back_demod_free(bounded_probe);
    delete_clause(rule);
    for (j = 0; j < PROBE_FAMILY; j++)
      delete_clause(clauses[j]);
    compact_back_demod_set_tree_admit_work(4096);
    compact_back_demod_set_tree_build_factor(8);
    compact_back_demod_set_position_options(
      4096, 4, 64, 16384, 20, TRUE);
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
    enum { ROUTE_FAMILY = 128, ROUTE_TARGET = 73 };
    Compact_back_demod_index adaptive;
    Compact_back_demod_index restored;
    struct compact_back_demod_stats cold_stats, selective_stats, broad_stats,
      renamed_stats, growth_stats, growth_probe_stats,
      stable_growth_stats, compacted_stats;
    Topform clauses[ROUTE_FAMILY], growth[ROUTE_FAMILY];
    Topform selective, broad, renamed;
    char text[128];
    unsigned long long tree_before_broad;
    unsigned long long *current_ids, *restored_ids;
    size_t current_count, restored_count;
    char state_dir[128], state_path[180];
    int j;
    compact_back_demod_set_tree_budget_kb(65536);
    compact_back_demod_set_tree_admit_work(1);
    compact_back_demod_set_tree_build_factor(1);
    compact_back_demod_set_position_options(4096, 4, 8, 65536, 20, FALSE);
    compact_back_demod_set_strategy(COMPACT_BACK_DEMOD_ADAPTIVE);
    adaptive = compact_back_demod_init();
    for (j = 0; j < ROUTE_FAMILY; j++) {
      (void) snprintf(text, sizeof(text),
                      "w(route(a,g(h(j(c%d))))).", j);
      clauses[j] = indexed_clause(text);
      CHECK(compact_back_demod_add(adaptive, clauses[j]),
            "add mixed-shape adaptive-route family");
    }
    (void) snprintf(text, sizeof(text), "route(a,g(h(j(c%d)))) = a.",
                    ROUTE_TARGET);
    selective = indexed_clause(text);
    for (j = 0; j < 16; j++) {
      ids = compact_back_demod_candidate_ids(
        adaptive, selective, ORIENTED, &count);
      CHECK(count == 1 && ids[0] == clauses[ROUTE_TARGET]->id,
            "selective adaptive route preserves the sole answer");
      safe_free(ids);
    }
    compact_back_demod_get_stats(adaptive, &cold_stats);
    CHECK(cold_stats.route_profile_occupied == 0 &&
          cold_stats.route_tree_choices == 0 &&
          cold_stats.route_tree_probes == 0 &&
          cold_stats.route_cold_fallbacks >= 15,
          "cold route class allocates no profile and pays no tree probe");
    for (; j < 35; j++) {
      ids = compact_back_demod_candidate_ids(
        adaptive, selective, ORIENTED, &count);
      CHECK(count == 1 && ids[0] == clauses[ROUTE_TARGET]->id,
            "hot selective adaptive route preserves the sole answer");
      safe_free(ids);
    }
    compact_back_demod_get_stats(adaptive, &selective_stats);
    CHECK(selective_stats.tree_root_admissions == 1 &&
          selective_stats.route_tree_choices >= 1 &&
          selective_stats.route_tree_observed_cost <
            selective_stats.route_mask_observed_cost &&
          selective_stats.route_profile_capacity == 4096 &&
          selective_stats.route_profile_bytes <= 512 * 1024 &&
          selective_stats.route_frequency_capacity == 65536 &&
          selective_stats.route_frequency_bytes == 256 * 1024,
          "selective shape promotes a bounded tree route");

    broad = indexed_clause("route(x,y) = a.");
    tree_before_broad = selective_stats.route_tree_choices;
    for (j = 0; j < 40; j++) {
      ids = compact_back_demod_candidate_ids(
        adaptive, broad, ORIENTED, &count);
      CHECK(count == ROUTE_FAMILY && ids[0] == clauses[ROUTE_FAMILY - 1]->id &&
            ids[ROUTE_FAMILY - 1] == clauses[0]->id,
            "broad adaptive route preserves every ordered answer");
      safe_free(ids);
    }
    compact_back_demod_get_stats(adaptive, &broad_stats);
    CHECK(broad_stats.route_tree_choices == tree_before_broad + 1 &&
          broad_stats.route_mask_choices >=
            selective_stats.route_mask_choices + 39,
          "broad shape under the same root rejects tree promotion");

    renamed = indexed_clause("route(y,x) = a.");
    ids = compact_back_demod_candidate_ids(
      adaptive, renamed, ORIENTED, &count);
    CHECK(count == ROUTE_FAMILY,
          "alpha-renamed broad route preserves every answer");
    safe_free(ids);
    compact_back_demod_get_stats(adaptive, &renamed_stats);
    CHECK(renamed_stats.route_profile_occupied ==
            broad_stats.route_profile_occupied &&
          renamed_stats.route_tree_choices == broad_stats.route_tree_choices &&
          renamed_stats.route_mask_choices == broad_stats.route_mask_choices + 1,
          "alpha renaming reuses the semantic shape calibration");

    for (j = 0; j < ROUTE_FAMILY; j++) {
      (void) snprintf(text, sizeof(text),
                      "w(route(a,g(h(k(c%d,c0))))).", j);
      growth[j] = indexed_clause(text);
      CHECK(compact_back_demod_add(adaptive, growth[j]),
            "double adaptive-route physical population");
    }
    for (j = 0; j < 33; j++) {
      ids = compact_back_demod_candidate_ids(
        adaptive, broad, ORIENTED, &count);
      CHECK(count == ROUTE_FAMILY * 2,
            "population-crossover training preserves every answer");
      safe_free(ids);
    }
    compact_back_demod_get_stats(adaptive, &growth_stats);
    CHECK(growth_stats.route_profile_occupied ==
            renamed_stats.route_profile_occupied + 1 &&
          growth_stats.route_mask_choices ==
            renamed_stats.route_mask_choices + 33 &&
          growth_stats.route_tree_choices ==
            renamed_stats.route_tree_choices + 1 &&
          growth_stats.route_tree_probes ==
            renamed_stats.route_tree_probes + 1 &&
          growth_stats.route_tree_probe_aborts ==
            renamed_stats.route_tree_probe_aborts + 1,
          "doubling population bounds one tree sample and falls back once");
    growth_probe_stats = growth_stats;
    ids = compact_back_demod_candidate_ids(
      adaptive, broad, ORIENTED, &count);
    CHECK(count == ROUTE_FAMILY * 2,
          "stable post-crossover route preserves every answer");
    safe_free(ids);
    compact_back_demod_get_stats(adaptive, &stable_growth_stats);
    CHECK(stable_growth_stats.route_tree_choices ==
            growth_probe_stats.route_tree_choices &&
          stable_growth_stats.route_mask_choices ==
            growth_probe_stats.route_mask_choices + 1,
          "stable population pays no repeated exploration tax");

    for (j = 0; j < ROUTE_FAMILY / 4; j++)
      CHECK(compact_back_demod_remove(adaptive, clauses[j]->id),
            "retire adaptive-route record before compaction");
    compact_back_demod_compact_all_stale(adaptive);
    ids = compact_back_demod_candidate_ids(
      adaptive, broad, ORIENTED, &count);
    CHECK(count == ROUTE_FAMILY * 2 - ROUTE_FAMILY / 4,
          "compacted adaptive route preserves every live broad answer");
    safe_free(ids);
    compact_back_demod_get_stats(adaptive, &compacted_stats);
    CHECK(compacted_stats.route_profile_occupied ==
            stable_growth_stats.route_profile_occupied &&
          compacted_stats.route_profile_bytes ==
            stable_growth_stats.route_profile_bytes &&
          compacted_stats.route_mask_choices ==
            stable_growth_stats.route_mask_choices + 1 &&
          compacted_stats.route_tree_choices ==
            stable_growth_stats.route_tree_choices,
          "compaction preserves bounded route calibration without retraining");

    (void) snprintf(state_dir, sizeof(state_dir),
                    "/tmp/p9-compact-back-state-%ld", (long) getpid());
    (void) snprintf(state_path, sizeof(state_path),
                    "%s/compact_back_adaptive.txt", state_dir);
    CHECK(mkdir(state_dir, 0700) == 0,
          "create adaptive-state test directory");
    CHECK(compact_back_demod_write_adaptive_state(adaptive, state_dir),
          "write bounded adaptive checkpoint sidecar");
    restored = compact_back_demod_init();
    for (j = ROUTE_FAMILY / 4; j < ROUTE_FAMILY; j++)
      CHECK(compact_back_demod_add(restored, clauses[j]),
            "rebuild live checkpoint route clause");
    for (j = 0; j < ROUTE_FAMILY; j++)
      CHECK(compact_back_demod_add(restored, growth[j]),
            "rebuild live checkpoint growth clause");
    CHECK(compact_back_demod_read_adaptive_state(restored, state_dir),
          "restore bounded adaptive checkpoint sidecar");
    current_ids = compact_back_demod_candidate_ids(
      adaptive, broad, ORIENTED, &current_count);
    restored_ids = compact_back_demod_candidate_ids(
      restored, broad, ORIENTED, &restored_count);
    CHECK(current_count == restored_count &&
          memcmp(current_ids, restored_ids,
                 current_count * sizeof(*current_ids)) == 0,
          "checkpoint restore preserves adaptive candidates and order");
    safe_free(current_ids);
    safe_free(restored_ids);
    compact_back_demod_get_stats(restored, &growth_stats);
    CHECK(growth_stats.route_profile_occupied ==
            compacted_stats.route_profile_occupied &&
          growth_stats.route_profile_bytes ==
            compacted_stats.route_profile_bytes &&
          growth_stats.route_mask_choices == 1 &&
          growth_stats.route_tree_choices == 0 &&
          growth_stats.tree_nodes > 0,
          "checkpoint restore resumes the calibrated route without retraining");
    compact_back_demod_free(restored);
    CHECK(remove(state_path) == 0 && rmdir(state_dir) == 0,
          "remove adaptive-state test artifacts");

    compact_back_demod_free(adaptive);
    delete_clause(selective);
    delete_clause(broad);
    delete_clause(renamed);
    for (j = 0; j < ROUTE_FAMILY; j++)
      delete_clause(clauses[j]);
    for (j = 0; j < ROUTE_FAMILY; j++)
      delete_clause(growth[j]);
    compact_back_demod_set_tree_admit_work(4096);
    compact_back_demod_set_tree_build_factor(8);
    compact_back_demod_set_position_options(
      4096, 4, 8, 65536, 20, TRUE);
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
    enum { COLD_ROUTE_CLASSES = 10000, HOT_ROUTE_CLASSES = 5000 };
    Compact_back_demod_index churn;
    struct compact_back_demod_stats churn_stats;
    Topform *clauses = safe_malloc(COLD_ROUTE_CLASSES * sizeof(*clauses));
    Topform *rules = safe_malloc(COLD_ROUTE_CLASSES * sizeof(*rules));
    char text[96];
    int j, round;
    compact_back_demod_set_tree_budget_kb(65536);
    compact_back_demod_set_tree_admit_work(1);
    compact_back_demod_set_tree_build_factor(1);
    compact_back_demod_set_position_options(4096, 4, 64, 16384, 20, FALSE);
    compact_back_demod_set_strategy(COMPACT_BACK_DEMOD_ADAPTIVE);
    churn = compact_back_demod_init();
    for (j = 0; j < COLD_ROUTE_CLASSES; j++) {
      (void) snprintf(text, sizeof(text), "w(cold_route_%d(a)).", j);
      clauses[j] = indexed_clause(text);
      CHECK(compact_back_demod_add(churn, clauses[j]),
            "add adversarial route-class subject");
      (void) snprintf(text, sizeof(text), "cold_route_%d(a) = a.", j);
      rules[j] = indexed_clause(text);
    }
    for (j = 0; j < COLD_ROUTE_CLASSES; j++) {
      ids = compact_back_demod_candidate_ids(
        churn, rules[j], ORIENTED, &count);
      CHECK(count == 1 && ids[0] == clauses[j]->id,
            "cold route first lookup preserves its sole answer");
      safe_free(ids);
      ids = compact_back_demod_candidate_ids(
        churn, rules[j], ORIENTED, &count);
      CHECK(count == 1 && ids[0] == clauses[j]->id,
            "cold route frequency lookup preserves its sole answer");
      safe_free(ids);
    }
    compact_back_demod_get_stats(churn, &churn_stats);
    CHECK(churn_stats.route_profile_occupied == 0 &&
          churn_stats.route_tree_probes == 0 &&
          churn_stats.route_cold_fallbacks == COLD_ROUTE_CLASSES,
          "ten thousand singleton classes allocate no profiles or probes");
    for (round = 0; round < 32; round++)
      for (j = 0; j < HOT_ROUTE_CLASSES; j++) {
        ids = compact_back_demod_candidate_ids(
          churn, rules[j], ORIENTED, &count);
        CHECK(count == 1 && ids[0] == clauses[j]->id,
              "hot route churn preserves its sole answer");
        safe_free(ids);
      }
    compact_back_demod_get_stats(churn, &churn_stats);
    CHECK(churn_stats.route_profile_occupied > 0 &&
          churn_stats.route_profile_occupied <=
            churn_stats.route_profile_capacity &&
          churn_stats.route_admission_rejections > 0 &&
          churn_stats.route_profile_replacements <= HOT_ROUTE_CLASSES &&
          churn_stats.route_frequency_bytes == 256 * 1024,
          "hot classes use bounded deterministic admission under table churn");
    CHECK(churn_stats.route_tree_probes <= HOT_ROUTE_CLASSES &&
          churn_stats.route_tree_probes * 32 <=
            churn_stats.route_mask_choices +
              churn_stats.route_tree_choices,
          "tree calibration probes remain below one per 32 routed lookups");
    compact_back_demod_free(churn);
    for (j = 0; j < COLD_ROUTE_CLASSES; j++) {
      delete_clause(clauses[j]);
      delete_clause(rules[j]);
    }
    safe_free(clauses);
    safe_free(rules);
    compact_back_demod_set_tree_admit_work(4096);
    compact_back_demod_set_tree_build_factor(8);
    compact_back_demod_set_position_options(
      4096, 4, 64, 16384, 20, TRUE);
    compact_back_demod_set_strategy(COMPACT_BACK_DEMOD_MASK8);
  }

  {
    enum {
      AGED_COLD_ROUTE_CLASSES = 128,
      AGED_COLD_HITS_PER_WINDOW = 20,
      ROUTE_FREQUENCY_DECAY_WINDOW = 65536 * 4,
      ROUTE_FREQUENCY_WINDOWS = 2
    };
    Compact_back_demod_index aged_churn;
    struct compact_back_demod_stats aged_stats;
    Topform clauses[AGED_COLD_ROUTE_CLASSES + 1];
    Topform rules[AGED_COLD_ROUTE_CLASSES + 1];
    char text[96];
    int cycle, j, round;
    compact_back_demod_set_tree_budget_kb(65536);
    compact_back_demod_set_tree_admit_work(1);
    compact_back_demod_set_tree_build_factor(1);
    compact_back_demod_set_position_options(4096, 4, 64, 16384, 20, FALSE);
    compact_back_demod_set_strategy(COMPACT_BACK_DEMOD_ADAPTIVE);
    aged_churn = compact_back_demod_init();
    for (j = 0; j <= AGED_COLD_ROUTE_CLASSES; j++) {
      (void) snprintf(text, sizeof(text), "w(aged_route_%d(a)).", j);
      clauses[j] = indexed_clause(text);
      CHECK(compact_back_demod_add(aged_churn, clauses[j]),
            "add multi-window route-aging subject");
      (void) snprintf(text, sizeof(text), "aged_route_%d(a) = a.", j);
      rules[j] = indexed_clause(text);
      ids = compact_back_demod_candidate_ids(
        aged_churn, rules[j], ORIENTED, &count);
      CHECK(count == 1 && ids[0] == clauses[j]->id,
            "initialize complete tree before route-aging traffic");
      safe_free(ids);
    }
    for (cycle = 0; cycle < ROUTE_FREQUENCY_WINDOWS; cycle++) {
      for (round = 0; round < AGED_COLD_HITS_PER_WINDOW; round++)
        for (j = 0; j < AGED_COLD_ROUTE_CLASSES; j++) {
          ids = compact_back_demod_candidate_ids(
            aged_churn, rules[j], ORIENTED, &count);
          CHECK(count == 1 && ids[0] == clauses[j]->id,
                "multi-window cold route preserves its sole answer");
          safe_free(ids);
        }
      /* The final class is intentionally hot clock traffic.  Advancing a
         complete decay window after each cold burst proves that 40 lifetime
         hits do not masquerade as 32 recent hits across a long run. */
      for (round = 0; round < ROUTE_FREQUENCY_DECAY_WINDOW; round++) {
        ids = compact_back_demod_candidate_ids(
          aged_churn, rules[AGED_COLD_ROUTE_CLASSES], ORIENTED, &count);
        CHECK(count == 1 &&
              ids[0] == clauses[AGED_COLD_ROUTE_CLASSES]->id,
              "route-aging clock lookup preserves its sole answer");
        safe_free(ids);
      }
    }
    compact_back_demod_get_stats(aged_churn, &aged_stats);
    CHECK(aged_stats.route_frequency_decays >= ROUTE_FREQUENCY_WINDOWS &&
          aged_stats.route_profile_occupied == 1 &&
          aged_stats.route_profile_replacements == 0 &&
          aged_stats.route_tree_probes == 1,
          "decay retains one genuinely hot class without cumulative cold churn");
    compact_back_demod_free(aged_churn);
    for (j = 0; j <= AGED_COLD_ROUTE_CLASSES; j++) {
      delete_clause(clauses[j]);
      delete_clause(rules[j]);
    }
    compact_back_demod_set_tree_admit_work(4096);
    compact_back_demod_set_tree_build_factor(8);
    compact_back_demod_set_position_options(
      4096, 4, 64, 16384, 20, TRUE);
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
          bounded_stats.position_admission_frozen &&
          bounded_stats.position_admission_freezes == 1 &&
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
    enum {
      APPEND_FEATURES = 32,
      APPEND_FAMILY = 64,
      APPEND_UNMATCHED = 128
    };
    Compact_back_demod_index appended;
    struct compact_back_demod_stats trained, extended;
    Topform family[APPEND_FAMILY];
    Topform rules[APPEND_FEATURES];
    Topform unmatched[APPEND_UNMATCHED];
    Topform matched;
    char text[128];
    int j, round;
    compact_back_demod_set_position_options(1, 4, 1, 65536, 20, TRUE);
    compact_back_demod_set_strategy(COMPACT_BACK_DEMOD_POSITION);
    appended = compact_back_demod_init();
    for (j = 0; j < APPEND_FAMILY; j++) {
      (void) snprintf(text, sizeof(text),
                      "w(append_fan(a,g(h(j(append_key_%d))))).", j);
      family[j] = indexed_clause(text);
      CHECK(compact_back_demod_add(appended, family[j]),
            "add multi-feature incremental-position family");
    }
    for (j = 0; j < APPEND_FEATURES; j++) {
      (void) snprintf(text, sizeof(text),
                      "append_fan(x,g(h(j(append_key_%d)))) = x.", j);
      rules[j] = indexed_clause(text);
      for (round = 0; round < 2; round++) {
        ids = compact_back_demod_candidate_ids(
          appended, rules[j], ORIENTED, &count);
        CHECK(count == 1 && ids[0] == family[j]->id,
              "training each position feature preserves its answer");
        safe_free(ids);
      }
    }
    compact_back_demod_get_stats(appended, &trained);
    CHECK(trained.position_admissions == APPEND_FEATURES &&
          trained.position_features == APPEND_FEATURES,
          "train thirty-two independent exact-position features");
    for (j = 0; j < APPEND_UNMATCHED; j++) {
      (void) snprintf(text, sizeof(text),
                      "w(append_fan(a,g(h(j(append_fresh_%d))))).", j);
      unmatched[j] = indexed_clause(text);
      CHECK(compact_back_demod_add(appended, unmatched[j]),
            "append record unmatched by thirty-two active features");
    }
    compact_back_demod_get_stats(appended, &extended);
    CHECK(extended.position_append_records -
            trained.position_append_records == APPEND_UNMATCHED &&
          extended.position_append_root_scans -
            trained.position_append_root_scans == APPEND_UNMATCHED &&
          extended.position_append_token_visits -
            trained.position_append_token_visits ==
              APPEND_UNMATCHED * 6 &&
          extended.position_append_feature_lookups -
            trained.position_append_feature_lookups ==
              APPEND_UNMATCHED * 5 &&
          extended.position_append_matches -
            trained.position_append_matches == 0,
          "incremental position work follows record shape, not feature count");
    matched = indexed_clause(
      "w(append_fan(a,g(h(j(append_key_0))))).");
    CHECK(compact_back_demod_add(appended, matched),
          "append a record matching one of many active features");
    ids = compact_back_demod_candidate_ids(
      appended, rules[0], ORIENTED, &count);
    CHECK(count == 2 && ids[0] == matched->id &&
          ids[1] == family[0]->id,
          "one-pass incremental update preserves exact posting membership");
    safe_free(ids);
    compact_back_demod_get_stats(appended, &extended);
    CHECK(extended.position_append_matches -
            trained.position_append_matches == 1,
          "one-pass incremental update records only its actual match");
    compact_back_demod_free(appended);
    delete_clause(matched);
    for (j = 0; j < APPEND_UNMATCHED; j++)
      delete_clause(unmatched[j]);
    for (j = 0; j < APPEND_FEATURES; j++)
      delete_clause(rules[j]);
    for (j = 0; j < APPEND_FAMILY; j++)
      delete_clause(family[j]);
    compact_back_demod_set_position_options(
      4096, 4, 64, 16384, 20, TRUE);
    compact_back_demod_set_strategy(COMPACT_BACK_DEMOD_MASK8);
  }

  {
    Compact_back_demod_index frozen, restored;
    struct compact_back_demod_stats frozen_stats, restored_stats;
    Topform clause, rule;
    char state_dir[128], state_path[180];
    int round;
    compact_back_demod_set_position_options(1, 1, 1, 8, 0, TRUE);
    compact_back_demod_set_strategy(COMPACT_BACK_DEMOD_ADAPTIVE);
    frozen = compact_back_demod_init();
    clause = indexed_clause("w(checkpoint_freeze(shared)).");
    rule = indexed_clause("checkpoint_freeze(shared) = a.");
    CHECK(compact_back_demod_add(frozen, clause),
          "add adaptive checkpoint-freeze subject");
    for (round = 0; round < 2; round++) {
      ids = compact_back_demod_candidate_ids(
        frozen, rule, ORIENTED, &count);
      CHECK(count == 1 && ids[0] == clause->id,
            "adaptive budget freeze preserves fallback answer");
      safe_free(ids);
    }
    compact_back_demod_get_stats(frozen, &frozen_stats);
    CHECK(frozen_stats.position_admission_frozen &&
          frozen_stats.position_credit_reservations == 1,
          "adaptive position budget freezes after one funded attempt");
    (void) snprintf(state_dir, sizeof(state_dir),
                    "/tmp/p9-position-freeze-%ld", (long) getpid());
    (void) snprintf(state_path, sizeof(state_path),
                    "%s/compact_back_adaptive.txt", state_dir);
    CHECK(mkdir(state_dir, 0700) == 0,
          "create position-freeze checkpoint directory");
    CHECK(compact_back_demod_write_adaptive_state(frozen, state_dir),
          "write frozen position ledger checkpoint");
    restored = compact_back_demod_init();
    CHECK(compact_back_demod_add(restored, clause),
          "rebuild position-freeze checkpoint subject");
    CHECK(compact_back_demod_read_adaptive_state(restored, state_dir),
          "restore frozen position ledger checkpoint");
    compact_back_demod_get_stats(restored, &restored_stats);
    CHECK(restored_stats.position_admission_frozen &&
          restored_stats.position_credit_balance ==
            frozen_stats.position_credit_balance &&
          restored_stats.position_credit_earned ==
            frozen_stats.position_credit_earned &&
          restored_stats.position_credit_spent ==
            frozen_stats.position_credit_spent &&
          restored_stats.position_credit_reservations ==
            frozen_stats.position_credit_reservations,
          "checkpoint preserves position credits, debit, and freeze");
    ids = compact_back_demod_candidate_ids(
      restored, rule, ORIENTED, &count);
    CHECK(count == 1 && ids[0] == clause->id,
          "restored frozen index preserves fallback answer");
    safe_free(ids);
    compact_back_demod_get_stats(restored, &restored_stats);
    CHECK(restored_stats.position_credit_reservations ==
            frozen_stats.position_credit_reservations,
          "restored admission freeze prevents a construction burst");
    compact_back_demod_free(restored);
    compact_back_demod_free(frozen);
    CHECK(remove(state_path) == 0 && rmdir(state_dir) == 0,
          "remove position-freeze checkpoint artifacts");
    delete_clause(clause);
    delete_clause(rule);
    compact_back_demod_set_position_options(
      4096, 4, 64, 16384, 20, TRUE);
    compact_back_demod_set_strategy(COMPACT_BACK_DEMOD_MASK8);
  }

  {
    enum { RETRY_FAMILY = 32 };
    Compact_back_demod_index retry_position;
    struct compact_back_demod_stats rejected, cooled, retried;
    Topform clauses[RETRY_FAMILY * 2], rule;
    int j, round;
    compact_back_demod_set_position_options(1, 4, 1, 65536, 20, TRUE);
    compact_back_demod_set_strategy(COMPACT_BACK_DEMOD_POSITION);
    retry_position = compact_back_demod_init();
    for (j = 0; j < RETRY_FAMILY; j++) {
      clauses[j] = indexed_clause("w(qr(shared)).");
      CHECK(compact_back_demod_add(retry_position, clauses[j]),
            "add nonselective position-cooldown subject");
    }
    rule = indexed_clause("qr(shared) = a.");
    for (round = 0; round < 2; round++) {
      ids = compact_back_demod_candidate_ids(
        retry_position, rule, ORIENTED, &count);
      CHECK(count == RETRY_FAMILY,
            "rejected position feature preserves every fallback answer");
      safe_free(ids);
    }
    compact_back_demod_get_stats(retry_position, &rejected);
    CHECK(rejected.position_rejections == 1 &&
          rejected.position_credit_reservations == 1 &&
          rejected.position_census_records == RETRY_FAMILY &&
          rejected.position_backfill_records == 0,
          "one global reservation buys one rejected feature census");
    for (round = 0; round < 10; round++) {
      ids = compact_back_demod_candidate_ids(
        retry_position, rule, ORIENTED, &count);
      safe_free(ids);
    }
    compact_back_demod_get_stats(retry_position, &cooled);
    CHECK(cooled.position_rejections == rejected.position_rejections &&
          cooled.position_credit_reservations ==
            rejected.position_credit_reservations &&
          cooled.position_census_records == rejected.position_census_records &&
          cooled.position_retry_deferrals > 0,
          "rejected feature cannot rescan before population doubles");
    for (j = RETRY_FAMILY; j < RETRY_FAMILY * 2; j++) {
      clauses[j] = indexed_clause("w(qr(shared)).");
      CHECK(compact_back_demod_add(retry_position, clauses[j]),
            "double population for deterministic position retry");
    }
    for (round = 0; round < 2; round++) {
      ids = compact_back_demod_candidate_ids(
        retry_position, rule, ORIENTED, &count);
      CHECK(count == RETRY_FAMILY * 2,
            "population retry preserves every fallback answer");
      safe_free(ids);
    }
    compact_back_demod_get_stats(retry_position, &retried);
    CHECK(retried.position_rejections == 2 &&
          retried.position_credit_reservations == 2 &&
          retried.position_census_records ==
            RETRY_FAMILY + RETRY_FAMILY * 2,
          "population doubling permits exactly one newly funded retry");
    compact_back_demod_free(retry_position);
    delete_clause(rule);
    for (j = 0; j < RETRY_FAMILY * 2; j++)
      delete_clause(clauses[j]);
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
    for (round = 0; round < 512; round++) {
      ids = compact_back_demod_candidate_ids(
        intersected, rule, ORIENTED, &count);
      CHECK(count == 1,
            "position intersection preserves every exact candidate");
      safe_free(ids);
      compact_back_demod_get_stats(intersected, &before_compact);
      if (before_compact.position_admissions == 2)
        break;
    }
    CHECK(round < 512,
          "two position features admit within the amortized training bound");
    ids = compact_back_demod_candidate_ids(
      intersected, rule, ORIENTED, &count);
    CHECK(count == 1,
          "trained position intersection preserves its exact candidate");
    safe_free(ids);
    compact_back_demod_get_stats(intersected, &before_compact);
    CHECK(before_compact.position_admissions == 2 &&
          before_compact.position_features == 2 &&
          before_compact.position_intersection_queries == 1 &&
          before_compact.position_dense_intersection_queries == 0 &&
          before_compact.position_intersection_scans == 4 &&
          before_compact.position_intersection_bit_checks == 4 &&
          before_compact.position_intersection_records == 1 &&
          before_compact.position_credit_reservations == 2 &&
          before_compact.position_census_records ==
            2 * INTERSECTION_FAMILY &&
          before_compact.position_backfill_records ==
            2 * INTERSECTION_FAMILY &&
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
    for (round = 0; round < 128; round++) {
      ids = compact_back_demod_candidate_ids(
        dense_intersection, rule, ORIENTED, &count);
      CHECK(count == DENSE_INTERSECTION_FAMILY / 16,
            "dense bitmap intersection preserves exact candidates");
      safe_free(ids);
      compact_back_demod_get_stats(dense_intersection, &dense_stats);
      if (dense_stats.position_admissions == 2)
        break;
    }
    CHECK(round < 128,
          "dense position features admit within their amortized bound");
    for (round = 0; round < 2; round++) {
      ids = compact_back_demod_candidate_ids(
        dense_intersection, rule, ORIENTED, &count);
      CHECK(count == DENSE_INTERSECTION_FAMILY / 16,
            "trained dense intersection preserves exact candidates");
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
    enum { POSITION_ROOT_FAMILY = 64, POSITION_DEMOTION_LIMIT = 32768 };
    Compact_back_demod_index bounded_position;
    struct compact_back_demod_stats before_s, after_s, bounded_stats;
    Topform clauses[POSITION_ROOT_FAMILY * 2 + POSITION_DEMOTION_LIMIT + 1];
    Topform f_rule, s_rule;
    char text[160];
    int added = POSITION_ROOT_FAMILY * 2, j, rounds;
    compact_back_demod_set_position_options(1, 4, 1, 352, 0, TRUE);
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
    for (rounds = 0; rounds < 128; rounds++) {
      ids = compact_back_demod_candidate_ids(
        bounded_position, f_rule, ORIENTED, &count);
      safe_free(ids);
      ids = compact_back_demod_candidate_ids(
        bounded_position, s_rule, ORIENTED, &count);
      safe_free(ids);
      compact_back_demod_get_stats(bounded_position, &bounded_stats);
      if (bounded_stats.position_admissions == 2)
        break;
    }
    CHECK(rounds < 128,
          "independent position features admit within amortized bound");
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
          bounded_stats.position_active_roots == 1 &&
          bounded_stats.position_physical_features == 2 &&
          bounded_stats.position_budget_exhaustions == 1 &&
          bounded_stats.position_admission_frozen &&
          bounded_stats.position_admission_freezes == 1 &&
          bounded_stats.position_complete,
          "position growth demotes only its affected feature");
    before_s = bounded_stats;
    clauses[added] = indexed_clause("w(f(a,g(h(j(fc31))))).");
    CHECK(compact_back_demod_add(bounded_position, clauses[added]),
          "append another record under a fully demoted position root");
    added++;
    compact_back_demod_get_stats(bounded_position, &after_s);
    CHECK(after_s.position_append_records ==
            before_s.position_append_records + 1 &&
          after_s.position_append_root_scans ==
            before_s.position_append_root_scans,
          "inactive position roots trigger no incremental subtree traversal");
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
          bounded_stats.position_active_roots == 1 &&
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

  compact_back_demod_set_tree_min_tokens(1);
  compact_back_demod_set_tree_budget_kb(65536);
  compact_back_demod_set_tree_admit_work(1);
  compact_back_demod_set_tree_build_factor(1);
  compact_back_demod_set_position_options(1, 1, 1, 65536, 20, TRUE);
  check_high_base_strategy(COMPACT_BACK_DEMOD_MASK8);
  check_high_base_strategy(COMPACT_BACK_DEMOD_SIGNATURE32);
  check_high_base_strategy(COMPACT_BACK_DEMOD_CODE_TREE);
  check_high_base_strategy(COMPACT_BACK_DEMOD_HYBRID_TREE);
  check_high_base_strategy(COMPACT_BACK_DEMOD_HOT_ROOT_TREE);
  check_high_base_strategy(COMPACT_BACK_DEMOD_POSITION);
  check_high_base_strategy(COMPACT_BACK_DEMOD_ADAPTIVE);
  compact_back_demod_set_tree_min_tokens(8);
  compact_back_demod_set_tree_admit_work(4096);
  compact_back_demod_set_tree_build_factor(8);
  compact_back_demod_set_position_options(4096, 4, 8, 65536, 20, TRUE);
  compact_back_demod_set_strategy(COMPACT_BACK_DEMOD_MASK8);

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
