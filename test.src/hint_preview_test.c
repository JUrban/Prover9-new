#include "../ladr/ladr.h"

static int Failures;

#define CHECK(test, message) do {                                      \
  if (!(test)) {                                                       \
    fprintf(stderr, "FAIL: %s (line %d)\n", (message), __LINE__);   \
    Failures++;                                                        \
  }                                                                    \
} while (0)

static void no_op_demod(Topform clause, int step_limit, int increase_limit,
                        BOOL print, BOOL lex_order_vars)
{
  (void) clause;
  (void) step_limit;
  (void) increase_limit;
  (void) print;
  (void) lex_order_vars;
}

static void run_case(BOOL packed, BOOL better, BOOL fast, int bsub)
{
  Topform hint = parse_clause_from_string("p(f(a)).");
  Topform candidate = parse_clause_from_string("p(f(a)).");
  Topform match;
  unsigned long long epoch_before, checks_before, checks_after;
  unsigned long long nb, rb, tb, nb_before, rb_before, tb_before;
  double adjusted = -1;
  BOOL flipped = TRUE;
  Attribute candidate_attributes = candidate->attributes;

  hint->attributes = set_int_attribute(hint->attributes, bsub, 7);
  init_hints(ORDINARY_UNIF, bsub, FALSE, FALSE, 2, packed, better, fast, 2048,
             8, NULL);
  index_hint(hint);
  epoch_before = hint_state_epoch();
  packed_hint_index_stats(&nb, &rb, &tb, &checks_before);
  nb_before = nb;
  rb_before = rb;
  tb_before = tb;

  match = preview_weight_with_hints(candidate, 42.0, TRUE, FALSE,
                                    &adjusted, &flipped);
  packed_hint_index_stats(&nb, &rb, &tb, &checks_after);
  CHECK(match == hint, "preview returns authoritative matcher identity");
  CHECK(adjusted == 7.0, "preview applies bsub_wt");
  CHECK(!flipped, "ordinary preview is not a flip match");
  CHECK(candidate->matching_hint == NULL, "preview does not assign matcher");
  CHECK(candidate->weight == 0, "preview does not assign candidate weight");
  CHECK(candidate->attributes == candidate_attributes,
        "preview does not assign candidate attributes");
  CHECK(hint->weight == 0 && hint->last_matched_given == 0,
        "preview does not update hint match state");
  CHECK(hint_state_epoch() == epoch_before,
        "preview does not advance hint epoch");
  CHECK(checks_after == checks_before,
        "preview does not charge packed candidate checks");
  CHECK(nb == nb_before && rb == rb_before && tb == tb_before,
        "preview does not change packed index allocations");

  candidate->weight = 42.0;
  adjust_weight_with_hints(candidate, TRUE, FALSE);
  CHECK(candidate->matching_hint == match,
        "preview and authoritative matcher identities agree");
  CHECK(candidate->weight == adjusted,
        "preview and authoritative adjusted weights agree");

  unindex_hint(hint);
  done_with_hints();
  delete_clause(candidate);
  delete_clause(hint);
}

static void run_variable_cache_case(int bsub)
{
  const char *text = "cache_probe(wide(a,b,c,d,e,f,g,h,i,j)).";
  Topform hint = parse_clause_from_string((char *) text);
  Topform first = parse_clause_from_string((char *) text);
  Topform second = parse_clause_from_string((char *) text);
  unsigned long long maximum_keys = 0;
  unsigned long long cache_bytes = 0;
  BOOL saw_cache = FALSE, saw_no_overflow = FALSE;
  FILE *stats = tmpfile();
  char line[4096];

  hint->attributes = set_int_attribute(hint->attributes, bsub, 5);
  init_hints(ORDINARY_UNIF, bsub, FALSE, FALSE, 2,
             TRUE, TRUE, TRUE, 64, 8, NULL);
  index_hint(hint);
  adjust_weight_with_hints(first, FALSE, FALSE);
  adjust_weight_with_hints(second, FALSE, FALSE);
  CHECK(first->matching_hint == hint && second->matching_hint == hint,
        "variable-length cache preserves authoritative matches");
  CHECK(stats != NULL, "open cache statistics stream");
  if (stats != NULL) {
    fprint_packed_hint_operation_stats(stats);
    rewind(stats);
    while (fgets(line, sizeof(line), stats) != NULL) {
      char *maximum = strstr(line, "max_keys=");
      char *table = strstr(line, "table_bytes=");
      if (strstr(line, "Packed_fast_cache:") != NULL) {
        saw_cache = TRUE;
        saw_no_overflow = strstr(line, "key_overflow=0,") != NULL;
      }
      if (maximum != NULL)
        (void) sscanf(maximum, "max_keys=%llu", &maximum_keys);
      if (table != NULL)
        (void) sscanf(table, "table_bytes=%llu", &cache_bytes);
    }
    fclose(stats);
  }
  CHECK(saw_cache && saw_no_overflow,
        "complete cache key fits without a fixed-profile overflow");
  CHECK(maximum_keys > 8,
        "cache accepts a complete profile beyond the former eight-key cap");
  CHECK(cache_bytes <= 64 * 1024 && cache_bytes > 0,
        "fast cache allocation obeys its KiB budget");
  unindex_hint(hint);
  done_with_hints();
  delete_clause(first);
  delete_clause(second);
  delete_clause(hint);
}

static void run_observed_stale_rebuild_case(int bsub)
{
  Topform stale = parse_clause_from_string("stale_probe(f(a)).");
  Topform candidate = parse_clause_from_string("stale_probe(f(a)).");
  unsigned long long rebuilds = 0, scan_triggers = 0;
  BOOL saw_maintenance = FALSE, saw_clean_postings = FALSE;
  FILE *stats;
  char line[4096];
  unsigned i;

  init_hints(ORDINARY_UNIF, bsub, FALSE, FALSE, 2,
             TRUE, TRUE, FALSE, 0, 1, NULL);
  index_hint(stale);
  unindex_hint(stale);

  /* The production floor intentionally avoids rebuild churn on short-lived
     garbage.  Cross it with a tiny one-posting query so this test exercises
     the observed-work trigger rather than the storage-volume trigger. */
  for (i = 0; i < 66000; i++)
    adjust_weight_with_hints(candidate, FALSE, FALSE);

  stats = tmpfile();
  CHECK(stats != NULL, "open posting-maintenance statistics stream");
  if (stats != NULL) {
    fprint_packed_hint_operation_stats(stats);
    rewind(stats);
    while (fgets(line, sizeof(line), stats) != NULL) {
      char *field;
      if (strstr(line, "Better_packed_postings:") != NULL) {
        field = strstr(line, "stale_features=");
        saw_clean_postings = field != NULL &&
          strtoull(field + strlen("stale_features="), NULL, 10) == 0;
        field = strstr(line, "rebuilds=");
        if (field != NULL)
          rebuilds = strtoull(field + strlen("rebuilds="), NULL, 10);
      }
      if (strstr(line, "Better_packed_maintenance:") != NULL) {
        saw_maintenance = TRUE;
        field = strstr(line, "scan_triggers=");
        if (field != NULL)
          scan_triggers = strtoull(field + strlen("scan_triggers="),
                                   NULL, 10);
      }
    }
    fclose(stats);
  }
  CHECK(saw_maintenance, "report observed-work maintenance statistics");
  CHECK(scan_triggers > 0 && rebuilds > 0,
        "observed stale scans trigger a posting rebuild");
  CHECK(saw_clean_postings, "posting rebuild removes stale references");

  done_with_hints();
  delete_clause(candidate);
  delete_clause(stale);
}

static void run_back_fingerprint_case(int bsub)
{
  Topform exact = parse_clause_from_string("p(f(g(h(a)))).");
  Topform deep_mismatch = parse_clause_from_string("p(f(g(h(b)))).");
  Topform demod = parse_clause_from_string("f(g(h(a))) = z.");
  unsigned long long rejects = 0, positives = 0, rewrites = 0;
  BOOL saw_back = FALSE;
  FILE *stats;
  char line[4096];

  init_hints(ORDINARY_UNIF, bsub, FALSE, TRUE, 2,
             TRUE, TRUE, TRUE, 2048, 8, no_op_demod);
  index_hint(exact);
  index_hint(deep_mismatch);
  back_demod_hints(demod, ORIENTED, FALSE);
  stats = tmpfile();
  CHECK(stats != NULL, "open back-fingerprint statistics stream");
  if (stats != NULL) {
    fprint_packed_hint_operation_stats(stats);
    rewind(stats);
    while (fgets(line, sizeof(line), stats) != NULL)
      if (strstr(line, "Packed_hint_operation: op=back_demod,") != NULL) {
        char *field;
        saw_back = TRUE;
        field = strstr(line, "fingerprint_rejects=");
        if (field != NULL)
          rejects = strtoull(
            field + strlen("fingerprint_rejects="), NULL, 10);
        field = strstr(line, "exact_positive=");
        if (field != NULL)
          positives = strtoull(
            field + strlen("exact_positive="), NULL, 10);
        field = strstr(line, "rewrites=");
        if (field != NULL)
          rewrites = strtoull(field + strlen("rewrites="), NULL, 10);
      }
    fclose(stats);
  }
  CHECK(saw_back && rejects == 1,
        "deep back fingerprint rejects only the impossible hint");
  CHECK(positives == 1 && rewrites == 0,
        "deep back fingerprint preserves exact rewrite authority");

  unindex_hint(exact);
  unindex_hint(deep_mismatch);
  done_with_hints();
  delete_clause(demod);
  delete_clause(deep_mismatch);
  delete_clause(exact);
}

int main(void)
{
  init_standard_ladr();
  (void) register_attribute("label", STRING_ATTRIBUTE);
  int bsub = register_attribute("preview_bsub_wt", INT_ATTRIBUTE);
  run_case(FALSE, FALSE, FALSE, bsub);
  run_case(TRUE, FALSE, FALSE, bsub);
  run_case(TRUE, TRUE, FALSE, bsub);
  run_case(TRUE, TRUE, TRUE, bsub);
  run_variable_cache_case(bsub);
  run_observed_stale_rebuild_case(bsub);
  run_back_fingerprint_case(bsub);
  if (Failures != 0) {
    fprintf(stderr, "hint_preview_test: %d failure(s)\n", Failures);
    return 1;
  }
  printf("hint_preview_test: PASS\n");
  return 0;
}
