/* Accelerated CPU/RAM longevity probes for compact indexes.

   This is deliberately synthetic: it makes historical-population and
   unrelated-bucket slopes visible in seconds, before a day-long prover run.
   Output is JSON Lines so powers-of-ten runs can be compared mechanically. */

#include "../provers.src/compact_back_demod.h"
#include "../provers.src/compact_feature_index.h"

#include <errno.h>
#include <stdint.h>

static int Failures;

#define CHECK(test, message) do {                                      \
  if (!(test)) {                                                       \
    fprintf(stderr, "FAIL: %s (line %d)\n", (message), __LINE__);     \
    Failures++;                                                        \
  }                                                                    \
} while (0)

static uint64_t mix64(uint64_t x)
{
  x ^= x >> 30;
  x *= UINT64_C(0xbf58476d1ce4e5b9);
  x ^= x >> 27;
  x *= UINT64_C(0x94d049bb133111eb);
  x ^= x >> 31;
  return x;
}

static size_t parse_size(const char *text, size_t low, size_t high,
                         const char *name)
{
  unsigned long long n;
  char *end = NULL;
  errno = 0;
  n = strtoull(text, &end, 10);
  if (errno != 0 || end == text || *end != '\0' || n < low || n > high) {
    fprintf(stderr, "compact_long_run_test: invalid %s: %s\n", name, text);
    exit(2);
  }
  return (size_t) n;
}

/* All subjects have the same shallow f(g(g(g(...)))) signature.  Their
   distinguishing symbols occur below mask8's shallow path summary, whereas
   the code tree can select the complete ground term. */
static void encoded_subject(char *buffer, size_t size, uint64_t serial)
{
  size_t used = 0;
  unsigned depth = 24, i;
  used += (size_t) snprintf(buffer + used, size - used, "f(g(g(g(");
  for (i = 0; i < depth; i++)
    used += (size_t) snprintf(buffer + used, size - used,
                              (serial & (UINT64_C(1) << i)) ? "r(" : "l(");
  used += (size_t) snprintf(buffer + used, size - used, "z");
  for (i = 0; i < depth + 4; i++)
    used += (size_t) snprintf(buffer + used, size - used, ")");
  if (used >= size)
    fatal_error("compact_long_run_test: encoded term overflow");
}

static Topform indexed_clause(const char *text)
{
  Topform c = parse_clause_from_string((char *) text);
  if (c == NULL)
    fatal_error("compact_long_run_test: clause parse failed");
  assign_clause_id(c);
  return c;
}

static void back_demod_longevity(size_t population, size_t queries)
{
  Compact_back_demod_index mask, hot;
  struct compact_back_demod_stats mask_before, hot_before;
  struct compact_back_demod_stats hot_progress, mask_final, hot_final;
  Topform *clauses = safe_malloc(population * sizeof(*clauses));
  Topform demod;
  char subject[512], text[600];
  unsigned long long *mask_ids, *hot_ids;
  size_t mask_count, hot_count, i, q, warmup_queries = 0;
  unsigned long long mask_steady_work, hot_steady_work;
  double mask_work_per_query, hot_work_per_query;
  const char *gate;

  compact_back_demod_set_strategy(COMPACT_BACK_DEMOD_MASK8);
  mask = compact_back_demod_init();
  compact_back_demod_set_strategy(COMPACT_BACK_DEMOD_HOT_ROOT_TREE);
  compact_back_demod_set_tree_budget_kb(64U * 1024U);
  compact_back_demod_set_tree_admit_work(4096);
  compact_back_demod_set_tree_build_factor(8);
  hot = compact_back_demod_init();

  for (i = 0; i < population; i++) {
    encoded_subject(subject, sizeof(subject), (uint64_t) i);
    (void) snprintf(text, sizeof(text), "p(%s).", subject);
    clauses[i] = indexed_clause(text);
    CHECK(compact_back_demod_add(mask, clauses[i]),
          "mask8 accepts long-run subject");
    CHECK(compact_back_demod_add(hot, clauses[i]),
          "hot-root index accepts long-run subject");
  }

  encoded_subject(subject, sizeof(subject), (uint64_t) population / 2);
  (void) snprintf(text, sizeof(text), "%s = z.", subject);
  demod = indexed_clause(text);
  mark_oriented_eq(demod->literals->atom);

  mask_ids = compact_back_demod_candidate_ids(mask, demod, ORIENTED,
                                               &mask_count);
  hot_ids = compact_back_demod_candidate_ids(hot, demod, ORIENTED,
                                              &hot_count);
  CHECK(mask_count == hot_count &&
        (mask_count == 0 ||
         memcmp(mask_ids, hot_ids, mask_count * sizeof(*mask_ids)) == 0),
        "hot-root first query preserves mask8 answer order");
  safe_free(mask_ids);
  safe_free(hot_ids);
  compact_back_demod_get_stats(hot, &hot_progress);
  while (hot_progress.tree_root_admissions == 0 &&
         hot_progress.tree_root_rejections == 0 &&
         warmup_queries < 128) {
    mask_ids = compact_back_demod_candidate_ids(mask, demod, ORIENTED,
                                                 &mask_count);
    hot_ids = compact_back_demod_candidate_ids(hot, demod, ORIENTED,
                                                &hot_count);
    CHECK(mask_count == hot_count &&
          (mask_count == 0 ||
           memcmp(mask_ids, hot_ids, mask_count * sizeof(*mask_ids)) == 0),
          "hot-root admission query preserves mask8 answer order");
    safe_free(mask_ids);
    safe_free(hot_ids);
    warmup_queries++;
    compact_back_demod_get_stats(hot, &hot_progress);
  }
  compact_back_demod_get_stats(mask, &mask_before);
  compact_back_demod_get_stats(hot, &hot_before);
  for (q = 0; q < queries; q++) {
    mask_ids = compact_back_demod_candidate_ids(mask, demod, ORIENTED,
                                                 &mask_count);
    hot_ids = compact_back_demod_candidate_ids(hot, demod, ORIENTED,
                                                &hot_count);
    CHECK(mask_count == hot_count &&
          (mask_count == 0 ||
           memcmp(mask_ids, hot_ids, mask_count * sizeof(*mask_ids)) == 0),
          "hot-root steady query preserves mask8 answer order");
    safe_free(mask_ids);
    safe_free(hot_ids);
  }
  compact_back_demod_get_stats(mask, &mask_final);
  compact_back_demod_get_stats(hot, &hot_final);
  mask_steady_work = mask_final.query_profile.work -
                     mask_before.query_profile.work;
  hot_steady_work = hot_final.query_profile.work -
                    hot_before.query_profile.work;
  mask_work_per_query = (double) mask_steady_work / queries;
  hot_work_per_query = (double) hot_steady_work / queries;
  gate = hot_final.tree_root_admissions > 0 &&
         (mask_steady_work == 0 || hot_steady_work * 10 < mask_steady_work) ?
    "pass" : "fail";
  if (strcmp(gate, "pass") != 0)
    Failures++;

  printf("{\"component\":\"back_demod\",\"phase\":\"warm\","
         "\"population\":%llu,\"queries\":%llu,\"answers\":%llu,"
         "\"admission_queries\":%llu,"
         "\"mask_work_per_query\":%.3f,"
         "\"hot_work_per_query\":%.3f,"
         "\"mask_bytes\":%llu,\"hot_bytes\":%llu,"
         "\"hot_admissions\":%llu,\"hot_rejections\":%llu,"
         "\"hot_cost_deferrals\":%llu,\"hot_censuses\":%llu,"
         "\"hot_census_occurrences\":%llu,"
         "\"mask_lookup_cpu\":%.6f,\"hot_lookup_cpu\":%.6f,"
         "\"hot_maintenance_cpu\":%.6f,\"gate\":\"%s\"}\n",
         (unsigned long long) population, (unsigned long long) queries,
         (unsigned long long) hot_count,
         (unsigned long long) warmup_queries + 1, mask_work_per_query,
         hot_work_per_query, mask_final.total_bytes, hot_final.total_bytes,
         hot_final.tree_root_admissions, hot_final.tree_root_rejections,
         hot_final.tree_root_cost_deferrals, hot_final.tree_root_censuses,
         hot_final.tree_root_census_occurrences,
         mask_final.lookup_seconds, hot_final.lookup_seconds,
         hot_final.maintenance_seconds, gate);

  compact_back_demod_free(mask);
  compact_back_demod_free(hot);
  delete_clause(demod);
  for (i = 0; i < population; i++)
    delete_clause(clauses[i]);
  safe_free(clauses);
}

/* This deliberately adversarial family has one compatible structural summary
   in a huge identical numerical-feature leaf.  It detects whether structural
   rejection happens before, or only after, a linear posting scan. */
static void nonunit_same_leaf_probe(size_t population)
{
  Compact_feature_index index = compact_feature_index_init(2, TRUE);
  struct compact_feature_index_stats before, after;
  struct compact_feature_structural_summary stored = {0, 0, 0};
  struct compact_feature_structural_summary query = {
    UINT64_MAX, 0, 0
  };
  int vector[2] = {1, 1};
  unsigned long long *ids;
  unsigned long long work;
  size_t i, count;
  const char *gate;

  for (i = 0; i < population; i++) {
    stored.rigid = i + 1 == population ? UINT64_MAX :
      mix64((uint64_t) i + 1) & ~UINT64_C(1);
    CHECK(compact_feature_index_add(index, (unsigned long long) i + 1,
                                    vector, stored),
          "add same-leaf structural record");
  }
  compact_feature_index_get_stats(index, &before);
  ids = compact_feature_back_candidates(index, vector, query, &count);
  compact_feature_index_get_stats(index, &after);
  work = after.back_profile.work - before.back_profile.work;
  CHECK(count == 1 && ids != NULL && ids[0] == population,
        "same-leaf probe preserves the sole compatible answer");
  gate = work <= 64 ? "pass" : "fail";
  printf("{\"component\":\"nonunit\","
         "\"phase\":\"same_feature_leaf\",\"population\":%llu,"
         "\"answers\":%llu,\"work\":%llu,\"bytes\":%llu,"
         "\"structural_rejects\":%llu,\"gate\":\"%s\"}\n",
         (unsigned long long) population, (unsigned long long) count,
         work, after.total_bytes,
         after.back_structural_rejects - before.back_structural_rejects,
         gate);
  safe_free(ids);
  compact_feature_index_free(index);
}

int main(int argc, char **argv)
{
  size_t population = 10000;
  size_t queries = 64;
  if (argc > 1)
    population = parse_size(argv[1], 100, 1000000, "population");
  if (argc > 2)
    queries = parse_size(argv[2], 2, 1000000, "queries");
  if (argc > 3) {
    fprintf(stderr, "usage: %s [population:100..1000000] "
                    "[queries:2..1000000]\n", argv[0]);
    return 2;
  }
  init_standard_ladr();
  clear_clause_id_tab();
  set_clause_id_count(0);
  back_demod_longevity(population, queries);
  nonunit_same_leaf_probe(population);
  clear_clause_id_tab();
  set_clause_id_count(0);
  if (Failures != 0) {
    fprintf(stderr, "compact_long_run_test: %d failure(s)\n", Failures);
    return 1;
  }
  return 0;
}
