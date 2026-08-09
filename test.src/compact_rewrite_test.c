#include "../provers.src/compact_rewrite.h"
#include "../provers.src/demodulate.h"

#include <stdio.h>
#include <string.h>

static int Failures;
static Clock Index_clock;

#define CHECK(test, message) do {                                      \
  if (!(test)) {                                                       \
    fprintf(stderr, "FAIL: %s (line %d)\n", (message), __LINE__);   \
    Failures++;                                                        \
  }                                                                    \
} while (0)

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
  CHECK(clause_ident(legacy->literals, compact->literals),
        "compact normal form equals legacy normal form");
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
  Topform rules[5];
  int types[5];
  int i;
  struct compact_rewrite_stats stats;

  init_standard_ladr();
  Index_clock = clock_init("compact rewrite test index");
  init_demodulator_index(DISCRIM_BIND, ORDINARY_UNIF, 0);
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

  compare_case(bank, "p(f(a)).");
  compare_case(bank, "p(g(f(a),f(a))).");
  compare_case(bank, "p(g(f(a),f(b))).");
  compare_case(bank, "p(k(g(a,a),g(a,a))).");
  compare_case(bank, "p(u(a),v(a)).");
  compare_case(bank, "g(f(a),f(a)) = k(f(b),f(b)).");

  compact_rewrite_get_stats(bank, &stats);
  CHECK(stats.rules_current == 5, "compact rule count");
  CHECK(stats.attempts > 0 && stats.rewrites > 0,
        "compact rewrite accounting");
  CHECK(stats.node_bytes > 0 && stats.posting_bytes > 0 &&
        stats.rule_bytes > 0 && stats.term_bytes > 0 &&
        stats.hash_bytes > 0 && stats.total_bytes > 0,
        "compact byte attribution");

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

  CHECK(compact_rewrite_remove(bank, 102), "remove compact rule");
  CHECK(!compact_rewrite_contains(bank, 102), "removed rule is absent");
  index_demodulator(rules[1], types[1], DELETE, Index_clock);
  compare_case(bank, "p(g(a,a)).");

  for (i = 0; i < 5; i++) {
    if (i != 1)
      index_demodulator(rules[i], types[i], DELETE, Index_clock);
    delete_clause(rules[i]);
  }
  destroy_demodulation_index();
  compact_rewrite_free(bank);
  free_clock(Index_clock);

  if (Failures != 0) {
    fprintf(stderr, "compact_rewrite_test: %d failure(s)\n", Failures);
    return 1;
  }
  printf("compact_rewrite_test: PASS\n");
  return 0;
}
