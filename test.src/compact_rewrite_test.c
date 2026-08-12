#include "../provers.src/compact_rewrite.h"
#include "../provers.src/demodulate.h"

#include <stdio.h>
#include <string.h>

static int Failures;
static Clock Index_clock;

struct overlap_result {
  unsigned count;
  unsigned long long expected;
  BOOL found;
};

#define CHECK(test, message) do {                                      \
  if (!(test)) {                                                       \
    fprintf(stderr, "FAIL: %s (line %d)\n", (message), __LINE__);   \
    Failures++;                                                        \
  }                                                                    \
} while (0)

static void note_overlap(unsigned long long proof_id, void *context)
{
  struct overlap_result *result = context;
  result->count++;
  if (proof_id == result->expected)
    result->found = TRUE;
}

static Topform make_rule(char *text, unsigned long long id, int type,
                         Compact_rewrite_bank bank)
{
  Topform rule = parse_clause_from_string(text);
  CHECK(rule != NULL, "parse rewrite rule");
  rule->id = id;
  if (type == ORIENTED)
    mark_oriented_eq(rule->literals->atom);
  index_demodulator(rule, type, INSERT, Index_clock);
  CHECK(compact_rewrite_add(bank, rule, type), "add compact rewrite rule");
  return rule;
}

static void compare_case(Compact_rewrite_bank bank, char *text)
{
  Topform legacy = parse_clause_from_string(text);
  Topform compact = copy_clause_ija(legacy);
  char *legacy_just = NULL, *compact_just = NULL;
  unsigned legacy_size = 0, compact_size = 0;

  demodulate_clause(legacy, -1, -1, FALSE, FALSE);
  compact_rewrite_clause(bank, compact, -1, -1, FALSE, TRUE);
  if (!clause_ident(legacy->literals, compact->literals)) {
    fprintf(stderr, "rewrite mismatch for %s\nlegacy: ", text);
    fprint_clause(stderr, legacy);
    fprintf(stderr, "compact: ");
    fprint_clause(stderr, compact);
    CHECK(FALSE, "compact normal form equals legacy normal form");
  }
  CHECK(encode_justification(legacy->justification,
                             &legacy_just, &legacy_size),
        "encode legacy justification");
  CHECK(encode_justification(compact->justification,
                             &compact_just, &compact_size),
        "encode compact justification");
  CHECK(legacy_size == compact_size &&
        memcmp(legacy_just, compact_just, legacy_size) == 0,
        "compact rewrite justification equals legacy sequence");
  safe_free(legacy_just);
  safe_free(compact_just);
  delete_clause(legacy);
  delete_clause(compact);
}

int main(void)
{
  Compact_rewrite_bank bank;
  Topform rules[6];
  int types[6];
  int i;
  struct compact_rewrite_stats stats;

  init_standard_ladr();
  Index_clock = clock_init("compact rewrite test index");
  init_demodulator_index(DISCRIM_BIND, ORDINARY_UNIF, 0);
  compact_rewrite_set_deep_child_cache_kb(8192);
  bank = compact_rewrite_init();

  types[0] = ORIENTED;
  rules[0] = make_rule("f(x) = x.", 101, types[0], bank);
  types[1] = ORIENTED;
  rules[1] = make_rule("g(x,x) = h(x).", 102, types[1], bank);
  types[2] = ORIENTED;
  rules[2] = make_rule("g(x,y) = k(x,y).", 103, types[2], bank);
  types[3] = ORIENTED;
  rules[3] = make_rule("k(x,x) = m(x).", 104, types[3], bank);
  types[4] = LEX_DEP_BOTH;
  rules[4] = make_rule("u(x) = v(x).", 105, types[4], bank);
  types[5] = ORIENTED;
  rules[5] = make_rule("z(f(x)) = x.", 106, types[5], bank);

  {
    struct overlap_result overlap;
    memset(&overlap, 0, sizeof(overlap));
    overlap.expected = 106;
    compact_rewrite_visit_overlaps(bank, 101, note_overlap, &overlap);
    CHECK(overlap.count == 1 && overlap.found,
          "reverse occurrence index finds only root-compatible old rule");
  }

  compare_case(bank, "p(f(a)).");
  compare_case(bank, "p(g(f(a),f(a))).");
  compare_case(bank, "p(g(f(a),f(b))).");
  compare_case(bank, "p(k(g(a,a),g(a,a))).");
  compare_case(bank, "p(u(a),v(a)).");
  compare_case(bank, "g(f(a),f(a)) = k(f(b),f(b)).");

  {
    const int depth = 200;
    char *text = safe_malloc((size_t) (3 * depth + 6));
    char *at = text;
    struct compact_rewrite_stats before, after;
    *at++ = 'p';
    *at++ = '(';
    for (i = 0; i < depth; i++) {
      *at++ = 's';
      *at++ = '(';
    }
    *at++ = 'a';
    for (i = 0; i < depth; i++)
      *at++ = ')';
    *at++ = ')';
    *at++ = '.';
    *at = '\0';
    compact_rewrite_get_stats(bank, &before);
    compare_case(bank, text);
    compact_rewrite_get_stats(bank, &after);
    CHECK(after.subject_atoms == before.subject_atoms + 1,
          "deep subject is flattened once per atom");
    CHECK(after.subject_initial_nodes == before.subject_initial_nodes +
          (unsigned long long) depth + 2,
          "one-time subject preparation is linear in atom size");
    CHECK(after.attempts == before.attempts +
          (unsigned long long) depth + 2,
          "deep no-match subject still probes every rigid subterm");
    safe_free(text);
  }

  compact_rewrite_get_stats(bank, &stats);
  CHECK(stats.rules_current == 6, "compact rule count");
  CHECK(stats.attempts > 0 && stats.rewrites > 0,
        "compact rewrite accounting");
  CHECK(stats.node_bytes > 0 && stats.posting_bytes > 0 &&
        stats.child_cache_bytes > 0 && stats.child_cache_capacity > 0 &&
        stats.occurrence_bytes > 0 &&
        stats.rule_bytes > 0 && stats.term_bytes > 0 &&
        stats.hash_bytes > 0 && stats.total_bytes > 0,
        "compact byte attribution");
  CHECK(stats.occurrence_stream_used > 0 &&
        stats.occurrence_stream_bytes >= stats.occurrence_stream_used,
        "delta occurrence block accounting is present");

  {
    unsigned long long identity = compact_rewrite_identity_hash(bank);
    unsigned long long retired = stats.rules_retired;
    CHECK(compact_rewrite_suspend(bank, 103), "suspend compact rule");
    CHECK(!compact_rewrite_contains(bank, 103), "suspended rule is absent");
    index_demodulator(rules[2], types[2], DELETE, Index_clock);
    compare_case(bank, "p(g(a,b)).");
    CHECK(compact_rewrite_add(bank, rules[2], types[2]),
          "reinsert selected compact rule");
    index_demodulator(rules[2], types[2], INSERT, Index_clock);
    compact_rewrite_get_stats(bank, &stats);
    CHECK(stats.rules_retired == retired,
          "selection suspension is not semantic retirement");
    CHECK(compact_rewrite_identity_hash(bank) == identity,
          "selection reinsertion restores compact identity");
    compare_case(bank, "p(g(a,b)).");
  }

  {
    unsigned long long identity = compact_rewrite_identity_hash(bank);
    unsigned long long bloated_bytes;
    for (i = 0; i < 1100; i++) {
      Topform transient = parse_clause_from_string("w(x) = x.");
      transient->id = 1000 + (unsigned long long) i;
      mark_oriented_eq(transient->literals->atom);
      CHECK(compact_rewrite_add(bank, transient, ORIENTED),
            "add transient compact rule");
      CHECK(compact_rewrite_remove(bank, transient->id),
            "retire transient compact rule");
      delete_clause(transient);
    }
    compact_rewrite_get_stats(bank, &stats);
    bloated_bytes = stats.total_bytes;
    CHECK(compact_rewrite_compaction_needed(bank),
          "compact bank detects tombstone pressure");
    compact_rewrite_compact(bank);
    compact_rewrite_get_stats(bank, &stats);
    CHECK(stats.compactions == 1 && stats.rules_physical == 6,
          "compact bank rebuild keeps only live rules");
    CHECK(stats.total_bytes < bloated_bytes && stats.bytes_reclaimed > 0,
          "compact bank rebuild reclaims physical pools");
    CHECK(compact_rewrite_identity_hash(bank) == identity,
          "compact bank rebuild preserves identity");
    compare_case(bank, "p(g(f(a),f(b))).");
  }

  CHECK(compact_rewrite_remove(bank, 102), "remove compact rule");
  CHECK(!compact_rewrite_contains(bank, 102), "removed rule is absent");
  index_demodulator(rules[1], types[1], DELETE, Index_clock);
  compare_case(bank, "p(g(a,a)).");

  {
    Topform wide_rules[9];
    char text[64];
    for (i = 0; i < 9; i++) {
      (void) snprintf(text, sizeof(text), "q%d(x) = x.", i);
      wide_rules[i] = make_rule(text, 500 + (unsigned long long) i,
                                ORIENTED, bank);
    }
    compare_case(bank, "p(q8(a)).");
    compact_rewrite_get_stats(bank, &stats);
    CHECK(stats.child_cache_capacity > 0 && stats.child_cache_bytes > 0,
          "root child cache is allocated by rigid rewrite rules");
    for (i = 0; i < 9; i++) {
      index_demodulator(wide_rules[i], ORIENTED, DELETE, Index_clock);
      delete_clause(wide_rules[i]);
    }
  }

  {
    Topform deep_rules[40];
    struct compact_rewrite_stats first, second;
    char text[96];
    for (i = 0; i < 40; i++) {
      (void) snprintf(text, sizeof(text), "deep(q%d(x)) = x.", i);
      deep_rules[i] = make_rule(
        text, 700 + (unsigned long long) i, ORIENTED, bank);
    }
    compare_case(bank, "p(deep(q39(a))).");
    compact_rewrite_get_stats(bank, &first);
    compare_case(bank, "p(deep(q39(a))).");
    compact_rewrite_get_stats(bank, &second);
    CHECK(first.deep_child_cache_parents > 0 &&
          first.deep_child_cache_capacity > 0 &&
          first.deep_child_cache_bytes <= 8 * 1024 * 1024,
          "wide internal radix parent enables a bounded child cache");
    CHECK(second.deep_child_cache_lookups >
            first.deep_child_cache_lookups &&
          second.deep_child_cache_hits > first.deep_child_cache_hits,
          "repeated internal rigid lookup hits the deep child cache");
    for (i = 0; i < 40; i++) {
      index_demodulator(deep_rules[i], ORIENTED, DELETE, Index_clock);
      delete_clause(deep_rules[i]);
    }
  }

  for (i = 0; i < 6; i++) {
    if (i != 1)
      index_demodulator(rules[i], types[i], DELETE, Index_clock);
    delete_clause(rules[i]);
  }
  destroy_demodulation_index();
  compact_rewrite_free(bank);

  {
    Compact_rewrite_bank block_bank = compact_rewrite_init();
    Topform block_rules[300];
    Topform block_root;
    struct overlap_result overlap;
    char text[64];
    for (i = 0; i < 300; i++) {
      (void) snprintf(text, sizeof(text), "z%d(f(x)) = x.", i);
      block_rules[i] = parse_clause_from_string(text);
      block_rules[i]->id = 10000 + (unsigned long long) i;
      mark_oriented_eq(block_rules[i]->literals->atom);
      CHECK(compact_rewrite_add(block_bank, block_rules[i], ORIENTED),
            "append rewrite occurrence across a block boundary");
    }
    block_root = parse_clause_from_string("f(x) = x.");
    block_root->id = 20000;
    mark_oriented_eq(block_root->literals->atom);
    CHECK(compact_rewrite_add(block_bank, block_root, ORIENTED),
          "add overlap query rule");
    memset(&overlap, 0, sizeof(overlap));
    overlap.expected = block_rules[299]->id;
    compact_rewrite_visit_overlaps(
      block_bank, block_root->id, note_overlap, &overlap);
    CHECK(overlap.count == 300 && overlap.found,
          "multi-block occurrence stream visits every exact overlap");
    compact_rewrite_get_stats(block_bank, &stats);
    CHECK(stats.occurrence_stream_used > 0 &&
          stats.occurrence_stream_bytes >= stats.occurrence_stream_used,
          "multi-block occurrence byte accounting is exact");
    compact_rewrite_free(block_bank);
    for (i = 0; i < 300; i++)
      delete_clause(block_rules[i]);
    delete_clause(block_root);
  }

  {
    Compact_term_pool high_pool = compact_term_pool_init();
    Compact_rewrite_bank high_bank;
    Compact_term_rebase_map high_map = compact_term_rebase_map_init();
    Topform high_rule = parse_clause_from_string("hf(x) = x.");
    Topform high_query = parse_clause_from_string("p(hf(a)).");
    Topform expected = parse_clause_from_string("p(a).");
    compact_term_pool_set_logical_base(
      high_pool, (unsigned long long) UINT32_MAX + 321ULL);
    high_bank = compact_rewrite_init_with_pool(high_pool);
    high_rule->id = 30001;
    mark_oriented_eq(high_rule->literals->atom);
    CHECK(compact_rewrite_add(high_bank, high_rule, ORIENTED),
          "rewrite bank admits a rule above the 32-bit token boundary");
    compact_rewrite_clause(high_bank, high_query, -1, -1, FALSE, TRUE);
    CHECK(clause_ident(high_query->literals, expected->literals),
          "high-base radix retrieval and contractum construction are exact");
    CHECK(compact_term_rebase_map_retain_clause(
            high_map, high_pool, high_rule->id),
          "high-base rewrite clause is retained for pool compaction");
    compact_term_pool_compact_retained(high_pool, high_map);
    compact_rewrite_rebase_term_pool(high_bank, high_pool, high_map);
    delete_clause(high_query);
    high_query = parse_clause_from_string("p(hf(b)).");
    delete_clause(expected);
    expected = parse_clause_from_string("p(b).");
    compact_rewrite_clause(high_bank, high_query, -1, -1, FALSE, TRUE);
    CHECK(clause_ident(high_query->literals, expected->literals),
          "high-base rewrite remains exact after retained rebasing");
    compact_term_rebase_map_free(high_map);
    compact_rewrite_free(high_bank);
    compact_term_pool_free(high_pool);
    delete_clause(high_rule);
    delete_clause(high_query);
    delete_clause(expected);
  }
  free_clock(Index_clock);

  if (Failures != 0) {
    fprintf(stderr, "compact_rewrite_test: %d failure(s)\n", Failures);
    return 1;
  }
  printf("compact_rewrite_test: PASS\n");
  return 0;
}
